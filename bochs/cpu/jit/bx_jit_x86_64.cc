/////////////////////////////////////////////////////////////////////////
// $Id$
/////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2026  The Bochs Project
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
/////////////////////////////////////////////////////////////////////////

// Bochs trace-caching JIT - x86-64 native code emitter.
//
// For each decoded trace (a run of bxInstruction_c in an icache entry) this
// emits a native stub that exactly reproduces the non-chaining interpreter
// loop body for that trace, but with two key differences that make it faster:
//   * the CPU pointer is pinned in r15, so per-instruction state updates
//     (RIP, prev_rip, icount, async_event) are straight-line memory ops
//     instead of going through the interpreter's dispatch machinery;
//   * each instruction's execute1 handler is called directly via an
//     immediate-loaded function pointer, instead of being loaded from the
//     bxInstruction_c every iteration.
//
// Per instruction k the stub emits (matching cpu.cc's non-chaining loop):
//   RIP += i[k].ilen();
//   execute1(&i[k]);
//   prev_rip = RIP;
//   icount++;
//   bx_pc_system.tick1();          // BX_SYNC_TIME_IF_SINGLE_PROCESSOR(0)
//   if (async_event) goto exit;
//
// The BX_INSTR_BEFORE/AFTER_EXECUTION hooks are no-ops in the default stub
// instrumentation build and are intentionally omitted; a build using a real
// instrumentation module should keep the JIT disabled.
//
// Calling convention: BX_USE_CPU_SMF=1, so execute1 is a plain
//   void f(bxInstruction_c*)
// with the argument in the host ABI's first integer register (rcx on Windows
// x64, rdi on System V x64). No "this" pointer is passed.

#include "bochs.h"
#include "cpu.h"
#include "pc_system.h"
#include "jit/bx_jit.h"
#include "jit/bx_jit_x86_64.h"

#include <string.h>

#if BX_SUPPORT_JIT

// Host x86-64 calling convention for the single execute1 argument.
#ifdef WIN32
static const unsigned ARG_REG = 1;   // rcx (Windows x64)
#else
static const unsigned ARG_REG = 7;   // rdi (System V x64)
#endif

static const unsigned RAX = 0;
static const unsigned R15 = 15;

// ---- raw emit helpers -------------------------------------------------

static inline void e8(Bit8u *&p, Bit8u b) { *p++ = b; }
static inline void e32(Bit8u *&p, Bit32u v) { memcpy(p, &v, 4); p += 4; }
static inline void e64(Bit8u *&p, Bit64u v) { memcpy(p, &v, 8); p += 8; }

// mov r64, imm64  (REX.W + B8+rd, imm64)
static inline void mov_r64_imm64(Bit8u *&p, unsigned reg, Bit64u val) {
  *p++ = (Bit8u)(0x48 | ((reg >> 3) & 1));
  *p++ = (Bit8u)(0xB8 + (reg & 7));
  e64(p, val);
}

// call r64  (FF /2)
static inline void call_r64(Bit8u *&p, unsigned reg) {
  if (reg >= 8) *p++ = 0x41;
  *p++ = 0xFF;
  *p++ = (Bit8u)(0xD0 + (reg & 7));
}

// push r64 / pop r64
static inline void push_r64(Bit8u *&p, unsigned reg) {
  if (reg >= 8) *p++ = 0x41;
  *p++ = (Bit8u)(0x50 + (reg & 7));
}
static inline void pop_r64(Bit8u *&p, unsigned reg) {
  if (reg >= 8) *p++ = 0x41;
  *p++ = (Bit8u)(0x58 + (reg & 7));
}

