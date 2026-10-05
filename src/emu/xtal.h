/* ES40 emulator.
 * Copyright (C) 2026 by gdwnldsKSC
 * Copyright (C) 2018 Olivier Galibert from MAME
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

// Adapted from MAME xtal.h (license:BSD-3-Clause, copyright-holders:Olivier Galibert)
// just enough for S3 for now

// Usage:
//   XTAL(25'174'800).value()           -> 25174800  (u32)
//   XTAL(28'636'363).dvalue()          -> 28636363.0 (double)
//   (XTAL(14'318'181) * 2).value()     -> 28636362
//   XTAL(14'318'181) / 3              -> XTAL with current_clock ~ 4772727

#ifndef XTAL_H
#define XTAL_H

#include <cstdint>
#include <cmath>

class XTAL
{
public:
  constexpr explicit XTAL(double base_clock) noexcept : m_base_clock(base_clock), m_current_clock(base_clock) {}

  constexpr double   dvalue() const noexcept { return m_current_clock; }
  constexpr uint32_t value()  const noexcept { return uint32_t(m_current_clock + 1e-3); }
  constexpr double   base()   const noexcept { return m_base_clock; }

  template <typename T> constexpr XTAL operator *(T mult) const noexcept { return XTAL(m_base_clock, m_current_clock * mult); }
  template <typename T> constexpr XTAL operator /(T div) const noexcept { return XTAL(m_base_clock, m_current_clock / div); }

  friend constexpr XTAL operator *(int          mult, const XTAL& xtal);
  friend constexpr XTAL operator *(unsigned int mult, const XTAL& xtal);
  friend constexpr XTAL operator *(double       mult, const XTAL& xtal);

private:
  double m_base_clock, m_current_clock;

  constexpr XTAL(double base_clock, double current_clock) noexcept : m_base_clock(base_clock), m_current_clock(current_clock) {}
};

template <typename T> constexpr auto operator /(T&& div, const XTAL& xtal) { return div / xtal.dvalue(); }

constexpr XTAL operator *(int          mult, const XTAL& xtal) { return XTAL(xtal.base(), mult * xtal.dvalue()); }
constexpr XTAL operator *(unsigned int mult, const XTAL& xtal) { return XTAL(xtal.base(), mult * xtal.dvalue()); }
constexpr XTAL operator *(double       mult, const XTAL& xtal) { return XTAL(xtal.base(), mult * xtal.dvalue()); }


constexpr XTAL operator ""_Hz_XTAL(long double clock) { return XTAL(double(clock)); }
constexpr XTAL operator ""_kHz_XTAL(long double clock) { return XTAL(double(clock * 1e3)); }
constexpr XTAL operator ""_MHz_XTAL(long double clock) { return XTAL(double(clock * 1e6)); }

constexpr XTAL operator ""_Hz_XTAL(unsigned long long clock) { return XTAL(double(clock)); }
constexpr XTAL operator ""_kHz_XTAL(unsigned long long clock) { return XTAL(double(clock) * 1e3); }
constexpr XTAL operator ""_MHz_XTAL(unsigned long long clock) { return XTAL(double(clock) * 1e6); }

// S3 crystal frequencies 
// These match MAME's pc_vga_s3.cpp s3_define_video_mode():
//   XTAL(25'174'800)  - 25.1748 MHz  (VGA 640px modes)
//   XTAL(28'636'363)  - 28.636363 MHz (VGA 720px modes, 8x NTSC subcarrier)
//   14.318 MHz         - XIN reference for S3 PLL (4x NTSC subcarrier)

#endif
