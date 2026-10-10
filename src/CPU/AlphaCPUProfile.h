/* ES40 emulator -- Alpha CPU identity profiles
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
#if !defined(INCLUDED_ALPHACPUPROFILE_H)
#define INCLUDED_ALPHACPUPROFILE_H

#include <cstdint>

// Identity and architectural feature-probe values chosen by configuration. 
struct AlphaCPUProfile
{
  std::uint64_t amask;       // Implemented extensions; AMASK returns input & ~amask.
  std::uint32_t implver;     // Architectural implementation version.
  std::uint32_t chip_id;     // Implementation-specific chip revision identifier.
  std::uint32_t type_major;  // System-reference processor type.
  std::uint32_t type_minor;  // System-reference processor revision.
};

// EV68CB pass 4: BWX | FIX | CIX | MVI | precise traps | prefetch modify.
inline constexpr AlphaCPUProfile kEv68CBProfile{0x1307, 2, 0x21, 12, 6};

#endif
