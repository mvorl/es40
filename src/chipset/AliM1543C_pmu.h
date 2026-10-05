/* ES40 emulator.
 * Copyright (C) 2026 by gdwnldsKSC
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * SPDX-License-Identifier: BSD-1-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
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

 /**
  * \file
  * Stub Power Management Unit (M7101) on the ALi M1543C bridge.
  * Exists so the OS sees an ACPI PM block at PCI 0:17:0; reads/writes
  * are stored but not acted on.
  **/
#if !defined(INCLUDED_ALIM1543C_PMU_H_)
#define INCLUDED_ALIM1543C_PMU_H_

#include "emu/PCIDevice.h"

class CAliM1543C;

class CAliM1543C_pmu : public CPCIDevice
{
public:
	virtual int   SaveState(FILE* f);
	virtual int   RestoreState(FILE* f);

	CAliM1543C_pmu(CConfigurator* cfg, class CSystem* c, int pcibus, int pcidev);
	virtual       ~CAliM1543C_pmu();
	virtual void  WriteMem_Bar(int func, int bar, u32 address, int dsize, u32 data);
	virtual u32   ReadMem_Bar(int func, int bar, u32 address, int dsize);
	void bind_isa_bridge(CAliM1543C* bridge) override;
	const CSystemComponent* memory_decode_owner() const noexcept override;
	bool uses_subtractive_decode(int index, u64 address, int dsize,
		bool write) const noexcept override;
	bool decodes_memory_access(int index, u64 address, int dsize,
		bool write) const noexcept override;
	u32 docking_config(u32 aligned_offset) const noexcept;

private:
	const CAliM1543C* isa_bridge = nullptr;

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
