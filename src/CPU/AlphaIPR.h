/* ES40 emulator -- Alpha privileged-register read policy
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
#if !defined(INCLUDED_ALPHAIPR_H)
#define INCLUDED_ALPHAIPR_H

#include <cstdint>

class CAlphaCPU;

// A family may expose a plain field to the JIT; all other reads use its callback.
enum class AlphaIPRReadKind
{
  Unsupported,
  Helper,
  ExceptionAddress,
  ExceptionSummary,
  PalBase,
  MemoryFaultStatus,
  FaultAddress
};

struct AlphaIPRReadDescriptor
{
  AlphaIPRReadKind kind;
  bool replay;       // Verification replays an asynchronous value from the interpreter.
  bool discard_safe; // A read whose destination is R31 can be omitted.
};

// Family read behavior, copied with the immutable CPU profile.
struct AlphaIPRReadPolicy
{
  std::uint64_t (*read)(CAlphaCPU& cpu, std::uint32_t function);
  AlphaIPRReadDescriptor (*describe)(std::uint32_t function);
};

#endif
