/* ES40 emulator.
 * Copyright (C) 2026 by gdwnldsKSC
 * Copyright (C) 2017 Vas Crabb from MAME
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
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

// license:BSD-3-Clause
// copyright-holders:Vas Crabb
// This file is part of the MAME project (https://www.mamedev.org/).
// Adapted for ES40
#if defined(MAME_EMU_LOGMACRO_H) || !defined(__EMU_H__)
#ifndef _WIN32
#error This file should only be included once per compilation unit after all other headers
#endif
#endif
#define MAME_EMU_LOGMACRO_H

#ifndef VERBOSE
#define VERBOSE 0
#endif

inline void logerror(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
}

#ifndef LOG_OUTPUT_FUNC
#ifdef LOG_OUTPUT_STREAM
#define LOG_OUTPUT_FUNC [] (auto &&... args) { util::stream_format((LOG_OUTPUT_STREAM), std::forward<decltype(args)>(args)...); }
#else
#define LOG_OUTPUT_FUNC logerror
#endif
#endif

#ifndef LOG_GENERAL
#define LOG_GENERAL (1U << 0)
#endif

#define LOGMASKED(mask, ...) do { if constexpr (VERBOSE & (mask)) (LOG_OUTPUT_FUNC)(__VA_ARGS__); } while (false)

#define LOG(...) LOGMASKED(LOG_GENERAL, __VA_ARGS__)
