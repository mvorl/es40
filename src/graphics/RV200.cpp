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

#include "RV200.h"
#include <algorithm>
#include <cstring>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>

static uint32_t field_mask(unsigned bits)
{
	return bits == 32 ? 0xffffffffu : (1u << bits) - 1u;
}

static uint32_t bytes_per_pixel(uint32_t format)
{
	switch (format)
	{
	case 2:
		return 1;
	case 3:
	case 4:
		return 2;
	case 5:
		return 3;
	case 6:
		return 4;
	default:
		return 0;
	}
}

// Alternate addresses of the RBBM registers.
static uint32_t alias(uint32_t a)
{
	if (a == 0x1740)
		return CRV200::RBBM_STATUS;
	if (a == 0xe44)
		return CRV200::RBBM_CNTL;
	if (a == 0xe48)
		return CRV200::RBBM_SOFT_RESET;
	return a;
}

static std::string hex(uint32_t a)
{
	std::ostringstream s;
	s << "0x" << std::hex << a;
	return s.str();
}

static bool valid_access(uint32_t a, unsigned bits, uint32_t limit)
{
	return (bits == 8 || bits == 16 || bits == 32) && (a % (bits / 8) == 0) &&
		a < limit && bits / 8 <= limit - a;
}

// Registers that are stored without a modeled effect.
static bool shadow_allowed(uint32_t a)
{
	return (a >= 0x10 && a <= 0x2c) || a == CRV200::BUS_CNTL || a == 0x34 ||
		a == CRV200::DAC_CNTL || a == 0x7c || a == CRV200::CONFIG_CNTL ||
		a == CRV200::RBBM_CNTL || a == CRV200::HOST_PATH_CNTL ||
		a == CRV200::MEM_CNTL || a == 0x144 || a == CRV200::MC_AGP_LOCATION ||
		a == CRV200::MEM_SDRAM_MODE_REG || a == 0x15c || a == 0x158 ||
		a == 0x170 || a == 0x174 || (a >= 0x1500 && a <= 0x1528) ||
		(a >= 0x15e0 && a <= 0x15f4) || a == CRV200::DSTCACHE_MODE ||
		a == CRV200::ISYNC_CNTL || a == 0x172c || (a >= 0x260 && a <= 0x270) ||
		a == 0x200 || a == 0x204 || a == 0x208 || a == 0x20c || a == 0x220 ||
		a == 0x23c || a == 0x240 || a == 0x244 || a == 0x248 || a == 0x250 ||
		a == 0x254 || (a >= 0x700 && a <= 0x77c) || a == 0x420 || a == 0x284 ||
		a == 0x288 || a == 0x2d0;
}

// CLR_CMP_CNTL source equality suppresses matching pixels (XAA keying).
// Deliberately bounded to source comparison: no destination/3D combination.
static bool compare_supported(uint32_t control)
{
	return control == 0 || control == 0x01000004 || control == 0x01000005;
}

static bool source_rejected(
	uint32_t control, uint32_t key, uint32_t mask, uint32_t source,
	uint32_t bytes)
{
	if (!control)
		return false;
	if (bytes < 4)
		mask &= (1u << (8 * bytes)) - 1;
	const bool equal = ((source ^ key) & mask) == 0;
	return (control & 7) == 4 ? equal : !equal;
}

// Select the bit order within each byte; byte order itself is unchanged.
static bool mono_bit(uint32_t word, unsigned pixel, bool lsb)
{
	const unsigned bit =
		(pixel & ~7u) + (lsb ? (pixel & 7u) : 7u - (pixel & 7u));
	return ((word >> bit) & 1u) != 0;
}

static bool rop_uses(uint8_t op, unsigned bit)
{
	for (unsigned i = 0; i < 8; i++)
		if (((op >> i) ^ (op >> (i ^ (1u << bit)))) & 1u)
			return true;
	return false;
}

// Register dispatch

uint32_t CRV200::ReadMem(uint32_t a, int dsize)
{
	m_counters.reads++;
	const unsigned bits = unsigned(dsize);
	if (!valid_access(a, bits, RegisterSpaceSize))
	{
		fail("invalid MMIO read " + hex(a));
		return 0xffffffffu;
	}
	if (a >= 0x3b0 && a < 0x3e0)
	{
		if (!m_vga_read)
		{
			fail("VGA register frontend is not connected");
			return field_mask(bits);
		}
		uint32_t value = 0;
		for (unsigned i = 0; i < bits / 8; ++i)
			value |= uint32_t(m_vga_read(a + i)) << (8 * i);
		return value;
	}
	if ((a & ~3u) == MM_DATA)
	{
		const uint32_t idx = r(MM_INDEX);
		uint32_t target;
		if (idx & 0x80000000u)
			return uint32_t(mem_read((idx & 0x7ffffffcu) + (a & 3u), dsize));
		return indexed_target(a & 3u, target) ? ReadMem(target, dsize) : 0;
	}
	// Side-effect ports cannot be split implicitly into multiple reads.
	const uint32_t base = alias(a & ~3u), shift = (a & 3u) * 8;
	if (base == PALETTE_DATA && bits != 32)
	{
		fail("palette data requires DWORD access");
		return 0;
	}
	return (read32(base) >> shift) & field_mask(bits);
}

void CRV200::WriteMem(uint32_t a, int dsize, uint32_t value)
{
	m_counters.writes++;
	const unsigned bits = unsigned(dsize);
	if (!valid_access(a, bits, RegisterSpaceSize))
	{
		fail("invalid MMIO write " + hex(a));
		return;
	}
	if (a >= 0x3b0 && a < 0x3e0)
	{
		if (!m_vga_write)
		{
			fail("VGA register frontend is not connected");
			return;
		}
		for (unsigned i = 0; i < bits / 8; ++i)
			m_vga_write(a + i, uint8_t(value >> (8 * i)));
		return;
	}
	if ((a & ~3u) == MM_DATA)
	{
		const uint32_t idx = r(MM_INDEX);
		uint32_t target;
		if (idx & 0x80000000u)
			mem_write((idx & 0x7ffffffcu) + (a & 3u), dsize, value);
		else if (indexed_target(a & 3u, target))
			WriteMem(target, dsize, value);
		return;
	}
	const uint32_t base = alias(a & ~3u), shift = (a & 3u) * 8,
				   mask = field_mask(bits) << shift;
	if ((base == PALETTE_DATA || base == DST_HEIGHT_WIDTH ||
		 base == DST_WIDTH_HEIGHT || base == DST_LINE_END ||
		 (base >= HOST_DATA0 && base <= HOST_DATA_LAST)) &&
		bits != 32)
	{
		fail("command/data port requires DWORD access at " + hex(a));
		return;
	}
	write32(base, (value << shift) & mask, mask);
}

