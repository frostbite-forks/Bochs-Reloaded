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

// Bochs trace-caching JIT - public interface.
//
// One bxJitC instance lives inside each BX_CPU_C. It owns a translation
// cache (a single executable buffer) and compiles eligible icache traces
// into native code stored on the trace's bxICacheEntry_c::jit_code field.
//
// When BX_SUPPORT_JIT is 0 (the default, or when handler chaining is on)
// every method is a no-op stub, so the JIT has zero overhead and zero
// semantic effect.

#ifndef BX_JIT_H
#define BX_JIT_H

#include "bx_jit_config.h"

class bxICacheEntry_c;
class BX_CPU_C;

class bxJitC {
public:
  bxJitC();
 ~bxJitC();

  // Called once after the sim interface and CPU are set up. Stores the
  // owning CPU pointer (baked into every compiled stub) and allocates the
  // translation cache.
  void init(BX_CPU_C *cpu);
  void exit();

  // JIT is compiled in and enabled at runtime (cache size > 0).
  bool enabled() const { return m_enabled; }

  // Try to compile a decoded trace. On success, stores the native code
  // pointer in entry->jit_code and returns true. Returns false (leaving
  // jit_code NULL) when disabled, ineligible, or compilation fails; the
  // interpreter then runs the trace unchanged.
  bool maybe_compile(bxICacheEntry_c *entry);

  // Drop the compiled code for a single entry (self-modifying-code
  // invalidation). The bytes are not reclaimed; the whole cache is reset
  // only when it fills up.
  void invalidate(bxICacheEntry_c *entry);

  // Drop all compiled code (cache full / global icache flush). Resets the
  // write pointer and clears every icache entry's jit_code pointer so no
  // stale references survive.
  void flush();

  Bit64u compiled_traces() const { return m_compiled; }
  Bit64u compiled_bytes() const { return m_used; }
  Bit64u failed_compiles() const { return m_compiled_fail; }
  Bit64u stub_faults() const { return m_faults; }

  // Record a fault raised inside a stub (caught by the SEH guard in
  // bx_jit_run_stub). After a small burst of faults the JIT disables itself
  // so a persistent codegen bug cannot loop forever.
  void note_fault();

  // Log compile/run counters (called from exit()).
  void print_stats();

private:
  bool       m_enabled;       // runtime enable
  BX_CPU_C  *m_cpu;           // owning CPU (baked into stubs)
  void      *m_cache_base;    // executable translation cache
  Bit64u     m_cache_size;    // total bytes allocated
  Bit64u     m_used;          // bytes consumed by compiled traces
  Bit64u     m_compiled;      // number of successfully compiled traces
  Bit64u     m_compiled_fail; // number of traces that failed compilation
  Bit64u     m_faults;        // number of SEH-caught faults while running stubs

#if BX_SUPPORT_JIT
  bool compile_trace(bxICacheEntry_c *entry);
  bool alloc_executable_cache(Bit64u size_bytes);
#else
  bool compile_trace(bxICacheEntry_c *) { return false; }
  bool alloc_executable_cache(Bit64u) { return false; }
#endif
};

// Run a compiled stub. On Windows/MSVC the call is wrapped in a structured
// exception handler so a codegen bug (or a faulting guest access inside a
// handler) cannot crash the simulator; on any other host it is a plain call.
// Returns true on normal return, false if an exception was caught.
bool bx_jit_run_stub(void *code);

#endif
