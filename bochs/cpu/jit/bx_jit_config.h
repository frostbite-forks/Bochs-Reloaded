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
//  but WITHOUT ANY WARRANTY; without the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
/////////////////////////////////////////////////////////////////////////

// Bochs trace-caching JIT - compile-time configuration.
//
// The JIT translates a decoded Bochs instruction trace (a run of
// bxInstruction_c objects stored in the icache) into native host code that
// calls the existing per-instruction execute1 handlers directly, advancing
// RIP and checking async_event between them, instead of going through the
// interpreter's per-instruction indirect dispatch loop.
//
// This design is inspired by WinUAE's JIT (translation cache + per-block
// direct handlers + SMC-driven flush), adapted to the trace cache Bochs
// already maintains.
//
// Correctness constraint: the per-instruction emission model is only valid
// when handler chaining is DISABLED (BX_SUPPORT_HANDLERS_CHAINING_SPEEDUPS
// == 0). With chaining enabled, each execute1 handler tail-calls the next
// instruction itself, so calling handlers per-instruction from the JIT would
// double-execute the trace. configure therefore only defines BX_SUPPORT_JIT
// when handler chaining is off; the #error below catches any mismatch.

#ifndef BX_JIT_CONFIG_H
#define BX_JIT_CONFIG_H

#include "config.h"

#ifndef BX_SUPPORT_JIT
#define BX_SUPPORT_JIT 0
#endif

#if BX_SUPPORT_JIT

  // Translation cache size in KB. Setting the runtime parameter to 0
  // disables the JIT (mirrors WinUAE "Cache Size" == 0).
  #ifndef BX_JIT_CACHE_SIZE_KB
  #define BX_JIT_CACHE_SIZE_KB 8192
  #endif

  // A trace must be observed this many times before it is compiled, so
  // one-shot code is not translated.
  #ifndef BX_JIT_HOT_THRESHOLD
  #define BX_JIT_HOT_THRESHOLD 2
  #endif

  #if !BX_SUPPORT_X86_64
  #error "The Bochs JIT currently requires --enable-x86-64 (64-bit guest RIP)"
  #endif

  #if !(defined(__x86_64__) || defined(_M_X64))
  #error "The Bochs JIT requires an x86-64 host"
  #endif

  #if !BX_USE_CPU_SMF
  #error "The Bochs JIT currently requires a single-CPU (BX_USE_CPU_SMF=1) build"
  #endif

  #if BX_SUPPORT_HANDLERS_CHAINING_SPEEDUPS
  #error "The Bochs JIT requires --disable-handlers-chaining (non-chaining interpreter)"
  #endif

  // The JIT does not emit BX_INSTR_* hooks; it is only correct with the
  // default stub instrumentation (BX_INSTRUMENTATION == 0).
  #if BX_INSTRUMENTATION
  #error "The Bochs JIT is incompatible with non-stub instrumentation (BX_INSTRUMENTATION)"
  #endif

#endif

#endif
