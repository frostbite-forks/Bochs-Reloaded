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

// Bochs trace-caching JIT - core (translation cache + trace compilation).
//
// Owns a single executable translation cache and, for each eligible decoded
// icache trace, asks the x86-64 backend (bx_jit_x86_64.cc) to emit a native
// stub that calls the existing execute1 handlers directly. The stub pointer
// is stored on the icache entry (jit_code) and run from cpu_loop.

#define NEED_CPU_REG_SHORTCUTS 1
#include "bochs.h"
#include "../cpu.h"
#include "bx_jit.h"
#include "bx_jit_x86_64.h"
#define LOG_THIS BX_CPU_THIS_PTR

#include "gui/siminterface.h"
#include "param_names.h"

#if BX_SUPPORT_JIT && !defined(WIN32)
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif
#endif

bxJitC::bxJitC()
  : m_enabled(false),
    m_cpu(NULL),
    m_cache_base(NULL),
    m_cache_size(0),
    m_used(0),
    m_compiled(0),
    m_compiled_fail(0),
    m_faults(0)
{
}

bxJitC::~bxJitC()
{
  bxJitC::exit();
}

void bxJitC::init(BX_CPU_C *cpu)
{
  m_cpu = cpu;
#if BX_SUPPORT_JIT
  bool enable = true;
  Bit64u cache_kb = BX_JIT_CACHE_SIZE_KB;

  bx_param_bool_c *penable = SIM->get_param_bool(BXPN_CPU_JIT_ENABLED);
  if (penable != NULL) enable = penable->get();
  bx_param_num_c *psize = SIM->get_param_num(BXPN_CPU_JIT_CACHE_SIZE);
  if (psize != NULL) cache_kb = (Bit64u) psize->get();

  if (cache_kb == 0) enable = false;

  if (enable) {
    if (alloc_executable_cache(cache_kb * 1024)) {
      m_enabled = true;
      BX_INFO(("JIT: enabled, translation cache %llu KB", (unsigned long long) cache_kb));
    } else {
      m_enabled = false;
      BX_ERROR(("JIT: failed to allocate translation cache, disabling"));
    }
  } else {
    m_enabled = false;
  }
#else
  m_enabled = false;
#endif
}

void bxJitC::exit()
{
#if BX_SUPPORT_JIT
  print_stats();
  if (m_cache_base != NULL) {
#ifdef WIN32
    VirtualFree(m_cache_base, 0, MEM_RELEASE);
#else
    munmap(m_cache_base, (size_t) m_cache_size);
#endif
    m_cache_base = NULL;
  }
#endif
  m_cache_size = 0;
  m_used = 0;
  m_enabled = false;
}

void bxJitC::note_fault()
{
  m_faults++;
  // A burst of faults almost always means a codegen ABI bug; stop generating
  // new stubs so the rest of the run proceeds under the interpreter. Already
  // cached stubs are dropped by the next flush().
  if (m_faults > 64 && m_enabled) {
    m_enabled = false;
    flush();
    BX_ERROR(("JIT: disabled after %llu stub faults", (unsigned long long) m_faults));
  }
}

void bxJitC::print_stats()
{
#if BX_SUPPORT_JIT
  if (m_compiled || m_compiled_fail || m_faults) {
    BX_INFO(("JIT: compiled=%llu traces (%llu bytes), failed=%llu, stub_faults=%llu",
             (unsigned long long) m_compiled,
             (unsigned long long) m_used,
             (unsigned long long) m_compiled_fail,
             (unsigned long long) m_faults));
  }
#endif
}

