/* ES40 emulator -- Alpha instruction-fetch policy
 * Copyright (C) 2026 by gdwnldsKSC
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#if !defined(INCLUDED_ALPHAINSTRUCTIONFETCH_H)
#define INCLUDED_ALPHAINSTRUCTIONFETCH_H

#include <cstdint>

class CAlphaCPU;

// Family fetch behavior, copied with the immutable CPU profile.
struct AlphaInstructionFetchPolicy
{
  // Fetch one decoded word at the current PC, including cache/cursor handling.
  // Return zero on success; nonzero means the attempt could not fetch a word.
  // The family delivers any fetch fault. PC advancement and retirement stay shared.
  // An absent callback cannot fetch and leaves architectural state unchanged.
  int (*fetch)(CAlphaCPU& cpu, std::uint32_t* instruction) = nullptr;

  // Optional entry preparation before the JIT probes or dispatches a block.
  // fetch() owns the equivalent preparation for interpreted execution.
  void (*prepare_dispatch)(CAlphaCPU& cpu) = nullptr;
};

#endif
