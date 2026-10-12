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

// EV68 HW_MFPR behavior extracted from cpu_pal.h.
#include "StdAfx.h"
#include "AlphaCPU.h"
#include "Ev68IPR.h"

std::uint64_t Ev68IPR::read(CAlphaCPU& cpu, std::uint32_t function)
{
  const auto& state = cpu.state;
  if ((function & 0xc0) == 0x40) // PCTX
    return ((u64)state.asn << 39) | ((u64)state.astrr << 9) | ((u64)state.aster << 5)
      | (state.fpen ? U64(0x1) << 2 : 0) | (state.ppcen ? U64(0x1) << 1 : 0);

  switch (function)
  {
  case 0x05: return state.pmpc;                         // PMPC
  case 0x06: return state.exc_addr;                     // EXC_ADDR
  case 0x07: return cpu.va_form(state.exc_addr, true);   // IVA_FORM
  case 0x08: case 0x09: case 0x0a: case 0x0b:            // IER_CM / CM / IER
    return (((u64)state.eien) << 33) | (((u64)state.slen) << 32)
      | (((u64)state.cren) << 31) | (((u64)state.pcen) << 29)
      | (((u64)state.sien) << 13) | (((u64)state.asten) << 13)
      | (((u64)state.cm) << 3);
  case 0x0c: return ((u64)state.sir) << 13;              // SIRR
  case 0x0d:                                           // ISUM
    return (((u64)(state.eir & state.eien)) << 33)
      | (((u64)(state.slr & state.slen)) << 32)
      | (((u64)(state.crr & state.cren)) << 31)
      | (((u64)(state.pcr & state.pcen)) << 29)
      | (((u64)(state.sir & state.sien)) << 13)
      | (((u64)(((U64(0x1) << (state.cm + 1)) - 1) & state.aster & state.astrr & (state.asten * 0x3))) << 3)
      | (((u64)(((U64(0x1) << (state.cm + 1)) - 1) & state.aster & state.astrr & (state.asten * 0xc))) << 7);
  case 0x0f: return state.exc_sum;                      // EXC_SUM
  case 0x10: return state.pal_base;                     // PAL_BASE
  case 0x11:                                           // I_CTL
    return state.i_ctl_other | (((u64)cpu.get_profile().chip_id) << 24) | (u64)state.i_ctl_vptb
      | (((u64)state.i_ctl_va_mode) << 15) | (state.hwe ? U64(0x1) << 12 : 0)
      | (state.sde ? U64(0x1) << 7 : 0) | (((u64)state.i_ctl_spe) << 3);
  case 0x14: return state.pctr_ctl;                     // PCTR_CTL
  case 0x16: return state.i_stat;                       // I_STAT
  case 0x27: return state.mm_stat;                      // MM_STAT
  case 0x2a: return state.dc_stat;                      // DC_STAT
  case 0x2b: return 0;                                  // C_DATA
  case 0xc0: return (((u64)state.cc_offset) << 32) | (state.cc & U64(0xffffffff)); // CC
  case 0xc2: return state.fault_va;                     // VA
  case 0xc3: return cpu.va_form(state.va_form_va, false); // VA_FORM
  default:
    // Unimplemented EV68 indices read zero and warn once per index.
    static bool warned_ipr[256];
    if (!warned_ipr[function & 0xff])
    {
      warned_ipr[function & 0xff] = true;
      printf("%%CPU-W-IPR: unimplemented HW_MFPR index %02x at pc=%016" PRIx64 " (ignored)\n",
        (unsigned)function, state.current_pc);
    }
    return 0;
  }
}
