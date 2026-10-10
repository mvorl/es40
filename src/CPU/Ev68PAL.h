/* ES40 emulator -- EV68 PAL entry rules
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
#if !defined(INCLUDED_EV68PAL_H)
#define INCLUDED_EV68PAL_H

#include "AlphaPAL.h"

namespace Ev68PAL
{
// EV6/EV68 PAL exception vectors.
constexpr AlphaPALExceptionEntry exception_entry(AlphaPALException reason,
                                                std::uint64_t current_pc)
{
  switch (reason)
  {
  case AlphaPALException::DataTlbDoubleMiss43:          return {0x100, current_pc, false};
  case AlphaPALException::DataTlbDoubleMiss48:          return {0x180, current_pc, false};
  case AlphaPALException::FloatingPointDisabled:       return {0x200, current_pc, true};
  case AlphaPALException::Unaligned:                   return {0x280, current_pc, true};
  case AlphaPALException::DataTlbMiss:                  return {0x300, current_pc, false};
  case AlphaPALException::DataFault:                    return {0x380, current_pc, true};
  case AlphaPALException::OpcodeDecode:                return {0x400, current_pc, true};
  case AlphaPALException::InstructionAccessViolation:  return {0x480, current_pc, true};
  case AlphaPALException::MachineCheck:                return {0x500, current_pc, true};
  case AlphaPALException::InstructionTlbMiss:           return {0x580, current_pc, false};
  case AlphaPALException::Arithmetic:                  return {0x600, current_pc, true};
  case AlphaPALException::Interrupt:                   return {0x680, current_pc, true};
  case AlphaPALException::MtFpcr:                      return {0x700, current_pc, true};
  case AlphaPALException::Reset:                       return {0x780, current_pc, true};
  }
  return {0, current_pc, true};
}

constexpr bool call_pal_valid(std::uint32_t function, std::uint32_t mode)
{
  return function <= 0x3f ? mode == 0 : function >= 0x80 && function <= 0xbf;
}

// EV68 records the CALL_PAL instruction address and stors next PC to
// R23, but could be  R55 when the the PAL shadow bank is enable.
constexpr AlphaPALCallEntry call_pal_entry(std::uint32_t function,
                                           std::uint64_t current_pc,
                                           std::uint64_t next_pc)
{
  const std::uint64_t offset = std::uint64_t(0x2000)
    | (std::uint64_t(function & 0x80) << 5)
    | (std::uint64_t(function & 0x3f) << 6) | 1;
  return {offset, current_pc, next_pc & ~std::uint64_t(2), 23, 55};
}
}

inline constexpr AlphaPALPolicy kEv68PALPolicy{
  &Ev68PAL::exception_entry, &Ev68PAL::call_pal_valid, &Ev68PAL::call_pal_entry
};

#endif