// add qword [r15+disp32], imm32   (REX.WB 81 /0)
static inline void add_m64r15_imm32(Bit8u *&p, Bit32s disp, Bit32u imm) {
  *p++ = 0x49; *p++ = 0x81; *p++ = 0x87; e32(p, (Bit32u) disp); e32(p, imm);
}
// mov rax, qword [r15+disp32]     (REX.WB 8B /r)
static inline void mov_rax_m64r15(Bit8u *&p, Bit32s disp) {
  *p++ = 0x49; *p++ = 0x8B; *p++ = 0x87; e32(p, (Bit32u) disp);
}
// mov qword [r15+disp32], rax     (REX.WB 89 /r)
static inline void mov_m64r15_rax(Bit8u *&p, Bit32s disp) {
  *p++ = 0x49; *p++ = 0x89; *p++ = 0x87; e32(p, (Bit32u) disp);
}
// inc qword [r15+disp32]          (REX.WB FF /0)
static inline void inc_m64r15(Bit8u *&p, Bit32s disp) {
  *p++ = 0x49; *p++ = 0xFF; *p++ = 0x87; e32(p, (Bit32u) disp);
}
// mov eax, dword [r15+disp32]     (REX.B 8B /r, 32-bit operand)
static inline void mov_eax_m32r15(Bit8u *&p, Bit32s disp) {
  *p++ = 0x41; *p++ = 0x8B; *p++ = 0x87; e32(p, (Bit32u) disp);
}
// test eax, eax  (32-bit)
static inline void test_eax_eax(Bit8u *&p) { *p++ = 0x85; *p++ = 0xC0; }
// test rax, rax  (64-bit)
static inline void test_rax_rax(Bit8u *&p) { *p++ = 0x48; *p++ = 0x85; *p++ = 0xC0; }

// mov r64, r15   (move the pinned CPU pointer into a register)
static inline void mov_reg_r15(Bit8u *&p, unsigned dest) {
  *p++ = (Bit8u)(0x49 | (((dest >> 3) & 1) ? 0x04 : 0));  // REX.W + REX.B (+R)
  *p++ = 0x8B;
  *p++ = (Bit8u)(0xC0 | ((dest & 7) << 3) | 7);
}

// jmp rax  (FF /4)
static inline void jmp_rax(Bit8u *&p) { *p++ = 0xFF; *p++ = 0xE0; }

// sub rsp, imm8 / add rsp, imm8  (used for the Windows x64 shadow space,
// which also keeps the stack 16-byte aligned for the System V ABI)
static inline void sub_rsp_imm8(Bit8u *&p, Bit8u imm) {
  *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = imm;
}
static inline void add_rsp_imm8(Bit8u *&p, Bit8u imm) {
  *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = imm;
}

// Per-instruction system clock tick (matches the non-chaining interpreter's
// BX_SYNC_TIME_IF_SINGLE_PROCESSOR(0), which is BX_TICK1() for a single CPU).
void bx_jit_tick1(void)
{
  bx_pc_system.tick1();
}

// Read-only successor lookup for cross-trace chaining. Mirrors the
// address computation in BX_CPU_C::getICacheEntry but performs no decode,
// compile, or flush, so it is safe to invoke from a running stub. Returns
// the successor stub's inner entry point, or NULL when the successor is not
// (yet) compiled or lies in a not-yet-prefetched page.
//
// This is also the page-split trace handling: when a trace ends with RIP in
// the next page (a page-split trace), eipBiased >= eipPageWindowSize holds
// and we return NULL, so the stub returns to cpu_loop which re-prefetches
// the new page exactly as the interpreter would. SMC on either page of a
// page-split trace is covered by flushSMC clearing jit_code.
void *bx_jit_lookup_next(BX_CPU_C *cpu)
{
  bx_address eipBiased = cpu->gen_reg[BX_64BIT_REG_RIP].rrx + cpu->eipPageBias;
  if (eipBiased >= cpu->eipPageWindowSize) return NULL;
  bx_phy_address pAddr = cpu->pAddrFetchPage + eipBiased;
  bxICacheEntry_c *e = cpu->iCache.find_entry(pAddr, cpu->fetchModeMask);
  if (e == NULL) return NULL;
  if (e->i->ilen() == 0) return NULL;
  if (e->jit_code == NULL) return NULL;
  return (void *)((Bit8u *) e->jit_code + BX_JIT_STUB_PROLOGUE_SIZE);
}

