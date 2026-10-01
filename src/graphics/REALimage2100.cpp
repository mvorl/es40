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

#include "REALimage2100.h"
#include <algorithm>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>

static bool valid_width(int bits)
{
	return bits == 8 || bits == 16 || bits == 32;
}

static uint32_t width_mask(int bits)
{
	return bits == 32 ? 0xffffffffu : (1u << bits) - 1u;
}

// Register dispatch

void CRealImage2100::dac_port_map(address_map& map)
{
	map.unmap_value_high();
	map(0x10, 0x10)
		.lrw8(
			NAME([this](offs_t) {
				return u8(m_dac_index);
			}),
			NAME([this](offs_t, u8 v) {
				m_dac_index = uint16_t((m_dac_index & 0xff00) | v);
				m_dac_component = 0;
			}));
	map(0x14, 0x14)
		.lrw8(
			NAME([this](offs_t) {
				return u8(m_dac_index >> 8);
			}),
			NAME([this](offs_t, u8 v) {
				m_dac_index =
					uint16_t((m_dac_index & 255) | (uint16_t(v) << 8));
				m_dac_component = 0;
			}));
	// RGB640 byte registers and the streamed tables used by the NT miniport.
	map(0x18, 0x18)
		.lrw8(
			NAME([this](offs_t) {
				return dac_data_read();
			}),
			NAME([this](offs_t, u8 v) {
				dac_data_write(v);
			}));
}

// RGB indices select triples; WAT and cursor RAM are byte addressed.
uint32_t CRealImage2100::dac_data_offset() const
{
	if (m_dac_index >= 0x4300 && m_dac_index < 0x4400)
		return 0x4300 + 3u * (m_dac_index - 0x4300) + m_dac_component;
	if (m_dac_index >= 0x4800 && m_dac_index < 0x4808)
		return 0x4800 + 3u * (m_dac_index - 0x4800) + m_dac_component;
	return m_dac_index;
}

void CRealImage2100::advance_dac_data()
{
	if ((m_dac_index >= 0x4300 && m_dac_index < 0x4400) ||
		(m_dac_index >= 0x4800 && m_dac_index < 0x4808))
	{
		if (++m_dac_component == 3)
		{
			m_dac_component = 0;
			++m_dac_index;
		}
	}
	else if ((m_dac_index >= 0x0100 && m_dac_index < 0x0140) ||
		(m_dac_index >= 0x0200 && m_dac_index < 0x0240) ||
		(m_dac_index >= 0x1000 && m_dac_index < 0x1400))
		++m_dac_index;
}

uint8_t CRealImage2100::dac_data_read()
{
	const uint8_t value = m_dac_regs[dac_data_offset()];
	advance_dac_data();
	return value;
}

void CRealImage2100::dac_data_write(uint8_t value)
{
	m_dac_regs[dac_data_offset()] = value;
	advance_dac_data();
}

// Readback latches, display selection and unit reset.
uint32_t* CRealImage2100::native_register(uint32_t a)
{
	if (a >= DMABase && a < DMABase + DMARegisterCount * 4)
		return &m_dma_regs[(a - DMABase) / 4];
	switch (a)
	{
	case UnitReset:
		return &m_unit_reset;
	case InterruptEnable:
		return &m_interrupt_enable;
	case VGAControl:
		return &m_vga_control;
	case DisplayControl:
		return &m_display_control;
	default:
		return nullptr;
	}
}

// Drawing is synchronous. Synthetic retrace sits inside vertical blank.
uint32_t CRealImage2100::status_read()
{
	const uint32_t phase = m_status_phase;
	m_status_phase = (phase + 1) % StatusFrameReads;
	if (m_status_phase == 0)
		advance_frame();
	uint32_t v = 0;
	if (phase >= StatusFrameReads - 8)
		v |= StatusVBlank;
	if (phase >= StatusFrameReads - 6 && phase < StatusFrameReads - 2)
		v |= StatusVRetrace;
	return v;
}

uint32_t CRealImage2100::ReadMem(uint32_t a, int bits)
{
	if (!valid_width(bits) || (a & (unsigned(bits) / 8 - 1)))
	{
		report(
			"ACCESS_WIDTH", a, uint32_t(bits),
			"Invalid native width or alignment");
		return 0xffffffffu;
	}
	if (a >= 0x838000 && a < 0x838020 &&
		m_dac_ports.has_handler(a - 0x838000))
		return m_dac_ports.read_byte(a - 0x838000);
	if (const uint32_t* reg = native_register(a & ~3u))
		return (*reg >> ((a & 3) * 8)) & width_mask(bits);
	if ((a & ~3u) == Status)
		return (status_read() >> ((a & 3) * 8)) & width_mask(bits);
	if ((a & ~3u) == BoardStatus)
	{
		const uint32_t v = m_frame_counter | (uint32_t(BoardStraps) << 16) |
			(uint32_t(m_board_control) << 24);
		return (v >> ((a & 3) * 8)) & width_mask(bits);
	}
	if ((a & ~3u) == BoardIO)
	{
		const uint32_t v = m_board_io | (uint32_t(BoardIDPCGA3) << 24);
		return (v >> ((a & 3) * 8)) & width_mask(bits);
	}
	const auto it = m_shadow.find(a & ~3u);
	const uint32_t value = it == m_shadow.end() ? 0 : it->second;
	// Reads return the stored value; readback is modeled, not measured.
	if (!native_storage_register(a & ~3u))
		unimplemented_once("REALimage register", a, value, false);
	return (value >> ((a & 3) * 8)) & width_mask(bits);
}