// MM_DATA is a window onto the register MM_INDEX selects; its byte lanes
// address the matching bytes of that register, as the x86 BIOS uses them.
bool CRV200::indexed_target(uint32_t lane, uint32_t& target)
{
	target = (r(MM_INDEX) & ~3u) + lane;
	if (target >= RegisterSpaceSize)
	{
		fail("MM_DATA register index outside MMIO aperture");
		return false;
	}
	if (target < 8)
	{
		fail("recursive MM_DATA index");
		return false;
	}
	return true;
}

uint32_t CRV200::read32(uint32_t a)
{
	if (a == CLOCK_CNTL_DATA)
	{
		const uint32_t i = r(CLOCK_CNTL_INDEX) & 63, v = m_pll[i];
		// PLL_TEST_CNTL[31:24] counts 27 MHz reference clocks; the x86 BIOS
		// times its delays with it. Each read observes one more microsecond.
		if (i == PLL_TEST_CNTL)
			m_pll[i] += 27u << 24;
		return v;
	}
	if (a == PALETTE_DATA)
	{
		const uint32_t idx = (r(PALETTE_INDEX) >> 16) & 255;
		const uint32_t val = m_palette[idx];
		r(PALETTE_INDEX) =
			(r(PALETTE_INDEX) & ~0xff0000u) | (((idx + 1) & 255) << 16);
		return val;
	}
	if (a == SRC_PITCH || a == DST_PITCH)
		return (r(a == SRC_PITCH ? SRC_PITCH_OFFSET : DST_PITCH_OFFSET) >> 16) &
			0x3fc0u;
	if (a == SRC_OFFSET || a == DST_OFFSET)
		return (r(a == SRC_OFFSET ? SRC_PITCH_OFFSET : DST_PITCH_OFFSET) &
				0x3fffffu)
			<< 10;
	if (a == SRC_X || a == DST_X)
		return r(a == SRC_X ? SRC_Y_X : DST_Y_X) & 0xffffu;
	if (a == SRC_Y || a == DST_Y)
		return r(a == SRC_Y ? SRC_Y_X : DST_Y_X) >> 16;
	if (a == DP_CNTL_XDIR_YDIR_YMAJOR)
		return ((r(DP_CNTL) & 1u) << 31) | ((r(DP_CNTL) & 2u) << 14) |
			(r(DP_CNTL) & 4u);
	// Synchronous PIO availability, NOT a cycle model.
	if (a == RBBM_STATUS)
		return (busy() ? GUI_ACTIVE : 0u) | (halted() ? 0u : 64u);
	// Both channels powered up (bits 1:0, polled by the BIOS) and idle; no
	// DRAM timing.
	if (a == MC_STATUS)
		return 7u;
	if (a == CRTC_STATUS)
	{
		// VBLANK_SAVE latches at vertical blank start until written with 1.
		const bool vblank =
			m_scanline >= (((r(CRTC_V_TOTAL_DISP) >> 16) & 0xfff) + 1);
		return (vblank ? CRTC_VBLANK_CUR : 0u) |
			(r(CRTC_STATUS) & CRTC_VBLANK_SAVE);
	}
	if (a == CRTC_VLINE_CRNT_VLINE)
		return (m_scanline & 0x1fff) << 16;
	if (a == CRTC_CRNT_FRAME)
		return m_frame;
	if (a == DSTCACHE_CTLSTAT)
		return busy() ? 0x80000000u : 0;
	// The CP never consumes commands in this model.
	if (a == CP_RB_RPTR)
		return 0;
	if (a >= 0xf00 && a < 0x1000)
		return m_config_read ? m_config_read(a - 0xf00) : 0xffffffffu;
	if (a >= GPIO_VGA_DDC && a <= GPIO_CRT2_DDC)
	{
		// Disconnected DDC: open-drain pull-ups, no EDID responder or ACK.
		uint32_t v = r(a) & ~0x300u;
		if (!(v & (1u << 16)) || (v & 1u))
			v |= 1u << 8;
		if (!(v & (1u << 17)) || (v & 2u))
			v |= 1u << 9;
		return v;
	}
	return r(a);
}

