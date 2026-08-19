/* ES40 emulator -- JIT engine: the ARM64 (AArch64) codegen backend.
 * Copyright (C) 2026 by the ES40 Emulator Project
 * All rights reserved.
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * SPDX-License-Identifier: BSD-1-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS AND CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef ES40_JIT

#include "jitengine.h"        // defines ES40_JIT_X64 / ES40_JIT_A64 from the host arch
#include "jitengine_internal.h"

#ifdef ES40_JIT_A64

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <initializer_list>
#include <vector>
#define ASMJIT_STATIC
// Only asmjit's core is needed today (the engine's JitRuntime lives in jitengine.cpp);
// switch to <asmjit/a64.h> when this backend starts emitting.
#include <asmjit/core.h>

void CJitEngine::emit_op(void*, const uint8_t*, void*, const HelperSet&,
                         bool, JitBlock*, uint32_t, uint32_t, RegAlloc&, void*, bool) {}

void CJitEngine::compile_block(JitBlock* b, const uint8_t*, uint64_t, void*, void*, void*,
    void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
    void*, void*, void*, void*)
{
  // Mark the attempt so record()'s hot path never re-requests a compile; code stays
  // null, so this block is interpreted permanently (no recompile churn).
  b->compiled = true;
  b->code = nullptr;
  b->jit_body = nullptr;
  b->prefix_len = 0;
}

void CJitEngine::compile_trace(TraceFragment*, JitBlock**, uint32_t,
                               const uint8_t*, uint64_t, const HelperSet&) {}

#endif // ES40_JIT_A64

#endif // ES40_JIT

// Keeps this translation unit non-empty when ARM64 is not the build host (MSVC LNK4221).
extern const char jit_a64_backend_tu;
const char jit_a64_backend_tu = 0;