void CRealImage2100::WriteMem(uint32_t a, int bits, uint32_t v)
{
	if (!valid_width(bits) || (a & (unsigned(bits) / 8 - 1)))
	{
		report("ACCESS_WIDTH", a, v, "Invalid native width or alignment");
		return;
	}
	if (a >= 0x838000 && a < 0x838020 &&
		m_dac_ports.has_handler(a - 0x838000))
	{
		m_dac_ports.write_byte(a - 0x838000, u8(v));
		return;
	}
	const uint32_t key = a & ~3u, shift = (a & 3) * 8,
				   lanes = width_mask(bits) << shift;
	if (uint32_t* reg = native_register(key))
	{
		*reg = (*reg & ~lanes) | ((v << shift) & lanes);
		// Unit resets cancel an in-flight host transfer, not color storage.
		if (key == UnitReset && !(*reg & (1u << 26)))
			m_pending = {};
		if (key == DMAReset && (*reg & 1))
			m_dma_regs[(DMACommand - DMABase) / 4] = 0;
		if (key == DMACommand && (lanes & 0xc0000000) &&
			(*reg & 0xc0000000))
		{
			if (m_dma_regs[(DMAReset - DMABase) / 4] & 1)
				*reg = 0;
			else
				// No completion write until the card-side stream is implemented.
				unimplemented("REALimage DMA transfer", key, *reg, true);
		}
		return;
	}
	if (key == Status)
	{
		unimplemented_once("REALimage status register", a, v, true);
		return;
	}
	// The Alpha miniport rewrites the whole longword from its byte copy;
	// counter and strap bytes ignore it.
	if (key == BoardStatus)
	{
		if (lanes & 0xff000000u)
			m_board_control = uint8_t(((v << shift) & lanes) >> 24);
		return;
	}
	if (key == BoardIO)
	{
		m_board_io = (m_board_io & ~lanes & 0x00ffffffu) |
			((v << shift) & lanes & 0x00ffffffu);
		if (lanes & 0xff000000u)
			m_board_timing = uint8_t(((v << shift) & lanes) >> 24);
		return;
	}
	if (a >= HostData && a < HostData + HostDataSize)
	{
		if (bits == 32)
			host_data(v);
		else
			unimplemented_once("REALimage host-data width", a, v, true);
		return;
	}
	auto it = m_shadow.find(key);
	if (it == m_shadow.end())
	{
		if (m_shadow.size() >= MaxShadowRegisters)
		{
			unimplemented_once("REALimage register", a, v, true);
			if (!m_shadow_full_warned)
			{
				m_shadow_full_warned = true;
				report(
					"SHADOW_LIMIT", a, v,
					"Native register storage full; further writes ignored");
			}
			return;
		}
		it = m_shadow.emplace(key, 0).first;
	}
	it->second = (it->second & ~lanes) | ((v << shift) & lanes);
	if (plane_write(key, lanes, (v << shift) & lanes))
		return;
	if (key == HostCommand || key == FillCommand || key == BlockCommand)
	{
		// Only longword command launches have been established by the driver.
		if (bits == 32)
		{
			if (key == BlockCommand)
				block_command(it->second);
			else
				start_command(key, it->second);
		}
		else
			unimplemented_once("REALimage command width", a, v, true);
	}
	else if (key == SyncCommand)
	{
		// Idle startup synchronization does not cancel an unfinished upload.
		if (bits != 32 || (it->second && it->second != 0x80000000u) ||
			m_pending.width)
			unimplemented_once("REALimage synchronization command", a, v, true);
	}
	else if (key == DisplaySelect)
	{
		if (it->second)
			unimplemented_once("REALimage display selector", a, v, true);
	}
	else if (!native_storage_register(key))
	{
		// Unmodeled mask aliases must not leave a stale block mask usable.
		if ((key & ~0x1e000u) == HostData + 0x400 &&
			(peek(DrawControl) & (1u << 26)))
			for (unsigned bank = 0; bank < PlaneCount; ++bank)
				if (peek(DrawControl) & (0x1000u << bank))
					m_planes[bank].unknown_masks |= (key >> 13) & 15;
		unimplemented_once("REALimage register", a, v, true);
	}
}

// NT 2D subset, decoded from pbxgdac.dll and the startup trace.

uint32_t CRealImage2100::peek(uint32_t a) const
{
	const auto it = m_shadow.find(a & ~3u);
	return it == m_shadow.end() ? 0 : it->second;
}