void CRV200::write32(uint32_t a, uint32_t val, uint32_t lanes)
{
	const uint32_t old = r(a), v = (old & ~lanes) | (val & lanes);
	if (a >= 0xf00 && a < 0x1000)
	{
		note("read-only PCI configuration mirror write " + hex(a));
		return;
	}
	if (a == CLOCK_CNTL_INDEX)
	{
		// Keep PLL_DIV_SEL [9:8], not only the index/write flag.
		r(a) = v & 0x3bfu;
		return;
	}
	if (a == CLOCK_CNTL_DATA)
	{
		// Byte lanes update only their part of the selected PLL register.
		uint32_t& pll = m_pll[r(CLOCK_CNTL_INDEX) & 63];
		if (r(CLOCK_CNTL_INDEX) & 0x80)
			pll = (pll & ~lanes) | (val & lanes);
		return;
	}
	if (a == CRTC_STATUS)
	{
		r(a) &= ~(val & lanes & CRTC_VBLANK_SAVE);
		return;
	}
	if (a == DAC_CNTL)
	{
		// The board's VGA frontend consumes DAC_8BIT_EN; other fields are stored.
		r(a) = v;
		return;
	}
	if (a == PALETTE_INDEX)
	{
		r(a) = v & 0x00ff00ffu;
		if (m_palette_index)
			m_palette_index();
		return;
	}
	if (a == PALETTE_DATA)
	{
		const uint32_t idx = r(PALETTE_INDEX) & 255;
		m_palette[idx] = v & 0xffffff;
		r(PALETTE_INDEX) = (r(PALETTE_INDEX) & ~255u) | ((idx + 1) & 255);
		return;
	}
	if (a == GEN_INT_STATUS)
	{
		r(a) &= ~(val & lanes & 1u);
		update_irq();
		return;
	}
	if (a == GEN_INT_CNTL)
	{
		r(a) = v;
		update_irq();
		return;
	}
	if (a == RBBM_SOFT_RESET)
	{
		if ((v & ~old) & (1u | 32u))
		{
			m_pending = Pending{};
			m_fault.clear();
		}
		r(a) = v;
		return;
	}
	if (a == RBBM_STATUS || a == MC_STATUS || a == CONFIG_MEMSIZE ||
		a == CONFIG_APER_SIZE || a == CONFIG_APER_0_BASE ||
		a == CONFIG_APER_1_BASE || a == CONFIG_REG_1_BASE ||
		a == CONFIG_REG_APER_SIZE || a == CRTC_CRNT_FRAME || a == CP_RB_RPTR)
		return;
	if (a == CP_RB_WPTR || a == CP_IB_BUFSZ || a == CP_CSQ_CNTL)
	{
		r(a) = v;
		if (v)
			fail(
				"CP/ring/indirect-buffer execution is unsupported at " +
				hex(a));
		return;
	}
	if (a >= 0x1000 && a < 0x1400)
	{
		r(a) = v;
		if (v)
			fail("CP command FIFO unsupported");
		return;
	}
	if (a == DST_LINE_END)
	{
		r(a) = v;
		if (!halted())
			draw_line(r(DST_LINE_START), v);
		return;
	}
	if (a == RB3D_CNTL)
	{
		r(a) = v;
		if (v)
			fail("3D pipeline unsupported");
		return;
	}
	if (a == CRTC2_GEN_CNTL)
	{
		r(a) = v;
		if (v & CRTC_EN)
			fail("second CRTC unsupported");
		return;
	}
	if (a == WAIT_UNTIL)
	{
		r(a) = v;
		if (v & ~(WAIT_2D | WAIT_DMA))
			note("WAIT_UNTIL contains unmodeled conditions " + hex(v));
		return;
	}
	// Immediate flush of software-backed memory.
	if (a == DSTCACHE_CTLSTAT)
	{
		r(a) = v & ~5u;
		return;
	}
	if (a == MM_INDEX || a == MC_FB_LOCATION || a == SURFACE_CNTL ||
		a == BUS_CNTL || a == HOST_PATH_CNTL || a == CRTC_H_TOTAL_DISP ||
		a == CRTC_V_TOTAL_DISP || a == CRTC_GEN_CNTL || a == CRTC_EXT_CNTL ||
		a == CRTC_OFFSET || a == CRTC_OFFSET_CNTL || a == CRTC_PITCH ||
		(a >= CUR_OFFSET && a <= CUR_CLR1) || a == DP_WRITE_MSK ||
		a == DP_CNTL || a == DEFAULT_PITCH_OFFSET ||
		a == DEFAULT_SC_BOTTOM_RIGHT || a == SC_TOP_LEFT ||
		a == SC_BOTTOM_RIGHT || a == SRC_SC_BOTTOM_RIGHT || a == DP_DATATYPE ||
		a == CLR_CMP_CLR_SRC || a == CLR_CMP_CLR_DST || a == CLR_CMP_MASK ||
		a == BRUSH_Y_X || a == BRUSH_DATA0 || a == BRUSH_DATA1 ||
		a == DST_LINE_START || a == DST_LINE_PATCOUNT || a == DP_MIX ||
		a == DP_BRUSH_FRGD_CLR || a == DP_BRUSH_BKGD_CLR ||
		a == DP_SRC_FRGD_CLR || a == DP_SRC_BKGD_CLR || a == CLR_CMP_CNTL ||
		a == DP_SRC_ENDIAN || (a >= GPIO_VGA_DDC && a <= GPIO_CRT2_DDC))
	{
		r(a) = v;
		return;
	}
	if (a == DP_GUI_MASTER_CNTL)
	{
		r(a) = v;
		r(DP_DATATYPE) =
			((v >> 8) & 15) | ((v & 0x30f0u) << 4) | ((v & 0x4000u) << 16);
		r(DP_MIX) = (v & 0xff0000u) | ((v >> 16) & 0x700u);
		if (!(v & GMC_SRC_PITCH))
			r(SRC_PITCH_OFFSET) = r(DEFAULT_PITCH_OFFSET);
		if (!(v & GMC_DST_PITCH))
			r(DST_PITCH_OFFSET) = r(DEFAULT_PITCH_OFFSET);
		if (!(v & GMC_DST_CLIP))
		{
			r(SC_TOP_LEFT) = 0;
			r(SC_BOTTOM_RIGHT) = r(DEFAULT_SC_BOTTOM_RIGHT);
		}
		if (!(v & GMC_SRC_CLIP))
			r(SRC_SC_BOTTOM_RIGHT) = r(DEFAULT_SC_BOTTOM_RIGHT);
		// GUI_MASTER is a setup alias: disable resets the compare control.
		// A subsequent explicit CLR_CMP_CNTL write can enable source keying.
		if (v & GMC_CLR_CMP_DISABLE)
			r(CLR_CMP_CNTL) = 0;
		if (v & GMC_WR_MSK_DISABLE)
			r(DP_WRITE_MSK) = 0xffffffffu;
		return;
	}
	if (a == DP_CNTL_XDIR_YDIR_YMAJOR)
	{
		r(a) = v;
		r(DP_CNTL) =
			(r(DP_CNTL) & ~7u) | ((v >> 31) & 1u) | ((v >> 14) & 2u) | (v & 4u);
		return;
	}
	if (a == SRC_PITCH_OFFSET || a == DST_PITCH_OFFSET)
	{
		r(a) = v;
		return;
	}
	if (a == SRC_OFFSET || a == DST_OFFSET)
	{
		const uint32_t t =
			a == SRC_OFFSET ? SRC_PITCH_OFFSET : DST_PITCH_OFFSET;
		if (v & 1023u)
		{
			fail("sub-kilobyte engine offset unsupported");
			return;
		}
		r(t) = (r(t) & 0xffc00000u) | ((v >> 10) & 0x3fffffu);
		r(a) = v;
		return;
	}
	if (a == SRC_PITCH || a == DST_PITCH)
	{
		if (v & 63u)
		{
			fail("non-64-byte source pitch unsupported");
			return;
		}
		const uint32_t target =
			a == SRC_PITCH ? SRC_PITCH_OFFSET : DST_PITCH_OFFSET;
		r(target) = (r(target) & 0xc03fffffu) | ((v << 16) & 0x3fc00000u);
		r(a) = v;
		return;
	}
	if (a == SRC_X || a == SRC_Y || a == DST_X || a == DST_Y)
	{
		r(a) = v;
		const uint32_t target = (a == SRC_X || a == SRC_Y) ? SRC_Y_X : DST_Y_X;
		r(target) = (a == SRC_X || a == DST_X)
			? ((r(target) & 0xffff0000u) | (v & 0xffff))
			: ((r(target) & 0xffffu) | (v << 16));
		return;
	}
	if (a == SRC_Y_X || a == DST_Y_X)
	{
		r(a) = v;
		return;
	}
	if (a == SRC_X_Y || a == DST_X_Y)
	{
		r(a) = v;
		r(a == SRC_X_Y ? SRC_Y_X : DST_Y_X) = (v << 16) | (v >> 16);
		return;
	}
	if (a == SC_LEFT || a == SC_TOP)
	{
		r(a) = v;
		r(SC_TOP_LEFT) = a == SC_LEFT
			? ((r(SC_TOP_LEFT) & 0xffff0000u) | (v & 0xffff))
			: ((r(SC_TOP_LEFT) & 0xffff) | (v << 16));
		return;
	}
	if (a == SC_RIGHT || a == SC_BOTTOM)
	{
		r(a) = v;
		r(SC_BOTTOM_RIGHT) = a == SC_RIGHT
			? ((r(SC_BOTTOM_RIGHT) & 0xffff0000u) | (v & 0xffff))
			: ((r(SC_BOTTOM_RIGHT) & 0xffff) | (v << 16));
		return;
	}
	if (a == DST_HEIGHT_WIDTH || a == DST_WIDTH_HEIGHT)
	{
		r(a) = v;
		if (halted())
			return;
		draw(
			a == DST_HEIGHT_WIDTH ? v & 0xffff : v >> 16,
			a == DST_HEIGHT_WIDTH ? v >> 16 : v & 0xffff);
		return;
	}
	if (a >= HOST_DATA0 && a <= HOST_DATA_LAST)
	{
		r(a) = v;
		host_word(v, a == HOST_DATA_LAST);
		return;
	}
	if (!shadow_allowed(a) && m_options.strict_mmio)
	{
		fail("unknown MMIO write " + hex(a));
		return;
	}
	r(a) = v;
	m_counters.shadow_writes++;
	if (!m_warned[a / 4])
	{
		m_warned[a / 4] = true;
		note("shadow-only register " + hex(a));
	}
}

