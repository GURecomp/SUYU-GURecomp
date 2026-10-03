/* test_host.c - memory/SVC host used by the differential tests.
 * Unwritten memory reads a deterministic pattern so the Unicorn side
 * (which maps pages lazily with the same pattern) sees identical data. */
#include "a32_runtime.h"

#define WCAP 8192
static uint32_t w_addr[WCAP];
static uint8_t w_val[WCAP];
static uint8_t w_used[WCAP];
static int w_count;
static int flags;

static inline uint8_t pat(uint32_t a) { return (uint8_t)((a * 0x9E3779B1u) >> 24); }
static int slot(uint32_t a) {
    uint32_t h = (a * 0x85EBCA6Bu) & (WCAP - 1);
    while (w_used[h] && w_addr[h] != a) h = (h + 1) & (WCAP - 1);
    return (int)h;
}
static uint32_t code_base, code_len;
static uint8_t code_bytes[4096];
void th_set_code(uint32_t base, const uint32_t* w, int n) {
    code_base = base; code_len = (uint32_t)n * 4;
    memcpy(code_bytes, w, code_len);
}
static uint8_t rb(uint32_t a) {
    int s = slot(a);
    if (!w_used[s] && a - code_base < code_len) return code_bytes[a - code_base];
    return w_used[s] ? w_val[s] : pat(a);
}
static void wb(uint32_t a, uint8_t v) {
    int s = slot(a);
    if (!w_used[s]) { w_used[s] = 1; w_addr[s] = a; w_count++; }
    w_val[s] = v;
}
uint8_t a32_read8(A32Context* c, uint32_t a) { (void)c; return rb(a); }
uint16_t a32_read16(A32Context* c, uint32_t a) { (void)c; return (uint16_t)(rb(a) | (rb(a + 1) << 8)); }
uint32_t a32_read32(A32Context* c, uint32_t a) {
    (void)c;
    return (uint32_t)rb(a) | ((uint32_t)rb(a + 1) << 8) | ((uint32_t)rb(a + 2) << 16) | ((uint32_t)rb(a + 3) << 24);
}
void a32_write8(A32Context* c, uint32_t a, uint8_t v) { (void)c; wb(a, v); }
void a32_write16(A32Context* c, uint32_t a, uint16_t v) { (void)c; wb(a, (uint8_t)v); wb(a + 1, (uint8_t)(v >> 8)); }
void a32_write32(A32Context* c, uint32_t a, uint32_t v) {
    (void)c;
    for (int k = 0; k < 4; k++) wb(a + k, (uint8_t)(v >> (8 * k)));
}
uint64_t a32_read_counter(A32Context* c) { (void)c; return 0x0123456789ABCDEFull; }
/* Local exclusive monitor, the single-core behaviour Unicorn models. */
static uint32_t mon_addr, mon_valid;
uint64_t a32_excl_read(A32Context* c, uint32_t a, unsigned size) {
    uint64_t v = size == 1 ? a32_read8(c, a) : size == 2 ? a32_read16(c, a) : size == 4 ? a32_read32(c, a) : a32_read64(c, a);
    mon_addr = a; mon_valid = 1;
    return v;
}
uint32_t a32_excl_write(A32Context* c, uint32_t a, unsigned size, uint64_t v) {
    uint32_t ok = mon_valid && mon_addr == a;
    mon_valid = 0;
    if (!ok) return 1;
    if (size == 1) a32_write8(c, a, (uint8_t)v);
    else if (size == 2) a32_write16(c, a, (uint16_t)v);
    else if (size == 4) a32_write32(c, a, (uint32_t)v);
    else a32_write64(c, a, v);
    return 0;
}
void a32_clrex(A32Context* c) { (void)c; mon_valid = 0; }
void a32_svc(A32Context* c, uint32_t imm) { (void)c; (void)imm; flags |= 2; }
void a32_unhandled(A32Context* c, uint32_t insn, uint32_t pc) { (void)c; (void)insn; (void)pc; flags |= 1; }

extern const A32BlockFn a32_cases[];
extern const unsigned a32_case_count;

int th_run(unsigned idx, A32Context* c) {
    for (int k = 0; k < WCAP; k++) w_used[k] = 0;
    w_count = 0;
    flags = 0;
    mon_valid = 0;
    a32_cases[idx](c);
    return flags;
}
int th_writes(uint32_t* addrs, uint8_t* vals, int max) {
    int n = 0;
    for (int k = 0; k < WCAP && n < max; k++)
        if (w_used[k]) { addrs[n] = w_addr[k]; vals[n] = w_val[k]; n++; }
    return n;
}
