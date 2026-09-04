/* ES40 Emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
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
  * Stub PMU/ACPI device at PCI 0:17:0 for the ALi M1543C bridge.
  *
  **/
#include "StdAfx.h"
#include "AliM1543C_pmu.h"
#include "System.h"
#include <chrono>

// PCI config-space defaults.
//   CFID  = 0x710110b9 : ALi M7101 power-management function
//   CFCS  = 0x02800000 : DEVSEL=medium; OS programs CMD bits as it goes
//   CFRV  = 0x06800000 : class 0x06 / subclass 0x80 (bridge / other)
//   CFIT  = 0x00000000 : M1543C datasheet page 78: PMU config 30h-3Fh is reserved
//   BAR0  = 0x00000001 : 64-byte I/O region (PM1 + GPE0 block)
//   BAR1  = 0x00000001 : 32-byte I/O region (SMBus host)
static u32 pmu_cfg_data[64] = {
	/*00*/  0x710110b9,
	/*04*/  0x02000000,
	/*08*/  0x06800000,
	/*0c*/  0x00000000,
	/*10*/  0x00000001,
	/*14*/  0x00000001,
	/*18*/  0x00000000,
	/*1c*/  0x00000000,
	/*20*/  0x00000000,
	/*24*/  0x00000000,
	/*28*/  0x00000000,
	/*2c*/  0x00000000,
	/*30*/  0x00000000,
	/*34*/  0x00000000,
	/*38*/  0x00000000,
	/*3c*/  0x00000000,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

// PCI config-space write masks.
//   CFCS  bit 0x1 only : M1543C datasheet page 77: BME/MEM always 0
//         I/O Space Enable is the sole R/W command bit
//   CFLT  bits 0xff00 : latency-timer field
//   BAR0  mask 0xffffffc0 : 64-byte alignment, low bit (IO type) read-only
//   BAR1  mask 0xffffffe0 : 32-byte alignment
//   CFIT  read-only zero (reserved range)
static u32 pmu_cfg_mask[64] = {
	/*00*/  0x00000000,
	/*04*/  0x00000001,
	/*08*/  0x00000000,
	/*0c*/  0x0000ff00,
	/*10*/  0xffffffc0,
	/*14*/  0xffffffe0,
	/*18*/  0x00000000,
	/*1c*/  0x00000000,
	/*20*/  0x00000000,
	/*24*/  0x00000000,
	/*28*/  0x00000000,
	/*2c*/  0x00000000,
	/*30*/  0x00000000,
	/*34*/  0x00000000,
	/*38*/  0x00000000,
	/*3c*/  0x00000000,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

CAliM1543C_pmu::CAliM1543C_pmu(CConfigurator* cfg, CSystem* c, int pcibus, int pcidev)
	: CPCIDevice(cfg, c, pcibus, pcidev)
{
	add_function(0, pmu_cfg_data, pmu_cfg_mask);

	for (int i = 0; i < (int)sizeof(state.pm_block); i++)
		state.pm_block[i] = 0;
	for (int i = 0; i < (int)sizeof(state.smb_block); i++)
		state.smb_block[i] = 0;

	// SMBus host status: bit 0 = HOST_BUSY, leave clear; bit 1 = INTR done,
	// leave clear so the first probe sees an idle controller.
	state.smb_block[0x00] = 0x00;

	state.pm_timer_anchor_us = 0;

	ResetPCI();

	printf("%s: $Id$\n", devid_string);
}

CAliM1543C_pmu::~CAliM1543C_pmu() {}

u32 CAliM1543C_pmu::ReadMem_Bar(int func, int bar, u32 address, int dsize)
{
	switch (bar)
	{
	case 0:   return pm_io_read(address, dsize);
	case 1:   return smb_io_read(address, dsize);
	default:
		printf("%%PMU-W-READBAR: bad BAR %d.\n", bar);
		return 0;
	}
}

void CAliM1543C_pmu::WriteMem_Bar(int func, int bar, u32 address, int dsize, u32 data)
{
	switch (bar)
	{
	case 0:   pm_io_write(address, dsize, data); return;
	case 1:   smb_io_write(address, dsize, data); return;
	default:
		printf("%%PMU-W-WRITEBAR: bad BAR %d.\n", bar);
	}
}

// Free-running 32-bit timer at 3.579545 MHz, the ACPI-fixed PM-timer rate.
// Returned as ticks since first read of the device, so the count starts at
// 0 the first time anyone looks and advances monotonically thereafter.
u32 CAliM1543C_pmu::pm_timer_value()
{
	using clk = std::chrono::steady_clock;
	auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
		clk::now().time_since_epoch()).count();
	if (state.pm_timer_anchor_us == 0)
		state.pm_timer_anchor_us = (u64)now_us;
	u64 elapsed_us = (u64)now_us - state.pm_timer_anchor_us;
	// 3.579545 MHz → 3.579545 ticks per us.  Use a /1000 factor that's close
	// enough for delay-calibration loops without needing floating-point.
	return (u32)((elapsed_us * 3579545ull) / 1000000ull);
}

// PM I/O block, 64 bytes wide.  Layout the OS expects:
//   0x00-0x01  PM1_STS    (RW1C in real silicon, plain RW here)
//   0x02-0x03  PM1_EN
//   0x04-0x05  PM1_CNT
//   0x08-0x0B  PM1_TMR    (32-bit, the only register with live behaviour)
//   0x18-0x19  GPE0_STS
//   0x1A-0x1B  GPE0_EN
u32 CAliM1543C_pmu::pm_io_read(u32 address, int dsize)
{
	const u32 off = address & 0x3f;

	if (off == 0x08 && dsize == 32)
	{
		u32 v = pm_timer_value();
#ifdef DEBUG_PMU
		printf("%%PMU-I-PMTMR: read %08x\n", v);
#endif
		return v;
	}

	u32 v = 0;
	const int bytes = dsize / 8;
	for (int i = 0; i < bytes && (off + i) < (int)sizeof(state.pm_block); i++)
		v |= ((u32)state.pm_block[off + i]) << (8 * i);

#ifdef DEBUG_PMU
	printf("%%PMU-I-PMRD: off=%02x dsize=%d -> %08x\n", off, dsize, v);
#endif
	return v;
}

void CAliM1543C_pmu::pm_io_write(u32 address, int dsize, u32 data)
{
	const u32 off = address & 0x3f;

#ifdef DEBUG_PMU
	printf("%%PMU-I-PMWR: off=%02x dsize=%d data=%08x\n", off, dsize, data);
#endif

	const int bytes = dsize / 8;
	for (int i = 0; i < bytes && (off + i) < (int)sizeof(state.pm_block); i++)
		state.pm_block[off + i] = (u8)(data >> (8 * i));
}

// SMBus I/O block, 32 bytes wide.  Stub: stores writes, returns last
// written value on read.  No transactions are actually performed.
u32 CAliM1543C_pmu::smb_io_read(u32 address, int dsize)
{
	const u32 off = address & 0x1f;

	u32 v = 0;
	const int bytes = dsize / 8;
	for (int i = 0; i < bytes && (off + i) < (int)sizeof(state.smb_block); i++)
		v |= ((u32)state.smb_block[off + i]) << (8 * i);

#ifdef DEBUG_PMU
	printf("%%PMU-I-SMBRD: off=%02x dsize=%d -> %08x\n", off, dsize, v);
#endif
	return v;
}

void CAliM1543C_pmu::smb_io_write(u32 address, int dsize, u32 data)
{
	const u32 off = address & 0x1f;

#ifdef DEBUG_PMU
	printf("%%PMU-I-SMBWR: off=%02x dsize=%d data=%08x\n", off, dsize, data);
#endif

	const int bytes = dsize / 8;
	for (int i = 0; i < bytes && (off + i) < (int)sizeof(state.smb_block); i++)
		state.smb_block[off + i] = (u8)(data >> (8 * i));

	// SMBus host status (offset 0).  Bit 0 (HOST_BUSY) auto-clears so the
	// next status read shows the (stub) transaction completed; bit 1
	// (INTR/done) gets set so polled drivers see success.
	if (off == 0x00)
	{
		state.smb_block[0] &= ~0x01;
		state.smb_block[0] |= 0x02;
	}
}

static u32 pmu_magic1 = 0x71011533;
static u32 pmu_magic2 = 0x33151071;

int CAliM1543C_pmu::SaveState(FILE* f)
{
	long ss = sizeof(state);
	int  res;

	if ((res = CPCIDevice::SaveState(f)))
		return res;

	fwrite(&pmu_magic1, sizeof(u32), 1, f);
	fwrite(&ss, sizeof(long), 1, f);
	fwrite(&state, sizeof(state), 1, f);
	fwrite(&pmu_magic2, sizeof(u32), 1, f);
	printf("%s: %d bytes saved.\n", devid_string, (int)ss);
	return 0;
}

int CAliM1543C_pmu::RestoreState(FILE* f)
{
	long   ss;
	u32    m1, m2;
	int    res;
	size_t r;

	if ((res = CPCIDevice::RestoreState(f)))
		return res;

	r = fread(&m1, sizeof(u32), 1, f);
	if (r != 1) { printf("%s: unexpected end of file!\n", devid_string); return -1; }
	if (m1 != pmu_magic1) { printf("%s: MAGIC 1 does not match!\n", devid_string); return -1; }

	fread(&ss, sizeof(long), 1, f);
	if (ss != sizeof(state)) { printf("%s: STRUCT SIZE does not match!\n", devid_string); return -1; }

	fread(&state, sizeof(state), 1, f);

	r = fread(&m2, sizeof(u32), 1, f);
	if (r != 1) { printf("%s: unexpected end of file!\n", devid_string); return -1; }
	if (m2 != pmu_magic2) { printf("%s: MAGIC 2 does not match!\n", devid_string); return -1; }

	printf("%s: %d bytes restored.\n", devid_string, (int)ss);
	return 0;
}