// Framebuffer aperture and 2D engine

// Aperture 1 mirrors aperture 0 with its own NONSURF byte swapping, unless
// HOST_PATH_CNTL.HDP_APER_CNTL maps it to the following VRAM instead.
bool CRV200::aperture_index(uint64_t a, size_t& index)
{
	if (a < 2ull * ApertureSize)
	{
		const bool ap1 = a >= ApertureSize;
		const uint32_t flags = (r(SURFACE_CNTL) >> (ap1 ? 22 : 20)) & 3u;
		const uint32_t sw = (flags & 1u) ^ ((flags & 2u) ? 3u : 0u);
		uint64_t offset = a;
		if (ap1 && !(r(HOST_PATH_CNTL) & HDP_APER_CNTL))
			offset -= ApertureSize;
		offset ^= sw;
		if (offset < m_vram.size())
		{
			index = size_t(offset);
			return true;
		}
	}
	if (!m_aperture_warned)
	{
		m_aperture_warned = true;
		note("aperture access beyond implemented VRAM " + hex(uint32_t(a)));
	}
	return false;
}

uint64_t CRV200::mem_read(uint32_t a, int dsize)
{
	if (dsize != 8 && dsize != 16 && dsize != 32 && dsize != 64)
	{
		fail("invalid aperture read size");
		return ~uint64_t(0);
	}
	// Unimplemented VRAM reads as open bus.
	uint64_t v = 0;
	for (unsigned i = 0; i < unsigned(dsize) / 8; i++)
	{
		size_t index = 0;
		const uint8_t byte =
			aperture_index(uint64_t(a) + i, index) ? m_vram[index] : 0xff;
		v |= uint64_t(byte) << (8 * i);
	}
	return v;
}

void CRV200::mem_write(uint32_t a, int dsize, uint64_t value)
{
	if (dsize != 8 && dsize != 16 && dsize != 32 && dsize != 64)
	{
		fail("invalid aperture write size");
		return;
	}
	for (unsigned i = 0; i < unsigned(dsize) / 8; i++)
	{
		size_t index = 0;
		if (aperture_index(uint64_t(a) + i, index))
			m_vram[index] = uint8_t(value >> (8 * i));
	}
}

bool CRV200::translate(uint64_t gpu, size_t count, size_t& index) const
{
	const uint64_t base = uint64_t(r(MC_FB_LOCATION) & 0xffff) << 16;
	const uint64_t end = (uint64_t(r(MC_FB_LOCATION) >> 16) + 1) << 16;
	if (gpu < base || gpu > end || count > end - gpu ||
		gpu - base > m_vram.size() || count > m_vram.size() - (gpu - base))
		return false;
	index = size_t(gpu - base);
	return true;
}

bool CRV200::pixel_address(
	uint32_t base, uint32_t pitch, int32_t x, int32_t y, uint32_t bpp,
	size_t& idx) const
{
	if (x < 0 || y < 0 || uint64_t(x) * bpp + bpp > pitch)
		return false;
	return translate(
		uint64_t(base) + uint64_t(y) * pitch + uint64_t(x) * bpp, bpp, idx);
}

uint32_t CRV200::pixel_read(size_t i, uint32_t b) const
{
	uint32_t v = 0;
	for (uint32_t j = 0; j < b; j++)
		v |= uint32_t(m_vram[i + j]) << (8 * j);
	return v;
}

void CRV200::pixel_write(size_t i, uint32_t b, uint32_t v)
{
	for (uint32_t j = 0; j < b; j++)
		m_vram[i + j] = uint8_t(v >> (8 * j));
	m_counters.pixels++;
}

uint32_t CRV200::rop3(uint8_t op, uint32_t p, uint32_t s, uint32_t d)
{
	uint32_t out = 0;
	for (unsigned i = 0; i < 8; i++)
		if (op & (1u << i))
			out |= (i & 4 ? p : ~p) & (i & 2 ? s : ~s) & (i & 1 ? d : ~d);
	return out;
}

// Scissor edges are 14-bit sign-magnitude; right/bottom are exclusive (XAA
// writes x2 + 1).
static int32_t scissor_edge(uint32_t v)
{
	return (v & 0x8000) ? -int32_t(v & 0x3fff) : int32_t(v & 0x3fff);
}

bool CRV200::clipped(int32_t x, int32_t y, uint32_t tl, uint32_t br) const
{
	return x < scissor_edge(tl) || y < scissor_edge(tl >> 16) ||
		x >= scissor_edge(br) || y >= scissor_edge(br >> 16);
}