int CRealImage2100::plane_register(uint32_t a)
{
	if (a >= PlaneClearColor && a < PlaneClearColor + 32)
		return 16 + int((a - PlaneClearColor) / 4);
	if (a == PlanePixelMask || a == PlanePixelMask0)
		return 24;
	switch (a)
	{
	case PlaneStateBase + 0x00: case PlaneStateBase + 0x04:
	case PlaneStateBase + 0x08: case PlaneStateBase + 0x0c:
	case PlaneStateBase + 0x10: case PlaneStateBase + 0x14:
	case PlaneStateBase + 0x18: case PlaneStateBase + 0x20:
	case PlaneStateBase + 0x24: case PlaneStateBase + 0x28:
	case PlaneStateBase + 0x2c: case PlaneStateBase + 0x38:
	case PlaneStateBase + 0x3c:
		return int((a - PlaneStateBase) / 4);
	default:
		return -1;
	}
}

bool CRealImage2100::plane_write(uint32_t a, uint32_t lanes, uint32_t value)
{
	const int index = plane_register(a);
	if (index < 0)
		return false;
	const uint32_t control = peek(DrawControl), banks = (control >> 12) & 15;
	if (!(control & (1u << 26)))
	{
		unimplemented_once("REALimage plane access", a, value, true);
		return true;
	}
	// Configuration writes broadcast to the selected 3D-RAM planes.
	for (unsigned bank = 0; bank < PlaneCount; ++bank)
		if (banks & (1u << bank))
		{
			auto& plane = m_planes[bank];
			if (a == PlanePixelMask && lanes == 0xffffffffu)
				plane.unknown_masks = 0;
			else if (a == PlanePixelMask0 && lanes == 0xffffffffu)
				plane.unknown_masks &= ~1u;
			const int end = index + (a == PlanePixelMask ? 4 : 1);
			for (int i = index; i < end; ++i)
			{
				plane.regs[i] = (plane.regs[i] & ~lanes) | value;
				plane.written |= 1u << i;
			}
		}
	return true;
}

uint32_t CRealImage2100::plane_value(
	unsigned bank, unsigned index, uint32_t fallback) const
{
	const auto& plane = m_planes[bank];
	return plane.written & (1u << index) ? plane.regs[index] : fallback;
}

bool CRealImage2100::plane_profile(unsigned bank) const
{
	// Only the driver's ordinary RGB profile; comparison/blending stay guarded.
	const uint32_t compare_mask = plane_value(bank, 10, 0);
	return plane_value(bank, 1, 0) == 0 && plane_value(bank, 2, 0) == 0 &&
		plane_value(bank, 3, 0) == 0 &&
		!(plane_value(bank, 4, 0x03030303) & 0xf0f0f0f0) &&
		plane_value(bank, 5, 0x0a000000) == 0x0a000000 &&
		plane_value(bank, 6, 0) == 0 && plane_value(bank, 8, 0) == 0 &&
		plane_value(bank, 9, 0) == 0 &&
		(compare_mask == 0 || compare_mask == 0x00ff0000) &&
		plane_value(bank, 11, 0x33300000) == 0x33300000 &&
		plane_value(bank, 14, 0x100) == 0x100 &&
		plane_value(bank, 15, 0) == 0;
}

bool CRealImage2100::native_storage_register(uint32_t a) const
{
	return a == DrawControl || a == MemoryControl || a == PixelControl ||
		a == Foreground || a == Background || a == HostOrigin ||
		a == MonoPattern0 || a == MonoPattern1 ||
		a == MonoPattern2 || a == MonoPattern3 ||
		a == HostExtent || a == FillOrigin || a == FillExtent ||
		a == HostCommand || a == FillCommand ||
		a == BlockSource || a == BlockDestination || a == BlockExtent ||
		a == BlockCommand ||
		a == ContextControl || a == DisplaySelect ||
		a == BoardTiming || a == WindowMask ||
		a == ClipXMax || a == ClipYMax || a == ClipXMin || a == ClipYMin ||
		a == GlobalControl0 || a == GlobalControl1 || a == GlobalControl2 ||
		a == PipelineControl0 || a == PipelineControl1 ||
		a == PipelineControl2 || a == PipelineControl3 ||
		(a >= TimingBase && a <= TimingBase + 0x1c);
}

bool CRealImage2100::native_copy_control_profile() const
{
	// Other programmed pipeline modes have not been decoded.
	const std::pair<uint32_t, uint32_t> profile[] = {
		{GlobalControl0, 1}, {GlobalControl1, 0x20811}, {GlobalControl2, 0x33},
		{PipelineControl0, 0}, {PipelineControl1, 0},
		{PipelineControl2, 0x10000000}, {PipelineControl3, 0}};
	for (const auto& reg : profile)
	{
		const auto it = m_shadow.find(reg.first);
		if (it != m_shadow.end() && it->second != reg.second)
			return false;
	}
	return true;
}

bool CRealImage2100::copy_profile() const
{
	// Only the observed RGB copy profile; bits 12..15 select banks.
	const uint32_t control = peek(DrawControl), banks = (control >> 12) & 15;
	return (control & ~0xf000u) == 0x81000702 && banks && !(banks & ~3u) &&
		peek(MemoryControl) == 0x0c008000 &&
		peek(PixelControl) == 0x42722060 && native_copy_control_profile();
}