bool bx_jit_x86_64_emit_trace(BX_CPU_C *cpu, bxICacheEntry_c *entry,
                              void *cache_base, Bit64u /*cache_size*/, Bit64u cache_used,
                              Bit64u *emitted_out, void **code_out)
{
  if (cpu == NULL || entry == NULL || entry->tlen == 0 || entry->i == NULL)
    return false;
  if (entry->tlen > BX_MAX_TRACE_LENGTH) return false;

  // Field offsets within the BX_CPU_C object (baked into the stub as disp32).
  char *base = (char *) cpu;
  Bit32s rip_off      = (Bit32s) ((char *) &cpu->gen_reg[BX_64BIT_REG_RIP].rrx - base);
  Bit32s prev_rip_off = (Bit32s) ((char *) &cpu->prev_rip      - base);
  Bit32s icount_off   = (Bit32s) ((char *) &cpu->icount        - base);
  Bit32s async_off    = (Bit32s) ((char *) &cpu->async_event   - base);

  const Bit64u cpu_ptr = (Bit64u)(bx_ptr_equiv_t) cpu;
  const Bit64u tick1_ptr = (Bit64u)(bx_ptr_equiv_t) &bx_jit_tick1;

  Bit8u *stub_start = (Bit8u *) cache_base + cache_used;
  Bit8u *p = stub_start;

  // Backpatch sites for the per-instruction "jnz exit" and the tail "jz exit"
  // rel32 immediates.
  unsigned patch_site[BX_MAX_TRACE_LENGTH + 2];
  unsigned npatch = 0;

  // --- prologue: pin the CPU pointer in r15 and reserve 32 bytes of shadow
  // space (Windows x64) which also keeps the stack 16-byte aligned (System V)
  // for the per-instruction calls. Stack layout at entry: rsp%16==8; after
  // push r15 -> rsp%16==0; after sub rsp,32 -> rsp%16==0 (call-aligned). ---
  push_r64(p, R15);
  mov_r64_imm64(p, R15, cpu_ptr);
  sub_rsp_imm8(p, 32);

  for (unsigned k = 0; k < entry->tlen; k++)
  {
    bxInstruction_c *ik = &entry->i[k];
    unsigned ilen = ik->ilen();
    Bit64u i_ptr    = (Bit64u)(bx_ptr_equiv_t) ik;
    Bit64u exec_ptr = (Bit64u)(bx_ptr_equiv_t) ik->execute1;

    // RIP += ilen
    add_m64r15_imm32(p, rip_off, (Bit32u) ilen);
    // load the bxInstruction_c* argument
    mov_r64_imm64(p, ARG_REG, i_ptr);
    // direct call to execute1(&i)
    mov_r64_imm64(p, RAX, exec_ptr);
    call_r64(p, RAX);
    // prev_rip = RIP
    mov_rax_m64r15(p, rip_off);
    mov_m64r15_rax(p, prev_rip_off);
    // icount++
    inc_m64r15(p, icount_off);
    // BX_SYNC_TIME (single CPU) -> bx_jit_tick1()
    mov_r64_imm64(p, RAX, tick1_ptr);
    call_r64(p, RAX);
    // if (async_event) goto exit
    mov_eax_m32r15(p, async_off);
    test_eax_eax(p);
    // jnz rel32 (placeholder; backpatched below)
    *p++ = 0x0F; *p++ = 0x85;
    if (npatch < BX_MAX_TRACE_LENGTH)
      patch_site[npatch++] = (unsigned)(p - stub_start);
    e32(p, 0);
  }

  // --- chaining tail: reached only when the last instruction left
  // async_event clear. Try to jump directly to the already-compiled successor
  // trace; if there is none, fall through to the epilogue and return to the
  // interpreter. ---
  mov_reg_r15(p, ARG_REG);                              // arg = cpu (r15)
  mov_r64_imm64(p, RAX, (Bit64u)(bx_ptr_equiv_t) &bx_jit_lookup_next);
  call_r64(p, RAX);                                     // next = lookup_next(cpu)
  test_rax_rax(p);
  // jz rel32 -> epilogue (backpatched)
  *p++ = 0x0F; *p++ = 0x84;
  if (npatch < (BX_MAX_TRACE_LENGTH + 2))
    patch_site[npatch++] = (unsigned)(p - stub_start);
  e32(p, 0);
  jmp_rax(p);                                           // chain to successor

  // --- epilogue ---
  unsigned exit_off = (unsigned)(p - stub_start);
  add_rsp_imm8(p, 32);
  pop_r64(p, R15);
  e8(p, 0xC3);  // ret

  // Backpatch every "jnz exit" with the real rel32 displacement.
  for (unsigned i = 0; i < npatch; i++) {
    Bit8u *imm = stub_start + patch_site[i];
    Bit32s disp = (Bit32s) exit_off - (Bit32s)(patch_site[i] + 4);
    memcpy(imm, &disp, 4);
  }

  *code_out = (void *) stub_start;
  *emitted_out = (Bit64u)(p - stub_start);
  return true;
}

#endif // BX_SUPPORT_JIT