void CRV200::draw(uint32_t w, uint32_t h)
{
	if (m_pending.active)
	{
		fail("new primitive while host upload is pending");
		return;
	}
	if (!w || !h)
		return;
	if (w > 8192 || h > 8192 || uint64_t(w) * h > 16u * 1024 * 1024)
	{
		fail("primitive exceeds bounded work limit");
		return;
	}
	const uint32_t g = r(DP_GUI_MASTER_CNTL), type = r(DP_DATATYPE),
				   bpp = bytes_per_pixel(type & 15);
	const uint8_t op = uint8_t(r(DP_MIX) >> 16);
	bool uses_s = rop_uses(op, 1);
	const bool uses_p = rop_uses(op, 2);
	const uint32_t source = (r(DP_MIX) >> 8) & 7, brush = (type >> 8) & 15;
	if (!bpp)
	{
		fail("unsupported pixel format");
		return;
	}
	if ((g & (1u << 27)) || !compare_supported(r(CLR_CMP_CNTL)))
	{
		fail("3D or unsupported color comparison in 2D operation");
		return;
	}
	const uint32_t compare = r(CLR_CMP_CNTL);
	const uint32_t key = r(CLR_CMP_CLR_SRC), key_mask = r(CLR_CMP_MASK);
	// A compare can require source data even when the selected ROP does not;
	// host input is consumed even for a source-independent ROP.
	if (compare || source == 3)
		uses_s = true;
	if (g & GMC_SRC_CLIP)
	{
		fail("source clipping unsupported");
		return;
	}
	if (uses_p && brush != 13 && brush != 0 && brush != 1)
	{
		fail("unsupported pattern brush datatype");
		return;
	}
	if (uses_p && brush <= 1 && (g & 0x80000000u))
	{
		fail("8x8 brush origin auto-load not implemented");
		return;
	}
	if (uses_s && source != 2 && source != 3)
	{
		fail("unsupported 2D source selector");
		return;
	}
	const uint32_t source_format = (type >> 16) & 3;
	const bool mono = source_format < 2;
	if (uses_s && source_format != 3 && !(source == 3 && mono))
	{
		fail("unsupported source datatype/transport");
		return;
	}
	if (uses_s && source == 3 && uses_p && brush != 13)
	{
		fail("combined host transfer and patterned brush unsupported");
		return;
	}
	if ((type & 0x20000000u) || r(DP_SRC_ENDIAN))
	{
		fail("host/source endianness mode unsupported");
		return;
	}
	const uint32_t dst = r(DST_PITCH_OFFSET), src = r(SRC_PITCH_OFFSET);
	if ((dst & 0xc0000000u) || (uses_s && source == 2 && (src & 0xc0000000u)))
	{
		fail("tiled engine surfaces unsupported");
		return;
	}
	const uint32_t db = (dst & 0x3fffffu) << 10, dp = (dst >> 16) & 0x3fc0u;
	const uint32_t sb = (src & 0x3fffffu) << 10, sp = (src >> 16) & 0x3fc0u;
	const int32_t x = int16_t(r(DST_Y_X) & 0xffff),
				  y = int16_t(r(DST_Y_X) >> 16);
	const int32_t sx = int16_t(r(SRC_Y_X) & 0xffff),
				  sy = int16_t(r(SRC_Y_X) >> 16);
	const int32_t xd = (r(DP_CNTL) & 1) ? 1 : -1,
				  yd = (r(DP_CNTL) & 2) ? 1 : -1;
	const uint32_t tl = r(SC_TOP_LEFT), br = r(SC_BOTTOM_RIGHT),
				   mask = r(DP_WRITE_MSK), p = r(DP_BRUSH_FRGD_CLR);
	const uint32_t brush_x = r(BRUSH_Y_X) & 7,
				   brush_y = (r(BRUSH_Y_X) >> 8) & 7;
	// Validate the entire destination/source footprint before modifying VRAM.
	for (uint32_t j = 0; j < h; j++)
		for (uint32_t i = 0; i < w; i++)
		{
			const int32_t xx = x + xd * int32_t(i), yy = y + yd * int32_t(j);
			size_t di = 0, si = 0;
			if (clipped(xx, yy, tl, br))
				continue;
			if (!pixel_address(db, dp, xx, yy, bpp, di) ||
				(uses_s && source == 2 &&
				 !pixel_address(
					 sb, sp, sx + xd * int32_t(i), sy + yd * int32_t(j), bpp,
					 si)))
			{
				fail("2D address outside GPU framebuffer window/pitch");
				return;
			}
		}
	m_counters.draws++;
	if (uses_s && source == 3)
	{
		if (bpp == 3 && !mono)
		{
			fail("24-bit HOST_DATA packing unsupported");
			return;
		}
		Pending& q = m_pending;
		q = Pending{};
		q.active = true;
		q.width = w;
		q.height = h;
		q.words_per_row = mono ? (w + 31) / 32 : (w * bpp + 3) / 4;
		q.x = x;
		q.y = y;
		q.dx = xd;
		q.dy = yd;
		q.base = db;
		q.pitch = dp;
		q.bpp = bpp;
		q.mask = mask;
		q.pattern = p;
		q.rop = op;
		q.sc_tl = tl;
		q.sc_br = br;
		q.source_format = source_format;
		q.mono_lsb = (type >> 30) & 1u;
		q.fg = r(DP_SRC_FRGD_CLR);
		q.bg = r(DP_SRC_BKGD_CLR);
		q.compare = compare;
		q.compare_color = key;
		q.compare_mask = key_mask;
		return;
	}
	for (uint32_t j = 0; j < h; j++)
		for (uint32_t i = 0; i < w; i++)
		{
			const int32_t xx = x + xd * int32_t(i), yy = y + yd * int32_t(j);
			size_t di = 0, si = 0;
			if (clipped(xx, yy, tl, br))
				continue;
			pixel_address(db, dp, xx, yy, bpp, di);
			uint32_t s = 0;
			if (uses_s && source == 2)
			{
				pixel_address(
					sb, sp, sx + xd * int32_t(i), sy + yd * int32_t(j), bpp,
					si);
				s = pixel_read(si, bpp);
			}
			if (source_rejected(compare, key, key_mask, s, bpp))
				continue;
			uint32_t pattern = p;
			if (uses_p && brush <= 1)
			{
				// Screen-aligned pattern; BRUSH_Y_X offsets its origin.
				const unsigned row = unsigned(yy + int32_t(brush_y)) & 7u;
				const unsigned col = unsigned(xx + int32_t(brush_x)) & 7u;
				const uint32_t data = r(row < 4 ? BRUSH_DATA0 : BRUSH_DATA1);
				const bool ink = mono_bit(
					data, (row & 3u) * 8u + col, (type & (1u << 30)) != 0);
				if (!ink && brush == 1)
					continue;
				pattern = ink ? p : r(DP_BRUSH_BKGD_CLR);
			}
			const uint32_t d = pixel_read(di, bpp);
			pixel_write(
				di, bpp, (rop3(op, pattern, s, d) & mask) | (d & ~mask));
		}
}

// Zero-width Bresenham line from DST_LINE_START, excluding the end pixel (XAA
// draws it separately). Solid and 32x1 mono brushes; the brush phase is
// DST_LINE_PATCOUNT[4:0].
void CRV200::draw_line(uint32_t start, uint32_t end)
{
	if (m_pending.active)
	{
		fail("new primitive while host upload is pending");
		return;
	}
	const uint32_t g = r(DP_GUI_MASTER_CNTL), type = r(DP_DATATYPE),
				   bpp = bytes_per_pixel(type & 15), brush = (type >> 8) & 15;
	const uint8_t op = uint8_t(r(DP_MIX) >> 16);
	const bool uses_p = rop_uses(op, 2);
	if (!bpp)
	{
		fail("unsupported pixel format");
		return;
	}
	if ((g & (1u << 27)) || r(CLR_CMP_CNTL))
	{
		fail("3D or color comparison in line operation");
		return;
	}
	if (rop_uses(op, 1))
	{
		fail("source-dependent line ROP unsupported");
		return;
	}
	if (uses_p && brush != 13 && brush != 6 && brush != 7)
	{
		fail("unsupported line brush datatype");
		return;
	}
	const uint32_t dst = r(DST_PITCH_OFFSET);
	if (dst & 0xc0000000u)
	{
		fail("tiled engine surfaces unsupported");
		return;
	}
	const uint32_t db = (dst & 0x3fffffu) << 10, dp = (dst >> 16) & 0x3fc0u;
	const int32_t x0 = int16_t(start & 0xffff), y0 = int16_t(start >> 16);
	const int32_t dx = int16_t(end & 0xffff) - x0, dy = int16_t(end >> 16) - y0;
	const int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
	const int32_t sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
	const bool xmajor = adx >= ady;
	const int32_t major = xmajor ? adx : ady, minor = xmajor ? ady : adx;
	const uint32_t tl = r(SC_TOP_LEFT), br = r(SC_BOTTOM_RIGHT),
				   mask = r(DP_WRITE_MSK), fg = r(DP_BRUSH_FRGD_CLR),
				   bg = r(DP_BRUSH_BKGD_CLR), pattern = r(BRUSH_DATA0),
				   phase = r(DST_LINE_PATCOUNT) & 31;
	const bool lsb = (type & (1u << 30)) != 0;
	// Validate the unclipped footprint before modifying VRAM.
	for (int pass = 0; pass < 2; ++pass)
	{
		int32_t x = x0, y = y0, err = 2 * minor - major;
		for (int32_t i = 0; i < major; ++i)
		{
			size_t di = 0;
			if (!clipped(x, y, tl, br))
			{
				if (!pixel_address(db, dp, x, y, bpp, di))
				{
					fail("line address outside GPU framebuffer window/pitch");
					return;
				}
				const bool ink =
					brush == 13 || mono_bit(pattern, (phase + i) & 31, lsb);
				if (pass && (ink || brush != 7 || !uses_p))
				{
					const uint32_t d = pixel_read(di, bpp);
					const uint32_t p = ink ? fg : bg;
					pixel_write(
						di, bpp, (rop3(op, p, 0, d) & mask) | (d & ~mask));
				}
			}
			if (err > 0)
			{
				if (xmajor)
					y += sy;
				else
					x += sx;
				err -= 2 * major;
			}
			err += 2 * minor;
			if (xmajor)
				x += sx;
			else
				y += sy;
		}
	}
	m_counters.draws++;
}

