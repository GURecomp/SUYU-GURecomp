/* a32_host_runtime.c - the hosted side of recompiled AArch32 code.
 *
 * Linked once into the executable that embeds recompiled modules (suyu-cmd's
 * static build). Every guest access goes through the A32HostMem bridge that
 * Core::ArmRecomp32 installs in the context, so recompiled code and the HLE
 * kernel share one address space.
 *
 * Contract with ArmRecomp32 (src/core/arm/recomp/arm_recomp32.cpp):
 *   - a32_svc parks: sets pending_svc, leaves r15 on the next instruction and
 *     stops the dispatcher. The kernel services the call and resumes.
 *   - a32_unhandled parks r15 on the instruction and sets
 *     halted = A32_HALT_UNHANDLED; the backend runs it on the dynarmic JIT.
 */
#include "a32_runtime.h"

/* Direct pointer to guest memory when the page is plainly mapped, else null.
 * Mirrors Core::Memory's fast path (see PageTableView). */
static unsigned char* host_ptr(A32Context* c, uint32_t va) {
    const A32HostMem* hm = c->host;
    uintptr_t raw, p;
    if (!hm || !hm->page_entries) return 0;
    if ((uint64_t)va >= hm->address_space_max) return 0;
    raw = *(const uintptr_t*)((const unsigned char*)hm->page_entries +
                              ((uint64_t)va >> hm->page_bits) * hm->page_entry_stride);
    p = raw & (uintptr_t)hm->pointer_mask;
    return p ? (unsigned char*)(p + (uintptr_t)va) : 0;
}

/* Accesses that straddle a page boundary must not use one pointer. */
static int same_page(A32Context* c, uint32_t va, unsigned size) {
    const uint64_t bits = c->host->page_bits;
    return ((uint64_t)va >> bits) == ((uint64_t)(va + size - 1) >> bits);
}

uint8_t a32_read8(A32Context* c, uint32_t a) {
    unsigned char* p = host_ptr(c, a);
    if (p) return *p;
    return (uint8_t)c->host->load(c->host->user, a, 1);
}
uint16_t a32_read16(A32Context* c, uint32_t a) {
    unsigned char* p = host_ptr(c, a);
    if (p && same_page(c, a, 2)) { uint16_t v; memcpy(&v, p, 2); return v; }
    return (uint16_t)c->host->load(c->host->user, a, 2);
}
uint32_t a32_read32(A32Context* c, uint32_t a) {
    unsigned char* p = host_ptr(c, a);
    if (p && same_page(c, a, 4)) { uint32_t v; memcpy(&v, p, 4); return v; }
    return (uint32_t)c->host->load(c->host->user, a, 4);
}
void a32_write8(A32Context* c, uint32_t a, uint8_t v) {
    unsigned char* p = host_ptr(c, a);
    if (p) { *p = v; return; }
    c->host->store(c->host->user, a, 1, v);
}
void a32_write16(A32Context* c, uint32_t a, uint16_t v) {
    unsigned char* p = host_ptr(c, a);
    if (p && same_page(c, a, 2)) { memcpy(p, &v, 2); return; }
    c->host->store(c->host->user, a, 2, v);
}
void a32_write32(A32Context* c, uint32_t a, uint32_t v) {
    unsigned char* p = host_ptr(c, a);
    if (p && same_page(c, a, 4)) { memcpy(p, &v, 4); return; }
    c->host->store(c->host->user, a, 4, v);
}

uint64_t a32_excl_read(A32Context* c, uint32_t a, unsigned size) {
    return c->host->excl_load(c->host->user, a, size);
}
uint32_t a32_excl_write(A32Context* c, uint32_t a, unsigned size, uint64_t v) {
    return c->host->excl_store(c->host->user, a, size, v);
}
void a32_clrex(A32Context* c) { c->host->clear_excl(c->host->user); }

uint64_t a32_read_counter(A32Context* c) { return c->host->read_counter(c->host->user); }

void a32_svc(A32Context* c, uint32_t imm) {
    c->pending_svc = imm;
    c->halted = 1;
}

void a32_unhandled(A32Context* c, uint32_t insn, uint32_t pc) {
    (void)insn;
    c->r[15] = pc;
    c->halted = A32_HALT_UNHANDLED;
}
