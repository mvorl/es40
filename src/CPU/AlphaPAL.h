/* ES40 emulator -- Alpha PAL entry policy
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS AND CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#if !defined(INCLUDED_ALPHAPAL_H)
#define INCLUDED_ALPHAPAL_H

#include <cstdint>

// Shared exceptions; each CPU supplies its own PAL entry rules.
enum class AlphaPALException
{
  DataTlbDoubleMiss43,
  DataTlbDoubleMiss48,
  FloatingPointDisabled,
  Unaligned,
  DataTlbMiss,
  DataFault,
  OpcodeDecode,
  InstructionAccessViolation,
  MachineCheck,
  InstructionTlbMiss,
  Arithmetic,
  Interrupt,
  MtFpcr,
  Reset
};

struct AlphaPALExceptionEntry
{
  std::uint64_t offset;    // Exception vector offset, withuot PALmode bit 0.
  std::uint64_t saved_pc;  // Value for EXC_ADDR.
  bool clear_lock;
};

struct AlphaPALCallEntry
{
  std::uint64_t offset;    // CALL_PAL entry offset, with PALmode bit 0.
  std::uint64_t saved_pc;  // Value for EXC_ADDR.
  std::uint64_t return_pc;
  unsigned link_register;
  unsigned shadow_link_register;
};

// Stateless family entry rules, copied with the CPU profile.
struct AlphaPALPolicy
{
  AlphaPALExceptionEntry (*exception_entry)(AlphaPALException reason,
                                            std::uint64_t current_pc);
  bool (*call_pal_valid)(std::uint32_t function, std::uint32_t mode);
  AlphaPALCallEntry (*call_pal_entry)(std::uint32_t function,
                                     std::uint64_t current_pc,
                                     std::uint64_t next_pc);
};

#endif
