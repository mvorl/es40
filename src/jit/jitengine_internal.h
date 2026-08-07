/* ES40 emulator -- JIT engine internals.
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
#if !defined(INCLUDED_JITENGINE_INTERNAL_H)
#define INCLUDED_JITENGINE_INTERNAL_H

#ifdef ES40_JIT

#include <cstdint>

// FNV-1a/64 over a block's source words -- record() uses it to revalidate kept code after a flush
// instead of recompiling, and to catch self-modifying code (changed bytes -> different hash).
static inline uint64_t src_hash(const uint8_t* p, uint32_t n_instr)
{
  const uint32_t* w = (const uint32_t*) p;
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t i = 0; i < n_instr; ++i) { h ^= w[i]; h *= 1099511628211ULL; }
  return h;
}

#ifdef JIT_STATS
// Short mnemonic for the opcode that ended a block's compiled prefix (the coverage gap).
// Defined in jitengine.cpp; the backends use it for their punch-list prints.
const char* jit_opcode_name(unsigned op);
#endif

#endif // ES40_JIT

#endif // INCLUDED_JITENGINE_INTERNAL_H