void CRealImage2100::color_write(
	uint32_t x, uint32_t y, uint32_t color, uint32_t banks)
{
	if (x >= ColorWidth || y >= ColorHeight)
		return;
	const uint32_t offset = y * ColorWidth + x;
	for (uint32_t bank = 0; bank < 2; ++bank)
		if (banks & (1u << bank))
		{
			uint32_t& destination = m_color[bank * ColorPixels + offset];
			const uint32_t mask = plane_value(bank, 0, 0xffffffff) & 0xffffff,
				rops = plane_value(bank, 4, 0x03030303);
			uint32_t result = 0;
			for (unsigned shift = 0; shift < 24; shift += 8)
			{
				const unsigned op = (rops >> shift) & 15;
				uint32_t channel = 0;
				if (op & 1) channel |= color & destination;
				if (op & 2) channel |= color & ~destination;
				if (op & 4) channel |= ~color & destination;
				if (op & 8) channel |= ~color & ~destination;
				result |= channel & (0xffu << shift);
			}
			destination = (destination & ~mask) | (result & mask);
		}
}

void CRealImage2100::block_command(uint32_t v)
{
	const uint32_t control = peek(DrawControl), banks = (control >> 12) & 15,
		source = peek(BlockSource), destination = peek(BlockDestination),
		extent = peek(BlockExtent);
	const bool copy = (v & 0x20000) != 0, configuration = (control & 0x04000000) != 0;
	const uint32_t dx = destination & 0x7ff, dy = destination >> 16,
		width = (extent & 0x7ff) + 1, height = (extent >> 16) + 1,
		scale_x = copy ? 80 : 8, scale_y = copy ? 16 : 4,
		x = dx * scale_x, y = dy * scale_y,
		right = x + width * scale_x, bottom = y + height * scale_y;
	bool geometry_known = false;
	auto invalidate_cache = [&]() {
		for (unsigned bank = 0; bank < 2; ++bank)
			if (banks & (1u << bank))
			{
				auto& cache = m_clear_cache[bank];
				const uint32_t cx = (cache.source & 0xffff) * 80,
					cy = (cache.source >> 16) * 16;
				if (!geometry_known ||
					(x < cx + 168 && right > cx && y < cy + 36 && bottom > cy))
					cache = {};
			}
	};
	auto reject = [&]() {
		invalidate_cache();
		unimplemented_once("REALimage block command/profile", BlockCommand, v, true);
	};
	if ((control & ~0x0400f000u) != 0x81000702 || !banks || (banks & ~7u) ||
		v != (((banks ^ 7u) << 18) | (copy ? 0x30000u : 0x10000u)) ||
		peek(MemoryControl) != 0x0c008000 || peek(PixelControl) != 0x42722060 ||
		!native_copy_control_profile() || m_pending.width ||
		((source | destination | extent) & ~0x07ff07ffu) ||
		(!copy && source) || (copy && !configuration) ||
		((banks & 4) && plane_value(2, 0, 0xffffffff)))
	{
		reject();
		return;
	}
	geometry_known = true;
	// The driver seeds 21x9 small blocks for its repeated 2x2-tile source.
	const bool seed = !configuration && !copy;
	if (seed && (extent != 0x00080014 || dx % 10 || dy % 4 ||
		(dx * 8 < ColorWidth && dy * 4 < ColorHeight)))
	{
		reject();
		return;
	}
	std::array<uint32_t, 2> colors{};
	for (unsigned bank = 0; bank < 2; ++bank)
	{
		const uint32_t mask = plane_value(bank, 0, 0xffffffff) & 0xffffff;
		if (!(banks & (1u << bank)) || !mask)
			continue;
		if (!plane_profile(bank) || m_planes[bank].unknown_masks)
		{
			reject();
			return;
		}
		if (copy)
		{
			const auto& cache = m_clear_cache[bank];
			if (cache.source != source || (cache.known & mask) != mask)
			{
				reject();
				return;
			}
			colors[bank] = cache.color;
		}
		else
		{
			colors[bank] = plane_value(bank, 16, 0) & 0xffffff;
			if ((m_planes[bank].written & 0x00ff0000) != 0x00ff0000)
			{
				reject();
				return;
			}
			for (unsigned i = 17; i < 24; ++i)
				if ((plane_value(bank, i, 0) ^ colors[bank]) & mask)
				{
					reject();
					return;
				}
		}
		// Accept only cases where block ROP application and bypass agree.
		const uint32_t rops = plane_value(bank, 4, 0x03030303);
		for (unsigned shift = 0; shift < 24; shift += 8)
			if ((mask & (0xffu << shift)) && ((rops >> shift) & 15) != 3 &&
				(((rops >> shift) & 15) != 0 ||
					(colors[bank] & mask & (0xffu << shift))))
			{
				reject();
				return;
			}
		for (unsigned i = 24; i < 28; ++i)
		{
			const uint32_t bits = plane_value(bank, i, 0xffffffff);
			if (bits != (bits & 255) * 0x01010101u || (seed && bits != 0xffffffffu))
			{
				reject();
				return;
			}
		}
	}
	if (seed)
	{
		const uint32_t key = ((dy / 4) << 16) | (dx / 10);
		for (unsigned bank = 0; bank < 2; ++bank)
			if (banks & (1u << bank))
			{
				const uint32_t mask = plane_value(bank, 0, 0xffffffff) & 0xffffff;
				if (!mask)
					continue;
				auto& cache = m_clear_cache[bank];
				if (cache.source != key)
					cache = {};
				cache.source = key;
				cache.color = (cache.color & ~mask) | (colors[bank] & mask);
				cache.known |= mask;
			}
		return;
	}
	invalidate_cache();
	for (uint32_t row = y; row < std::min(bottom, ColorHeight); ++row)
		for (uint32_t col = x; col < std::min(right, ColorWidth); ++col)
			for (unsigned bank = 0; bank < 2; ++bank)
				if (banks & (1u << bank))
				{
					const uint32_t bits = plane_value(bank, 24 + (col & 3), 0xffffffff),
						bit = 2 * (row & 3) + ((col >> 2) & 1);
					if (bits & (1u << bit))
						color_write(col, row, colors[bank], 1u << bank);
				}
}

