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

// Bochs trace-caching JIT - x86-64 native code emitter interface.
//
// Phase 2 fills in the emitter primitives and emit_trace(). The header is
// kept minimal for now so the build can reference the translation unit.

#ifndef BX_JIT_X86_64_H
#define BX_JIT_X86_64_H

#include "bx_jit_config.h"

#if BX_SUPPORT_JIT

class bxICacheEntry_c;
class BX_CPU_C;

// Byte size of the per-stub prologue (push r15 + mov r15,imm64 + sub rsp,32).
// The inner entry point (skipping the prologue) used for cross-trace chaining
// is jit_code + BX_JIT_STUB_PROLOGUE_SIZE. Must match emit_trace's prologue.
#define BX_JIT_STUB_PROLOGUE_SIZE 16

// Walk entry->i[0 .. entry->tlen-1] and emit a native stub into the JIT
// translation cache at cache_base + cache_used. The stub pins the CPU pointer
// in r15, then for each instruction: advances RIP by i->ilen(), calls
// i->execute1 directly, commits prev_rip, bumps icount, ticks the system
// clock, and exits if async_event becomes set. At the trace tail it tries to
// chain directly to the already-compiled successor trace (read-only lookup),
// falling back to returning to the interpreter when there is no compiled
// successor.
//
// On success returns true, sets *emitted_out to the number of bytes written
// and *code_out to the stub entry point (cache_base + cache_used). Returns
// false on failure (insufficient room is checked by the caller).
bool bx_jit_x86_64_emit_trace(BX_CPU_C *cpu, bxICacheEntry_c *entry,
                              void *cache_base, Bit64u cache_size, Bit64u cache_used,
                              Bit64u *emitted_out, void **code_out);

// Per-instruction system clock tick, matching the non-chaining interpreter's
// BX_SYNC_TIME_IF_SINGLE_PROCESSOR(0) (which is BX_TICK1() for a single CPU).
void bx_jit_tick1(void);

// Read-only lookup of the trace at the current RIP. Returns the successor
// stub's INNER entry point (jit_code + BX_JIT_STUB_PROLOGUE_SIZE) if the
// successor trace is already present in the icache and compiled, else NULL.
// Never decodes, compiles, or flushes, so it is safe to call from a running
// stub.
void *bx_jit_lookup_next(BX_CPU_C *cpu);

#endif

#endif
