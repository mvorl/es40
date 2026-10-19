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

// Observed instruction mapping; a failed probe has valid == false.
// Physical addresses currently identify sources for the legacy DRAM-backed JIT.
struct AlphaInstructionMapping
{
  std::uint64_t physical = 0;
  bool asm_global = false;
  bool valid = false;
};

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

  // Observational JIT hooks: no instruction fetch, cache/TB fill, fault or bus read.
  // Missing hooks cannot resolve a source, override a word, approve a span or chain.
  // probe consumes the supplied PC without imposing shared alignment rules.
  // It may update translation lookup hints, but cannot deliver a guest fault.
  AlphaInstructionMapping (*probe)(CAlphaCPU& cpu, std::uint64_t pc) = nullptr;

  // Peek the current instruction's cached word without consuming its cursor.
  // False means no cached override: leave the caller's legacy source word intact.
  bool (*peek_cached)(const CAlphaCPU& cpu, std::uint32_t* instruction) = nullptr;

  // Can the legacy JIT record this interpreted span from its backing source?
  // Detect cached instructions that differ from the current backing bytes.
  bool (*matches_interpreted)(const CAlphaCPU& cpu, std::uint64_t start_pc,
                              std::uint32_t count) = nullptr;

  // Reject targets that need dispatcher preparation before entering compiled code.
  // Called before lookup; a PAL target need not subsequently probe a translation.
  bool (*can_chain)(const CAlphaCPU& cpu, std::uint64_t target) = nullptr;
};

#endif