void CRealImage2100::start_command(uint32_t a, uint32_t v)
{
	for (unsigned bank = 0; bank < 2; ++bank)
		if (peek(DrawControl) & (0x1000u << bank))
			m_clear_cache[bank] = {};
	// A new launch cannot inherit the tail of an earlier host upload.
	if (m_pending.width)
		report("HOST_INTERRUPTED", a, v, "Incomplete native host upload replaced");
	m_pending = {};
	if (!v)
		return;
	const bool upload = a == HostCommand && v == 0x01000032;
	const bool fill = a == FillCommand && v == 0x09000832;
	const uint32_t selected = (peek(DrawControl) >> 12) & 3;
	if ((!upload && !fill) || !copy_profile() ||
		((selected & 1) && !plane_profile(0)) ||
		((selected & 2) && !plane_profile(1)))
	{
		unimplemented_once("REALimage 2D command/profile", a, v, true);
		return;
	}
	const uint32_t origin = peek(upload ? HostOrigin : FillOrigin),
		extent = peek(upload ? HostExtent : FillExtent),
		width = (extent & 0xffff) + 1, height = (extent >> 16) + 1,
		banks = (peek(DrawControl) >> 12) & 3;
	const int32_t x = int16_t(origin & 0xffff), y = int16_t(origin >> 16);
	if (upload)
	{
		if (uint64_t(width) * height > 0xffffffffu)
		{
			report("HOST_BOUNDS", a, v, "Native host extent exceeds model limit");
			return;
		}
		m_pending = {origin & 0xffff, origin >> 16, width, height, 0, banks};
		return;
	}
	// Clip before iterating so malformed extents cannot cause unbounded work.
	const int32_t left = std::max(x, int32_t(0)),
		top = std::max(y, int32_t(0)),
		right = std::min(x + int32_t(width), int32_t(ColorWidth)),
		bottom = std::min(y + int32_t(height), int32_t(ColorHeight));
	const uint32_t color = peek(Foreground);
	for (int32_t row = top; row < bottom; ++row)
		for (int32_t col = left; col < right; ++col)
			color_write(uint32_t(col), uint32_t(row), color, banks);
}

void CRealImage2100::host_data(uint32_t v)
{
	if (!m_pending.width)
	{
		unimplemented_once("REALimage host data without upload", HostData, v, true);
		return;
	}
	const int32_t x = int16_t(m_pending.x) +
		int32_t(m_pending.word % m_pending.width);
	const int32_t y = int16_t(m_pending.y) +
		int32_t(m_pending.word / m_pending.width);
	color_write(uint32_t(x), uint32_t(y), v, m_pending.banks);
	if (++m_pending.word == uint64_t(m_pending.width) * m_pending.height)
		m_pending = {};
}

CRealImage2100::Frame CRealImage2100::scanout(std::string* error) const
{
	auto reject = [&](const char* text) {
		if (error)
			*error = text;
		return Frame{};
	};
	if (error)
		error->clear();
	if (!native_display() || (m_board_io & 0x80))
		return reject("Native display disabled/blanked");
	if (peek(MemoryControl) != 0x0c008000 ||
		peek(PixelControl) != 0x42722060)
		return reject("Native pixel layout unsupported");
	if (!(m_dac_regs[0x0b] & 1) || !(m_dac_regs[0x0d] & 4))
		return reject("Native DAC disabled");
	// Only the captured RGB640 window format is decoded.
	for (uint32_t i = 0; i < 16; ++i)
	{
		const uint32_t fb = 0x100 + i * 4, overlay = 0x200 + i * 4;
		if (m_dac_regs[fb] != 8 || m_dac_regs[fb + 1] != 12 ||
			m_dac_regs[fb + 2] || m_dac_regs[fb + 3] ||
			m_dac_regs[overlay] != 4 || m_dac_regs[overlay + 1] ||
			m_dac_regs[overlay + 2] || m_dac_regs[overlay + 3] != 0x48)
			return reject("Native DAC window format unsupported");
	}
	// Unlike VGA, vertical display is a count, not a last-line index.
	const uint32_t horizontal = peek(TimingBase),
		overflow = peek(TimingBase + 4) >> 24,
		vertical = peek(TimingBase + 0x10);
	const uint32_t width = (((horizontal >> 8) & 255) + 1) * 8,
		height = ((vertical >> 16) & 255) |
			((overflow & 2) << 7) | ((overflow & 0x40) << 3);
	// Extended vertical timing and other serializer modes are not established.
	if ((peek(TimingBase + 0x1c) & 0x00ff0000) || !height ||
		width > ColorWidth || height > ColorHeight)
		return reject("Native timing unsupported");
	Frame frame;
	frame.width = width;
	frame.height = height;
	frame.argb.resize(size_t(width) * height);
	// This profile displays bank 1; page flips and overlays are unmodeled.
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x)
			frame.argb[size_t(y) * width + x] =
				0xff000000 | m_color[size_t(y) * ColorWidth + x];
	return frame;
}