void CRV200::host_word(uint32_t value, bool last)
{
	if (halted())
		return;
	if (!m_pending.active)
	{
		fail("HOST_DATA without pending upload");
		return;
	}
	auto& p = m_pending;
	const bool mono = p.source_format < 2;
	const uint32_t pixels_per_word = mono ? 32u : 4u / p.bpp;
	const uint32_t row = p.word / p.words_per_row,
				   col = (p.word % p.words_per_row) * pixels_per_word;
	if (last && p.word + 1 != p.words_per_row * p.height)
	{
		fail("premature HOST_DATA_LAST");
		return;
	}
	for (uint32_t n = 0; n < pixels_per_word && col + n < p.width; n++)
	{
		const int32_t x = p.x + p.dx * int32_t(col + n),
					  y = p.y + p.dy * int32_t(row);
		size_t idx = 0;
		if (clipped(x, y, p.sc_tl, p.sc_br))
			continue;
		if (!pixel_address(p.base, p.pitch, x, y, p.bpp, idx))
		{
			fail("host upload address invalidated");
			return;
		}
		uint32_t source;
		if (mono)
		{
			const bool ink = mono_bit(value, n, p.mono_lsb != 0);
			if (!ink && p.source_format == 1)
				continue;
			source = ink ? p.fg : p.bg;
		}
		else
			source = value >> (8 * p.bpp * n);
		if (source_rejected(
				p.compare, p.compare_color, p.compare_mask, source, p.bpp))
			continue;
		const uint32_t d = pixel_read(idx, p.bpp);
		pixel_write(
			idx, p.bpp,
			(rop3(uint8_t(p.rop), p.pattern, source, d) & p.mask) |
				(d & ~p.mask));
	}
	if (++p.word == p.words_per_row * p.height)
		p.active = false;
}

// Display and video timing

void CRV200::advance_scanlines(uint32_t lines)
{
	// Register-derived vertical timing; PLL frequency is not evaluated.
	if (lines > 1000000u)
	{
		fail("scanline budget exceeded");
		return;
	}
	// An atomic PLL update completes on the next model step.
	m_pll[PPLL_REF_DIV] &= ~(1u << 15);
	if (!(r(CRTC_GEN_CNTL) & CRTC_EN))
		return;
	const uint32_t total = (r(CRTC_V_TOTAL_DISP) & 0xfff) + 1,
				   display = ((r(CRTC_V_TOTAL_DISP) >> 16) & 0xfff) + 1;
	if (display >= total)
		return;
	for (uint32_t n = 0; n < lines; n++)
	{
		if (++m_scanline >= total)
		{
			m_scanline = 0;
			m_frame++;
		}
		if (m_scanline == display)
		{
			r(GEN_INT_STATUS) |= 1;
			r(CRTC_STATUS) |= CRTC_VBLANK_SAVE;
		}
	}
	update_irq();
}

CRV200::Frame CRV200::scanout(std::string* error) const
{
	auto reject = [error](const char* why) {
		if (error)
			*error = why;
		return Frame{};
	};
	const uint32_t gen = r(CRTC_GEN_CNTL);
	if (!(gen & CRTC_EN) || (r(CRTC_EXT_CNTL) & DISPLAY_DIS))
		return reject("CRTC disabled/blanked");
	if (gen & 3u)
		return reject("interlaced/doublescan scanout unsupported");
	if (r(CRTC_OFFSET_CNTL))
		return reject(
			"nonzero CRTC_OFFSET_CNTL requires an unimplemented display "
			"layout");
	const uint32_t fmt = (gen >> 8) & 15, bpp = bytes_per_pixel(fmt);
	const uint32_t w = (((r(CRTC_H_TOTAL_DISP) >> 16) & 0x1ff) + 1) * 8,
				   h = ((r(CRTC_V_TOTAL_DISP) >> 16) & 0xfff) + 1;
	const uint32_t pitch = (r(CRTC_PITCH) & 0x7ff) * 8 * bpp,
				   base = r(CRTC_OFFSET) & ~7u;
	if (!bpp || w > 4096 || h > 4096 || uint64_t(w) * bpp > pitch)
		return reject("unsupported/invalid display format or pitch");
	size_t first = 0, last = 0;
	if (!translate(base, bpp, first) ||
		!translate(
			uint64_t(base) + uint64_t(h - 1) * pitch + uint64_t(w - 1) * bpp,
			bpp, last))
		return reject("display address outside GPU framebuffer window");
	Frame f;
	f.width = w;
	f.height = h;
	f.argb.resize(size_t(w) * h);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
		{
			uint32_t v = pixel_read(
						 first + size_t(y) * pitch + size_t(x) * bpp, bpp),
					 rr = 0, gg = 0, bb = 0;
			if (fmt == 2)
			{
				v = m_palette[v & 255];
				rr = (v >> 16) & 255;
				gg = (v >> 8) & 255;
				bb = v & 255;
			}
			else if (fmt == 3 || fmt == 4)
			{
				rr = (v >> (fmt == 3 ? 10 : 11)) & 31;
				gg = (v >> 5) & (fmt == 3 ? 31 : 63);
				bb = v & 31;
				rr = (rr << 3) | (rr >> 2);
				bb = (bb << 3) | (bb >> 2);
				gg = fmt == 3 ? ((gg << 3) | (gg >> 2))
							  : ((gg << 2) | (gg >> 4));
			}
			else
			{
				rr = (v >> 16) & 255;
				gg = (v >> 8) & 255;
				bb = v & 255;
			}
			// True-color LUT/gamma is bypassed by this diagnostic presenter.
			f.argb[size_t(y) * w + x] =
				0xff000000u | (rr << 16) | (gg << 8) | bb;
		}
	composite_cursor(f);
	return f;
}

