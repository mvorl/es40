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
			}));
	map(0x14, 0x14)
		.lrw8(
			NAME([this](offs_t) {
				return u8(m_dac_index >> 8);
			}),
			NAME([this](offs_t, u8 v) {
				m_dac_index =
					uint16_t((m_dac_index & 255) | (uint16_t(v) << 8));
			}));
	// The ROM proves only the transport; clock, mux and cursor semantics of the
	// individual indexed registers are not modeled.
	map(0x18, 0x18)
		.lrw8(
			NAME([this](offs_t) {
				return m_dac_regs[m_dac_index];
			}),
			NAME([this](offs_t, u8 v) {
				m_dac_regs[m_dac_index] = v;
			}));
}

// Nothing modeled depends on these yet; their values read back as written.
uint32_t* CRealImage2100::native_register(uint32_t a)
{
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

// No engine is modeled, so busy bits read 0. Retrace sits inside blank near
// the end of each virtual frame, so both set-then-clear waits finish.
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
	if (bits == 8 && a >= 0x838000 && a < 0x838020 &&
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
	if (bits == 8 && a >= 0x838000 && a < 0x838020 &&
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
		// Only the VGA side is displayed; say so when the driver leaves it.
		if (key == VGAControl && native_display())
			unimplemented("REALimage native display", key, *reg, true);
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
	unimplemented_once("REALimage register", a, v, true);
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
	if (!m_aperture_warned)
	{
		m_aperture_warned = true;
		unimplemented("REALimage BAR1 aperture", a, 0, false);
	}
	return width_mask(valid_width(bits) ? bits : 32);
}

void CRealImage2100::mem_write(uint32_t a, int bits, uint32_t v)
{
	(void)bits;
	if (!m_aperture_warned)
	{
		m_aperture_warned = true;
		unimplemented("REALimage BAR1 aperture", a, v, true);
	}
}

// Device lifecycle and diagnostics

CRealImage2100::CRealImage2100()
	: m_vga_memory(VGAMemorySize), m_dac_regs(DACRegisterCount)
{
	dac_port_map(m_dac_ports);
	reset();
}

void CRealImage2100::reset(bool clear)
{
	// PCI configuration belongs to the board wrapper and is NOT reset here.
	if (clear)
		std::fill(m_vga_memory.begin(), m_vga_memory.end(), uint8_t(0));
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

// Selectors, native registers, palette, DAC registers, shadow count, VGA memory.
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
	const auto bytes = p.str();
	put32(out, 0x30324952); // RI20
	put32(out, 4);
	put32(out, uint32_t(bytes.size()));
	put32(out, crc32(bytes));
	out.write(bytes.data(), bytes.size());
	if (!out)
		throw std::runtime_error("PowerStorm snapshot write failed");
}

void CRealImage2100::RestoreState(std::istream& in)
{
	if (get32(in) != 0x30324952 || get32(in) != 4)
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
		FixedPayload + uint64_t(count) * 8 != size)
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
	if (!p || p.peek() != std::char_traits<char>::eof())
		throw std::runtime_error("Invalid REALimage snapshot payload");
	// Commit only after validation; keep the storage CVGA borrows in place.
	std::copy(memory.begin(), memory.end(), m_vga_memory.begin());
	m_palette = pal;
	m_dac_regs.swap(dac_regs);
	m_shadow.swap(shadow);
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
	m_palette_read = uint8_t(rd);
	m_palette_write = uint8_t(wr);
}