// BAR2 index/data pair: dword index at +0, width-aware data at +4.
uint32_t CRealImage2100::io_read(uint32_t a, int bits)
{
	if (a == 0 && bits == 32)
		return m_io_index;
	if (a == 4 && valid_width(bits))
		return ReadMem(m_io_index, bits);
	unimplemented("REALimage BAR2 port", a, 0, false);
	return width_mask(valid_width(bits) ? bits : 32);
}

void CRealImage2100::io_write(uint32_t a, int bits, uint32_t v)
{
	if (a == 0 && bits == 32)
	{
		m_io_index = v;
		return;
	}
	if (a == 4 && valid_width(bits))
	{
		WriteMem(m_io_index, bits, v);
		return;
	}
	unimplemented("REALimage BAR2 port", a, v, true);
}

uint32_t CRealImage2100::mem_read(uint32_t a, int bits)
{
	if (!valid_width(bits) || (a & (unsigned(bits) / 8 - 1)))
	{
		report("ACCESS_WIDTH", a, uint32_t(bits), "Invalid BAR1 width or alignment");
		return 0xffffffffu;
	}
	const uint32_t offset = texture_offset(a);
	if (offset == 0xffffffffu)
		return width_mask(bits);
	uint32_t value = 0;
	for (unsigned i = 0; i < unsigned(bits) / 8; ++i)
		value |= uint32_t(m_texture[offset + i]) << (i * 8);
	return value;
}

void CRealImage2100::mem_write(uint32_t a, int bits, uint32_t v)
{
	if (!valid_width(bits) || (a & (unsigned(bits) / 8 - 1)))
	{
		report("ACCESS_WIDTH", a, v, "Invalid BAR1 width or alignment");
		return;
	}
	const uint32_t offset = texture_offset(a);
	if (offset == 0xffffffffu)
		return;
	for (unsigned i = 0; i < unsigned(bits) / 8; ++i)
		m_texture[offset + i] = uint8_t(v >> (i * 8));
}

uint32_t CRealImage2100::texture_offset(uint32_t a) const
{
	if (a >= MaxTextureSize)
		return 0xffffffffu;
	if (m_texture.size() == MaxTextureSize)
		return a;
	// The 300 populates two of four 4 KiB tiles per aperture row.
	if (a & 0x2000)
		return 0xffffffffu;
	return (a & 0x1fff) | ((a >> 14) << 13);
}

void CRealImage2100::configure_texture_memory(uint32_t bytes)
{
	if (bytes != MinTextureSize && bytes != MaxTextureSize)
		throw std::invalid_argument("Invalid PowerStorm texture memory size");
	if (m_texture.size() != bytes)
		m_texture.assign(bytes, 0);
}

// Device lifecycle and diagnostics

CRealImage2100::CRealImage2100()
	: m_vga_memory(VGAMemorySize), m_dac_regs(DACRegisterCount),
	  m_color(ColorPixels * 2), m_texture(MinTextureSize)
{
	dac_port_map(m_dac_ports);
	reset();
}

void CRealImage2100::reset(bool clear)
{
	// PCI configuration belongs to the board wrapper and is NOT reset here.
	if (clear)
	{
		std::fill(m_vga_memory.begin(), m_vga_memory.end(), uint8_t(0));
		std::fill(m_color.begin(), m_color.end(), uint32_t(0));
		std::fill(m_texture.begin(), m_texture.end(), uint8_t(0));
	}
	m_dma_regs.fill(0);
	m_planes = {};
	m_clear_cache = {};
	m_pending = {};
	m_palette.fill(0);
	std::fill(m_dac_regs.begin(), m_dac_regs.end(), uint8_t(0));
	m_shadow.clear();
	m_warned.clear();
	m_aperture_warned = m_shadow_full_warned = false;
	// Power-on values are undocumented; the card presents VGA before POST.
	m_unit_reset = m_interrupt_enable = m_display_control = 0;
	m_vga_control = VGAControlVGA;
	m_status_phase = 0;
	m_frame_counter = 0;
	m_board_control = m_board_timing = 0;
	m_board_io = 0;
	m_io_index = 0;
	m_dac_index = 0;
	m_dac_component = 0;
	m_palette_read = m_palette_write = 0;
}

void CRealImage2100::report(
	const char* code, uint32_t a, uint32_t v, const char* message, bool fatal)
{
	if (m_diagnostic)
		m_diagnostic(Diagnostic{code, message, a, v, fatal});
}