// 64x64 cursor at CUR_OFFSET. Mono rows: 8 AND then 8 XOR bytes, MSB first
// (XFree86 R128/Radeon); ARGB rows: 64 premultiplied pixels. HORZ_VERT_OFF
// skips left columns and shortens the top.
void CRV200::composite_cursor(Frame& f) const
{
	const uint32_t gen = r(CRTC_GEN_CNTL), mode = (gen >> 20) & 7;
	if (!(gen & CRTC_CUR_EN) || (mode != 0 && mode != 2))
		return;
	const uint32_t posn = r(CUR_HORZ_VERT_POSN), off = r(CUR_HORZ_VERT_OFF);
	const uint32_t px = (posn >> 16) & 0x3fff, py = posn & 0x3fff;
	const uint32_t xoff = (off >> 16) & 0x3f, yoff = off & 0x3f;
	const uint32_t stride = mode ? 256 : 16;
	const uint32_t clr0 = r(CUR_CLR0) & 0xffffff, clr1 = r(CUR_CLR1) & 0xffffff;
	for (uint32_t row = 0; row < 64 - yoff && py + row < f.height; ++row)
	{
		size_t line = 0;
		if (!translate(
				uint64_t(r(CUR_OFFSET)) + uint64_t(row) * stride, stride, line))
			return;
		for (uint32_t col = xoff; col < 64 && px + col - xoff < f.width; ++col)
		{
			uint32_t& d = f.argb[size_t(py + row) * f.width + px + col - xoff];
			if (!mode)
			{
				const unsigned bit = 7 - (col & 7);
				const bool and_bit = (m_vram[line + col / 8] >> bit) & 1;
				const bool xor_bit = (m_vram[line + 8 + col / 8] >> bit) & 1;
				if (!and_bit)
					d = 0xff000000u | (xor_bit ? clr1 : clr0);
				else if (xor_bit)
					d ^= 0x00ffffffu;
				continue;
			}
			const uint32_t c = pixel_read(line + col * 4, 4), a = c >> 24;
			uint32_t out = 0xff000000u;
			for (unsigned shift = 0; shift < 24; shift += 8)
			{
				const uint32_t v = ((c >> shift) & 255) +
					(((d >> shift) & 255) * (255 - a) + 127) / 255;
				out |= std::min(v, 255u) << shift;
			}
			d = out;
		}
	}
}

void CRV200::write_ppm(std::ostream& out, const Frame& f)
{
	if (!f.width || !f.height || f.argb.size() != size_t(f.width) * f.height)
		throw std::invalid_argument("Cannot export empty/invalid scanout");
	out << "P6\n" << f.width << ' ' << f.height << "\n255\n";
	for (const uint32_t p : f.argb)
	{
		const char rgb[3] = {char(p >> 16), char(p >> 8), char(p)};
		out.write(rgb, 3);
	}
	if (!out)
		throw std::runtime_error("PPM write failed");
}

// Device lifecycle

CRV200::CRV200() : CRV200(Options{}) {}

CRV200::CRV200(const Options& o) : m_options(o)
{
	if (o.vram_bytes < 1024 * 1024 || o.vram_bytes > 64u * 1024 * 1024 ||
		(o.vram_bytes & (o.vram_bytes - 1)))
		throw std::invalid_argument(
			"VRAM must be a power of two from 1 to 64 MiB");
	m_vram.resize(o.vram_bytes);
	reset();
}

void CRV200::reset(bool clear_vram)
{
	// PCI configuration belongs to the board wrapper and is NOT reset here.
	m_regs.fill(0);
	m_pll.fill(0);
	m_warned.fill(false);
	m_aperture_warned = false;
	m_fault.clear();
	m_pending = Pending{};
	m_counters = {};
	m_scanline = m_frame = 0;
	if (clear_vram)
		std::fill(m_vram.begin(), m_vram.end(), uint8_t(0));
	for (uint32_t i = 0; i < 256; i++)
		m_palette[i] = (i << 16) | (i << 8) | i;
	r(CONFIG_MEMSIZE) = m_options.vram_bytes;
	r(CONFIG_APER_SIZE) = ApertureSize;
	r(CONFIG_REG_APER_SIZE) = RegisterSpaceSize;
	r(MC_FB_LOCATION) = ((m_options.vram_bytes >> 16) - 1) << 16;
	// Empty AGP range and a provisional 128-bit DDR profile, not a reset dump.
	r(MC_AGP_LOCATION) = 0xffff0000u;
	r(MEM_CNTL) = 1;
	r(MEM_SDRAM_MODE_REG) = 1u << 30;
	r(DP_WRITE_MSK) = 0xffffffffu;
	r(CLR_CMP_MASK) = 0xffffffffu;
	r(DP_CNTL) = 3;
	r(DEFAULT_SC_BOTTOM_RIGHT) = r(SC_BOTTOM_RIGHT) = r(SRC_SC_BOTTOM_RIGHT) =
		0x1fff1fff;
	update_irq();
}

void CRV200::set_aperture_bases(uint32_t fb, uint32_t mmio)
{
	r(CONFIG_APER_0_BASE) = fb;
	r(CONFIG_APER_1_BASE) = fb + ApertureSize;
	r(CONFIG_REG_1_BASE) = mmio;
}

void CRV200::note(const std::string& s)
{
	if (m_diagnostic)
		m_diagnostic(s, false);
}

void CRV200::fail(const std::string& s)
{
	if (m_fault.empty())
	{
		m_fault = s;
		if (m_diagnostic)
			m_diagnostic(s, true);
	}
	update_irq();
}

bool CRV200::busy() const
{
	return m_pending.active || halted();
}

bool CRV200::irq_asserted() const
{
	return (r(GEN_INT_CNTL) & r(GEN_INT_STATUS) & 1u) != 0;
}

void CRV200::update_irq()
{
	if (m_irq_callback)
		m_irq_callback(irq_asserted());
}

// Snapshots

static uint32_t crc32(const std::vector<uint8_t>& b)
{
	uint32_t c = ~0u;
	for (const uint8_t v : b)
	{
		c ^= v;
		for (int i = 0; i < 8; i++)
			c = (c >> 1) ^ (0xedb88320u & uint32_t(0 - int32_t(c & 1)));
	}
	return ~c;
}

static void put32(std::vector<uint8_t>& b, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		b.push_back(uint8_t(v >> (8 * i)));
}

static uint32_t get32(const std::vector<uint8_t>& b, size_t& i)
{
	if (i > b.size() || b.size() - i < 4)
		throw std::runtime_error("Truncated RV200 snapshot");
	uint32_t v = 0;
	for (int n = 0; n < 4; n++)
		v |= uint32_t(b[i++]) << (8 * n);
	return v;
}

