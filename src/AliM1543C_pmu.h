/* ES40 Emulator.
 * Copyright (C) 2007-2026 by the ES40 Emulator Project
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

 /**
  * \file
  * Stub Power Management Unit (M7101) on the ALi M1543C bridge.
  * Exists so the OS sees an ACPI PM block at PCI 0:17:0; reads/writes
  * are stored but not acted on.
  **/
#if !defined(INCLUDED_ALIM1543C_PMU_H_)
#define INCLUDED_ALIM1543C_PMU_H_

#include "PCIDevice.h"

class CAliM1543C_pmu : public CPCIDevice
{
public:
	virtual int   SaveState(FILE* f);
	virtual int   RestoreState(FILE* f);

	CAliM1543C_pmu(CConfigurator* cfg, class CSystem* c, int pcibus, int pcidev);
	virtual       ~CAliM1543C_pmu();
	virtual void  WriteMem_Bar(int func, int bar, u32 address, int dsize, u32 data);
	virtual u32   ReadMem_Bar(int func, int bar, u32 address, int dsize);

private:
	u32   pm_io_read(u32 address, int dsize);
	void  pm_io_write(u32 address, int dsize, u32 data);
	u32   smb_io_read(u32 address, int dsize);
	void  smb_io_write(u32 address, int dsize, u32 data);
	u32   pm_timer_value();

	struct SPMU_state
	{
		// 64-byte PM I/O block.  Holds the latched values of every read/write
		u8 pm_block[64];
		// 32-byte SMBus I/O block (host status/control + data).
		u8 smb_block[32];
		// Wall-clock anchor for the 3.579545 MHz PM timer
		u64 pm_timer_anchor_us;
	} state;
};

#endif // !defined(INCLUDED_ALIM1543C_PMU_H_)