void CRealImage2100::unimplemented(
	const char* what, uint32_t a, uint32_t v, bool write)
{
	if (m_unimplemented)
		m_unimplemented(what, a, v, write);
}

void CRealImage2100::unimplemented_once(
	const char* what, uint32_t a, uint32_t v, bool write)
{
	if (m_warned.size() < MaxShadowRegisters && m_warned.insert(a & ~3u).second)
		unimplemented(what, a, v, write);
}

void CRealImage2100::write_ppm(std::ostream& out, const Frame& f)
{
	if (!f.width || !f.height || f.argb.size() != uint64_t(f.width) * f.height)
		throw std::runtime_error("Invalid PowerStorm frame");
	out << "P6\n" << f.width << ' ' << f.height << "\n255\n";
	for (const uint32_t c : f.argb)
	{
		out.put(char(c >> 16));
		out.put(char(c >> 8));
		out.put(char(c));
	}
	if (!out)
		throw std::runtime_error("PowerStorm frame write failed");
}

// Snapshots

static void put32(std::ostream& out, uint32_t v)
{
	for (unsigned i = 0; i < 4; ++i)
		out.put(char(v >> (8 * i)));
	if (!out)
		throw std::runtime_error("PowerStorm snapshot write failed");
}

static uint32_t get32(std::istream& in)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < 4; ++i)
	{
		const int b = in.get();
		if (b == std::char_traits<char>::eof())
			throw std::runtime_error("PowerStorm snapshot truncated");
		v |= uint32_t(uint8_t(b)) << (8 * i);
	}
	return v;
}

