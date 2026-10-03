/* funchost.c - host for whole-function differential tests of an emitted module.
 *
 * Plays the part suyu's ArmRecomp32 plays: provides an A32HostMem with a real
 * page table for the mapped regions (image, stack, heap) so the generated
 * code's fast path is exercised, and load/store callbacks for everything else
 * (pattern-filled, writes kept in an overlay). Runs the dispatcher loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include "a32_runtime.h"

A32BlockFn recomp_image_lookup(uint64_t pc);
void recomp_image_set_base(uint64_t base);

#define PAGE_BITS 12
#define NPAGES (1u << (32 - PAGE_BITS))

typedef struct { uint32_t va, size; uint8_t* live; uint8_t* pristine; } Region;
static Region regions[3];
static int nregions;
static uintptr_t* page_table;
static A32HostMem host;

/* overlay for addresses outside the regions */
#define OCAP (1u << 20)
static uint32_t o_addr[OCAP];
static uint8_t o_val[OCAP], o_used[OCAP];
static uint32_t o_list[OCAP];
static unsigned o_count;

static inline uint8_t pat(uint32_t a) { return (uint8_t)((a * 0x9E3779B1u) >> 24); }
static unsigned oslot(uint32_t a) {
    unsigned h = (a * 0x85EBCA6Bu) & (OCAP - 1);
    while (o_used[h] && o_addr[h] != a) h = (h + 1) & (OCAP - 1);
    return h;
}
static uint8_t* region_ptr(uint32_t a) {
    for (int k = 0; k < nregions; k++)
        if (a - regions[k].va < regions[k].size) return regions[k].live + (a - regions[k].va);
    return 0;
}
static uint8_t rb(uint32_t a) {
    uint8_t* p = region_ptr(a);
    if (p) return *p;
    unsigned s = oslot(a);
    return o_used[s] ? o_val[s] : pat(a);
}
static int overflow;
static void wbyte(uint32_t a, uint8_t v) {
    uint8_t* p = region_ptr(a);
    if (p) { *p = v; return; }
    if (o_count >= OCAP / 2) { overflow = 1; return; } /* keep the table sparse enough to probe */
    unsigned s = oslot(a);
    if (!o_used[s]) { o_used[s] = 1; o_addr[s] = a; o_list[o_count++] = s; }
    o_val[s] = v;
}
static uint64_t cb_load(void* u, uint64_t va, uint32_t size) {
    (void)u;
    uint64_t v = 0;
    for (uint32_t k = 0; k < size; k++) v |= (uint64_t)rb((uint32_t)va + k) << (8 * k);
    return v;
}
static void cb_store(void* u, uint64_t va, uint32_t size, uint64_t v) {
    (void)u;
    for (uint32_t k = 0; k < size; k++) wbyte((uint32_t)va + k, (uint8_t)(v >> (8 * k)));
}
static uint32_t mon_addr, mon_valid;
static uint64_t cb_excl_load(void* u, uint64_t va, uint32_t size) {
    mon_addr = (uint32_t)va; mon_valid = 1;
    return cb_load(u, va, size);
}
static uint32_t cb_excl_store(void* u, uint64_t va, uint32_t size, uint64_t v) {
    uint32_t ok = mon_valid && mon_addr == (uint32_t)va;
    mon_valid = 0;
    if (!ok) return 1;
    cb_store(u, va, size, v);
    return 0;
}
static void cb_clear(void* u) { (void)u; mon_valid = 0; }
static uint64_t cb_counter(void* u) { (void)u; return 0x0123456789ABCDEFull; }

static void add_region(uint32_t va, uint32_t size, const uint8_t* init, int fill_pattern) {
    Region* r = &regions[nregions++];
    r->va = va; r->size = size;
    r->live = malloc(size); r->pristine = malloc(size);
    for (uint32_t k = 0; k < size; k++) r->pristine[k] = init ? init[k] : (fill_pattern ? pat(va + k) : 0);
    memcpy(r->live, r->pristine, size);
    for (uint32_t pg = va >> PAGE_BITS; pg < (va + size) >> PAGE_BITS; pg++)
        page_table[pg] = (uintptr_t)r->live - (uintptr_t)va;
}

/* image: module image (relocated); regions are page aligned by the caller */
int fh_init(const char* image_path, uint32_t base, uint32_t stack_va, uint32_t stack_size, uint32_t heap_va,
            uint32_t heap_size, int use_page_table) {
    FILE* f = fopen(image_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t img_size = (uint32_t)((n + 0xFFF) & ~0xFFF);
    uint8_t* buf = calloc(1, img_size);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -2; }
    fclose(f);
    page_table = calloc(NPAGES, sizeof(uintptr_t));
    add_region(base, img_size, buf, 0);
    add_region(stack_va, stack_size, 0, 1);
    add_region(heap_va, heap_size, 0, 1);
    free(buf);
    host.user = 0;
    host.load = cb_load; host.store = cb_store;
    host.excl_load = cb_excl_load; host.excl_store = cb_excl_store; host.clear_excl = cb_clear;
    host.read_counter = cb_counter;
    host.page_entries = use_page_table ? page_table : 0;
    host.page_entry_stride = sizeof(uintptr_t);
    host.page_bits = PAGE_BITS;
    host.pointer_mask = ~(uint64_t)0;
    host.address_space_max = (uint64_t)1 << 32;
    recomp_image_set_base(base);
    return (int)img_size;
}

/* status: 0 returned to sentinel, 1 lookup miss, 2 unhandled, 3 svc, 4 block cap, 5 thumb,
 * 6 wrote too much unmapped memory for the test overlay */
int fh_run(A32Context* c, uint32_t sentinel, unsigned max_blocks, unsigned* blocks_run) {
    c->host = &host;
    c->halted = 0;
    c->pending_svc = A32_NO_SVC;
    mon_valid = 0;
    overflow = 0;
    unsigned n = 0;
    int st = 4;
    while (n < max_blocks) {
        if (overflow) { st = 6; break; }
        if (c->thumb) { st = 5; break; }
        if (c->r[15] == sentinel) { st = 0; break; }
        A32BlockFn fn = recomp_image_lookup(c->r[15]);
        if (!fn) { st = 1; break; }
        host.chain_budget = getenv("FH_NOCHAIN") ? 0 : 64;
        fn(c);
        n++;
        if (c->halted) { st = c->halted == A32_HALT_UNHANDLED ? 2 : 3; break; }
    }
    *blocks_run = n;
    return st;
}

/* Bytes changed since the last reset: regions vs pristine, plus the overlay.
 * Resets everything afterwards. Returns the number of entries written. */
int fh_diff_reset(uint32_t* addrs, uint8_t* vals, int max) {
    int n = 0;
    for (int k = 0; k < nregions; k++) {
        Region* r = &regions[k];
        for (uint32_t pg = 0; pg < r->size; pg += 4096) {
            if (!memcmp(r->live + pg, r->pristine + pg, 4096)) continue;
            for (uint32_t b = pg; b < pg + 4096; b++) {
                if (r->live[b] != r->pristine[b]) {
                    if (n < max) { addrs[n] = r->va + b; vals[n] = r->live[b]; }
                    n++;
                }
            }
            memcpy(r->live + pg, r->pristine + pg, 4096);
        }
    }
    for (unsigned k = 0; k < o_count; k++) {
        unsigned s = o_list[k];
        if (n < max) { addrs[n] = o_addr[s]; vals[n] = o_val[s]; }
        n++;
        o_used[s] = 0;
    }
    o_count = 0;
    return n;
}