// Run a compiled stub. On Windows/MSVC the call is wrapped in a structured
// exception handler so a fault inside generated code is caught and reported
// instead of crashing the simulator; the caller drops the entry and falls
// back to the interpreter for that trace. On other hosts the call is direct.
#if BX_SUPPORT_JIT
bool bx_jit_run_stub(void *code)
{
  if (code == NULL) return true;
#if defined(WIN32) && defined(_MSC_VER)
  __try {
    ((void (*)()) code)();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
#else
  ((void (*)()) code)();
  return true;
#endif
}
#else
bool bx_jit_run_stub(void *) { return true; }
#endif

void bxJitC::flush()
{
  // Hard flush: discard all compiled code and reset the write pointer. We
  // also null out every icache entry's jit_code so no live stub pointers
  // remain. This is safe to call from compile_trace (while an icache miss is
  // being served): it touches only jit_code pointers, never the decoded
  // trace data (pAddr/i/tlen), so the entry currently being built stays
  // valid for the interpreter to run.
  m_used = 0;
#if BX_SUPPORT_JIT
  if (m_cpu != NULL) {
    bxICacheEntry_c *e = m_cpu->iCache.entry;
    for (unsigned i = 0; i < BxICacheEntries; i++, e++)
      e->jit_code = NULL;
  }
#endif
}

void bxJitC::invalidate(bxICacheEntry_c *entry)
{
  // The caller (flushSMC / icache invalidation) clears entry->jit_code.
  // Compiled bytes are not reclaimed individually; the whole cache is reset
  // only when it fills (see maybe_compile).
  (void) entry;
}

bool bxJitC::maybe_compile(bxICacheEntry_c *entry)
{
  if (! m_enabled) return false;
  if (entry == NULL) return false;

  // Never compile while the internal debugger or gdbstub is active: they
  // need per-instruction control that the JIT bypasses.
  if (bx_dbg.debugger_active) return false;
#if BX_GDBSTUB
  if (bx_dbg.gdbstub_enabled) return false;
#endif

  // Already compiled (or a previous successful compilation survives).
  if (entry->jit_code != NULL) return true;

  // Never compile a trace that is being invalidated.
  if (entry->pAddr == BX_ICACHE_INVALID_PHY_ADDRESS) {
    entry->jit_code = NULL;
    return false;
  }

#if BX_SUPPORT_JIT
  if (compile_trace(entry)) {
    m_compiled++;
    return true;
  }
  m_compiled_fail++;
#endif
  entry->jit_code = NULL;
  return false;
}

#if BX_SUPPORT_JIT

// Allocate a single executable buffer for all compiled traces. The emitter
// currently uses absolute indirect calls (mov rax, imm64; call rax), so the
// cache can live anywhere in the host address space; the low-address attempt
// is just a harmless best effort reserved for a future rel32 optimization.
bool bxJitC::alloc_executable_cache(Bit64u size_bytes)
{
  if (size_bytes == 0) return false;

#ifdef WIN32
  // Try a low-address reservation first to enable direct rel32 calls.
  void *p = VirtualAlloc((LPVOID) 0x10000000, (SIZE_T) size_bytes,
                         MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (p == NULL)
    p = VirtualAlloc(NULL, (SIZE_T) size_bytes,
                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (p == NULL) return false;
#else
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__x86_64__) && defined(MAP_32BIT)
  void *p = mmap((void *) 0x10000000, (size_t) size_bytes,
                 PROT_READ | PROT_WRITE | PROT_EXEC, flags | MAP_32BIT,
                 -1, 0);
  if (p == MAP_FAILED)
    p = mmap(NULL, (size_t) size_bytes,
             PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
#else
  void *p = mmap(NULL, (size_t) size_bytes,
                 PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
#endif
  if (p == MAP_FAILED) return false;
#endif

  m_cache_base = p;
  m_cache_size = size_bytes;
  m_used = 0;
  return true;
}

// Compile one trace into the translation cache. The actual native code
// emission is done by the x86-64 backend in bx_jit_x86_64.cc.
bool bxJitC::compile_trace(bxICacheEntry_c *entry)
{
  if (m_cpu == NULL || m_cache_base == NULL) return false;
  if (entry->tlen == 0) return false;

  // Conservative per-stub byte budget (prologue + per-instruction sequence
  // for the longest possible trace + epilogue). If the cache cannot hold it,
  // hard-flush and let this trace be (re)compiled on a later miss.
  const Bit64u STUB_RESERVE = 8192;
  if (m_used + STUB_RESERVE > m_cache_size) {
    flush();
    if (m_used + STUB_RESERVE > m_cache_size) return false;
  }

  void *code = NULL;
  Bit64u emitted = 0;
  if (! bx_jit_x86_64_emit_trace(m_cpu, entry, m_cache_base, m_cache_size,
                                 m_used, &emitted, &code))
    return false;

  m_used += emitted;
  entry->jit_code = code;
  return true;
}

#endif // BX_SUPPORT_JIT