static uint32_t crc32(const std::string& bytes)
{
	uint32_t crc = 0xffffffff;
	for (const unsigned char byte : bytes)
	{
		crc ^= byte;
		for (unsigned i = 0; i < 8; ++i)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

// Selectors, latches, upload, palette/DAC, shadow, VGA and color banks.
static constexpr uint32_t FixedPayload = CRealImage2100::MinStateSize - 16;

void CRealImage2100::SaveState(std::ostream& out) const
{
	std::ostringstream p(std::ios::binary);
	put32(p, m_io_index);
	put32(p, m_dac_index);
	put32(p, m_palette_read);
	put32(p, m_palette_write);
	put32(p, m_unit_reset);
	put32(p, m_interrupt_enable);
	put32(p, m_vga_control);
	put32(p, m_display_control);
	put32(p, m_status_phase);
	put32(p, m_frame_counter);
	put32(p, m_board_control);
	put32(p, m_board_io);
	put32(p, m_board_timing);
	put32(p, m_dac_component);
	put32(p, m_pending.x);
	put32(p, m_pending.y);
	put32(p, m_pending.width);
	put32(p, m_pending.height);
	put32(p, m_pending.word);
	put32(p, m_pending.banks);
	put32(p, uint32_t(m_texture.size()));
	for (const uint32_t reg : m_dma_regs)
		put32(p, reg);
	for (const auto& plane : m_planes)
	{
		put32(p, plane.written);
		put32(p, plane.unknown_masks);
		for (const uint32_t reg : plane.regs)
			put32(p, reg);
	}
	for (const auto& cache : m_clear_cache)
	{
		put32(p, cache.source);
		put32(p, cache.color);
		put32(p, cache.known);
	}
	for (const uint32_t c : m_palette)
		put32(p, c);
	p.write(reinterpret_cast<const char*>(m_dac_regs.data()), DACRegisterCount);
	put32(p, uint32_t(m_shadow.size()));
	for (const auto& reg : m_shadow)
	{
		put32(p, reg.first);
		put32(p, reg.second);
	}
	p.write(reinterpret_cast<const char*>(m_vga_memory.data()), VGAMemorySize);
	for (const uint32_t c : m_color)
		put32(p, c);
	p.write(reinterpret_cast<const char*>(m_texture.data()), m_texture.size());
	const auto bytes = p.str();
	put32(out, 0x30324952); // RI20
	put32(out, 8);
	put32(out, uint32_t(bytes.size()));
	put32(out, crc32(bytes));
	out.write(bytes.data(), bytes.size());
	if (!out)
		throw std::runtime_error("PowerStorm snapshot write failed");
}

void CRealImage2100::RestoreState(std::istream& in)
{
	if (get32(in) != 0x30324952 || get32(in) != 8)
		throw std::runtime_error("Wrong REALimage snapshot version");
	const auto size = get32(in), crc = get32(in);
	if (size < FixedPayload || size > MaxStateSize - 16)
		throw std::runtime_error("Invalid REALimage snapshot length");
	std::string bytes(size, '\0');
	in.read(&bytes[0], size);
	if (!in || crc32(bytes) != crc)
		throw std::runtime_error("REALimage snapshot truncated/corrupt");
	std::istringstream p(bytes, std::ios::binary);
	const auto index = get32(p), dac = get32(p), rd = get32(p), wr = get32(p);
	if (dac > 65535 || rd > 255 || wr > 255)
		throw std::runtime_error("Invalid REALimage snapshot selectors");
	const auto unit_reset = get32(p), interrupt_enable = get32(p),
			   vga_control = get32(p), display_control = get32(p),
			   status_phase = get32(p), frame_counter = get32(p),
			   board_control = get32(p), board_io = get32(p),
			   board_timing = get32(p);
	if (status_phase >= StatusFrameReads || frame_counter > 0xffff ||
		board_control > 0xff || board_io > 0xffffff || board_timing > 0xff)
		throw std::runtime_error("Invalid REALimage status state");
	const uint32_t dac_component = get32(p);
	Pending pending;
	pending.x = get32(p);
	pending.y = get32(p);
	pending.width = get32(p);
	pending.height = get32(p);
	pending.word = get32(p);
	pending.banks = get32(p);
	const uint64_t words = uint64_t(pending.width) * pending.height;
	if (dac_component > 2 || pending.x > 65535 || pending.y > 65535 ||
		pending.width > 65536 || pending.height > 65536 ||
		(pending.width && (!pending.height || words > 0xffffffffu ||
			pending.word >= words || !pending.banks || (pending.banks & ~3u))) ||
		(!pending.width && (pending.x || pending.y || pending.height ||
			pending.word || pending.banks)))
		throw std::runtime_error("Invalid REALimage native transfer state");
	const uint32_t texture_size = get32(p);
	if (texture_size != MinTextureSize && texture_size != MaxTextureSize)
		throw std::runtime_error("Invalid REALimage texture memory size");
	std::array<uint32_t, DMARegisterCount> dma_regs{};
	for (uint32_t& reg : dma_regs)
		reg = get32(p);
	std::array<PlaneState, PlaneCount> planes{};
	for (auto& plane : planes)
	{
		plane.written = get32(p);
		plane.unknown_masks = get32(p);
		if ((plane.written & ~0x0fffcf7fu) || plane.unknown_masks > 15)
			throw std::runtime_error("Invalid REALimage plane register mask");
		for (unsigned i = 0; i < PlaneRegisterCount; ++i)
		{
			plane.regs[i] = get32(p);
			if (!(plane.written & (1u << i)) && plane.regs[i])
				throw std::runtime_error("Invalid REALimage plane register state");
		}
	}
	std::array<ClearCache, 2> clear_cache{};
	for (auto& cache : clear_cache)
	{
		cache.source = get32(p);
		cache.color = get32(p);
		cache.known = get32(p);
		if ((cache.source & ~0x07ff07ffu) || (cache.known & 0xff000000) ||
			(cache.color & ~cache.known) || (!cache.known && cache.source))
			throw std::runtime_error("Invalid REALimage clear cache");
	}
	std::array<uint32_t, 256> pal{};
	for (auto& c : pal)
	{
		c = get32(p);
		if (c & 0xff000000)
			throw std::runtime_error("Invalid palette encoding");
	}
	std::vector<uint8_t> dac_regs(DACRegisterCount);
	p.read(reinterpret_cast<char*>(dac_regs.data()), dac_regs.size());
	const uint32_t count = get32(p);
	if (count > MaxShadowRegisters ||
		FixedPayload + uint64_t(count) * 8 + texture_size - MinTextureSize != size)
		throw std::runtime_error("Invalid REALimage shadow register count");
	std::map<uint32_t, uint32_t> shadow;
	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t address = get32(p), value = get32(p);
		if ((address & 3) || !shadow.emplace(address, value).second)
			throw std::runtime_error("Invalid REALimage shadow register");
	}
	std::vector<uint8_t> memory(VGAMemorySize);
	p.read(reinterpret_cast<char*>(memory.data()), memory.size());
	std::vector<uint32_t> color(ColorPixels * 2);
	for (uint32_t& c : color)
	{
		c = get32(p);
		if (c & 0xff000000)
			throw std::runtime_error("Invalid REALimage color buffer");
	}
	std::vector<uint8_t> texture(texture_size);
	p.read(reinterpret_cast<char*>(texture.data()), texture.size());
	if (!p || p.peek() != std::char_traits<char>::eof())
		throw std::runtime_error("Invalid REALimage snapshot payload");
	// Commit only after validation; keep the storage CVGA borrows in place.
	std::copy(memory.begin(), memory.end(), m_vga_memory.begin());
	m_palette = pal;
	m_dac_regs.swap(dac_regs);
	m_shadow.swap(shadow);
	m_color.swap(color);
	m_texture.swap(texture);
	m_dma_regs = dma_regs;
	m_planes = planes;
	m_clear_cache = clear_cache;
	m_pending = pending;
	m_warned.clear();
	m_aperture_warned = m_shadow_full_warned = false;
	m_unit_reset = unit_reset;
	m_interrupt_enable = interrupt_enable;
	m_vga_control = vga_control;
	m_display_control = display_control;
	m_status_phase = status_phase;
	m_frame_counter = uint16_t(frame_counter);
	m_board_control = uint8_t(board_control);
	m_board_io = board_io;
	m_board_timing = uint8_t(board_timing);
	m_io_index = index;
	m_dac_index = uint16_t(dac);
	m_dac_component = uint8_t(dac_component);
	m_palette_read = uint8_t(rd);
	m_palette_write = uint8_t(wr);
}