static void stream32(std::ostream& s, uint32_t v)
{
	char b[4];
	for (int i = 0; i < 4; i++)
		b[i] = char(v >> (i * 8));
	s.write(b, 4);
}

static uint32_t stream32(std::istream& s)
{
	unsigned char b[4];
	s.read(reinterpret_cast<char*>(b), 4);
	if (!s)
		throw std::runtime_error("Truncated RV200 snapshot header");
	return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
		(uint32_t(b[3]) << 24);
}

void CRV200::SaveState(std::ostream& out) const
{
	std::vector<uint8_t> b;
	b.reserve(m_vram.size() + 24000);
	put32(b, m_options.vram_bytes);
	put32(b, m_options.strict_mmio ? 1 : 0);
	for (const uint32_t v : m_regs)
		put32(b, v);
	for (const uint32_t v : m_pll)
		put32(b, v);
	for (const uint32_t v : m_palette)
		put32(b, v);
	put32(b, m_scanline);
	put32(b, m_frame);
	const auto& p = m_pending;
	for (const uint32_t v :
		 {uint32_t(p.active),
		  p.width,
		  p.height,
		  p.word,
		  p.words_per_row,
		  uint32_t(p.x),
		  uint32_t(p.y),
		  uint32_t(p.dx),
		  uint32_t(p.dy),
		  p.base,
		  p.pitch,
		  p.bpp,
		  p.mask,
		  p.pattern,
		  p.rop,
		  p.sc_tl,
		  p.sc_br,
		  p.source_format,
		  p.mono_lsb,
		  p.fg,
		  p.bg,
		  p.compare,
		  p.compare_color,
		  p.compare_mask})
		put32(b, v);
	// Preserve the fault, not host callbacks.
	put32(b, uint32_t(m_fault.size()));
	b.insert(b.end(), m_fault.begin(), m_fault.end());
	for (const uint64_t v :
		 {m_counters.reads, m_counters.writes, m_counters.draws,
		  m_counters.pixels, m_counters.shadow_writes})
	{
		put32(b, uint32_t(v));
		put32(b, uint32_t(v >> 32));
	}
	b.insert(b.end(), m_vram.begin(), m_vram.end());
	out.write("RV2SNP01", 8);
	stream32(out, uint32_t(b.size()));
	stream32(out, crc32(b));
	out.write(
		reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
	if (!out)
		throw std::runtime_error("RV200 snapshot write failed");
}

void CRV200::RestoreState(std::istream& in)
{
	char magic[8];
	in.read(magic, 8);
	if (!in || std::memcmp(magic, "RV2SNP01", 8))
		throw std::runtime_error("RV200 snapshot format mismatch");
	const uint32_t len = stream32(in), crc = stream32(in);
	if (len < m_options.vram_bytes || len > m_options.vram_bytes + 65536u)
		throw std::runtime_error("RV200 snapshot size outside bounds");
	std::vector<uint8_t> b(len);
	in.read(reinterpret_cast<char*>(b.data()), std::streamsize(len));
	if (!in || crc32(b) != crc)
		throw std::runtime_error("Truncated/corrupt RV200 snapshot");
	size_t i = 0;
	if (get32(b, i) != m_options.vram_bytes ||
		get32(b, i) != (m_options.strict_mmio ? 1u : 0u))
		throw std::runtime_error("RV200 snapshot options mismatch");
	CRV200 tmp(m_options);
	for (auto& v : tmp.m_regs)
		v = get32(b, i);
	for (auto& v : tmp.m_pll)
		v = get32(b, i);
	for (auto& v : tmp.m_palette)
		v = get32(b, i);
	tmp.m_scanline = get32(b, i);
	tmp.m_frame = get32(b, i);
	auto& p = tmp.m_pending;
	const uint32_t active = get32(b, i);
	p.active = active != 0;
	p.width = get32(b, i);
	p.height = get32(b, i);
	p.word = get32(b, i);
	p.words_per_row = get32(b, i);
	p.x = int32_t(get32(b, i));
	p.y = int32_t(get32(b, i));
	p.dx = int32_t(get32(b, i));
	p.dy = int32_t(get32(b, i));
	p.base = get32(b, i);
	p.pitch = get32(b, i);
	p.bpp = get32(b, i);
	p.mask = get32(b, i);
	p.pattern = get32(b, i);
	p.rop = get32(b, i);
	p.sc_tl = get32(b, i);
	p.sc_br = get32(b, i);
	p.source_format = get32(b, i);
	p.mono_lsb = get32(b, i);
	p.fg = get32(b, i);
	p.bg = get32(b, i);
	p.compare = get32(b, i);
	p.compare_color = get32(b, i);
	p.compare_mask = get32(b, i);
	const bool mono = p.source_format < 2;
	if (active > 1 || tmp.m_scanline > 4096 || p.width > 8192 ||
		p.height > 8192 ||
		(p.active &&
		 (!p.width || !p.height ||
		  (p.bpp != 1 && p.bpp != 2 && p.bpp != 4 && !(mono && p.bpp == 3)) ||
		  (p.source_format != 0 && p.source_format != 1 &&
		   p.source_format != 3) ||
		  p.mono_lsb > 1 ||
		  (p.compare != 0 && p.compare != 0x01000004 &&
		   p.compare != 0x01000005) ||
		  (p.dx != 1 && p.dx != -1) || (p.dy != 1 && p.dy != -1) ||
		  p.words_per_row !=
			  (mono ? (p.width + 31) / 32 : (p.width * p.bpp + 3) / 4) ||
		  p.word >= uint64_t(p.words_per_row) * p.height || p.rop > 255)))
		throw std::runtime_error("Invalid RV200 snapshot execution state");
	const uint32_t fl = get32(b, i);
	if (fl > 4096 || fl > b.size() - i)
		throw std::runtime_error("Invalid RV200 snapshot fault length");
	tmp.m_fault.assign(reinterpret_cast<const char*>(b.data() + i), fl);
	i += fl;
	uint64_t* counts[] = {
		&tmp.m_counters.reads, &tmp.m_counters.writes, &tmp.m_counters.draws,
		&tmp.m_counters.pixels, &tmp.m_counters.shadow_writes};
	for (auto* v : counts)
	{
		const uint64_t lo = get32(b, i), hi = get32(b, i);
		*v = lo | (hi << 32);
	}
	if (b.size() - i != m_vram.size())
		throw std::runtime_error("RV200 snapshot payload length mismatch");
	std::copy(b.begin() + std::ptrdiff_t(i), b.end(), tmp.m_vram.begin());
	// Commit only after all checks. Preserve the host's callbacks.
	m_regs = tmp.m_regs;
	m_pll = tmp.m_pll;
	m_palette = tmp.m_palette;
	m_pending = tmp.m_pending;
	m_scanline = tmp.m_scanline;
	m_frame = tmp.m_frame;
	m_fault = tmp.m_fault;
	m_counters = tmp.m_counters;
	m_vram.swap(tmp.m_vram);
	m_warned.fill(false);
	m_aperture_warned = false;
	update_irq();
}
