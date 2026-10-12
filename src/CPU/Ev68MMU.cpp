/* ES40 Emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2026 by gdwnldsKSC
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might serve
 * the general public.
 *
 */

// EV68 translation-fill decoding extracted from AlphaCPU.cpp.
#include "StdAfx.h"
#include "Ev68MMU.h"

AlphaTBFill Ev68MMU::decode_fill(const CAlphaCPU&, std::uint64_t virt,
                                std::uint64_t pte_phys, std::uint64_t pte_flags,
                                int)
{
  // EV68CB/EV68DC HRM 5.3.1/5.3.2: VA[47:13], PA[43:13], GH[1:0].
  // GH widens the granule by three bits per step (13/16/19/22 offset bits).
  static constexpr std::uint64_t match_masks[4] = {
    U64(0x0000ffffffffe000), U64(0x0000ffffffff0000),
    U64(0x0000fffffff80000), U64(0x0000ffffffc00000)
  };
  static constexpr std::uint64_t phys_masks[4] = {
    U64(0x00000fffffffe000), U64(0x00000fffffff0000),
    U64(0x00000ffffff80000), U64(0x00000fffffc00000)
  };
  static constexpr std::uint64_t keep_masks[4] = {
    U64(0x0000000000001fff), U64(0x000000000000ffff),
    U64(0x000000000007ffff), U64(0x00000000003fffff)
  };
  const unsigned gh = (unsigned)((pte_flags >> 5) & 3);

  AlphaTBFill fill{};
  fill.virt = virt;
  fill.phys = pte_phys & phys_masks[gh];
  fill.match_mask = match_masks[gh];
  fill.keep_mask = keep_masks[gh];
  fill.fault[0] = (int)pte_flags & 2;
  fill.fault[1] = (int)pte_flags & 4;
  fill.fault[2] = (int)pte_flags & 8;
  fill.access[0][0] = (int)pte_flags & 0x100;
  fill.access[1][0] = (int)pte_flags & 0x1000;
  fill.access[0][1] = (int)pte_flags & 0x200;
  fill.access[1][1] = (int)pte_flags & 0x2000;
  fill.access[0][2] = (int)pte_flags & 0x400;
  fill.access[1][2] = (int)pte_flags & 0x4000;
  fill.access[0][3] = (int)pte_flags & 0x800;
  fill.access[1][3] = (int)pte_flags & 0x8000;
  fill.asm_bit = (int)pte_flags & 0x10;
  return fill;
}
