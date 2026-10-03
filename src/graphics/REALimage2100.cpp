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
#include <cmath>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>

// Finite repeat coordinates only need the fractional IEEE-754 bits.
static double realimage_repeat_fraction(double value)
{
	if constexpr (sizeof(double) != sizeof(uint64_t) ||
		!std::numeric_limits<double>::is_iec559 ||
		std::numeric_limits<double>::digits != 53 ||
		std::numeric_limits<double>::max_exponent != 1024)
		return std::fmod(value, 1.0);
	uint64_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	const unsigned exponent = unsigned((bits >> 52) & 0x7ff);
	if (exponent < 1023)
		return value;
	if (exponent >= 1075)
		return 0;
	bits &= ~((uint64_t(1) << (1075 - exponent)) - 1);
	double integral;
	std::memcpy(&integral, &bits, sizeof(integral));
	return value - integral;
}

#if (defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)) && !defined(REALIMAGE_SCALAR_ONLY)
#define REALIMAGE_SSE2 1
#include <emmintrin.h>
static __m128i realimage_texture2(__m128d s, __m128d t, const uint8_t* texture,
	uint32_t base, uint32_t width, uint32_t height, unsigned row_shift)
{
	auto repeat = [](__m128d q) {
		return _mm_set_pd(realimage_repeat_fraction(_mm_cvtsd_f64(_mm_unpackhi_pd(q, q))),
			realimage_repeat_fraction(_mm_cvtsd_f64(q)));
	};
	const __m128d u = _mm_sub_pd(_mm_mul_pd(repeat(s), _mm_set1_pd(width)), _mm_set1_pd(0.5)),
		v = _mm_sub_pd(_mm_mul_pd(repeat(t), _mm_set1_pd(height)), _mm_set1_pd(0.5));
	auto floor_int = [](__m128d q) {
		const __m128i truncated = _mm_cvttpd_epi32(q);
		const __m128i below = _mm_castpd_si128(_mm_cmplt_pd(q, _mm_cvtepi32_pd(truncated)));
		return _mm_sub_epi32(truncated, _mm_and_si128(
			_mm_shuffle_epi32(below, _MM_SHUFFLE(3, 1, 2, 0)), _mm_set1_epi32(1)));
	};
	const __m128i x = floor_int(u), y = floor_int(v);
	const __m128d fx = _mm_sub_pd(u, _mm_cvtepi32_pd(x)), fy = _mm_sub_pd(v, _mm_cvtepi32_pd(y));
	const __m128i xmask = _mm_set1_epi32(width - 1), ymask = _mm_set1_epi32(height - 1);
	const __m128i x0 = _mm_slli_epi32(_mm_and_si128(x, xmask), 1),
		x1 = _mm_slli_epi32(_mm_and_si128(_mm_add_epi32(x, _mm_set1_epi32(1)), xmask), 1),
		y0 = _mm_sll_epi32(_mm_and_si128(y, ymask), _mm_cvtsi32_si128(row_shift)),
		y1 = _mm_sll_epi32(_mm_and_si128(_mm_add_epi32(y, _mm_set1_epi32(1)), ymask), _mm_cvtsi32_si128(row_shift));
	auto texel = [&](__m128i xx, __m128i yy) {
		const __m128i at = _mm_add_epi32(_mm_set1_epi32(base), _mm_add_epi32(xx, yy));
		const unsigned a = unsigned(_mm_cvtsi128_si32(at)), b = unsigned(_mm_cvtsi128_si32(_mm_srli_si128(at, 4)));
		uint16_t first, second;
		std::memcpy(&first, texture + a, 2);
		std::memcpy(&second, texture + b, 2);
		return _mm_set_epi32(0, 0, second, first);
	};
	const __m128i p00 = texel(x0, y0), p10 = texel(x1, y0), p01 = texel(x0, y1), p11 = texel(x1, y1);
	auto channel = [&](unsigned shift, uint32_t mask) {
		const __m128i shifts = _mm_cvtsi32_si128(shift), masks = _mm_set1_epi32(mask);
		auto expand = [&](const __m128i p) {
			return _mm_cvtepi32_pd(_mm_and_si128(_mm_srl_epi32(p, shifts), masks));
		};
		const __m128d a = expand(p00), b = expand(p10), c = expand(p01), d = expand(p11);
		const __m128d top = _mm_add_pd(a, _mm_mul_pd(fx, _mm_sub_pd(b, a))),
			bottom = _mm_add_pd(c, _mm_mul_pd(fx, _mm_sub_pd(d, c)));
		const __m128d color = _mm_div_pd(_mm_mul_pd(_mm_add_pd(top,
			_mm_mul_pd(fy, _mm_sub_pd(bottom, top))), _mm_set1_pd(255)), _mm_set1_pd(mask));
		return _mm_cvttpd_epi32(_mm_max_pd(_mm_setzero_pd(), _mm_min_pd(_mm_set1_pd(255), color)));
	};
	return _mm_or_si128(_mm_slli_epi32(channel(11, 31), 16),
		_mm_or_si128(_mm_slli_epi32(channel(5, 63), 8), channel(0, 31)));
}
#endif

static bool valid_width(int bits)
{
	return bits == 8 || bits == 16 || bits == 32;
}

static uint32_t width_mask(int bits)
{
	return bits == 32 ? 0xffffffffu : (1u << bits) - 1u;
}

// Bulk context templates write zero to these otherwise undecoded slots.
static bool zero_context_register(uint32_t a)
{
	return a == 0x008005cc || a == 0x008005d4 || a == 0x008005dc;
}

static bool flat_vertex_register(uint32_t a)
{
	return a >= CRealImage2100::FlatVertexBase &&
		a < CRealImage2100::FlatVertexBase + 3 * CRealImage2100::VertexStride &&
		(a - CRealImage2100::FlatVertexBase) % CRealImage2100::VertexStride < 0x34;
}

static bool vertex_register(uint32_t a)
{
	return flat_vertex_register(a) ||
		(a >= CRealImage2100::VertexTextureBase &&
		 a < CRealImage2100::VertexTextureBase + 3 * CRealImage2100::VertexStride &&
		 (a - CRealImage2100::VertexTextureBase) % CRealImage2100::VertexStride < 0x34);
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
	if (flat_vertex_register(a & ~3u))
		a -= 0x100;
	uint32_t pixel = 0;
	if (framebuffer_access(a, bits, pixel, false))
		return pixel;
	if (a >= 0x838000 && a < 0x838020 &&
		m_dac_ports.has_handler(a - 0x838000))
		return m_dac_ports.read_byte(a - 0x838000);
	if (const uint32_t* reg = native_register(a & ~3u))
		return (*reg >> ((a & 3) * 8)) & width_mask(bits);
	if ((a & ~3u) == Status)
		return (status_read() >> ((a & 3) * 8)) & width_mask(bits);
	if ((a & ~3u) == BoardStatus)
	{
		const uint32_t v = m_frame_counter | (uint32_t(m_board_straps) << 16) |
			(uint32_t(m_board_control) << 24);
		return (v >> ((a & 3) * 8)) & width_mask(bits);
	}
	if ((a & ~3u) == BoardIO)
	{
		const uint32_t v = m_board_io | (uint32_t(BoardIDPCGA3) << 24);
		return (v >> ((a & 3) * 8)) & width_mask(bits);
	}
	if (m_readback.width && a >= HostData && a < HostData + HostReadSize)
	{
		if (bits == 32)
			return host_read();
		unimplemented_once("REALimage host readback", a, 0, false);
		return 0;
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
	const uint32_t original_address = a;
	if (flat_vertex_register(a & ~3u))
	{
		if (bits != 32)
		{
			unimplemented_once("REALimage vertex width (command rejected)", a, v, true);
			return;
		}
		// Flat and smooth ports address the same vertex slots.
		a -= 0x100;
	}
	if (framebuffer_access(a, bits, v, true))
		return;
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
		{
			m_pending = {};
			m_readback = {};
		}
		if (key == DMAReset && (*reg & 1))
			m_dma_regs[(DMACommand - DMABase) / 4] = 0;
		if (key == DMACommand && (lanes & 0xc0000000) &&
			(*reg & 0xc0000000))
		{
			if (m_dma_regs[(DMAReset - DMABase) / 4] & 1)
				*reg = 0;
			else if (bits == 32)
				dma_command(*reg);
			else
				unimplemented(
					"REALimage DMA transfer (rejected; completion not written)",
					key, *reg, true);
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
	// Configuration accesses must not consume a pending pixel upload.
	if (a >= HostData && (a < HostData + HostDataSize ||
		(m_pending.width && !(peek(DrawControl) & 0x04000000u) &&
			a < HostData + HostUploadSize)))
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
	const uint32_t old_value = it->second;
	it->second = (old_value & ~lanes) | ((v << shift) & lanes);
	if ((key == DrawControl && ((old_value ^ it->second) & 0x00000f01u)) ||
		(key == MemoryControl && old_value != it->second))
		m_clear_cache = {};
	if (plane_write(key, lanes, (v << shift) & lanes))
		return;
	if (vertex_register(key))
	{
		if (bits != 32)
			unimplemented_once("REALimage vertex width (command rejected)", a, v, true);
		else if ((key - VertexBase) % VertexStride == 0x1c)
			triangle_command(original_address, it->second);
	}
	else if (key == HostCommand || key == FillCommand || key == BlockCommand)
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
			unimplemented_once("REALimage command width (command rejected)", a, v, true);
	}
	else if (key == SyncCommand)
	{
		// Idle startup synchronization does not cancel an unfinished upload.
		if (bits != 32 || (it->second && it->second != 0x80000000u) ||
			m_pending.width || m_readback.width)
			unimplemented_once("REALimage synchronization command (command rejected)", a, v, true);
	}
	else if (key == DisplaySelect)
	{
		if (it->second)
			unimplemented_once("REALimage display selector", a, v, true);
	}
	else if (zero_context_register(key))
	{
		if (it->second)
			unimplemented_once("REALimage context register/profile", a, v, true);
	}
	else if (!native_storage_register(key))
	{
		// Unmodeled mask aliases must not leave a stale block mask usable.
		if ((key & ~0x1fe000u) == HostData + 0x400 &&
			(peek(DrawControl) & (1u << 26)))
			for (unsigned bank = 0; bank < PlaneCount; ++bank)
				if (peek(DrawControl) & (0x1000u << bank))
					m_planes[bank].unknown_masks |= (key >> 13) &
						(m_color_width == MaxColorWidth ? 255u : 15u);
		unimplemented_once("REALimage register", a, v, true);
	}
}

// Native drawing profiles decoded from the NT drivers and traces.

uint32_t CRealImage2100::peek(uint32_t a) const
{
	const auto it = m_shadow.find(a & ~3u);
	return it == m_shadow.end() ? 0 : it->second;
}

int CRealImage2100::plane_register(uint32_t a)
{
	if (a >= PlaneClearColor && a < PlaneClearColor + 32)
		return 16 + int((a - PlaneClearColor) / 4);
	if (a == PlanePixelMask ||
		((a & ~0x1fe000u) == HostData + 0x400 && (a & 0x1fe000u)))
		return 24;
	// DFE7xx and FFE7xx are the two observed full-selector state forms.
	const uint32_t state_address = a | 0x00200000u;
	switch (state_address)
	{
	case PlaneStateBase + 0x00: case PlaneStateBase + 0x04:
	case PlaneStateBase + 0x08: case PlaneStateBase + 0x0c:
	case PlaneStateBase + 0x10: case PlaneStateBase + 0x14:
	case PlaneStateBase + 0x18: case PlaneStateBase + 0x1c:
	case PlaneStateBase + 0x20:
	case PlaneStateBase + 0x24: case PlaneStateBase + 0x28:
	case PlaneStateBase + 0x2c: case PlaneStateBase + 0x38:
	case PlaneStateBase + 0x3c:
		return int((state_address - PlaneStateBase) / 4);
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
	const uint32_t available = m_color_width == MaxColorWidth ? 255u : 15u,
		groups = a == PlanePixelMask ? available : (a >> 13) & 255;
	if (index == 24 && (groups & ~available))
	{
		for (unsigned bank = 0; bank < PlaneCount; ++bank)
			if (banks & (1u << bank))
				m_planes[bank].unknown_masks |= groups & available;
		unimplemented_once("REALimage plane mask", a, value, true);
		return true;
	}
	// Configuration writes broadcast to the selected 3D-RAM planes.
	for (unsigned bank = 0; bank < PlaneCount; ++bank)
		if (banks & (1u << bank))
		{
			auto& plane = m_planes[bank];
			if (index == 24 && lanes == 0xffffffffu)
				plane.unknown_masks &= ~groups;
			const unsigned end = index == 24 ? PlaneRegisterCount : index + 1;
			for (unsigned i = unsigned(index); i < end; ++i)
			{
				if (index == 24 && !(groups & (1u << (i - 24))))
					continue;
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

bool CRealImage2100::plane_profile(unsigned bank, uint32_t format, uint32_t rop_high) const
{
	// Callers select the supported operation; comparisons remain guarded.
	const uint32_t compare_mask = plane_value(bank, 10, 0);
	return plane_value(bank, 1, 0) == 0 && plane_value(bank, 2, 0) == 0 &&
		plane_value(bank, 3, 0) == 0 &&
		(plane_value(bank, 4, 0x03030303) & 0xf0f0f0f0) == rop_high &&
		plane_value(bank, 5, 0x0a000000) == 0x0a000000 &&
		plane_value(bank, 6, 0) == 0 && plane_value(bank, 7, 0) == 0 &&
		plane_value(bank, 8, 0) == 0 &&
		plane_value(bank, 9, 0) == 0 &&
		(compare_mask == 0 || compare_mask == 0x00ff0000) &&
		plane_value(bank, 11, 0x33300000) == 0x33300000 &&
		plane_value(bank, 14, 0x100) == format &&
		plane_value(bank, 15, 0) == 0;
}

bool CRealImage2100::native_storage_register(uint32_t a) const
{
	return vertex_register(a) || a == TextureBase || a == ContextLink ||
		a == DrawControl || a == MemoryControl || a == PixelControl ||
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
		a == PipelineControl4 || a == PipelineControl5 ||
		zero_context_register(a) ||
		(a >= TimingBase && a <= TimingBase + 0x1c);
}

bool CRealImage2100::native_pixel_profile() const
{
	const uint32_t memory = peek(MemoryControl);
	return (memory & 0x00ffffff) == 0x8000 &&
		peek(PixelControl) == (m_color_width == MaxColorWidth ? 0x62722060u : 0x42722060u);
}

uint32_t CRealImage2100::block_width() const
{
	const uint32_t format = peek(DrawControl) & 255;
	return format == 2 ? 8 : format == 3 && m_color_width == MaxColorWidth ? 16 : 0;
}

bool CRealImage2100::native_copy_control_profile(bool clear) const
{
	const uint32_t width = block_width(), columns = (peek(DrawControl) >> 8) & 15,
		copy_columns = ((peek(MemoryControl) >> 24) & 63) + 1;
	// Each page column spans two copy blocks; the top two bits control timing.
	if (!native_pixel_profile() || !width || columns != (copy_columns + 1) / 2 ||
		copy_columns > (m_color_width + 10 * width - 1) / (10 * width))
		return false;
	const uint32_t global = peek(GlobalControl0);
	const bool context_clear = clear && (global == 0x180 || global == 0x190) &&
		peek(GlobalControl1) == 0x20800;
	// Other programmed pipeline modes have not been decoded.
	const std::pair<uint32_t, uint32_t> profile[] = {
		{GlobalControl0, context_clear ? global : 1u},
		{GlobalControl1, context_clear ? 0x20800u : 0x20811u}, {GlobalControl2, 0x33},
		{PipelineControl0, 0}, {PipelineControl1, 0},
		{PipelineControl2, 0x10000000}, {PipelineControl3, 0},
		{PipelineControl4, 0}, {PipelineControl5, 0},
		{0x008005cc, 0}, {0x008005d4, 0}, {0x008005dc, 0}};
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
	return (control & ~0xff01u) == 0x81000002 && banks && !(banks & ~3u) &&
		native_copy_control_profile();
}

bool CRealImage2100::fast_copy_profile() const
{
	const uint32_t control = peek(DrawControl), banks = (control >> 12) & 15;
	if ((control & ~0xff01u) != 0x21000002 || (banks != 1 && banks != 2) ||
		!native_copy_control_profile())
		return false;
	const unsigned bank = banks == 2 ? 1 : 0;
	if ((plane_value(bank, 0, 0xffffffff) & 0xffffff) != 0xffffff ||
		plane_value(bank, 1, 0) != 0 || plane_value(bank, 2, 0) != 0 ||
		plane_value(bank, 3, 0) != 0 ||
		plane_value(bank, 4, 0x03030303) != 0x05050505 ||
		plane_value(bank, 5, 0x0a000000) != 0 || plane_value(bank, 6, 0) != 1 ||
		plane_value(bank, 7, 0) != 0 ||
		plane_value(bank, 8, 0) != 0 || plane_value(bank, 9, 0) != 0 ||
		plane_value(bank, 10, 0) != 0 ||
		plane_value(bank, 11, 0x33300000) != 0x33300000 ||
		plane_value(bank, 14, 0x100) != 0x100 || plane_value(bank, 15, 0) != 0 ||
		m_planes[bank].unknown_masks)
		return false;
	for (unsigned i = 24; i < 24 + block_width() / 2; ++i)
		if (plane_value(bank, i, 0xffffffff) != 0xffffffff)
			return false;
	return true;
}

void CRealImage2100::color_write(
	uint32_t x, uint32_t y, uint32_t color, uint32_t banks, uint32_t lanes)
{
	if (x >= m_color_width || y >= m_color_height)
		return;
	const uint32_t offset = y * m_color_width + x;
	for (uint32_t bank = 0; bank < 2; ++bank)
		if (banks & (1u << bank))
		{
			uint32_t& destination = m_color[bank * m_color_pixels + offset];
			const uint32_t mask = plane_value(bank, 0, 0xffffffff) & lanes & 0xffffff,
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

bool CRealImage2100::framebuffer_access(
	uint32_t a, int bits, uint32_t& value, bool write)
{
	if (a < 0x01000000 || a >= 0x03000000)
		return false;
	// Fixed front/back windows use an 8 KiB row pitch.
	const uint32_t bank = (a >> 24) - 1, x = (a & 0x1fff) >> 2,
		y = (a & 0x00ffffff) >> 13, shift = (a & 3) * 8;
	if (!copy_profile() || !plane_profile(bank) ||
		x >= m_color_width || y >= m_color_height)
	{
		unimplemented_once("REALimage framebuffer aperture/profile", a, value, write);
		if (!write)
			value = width_mask(bits);
		return true;
	}
	if (write)
	{
		m_clear_cache[bank] = {};
		color_write(x, y, value << shift, 1u << bank, width_mask(bits) << shift);
	}
	else
		value = (m_color[size_t(bank) * m_color_pixels + y * m_color_width + x] >> shift) &
			width_mask(bits);
	return true;
}

bool CRealImage2100::fill_profile() const
{
	const uint32_t control = peek(DrawControl), banks = (control >> 12) & 15;
	// This clear path retains the context's WID and configuration selector.
	return (control & ~0x040fff01u) == 0x81000002 && banks && !(banks & ~3u) &&
		native_copy_control_profile(true);
}

void CRealImage2100::block_command(uint32_t v)
{
	const uint32_t control = peek(DrawControl), banks = (control >> 12) & 15,
		source = peek(BlockSource), destination = peek(BlockDestination),
		extent = peek(BlockExtent), auxiliary_mask = plane_value(2, 0, 0xffffffff),
		auxiliary_value = plane_value(2, 16, 0);
	const bool copy = (v & 0x20000) != 0, configuration = (control & 0x04000000) != 0;
	const bool auxiliary_clear = !copy && (banks & 4) && auxiliary_mask;
	const bool seed = !configuration && !copy;
	const uint32_t clear_width = block_width(), groups = clear_width / 2,
		dx = destination & 0x7ff, dy = destination >> 16,
		width = (extent & 0x7ff) + 1, height = (extent >> 16) + 1,
		scale_x = copy ? 10 * clear_width : clear_width, scale_y = copy ? 16 : 4,
		x = dx * scale_x, y = dy * scale_y,
		right = x + width * scale_x, bottom = y + height * scale_y;
	bool geometry_known = false;
	auto invalidate_cache = [&]() {
		for (unsigned bank = 0; bank < 3; ++bank)
			if (banks & (1u << bank))
			{
				auto& cache = m_clear_cache[bank];
				const uint32_t cx = (cache.source & 0xffff) * 10 * clear_width,
					cy = (cache.source >> 16) * 16;
				if (!geometry_known ||
					(x < cx + 21 * clear_width && right > cx && y < cy + 36 && bottom > cy))
					cache = {};
			}
	};
	auto reject = [&]() {
		invalidate_cache();
		unimplemented_once(
			"REALimage block command/profile (command rejected)", BlockCommand, v, true);
	};
	const uint32_t control_fields = 0x040fff01u;
	if ((control & ~control_fields) != 0x81000002 || !banks || (banks & ~7u) ||
		v != (((banks ^ 7u) << 18) | (copy ? 0x30000u : 0x10000u)) ||
		!native_copy_control_profile(true) || m_pending.width || m_readback.width ||
		((source | destination | extent) & ~0x07ff07ffu) ||
		(!copy && source) || (copy && !configuration) ||
		((banks & 4) && auxiliary_mask && !auxiliary_clear))
	{
		reject();
		return;
	}
	geometry_known = true;
	// The driver seeds 21x9 small blocks for its repeated 2x2-tile source.
	if (seed && (extent != 0x00080014 || dx % 10 || dy % 4 ||
		(dx * clear_width < m_color_width && dy * 4 < m_color_height)))
	{
		reject();
		return;
	}
	if (auxiliary_clear)
	{
		const uint32_t depth_control = plane_value(2, 5, 0);
		const bool neutral_clear = plane_profile(2, 0) && plane_value(2, 10, 0) == 0 &&
			(auxiliary_mask == 0xffffffffu ||
				(configuration && !(auxiliary_mask & ~0x0000f000u)));
		// Block clears bypass the retained depth comparison.
		const bool packed_clear = auxiliary_mask == 0xffffffffu &&
			plane_value(2, 1, 0) == 0 && plane_value(2, 2, 0) == 0xf000 &&
			plane_value(2, 3, 0) == 0x0fff0fff &&
			(depth_control == 0x0a000200 || depth_control == 0x0a000205 ||
				depth_control == 0x0a000207) &&
			plane_value(2, 6, 0) == 0 && plane_value(2, 7, 0) == 0 &&
			plane_value(2, 8, 0) == 0 && plane_value(2, 9, 0) == 0 &&
			plane_value(2, 10, 0) == 0x00ff0000 &&
			plane_value(2, 11, 0) == 0x33300000 &&
			plane_value(2, 14, 0) == 0 && plane_value(2, 15, 0) == 0;
		if ((!neutral_clear && !packed_clear) ||
			plane_value(2, 4, 0x03030303) != 0x03030303 ||
			m_planes[2].unknown_masks ||
			(m_planes[2].written & 0x00ff0000) != 0x00ff0000)
		{
			reject();
			return;
		}
		for (unsigned i = 17; i < 24; ++i)
			if ((plane_value(2, i, 0) ^ auxiliary_value) & auxiliary_mask)
			{
				reject();
				return;
			}
		for (unsigned i = 24; i < 24 + groups; ++i)
		{
			const uint32_t bits = plane_value(2, i, 0xffffffff);
			if (bits != (bits & 255) * 0x01010101u)
			{
				reject();
				return;
			}
		}
	}
	std::array<uint32_t, 2> colors{};
	std::array<bool, 2> replace_color{};
	for (unsigned bank = 0; bank < 2; ++bank)
	{
		const uint32_t mask = plane_value(bank, 0, 0xffffffff) & 0xffffff,
			rops = plane_value(bank, 4, 0x03030303);
		if (!(banks & (1u << bank)) || !mask)
			continue;
		const bool blend = rops == 0xd0d0d0d0;
		replace_color[bank] = blend || (!copy && (rops & 0xffffff) == 0x060606);
		if (!plane_profile(bank, 0x100, blend ? rops : 0) ||
			m_planes[bank].unknown_masks)
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
		// Block clears bypass retained XOR and blend operations.
		for (unsigned shift = 0; shift < 24; shift += 8)
			if (!seed && !replace_color[bank] &&
				(mask & (0xffu << shift)) && ((rops >> shift) & 15) != 3 &&
				(((rops >> shift) & 15) != 0 ||
					(colors[bank] & mask & (0xffu << shift))))
			{
				reject();
				return;
			}
		for (unsigned i = 24; i < 24 + groups; ++i)
		{
			const uint32_t bits = plane_value(bank, i, 0xffffffff);
			if (bits != (bits & 255) * 0x01010101u)
			{
				reject();
				return;
			}
		}
	}
	if (seed)
	{
		// Offscreen seeds bypass retained configuration pixel masks.
		const uint32_t key = ((dy / 4) << 16) | (dx / 10);
		if (auxiliary_clear)
			m_clear_cache[2] = {key, auxiliary_value & auxiliary_mask, auxiliary_mask};
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
	for (uint32_t row = y; row < std::min(bottom, m_color_height); ++row)
		for (uint32_t col = x; col < std::min(right, m_color_width); ++col)
		{
			if (auxiliary_clear)
			{
				const uint32_t bits = plane_value(2, 24 + col % groups, 0xffffffff),
					bit = 2 * (row & 3) + ((col / groups) & 1);
				if (bits & (1u << bit))
				{
					auto& pixel = m_auxiliary[size_t(row) * m_color_width + col];
					pixel = (pixel & ~auxiliary_mask) | (auxiliary_value & auxiliary_mask);
				}
			}
			for (unsigned bank = 0; bank < 2; ++bank)
				if (banks & (1u << bank))
				{
					const uint32_t bits = plane_value(bank, 24 + col % groups, 0xffffffff),
						bit = 2 * (row & 3) + ((col / groups) & 1);
					if (bits & (1u << bit))
					{
						if (replace_color[bank])
						{
							const uint32_t mask = plane_value(bank, 0, 0xffffffff) & 0xffffff;
							auto& pixel = m_color[size_t(bank) * m_color_pixels +
								size_t(row) * m_color_width + col];
							pixel = (pixel & ~mask) | (colors[bank] & mask);
						}
						else
							color_write(col, row, colors[bank], 1u << bank);
					}
				}
		}
}

void CRealImage2100::start_command(uint32_t a, uint32_t v)
{
	const bool readback = a == HostCommand && v == 0x01000052;
	const bool cross_copy = a == HostCommand &&
		(v == 0x01008072 || v == 0x00008062 || v == 0x00008072 || v == 0x01008062);
	const uint32_t selected = (peek(DrawControl) >> 12) & 3;
	// Cross-bank copies read the selected bank and write the opposite bank.
	const uint32_t destination_banks = cross_copy ?
		((selected >> 1) | (selected << 1)) & 3 : selected;
	if (!readback)
		for (unsigned bank = 0; bank < 2; ++bank)
			if (destination_banks & (1u << bank))
				m_clear_cache[bank] = {};
	// A new launch cannot inherit the tail of an earlier host upload.
	if (m_pending.width)
		report("HOST_INTERRUPTED", a, v, "Incomplete native host upload replaced");
	if (m_readback.width)
		report("READBACK_INTERRUPTED", a, v, "Incomplete native host readback replaced");
	m_pending = {};
	m_readback = {};
	if (!v)
		return;
	const bool upload = a == HostCommand && v == 0x01000032;
	const bool fast_copy = a == HostCommand &&
		(v == 0x00200062 || v == 0x00200072 || v == 0x01200062 || v == 0x01200072);
	const bool copy = (a == HostCommand &&
		(v == 0x01000062 || v == 0x00000062 || v == 0x01000072 || v == 0x00000072)) ||
		fast_copy || cross_copy;
	const bool fill = a == FillCommand && v == 0x09000832;
	const bool host_mono = a == HostCommand && (v & ~0x7780u) == 0x01000872;
	const bool transparent = host_mono && !(v & 0x80);
	const uint32_t mono_offset = host_mono ? (v >> 8) & 7 : 0;
	// Base glyph commands also carry widths without encoding them in the command.
	const uint32_t mono_width = host_mono && (v & 0x7700) ? ((v >> 12) & 7) + 1 : 0;
	const bool mono = host_mono || (a == FillCommand && v == 0x010008f2);
	const bool profile = fast_copy ? fast_copy_profile() :
		(fill ? fill_profile() : copy_profile()) && (!(selected & 1) || plane_profile(0)) &&
		(!(selected & 2) || plane_profile(1)) &&
		(!cross_copy || plane_profile(selected == 2 ? 0 : 1));
	if ((!upload && !copy && !fill && !mono && !readback) || !profile ||
		((copy || readback) && selected != 1 && selected != 2))
	{
		unimplemented_once("REALimage 2D command/profile (command rejected)", a, v, true);
		return;
	}
	const uint32_t origin = peek(a == HostCommand ? HostOrigin : FillOrigin),
		extent = peek(a == HostCommand ? HostExtent : FillExtent),
		width = (extent & 0xffff) + 1, height = (extent >> 16) + 1,
		banks = (peek(DrawControl) >> 12) & 3;
	const int32_t x = int16_t(origin & 0xffff), y = int16_t(origin >> 16);
	if (readback)
	{
		const uint32_t source = peek(BlockSource), sx = source & 0xffff,
			sy = source >> 16;
		if (sx >= m_color_width || sy >= m_color_height ||
			width > m_color_width - sx || height > m_color_height - sy)
		{
			unimplemented_once("REALimage readback source bounds (command rejected)", a, v, true);
			return;
		}
		m_readback = {sx, sy, width, height, 0, selected};
		return;
	}
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
	if (copy)
	{
		const uint32_t source = peek(BlockSource), source_bank = selected == 2 ? 1 : 0,
			bank = cross_copy ? source_bank ^ 1u : source_bank;
		const bool right_to_left = !(v & 0x01000000), bottom_to_top = !(v & 0x10);
		if (fast_copy && ((source ^ origin) & (block_width() / 2 - 1)))
		{
			unimplemented_once("REALimage fast-copy alignment (command rejected)", a, v, true);
			return;
		}
		const int32_t sx = int16_t(source & 0xffff), sy = int16_t(source >> 16),
			left = std::max(right_to_left ? x - int32_t(width) + 1 : x, int32_t(0)),
			right = std::min(x + (right_to_left ? 1 : int32_t(width)), int32_t(m_color_width)),
			top = std::max(bottom_to_top ? y - int32_t(height) + 1 : y, int32_t(0)),
			bottom = std::min(y + (bottom_to_top ? 0 : int32_t(height) - 1),
				int32_t(m_color_height) - 1);
		if (left >= right || top > bottom)
			return;
		if (sx + left - x < 0 || sx + right - x > int32_t(m_color_width) ||
			sy + top - y < 0 || sy + bottom - y >= int32_t(m_color_height))
		{
			unimplemented_once("REALimage copy source bounds (command rejected)", a, v, true);
			return;
		}
		// Bits 24 and 4 select increasing X and Y.
		const int32_t first = right_to_left ? right - 1 : left,
			end = right_to_left ? left - 1 : right, step = right_to_left ? -1 : 1,
			first_row = bottom_to_top ? bottom : top,
			end_row = bottom_to_top ? top - 1 : bottom + 1,
			row_step = bottom_to_top ? -1 : 1;
		for (int32_t row = first_row; row != end_row; row += row_step)
			for (int32_t col = first; col != end; col += step)
			{
				const size_t offset = size_t(source_bank) * m_color_pixels +
					size_t(sy + row - y) * m_color_width + size_t(sx + col - x);
				if (fast_copy)
					// The optimized RAM-copy profile bypasses its programmed ROP 5.
					m_color[size_t(bank) * m_color_pixels + size_t(row) * m_color_width + col] =
						m_color[offset] & 0xffffff;
				else
					color_write(uint32_t(col), uint32_t(row), m_color[offset], destination_banks);
			}
		return;
	}
	const uint32_t pattern[] = {peek(MonoPattern0), peek(MonoPattern1),
		peek(MonoPattern2), peek(MonoPattern3)};
	if (mono && ((mono_width && width != mono_width) ||
		(a == HostCommand && (width + mono_offset > 8 || height > 16)) ||
		(a == FillCommand && (pattern[0] != pattern[2] || pattern[1] != pattern[3]))))
	{
		unimplemented_once("REALimage monochrome layout (command rejected)", a, v, true);
		return;
	}
	// Clip before iterating so malformed extents cannot cause unbounded work.
	const int32_t left = std::max(x, int32_t(0)),
		top = std::max(y, int32_t(0)),
		right = std::min(x + int32_t(width), int32_t(m_color_width)),
		bottom = std::min(y + int32_t(height), int32_t(m_color_height));
	const uint32_t foreground = peek(Foreground), background = peek(Background);
	for (int32_t row = top; row < bottom; ++row)
		for (int32_t col = left; col < right; ++col)
		{
			uint32_t color = foreground;
			if (mono)
			{
				// Local origin, MSB first; brush fills duplicate their eight rows.
				const uint32_t px = (uint32_t(col - x) + mono_offset) & 7,
					py = uint32_t(row - y) & 15;
				if (!(pattern[3 - py / 4] & (1u << (31 - 8 * (py & 3) - px))))
				{
					if (transparent)
						continue;
					color = background;
				}
			}
			color_write(uint32_t(col), uint32_t(row), color, banks);
		}
}

bool CRealImage2100::triangle_profile() const
{
	const uint32_t pipeline = peek(PipelineControl0);
	const bool textured = (pipeline & 0x80000000u) != 0,
		flat = pipeline == 0x0a4c2770, no_depth = textured || flat,
		blend = peek(PipelineControl2) == 0x20000080;
	if ((pipeline != 0x05008001 && pipeline != 0x8a4c2660 &&
		pipeline != 0x8a4c2770 && pipeline != 0x8a4c2880 && !flat) || (blend && !flat))
		return false;
	const uint32_t control = peek(DrawControl), banks = (control >> 12) & 15,
		bank = (banks & 3) == 2 ? 1 : 0,
		global = peek(GlobalControl0),
		width = block_width(), columns = (control >> 8) & 15,
		copy_columns = ((peek(MemoryControl) >> 24) & 63) + 1;
	if ((control & ~0x000fff01u) != 0x81800002 || (banks != 5 && banks != 6) ||
		(global != 0x180 && global != 0x190) ||
		!native_pixel_profile() || !width || columns != (copy_columns + 1) / 2 ||
		copy_columns > (m_color_width + 10 * width - 1) / (10 * width) ||
		m_pending.width || m_readback.width)
		return false;
	if (no_depth && global != 0x180)
		return false;
	if (textured)
	{
		const uint32_t base = peek(TextureBase), texture_width = 1u << ((pipeline >> 8) & 15),
			texture_height = 1u << ((pipeline >> 4) & 15),
			row_bytes = m_texture.size() == MaxTextureSize ? 0x4000 : 0x2000;
		if ((base & 1) || (base & 0x3fff) + texture_width * 2 > row_bytes ||
			uint64_t(base) + (texture_height - 1) * 0x4000 + texture_width * 2 > MaxTextureSize)
			return false;
	}
	const std::pair<uint32_t, uint32_t> profile[] = {
		{GlobalControl1, 0x20800}, {GlobalControl2, 0x33},
		{PipelineControl1, 0},
		{PipelineControl2, blend ? 0x20000080u : 0x10000000u}, {PipelineControl3, 0},
		{PipelineControl4, 0}, {PipelineControl5, 0},
		{0x008005cc, 0}, {0x008005d4, 0}, {0x008005dc, 0}, {ContextControl, 0}};
	for (const auto& reg : profile)
		if (peek(reg.first) != reg.second)
			return false;
	if ((peek(ClipXMin) | peek(ClipXMax) | peek(ClipYMin) | peek(ClipYMax)) & 0xffff000fu)
		return false;
	const uint32_t depth_control = plane_value(2, 5, 0);
	if (!plane_profile(bank, 0x100, blend ? 0xd0d0d0d0u : 0) ||
		plane_value(bank, 0, 0) != 0xffffffffu ||
		plane_value(bank, 4, 0) != (blend ? 0xd0d0d0d0u : 0x03030303u) ||
		plane_value(bank, 10, 0) != 0 ||
		(no_depth ? depth_control != 0x0a000200 :
			(depth_control != 0x0a000205 && depth_control != 0x0a000207)))
		return false;
	const uint32_t depth[] = {no_depth ? 0u : 0x0fff0fffu, 0, 0xf000, 0x0fff0fff,
		0x03030303, depth_control, 0, 0, 0, 0, 0x00ff0000, 0x33300000};
	for (unsigned i = 0; i < std::size(depth); ++i)
		if (plane_value(2, i, 0) != depth[i])
			return false;
	if (plane_value(2, 14, 0) || plane_value(2, 15, 0))
		return false;
	for (unsigned plane : {bank, 2u})
	{
		if (m_planes[plane].unknown_masks)
			return false;
		for (unsigned i = 24; i < 24 + width / 2; ++i)
			if (plane_value(plane, i, 0xffffffff) != 0xffffffffu)
				return false;
	}
	return true;
}

void CRealImage2100::triangle_command(uint32_t address, uint32_t value)
{
	// These commands retain the slot without launching a primitive.
	if (!value || value == 0x10)
		return;
	auto reject = [&]() {
		unimplemented_once("REALimage triangle command/profile (command rejected)",
			address, value, true);
	};
	const bool textured = (peek(PipelineControl0) & 0x80000000u) != 0,
		flat = flat_vertex_register(address), no_depth = peek(PipelineControl0) != 0x05008001,
		blend = peek(PipelineControl2) == 0x20000080;
	if (value != 0x13 || !triangle_profile() ||
		(flat && !no_depth) || (!textured && no_depth && !flat))
	{
		reject();
		return;
	}
	struct Vertex { double alpha, red, green, blue, x, y, z, s, t; } vertex[3]{};
	for (unsigned slot = 0; slot < 3; ++slot)
	{
		double component[7]{};
		for (unsigned i = textured || flat ? 4 : 0; i < 7; ++i)
		{
			const auto reg = m_shadow.find(VertexBase + slot * VertexStride + i * 4);
			if (reg == m_shadow.end())
			{
				reject();
				return;
			}
			float input;
			static_assert(sizeof(input) == sizeof(reg->second), "IEEE vertex word size");
			std::memcpy(&input, &reg->second, sizeof(input));
			if (!std::isfinite(input) ||
				(i == 0 && (input < 0 || input > 256)) ||
				((i == 4 || i == 5) && (input < -32768 || input >= 32768)) ||
				(i == 6 && (no_depth ? input == 0 : (input < 0 || input > 1))))
			{
				reject();
				return;
			}
			// Lighting can emit overrange RGB; clamp before interpolation.
			component[i] = i > 0 && i < 4 ? std::clamp(double(input), 0.0, 256.0) : input;
		}
		vertex[slot] = {component[0], component[1], component[2], component[3],
			component[4], component[5], component[6], 0, 0};
		if (textured)
		{
			float coordinates[4];
			for (unsigned i = 0; i < 4; ++i)
			{
				const auto reg = m_shadow.find(VertexTextureBase + slot * VertexStride + i * 4);
				if (reg == m_shadow.end())
				{
					reject();
					return;
				}
				std::memcpy(&coordinates[i], &reg->second, sizeof(float));
				if (!std::isfinite(coordinates[i]))
				{
					reject();
					return;
				}
			}
			if (coordinates[0] != 1 || coordinates[3] != 0)
			{
				reject();
				return;
			}
			vertex[slot].s = coordinates[1];
			vertex[slot].t = coordinates[2];
		}
	}
	// The shared projection scale can be negative; reject a horizon crossing.
	if (textured && ((vertex[0].z < 0) != (vertex[1].z < 0) ||
		(vertex[0].z < 0) != (vertex[2].z < 0)))
	{
		reject();
		return;
	}
	double flat_color[4]{};
	if (flat && !textured)
	{
		// The launching slot supplies flat color, regardless of winding.
		const uint32_t slot = (address - FlatVertexBase) / VertexStride;
		for (unsigned i = blend ? 0 : 1; i < 4; ++i)
		{
			const auto reg = m_shadow.find(VertexBase + slot * VertexStride + i * 4);
			if (reg == m_shadow.end())
			{
				reject();
				return;
			}
			float input;
			std::memcpy(&input, &reg->second, sizeof(input));
			if (!std::isfinite(input) || (i == 0 && (input < 0 || input > 256)))
			{
				reject();
				return;
			}
			flat_color[i] = i ? std::clamp(double(input), 0.0, 255.0) : input / 256.0;
		}
	}
	auto edge = [](const Vertex& a, const Vertex& b, double x, double y) {
		return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
	};
	double area = edge(vertex[0], vertex[1], vertex[2].x, vertex[2].y);
	if (!area)
		return;
	if (area < 0)
	{
		std::swap(vertex[1], vertex[2]);
		area = -area;
	}
	const Vertex &a = vertex[0], &b = vertex[1], &c = vertex[2];
	const int left = std::max(int(peek(ClipXMin) >> 4),
		int(std::ceil(std::min({a.x, b.x, c.x}) - 0.5))),
		top = std::max(int(peek(ClipYMin) >> 4),
			int(std::ceil(std::min({a.y, b.y, c.y}) - 0.5))),
		right = std::min({int(m_color_width) - 1, int(peek(ClipXMax) >> 4),
			int(std::floor(std::max({a.x, b.x, c.x}) - 0.5))}),
		bottom = std::min({int(m_color_height) - 1, int(peek(ClipYMax) >> 4),
			int(std::floor(std::max({a.y, b.y, c.y}) - 0.5))});
	if (left > right || top > bottom)
		return;
	auto top_left = [](const Vertex& p, const Vertex& q) {
		return q.y < p.y || (q.y == p.y && q.x > p.x);
	};
	const bool include_a = top_left(b, c), include_b = top_left(c, a), include_c = top_left(a, b);
	const uint32_t wid = (peek(DrawControl) >> 4) & 0xf000,
		bank = ((peek(DrawControl) >> 12) & 3) == 2 ? 1 : 0;
	const bool depth_less = plane_value(2, 5, 0) == 0x0a000207;
	const uint32_t texture_profile = peek(PipelineControl0),
		texture_base = textured ? texture_offset(peek(TextureBase)) : 0,
		texture_width = 1u << ((texture_profile >> 8) & 15),
		texture_height = 1u << ((texture_profile >> 4) & 15);
	const unsigned texture_row_shift = m_texture.size() == MaxTextureSize ? 14 : 13;
	// Validated triangle profiles write every RGB channel.
	uint32_t* const color_plane = m_color.data() + size_t(bank) * m_color_pixels;
	m_clear_cache[bank] = {};
	struct RowEdge { const Vertex *p, *q; double slope; bool inclusive; };
	const RowEdge row_edges[] = {
		{&b, &c, c.y == b.y ? 0 : (c.x - b.x) / (c.y - b.y), include_a},
		{&c, &a, a.y == c.y ? 0 : (a.x - c.x) / (a.y - c.y), include_b},
		{&a, &b, b.y == a.y ? 0 : (b.x - a.x) / (b.y - a.y), include_c}};
	// Skip only spans rejected by the original edge test.
	auto row_span = [&](int y, int& span_left, int& span_right) {
		span_left = left;
		span_right = right;
		for (const auto& limit : row_edges)
		{
			if (span_left > span_right)
				break;
			const Vertex &p = *limit.p, &q = *limit.q;
			auto outside = [&](int x) {
				const double e = edge(p, q, x + 0.5, y + 0.5);
				return e < 0 || (e == 0 && !limit.inclusive);
			};
			if (q.y == p.y)
			{
				if (outside(span_left))
					span_left = span_right + 1;
				continue;
			}
			const double crossing = p.x + (y + 0.5 - p.y) * limit.slope - 0.5;
			if (!std::isfinite(crossing))
				continue;
			if (q.y < p.y)
			{
				const int bound = int(std::clamp(std::floor(crossing) - 1,
					double(span_left), double(span_right + 1)));
				if (bound > span_left && outside(bound - 1))
					span_left = bound;
			}
			else
			{
				const int bound = int(std::clamp(std::ceil(crossing) + 1,
					double(span_left - 1), double(span_right)));
				if (bound < span_right && outside(bound + 1))
					span_right = bound;
			}
		}
	};

#if defined(REALIMAGE_SSE2)
	if (textured)
	{
		const __m128d zero = _mm_setzero_pd(), area2 = _mm_set1_pd(area);
		auto vector_edge = [&] (const Vertex& p, const Vertex& q, __m128d xx, __m128d yy) {
			return _mm_sub_pd(_mm_mul_pd(_mm_set1_pd(q.x - p.x), _mm_sub_pd(yy, _mm_set1_pd(p.y))),
				_mm_mul_pd(_mm_set1_pd(q.y - p.y), _mm_sub_pd(xx, _mm_set1_pd(p.x))));
		};
		auto inside = [&] (__m128d e, bool include) {
			return include ? _mm_cmpge_pd(e, zero) : _mm_cmpgt_pd(e, zero);
		};
		for (int y = top; y <= bottom; ++y)
		{
			int span_left, span_right;
			row_span(y, span_left, span_right);
			for (int x = span_left; x <= span_right; x += 2)
			{
				const __m128d xx = _mm_set_pd(x + 1.5, x + 0.5), yy = _mm_set1_pd(y + 0.5);
				const __m128d ea = vector_edge(b, c, xx, yy), eb = vector_edge(c, a, xx, yy), ec = vector_edge(a, b, xx, yy);
				unsigned active = unsigned(_mm_movemask_pd(_mm_and_pd(inside(ea, include_a),
					_mm_and_pd(inside(eb, include_b), inside(ec, include_c))))) & (x < span_right ? 3u : 1u);
				const size_t offset = size_t(y) * m_color_width + unsigned(x);
				if ((active & 1) && (m_auxiliary[offset] & 0xf000) != wid)
					active &= ~1u;
				if ((active & 2) && (m_auxiliary[offset + 1] & 0xf000) != wid)
					active &= ~2u;
				if (!active)
					continue;
				const __m128d wb = _mm_div_pd(eb, area2), wc = _mm_div_pd(ec, area2),
					wa = _mm_div_pd(_mm_div_pd(ea, area2), _mm_set1_pd(a.z)),
					tb = _mm_div_pd(wb, _mm_set1_pd(b.z)), tc = _mm_div_pd(wc, _mm_set1_pd(c.z));
				const __m128d mask = _mm_castsi128_pd(_mm_set_epi64x(active & 2 ? -1ll : 0, active & 1 ? -1ll : 0));
				const __m128d denominator = _mm_or_pd(_mm_and_pd(mask, _mm_add_pd(_mm_add_pd(wa, tb), tc)),
					_mm_andnot_pd(mask, _mm_set1_pd(1)));
				const __m128d s = _mm_div_pd(_mm_add_pd(_mm_add_pd(_mm_mul_pd(wa, _mm_set1_pd(a.s)),
					_mm_mul_pd(tb, _mm_set1_pd(b.s))), _mm_mul_pd(tc, _mm_set1_pd(c.s))), denominator),
					t = _mm_div_pd(_mm_add_pd(_mm_add_pd(_mm_mul_pd(wa, _mm_set1_pd(a.t)),
					_mm_mul_pd(tb, _mm_set1_pd(b.t))), _mm_mul_pd(tc, _mm_set1_pd(c.t))), denominator);
				const __m128i color = realimage_texture2(s, t, m_texture.data(), texture_base,
					texture_width, texture_height, texture_row_shift);
				if (active & 1)
					color_plane[offset] = (color_plane[offset] & 0xff000000u) | unsigned(_mm_cvtsi128_si32(color));
				if (active & 2)
					color_plane[offset + 1] = (color_plane[offset + 1] & 0xff000000u) |
						unsigned(_mm_cvtsi128_si32(_mm_srli_si128(color, 4)));
			}
		}
		return;
	}
#endif
	// Launches reuse all three slots; sorting must leave their registers intact.
	for (int y = top; y <= bottom; ++y)
	{
		int span_left, span_right;
		row_span(y, span_left, span_right);
		for (int x = span_left; x <= span_right; ++x)
		{
			const double ea = edge(b, c, x + 0.5, y + 0.5),
				eb = edge(c, a, x + 0.5, y + 0.5), ec = edge(a, b, x + 0.5, y + 0.5);
			if (ea < 0 || (ea == 0 && !include_a) ||
				eb < 0 || (eb == 0 && !include_b) || ec < 0 || (ec == 0 && !include_c))
				continue;
			const size_t offset = size_t(y) * m_color_width + unsigned(x);
			uint32_t& auxiliary = m_auxiliary[offset];
			if ((auxiliary & 0xf000) != wid)
				continue;
			uint32_t& destination = color_plane[offset];
			const double wb = eb / area, wc = ec / area;
			if (textured)
			{
				// This profile supplies scaled clip W in the vertex Z slot.
				const double wa = ea / area / a.z, tb = wb / b.z, tc = wc / c.z,
					denominator = wa + tb + tc;
				const double s = (wa * a.s + tb * b.s + tc * c.s) / denominator,
					t = (wa * a.t + tb * b.t + tc * c.t) / denominator;
				const uint32_t color = texture_color(s, t, texture_base,
					texture_width, texture_height, texture_row_shift);
				destination = (destination & 0xff000000u) | color;
				continue;
			}
			if (flat)
			{
				uint32_t color = 0;
				for (unsigned i = 1; i < 4; ++i)
				{
					const unsigned shift = (3 - i) * 8;
					const double channel = blend ? flat_color[i] * flat_color[0] +
						((destination >> shift) & 255) * (1 - flat_color[0]) : flat_color[i];
					color |= uint32_t(channel) << shift;
				}
				destination = (destination & 0xff000000u) | color;
				continue;
			}
			const uint32_t z = uint32_t(std::clamp(a.z + wb * (b.z - a.z) + wc * (c.z - a.z),
				0.0, 1.0) * 16777215),
				old_z = (auxiliary & 0xfff) | ((auxiliary >> 4) & 0xfff000);
			if (z > old_z || (depth_less && z == old_z))
				continue;
			auto channel = [&](double av, double bv, double cv) {
				return uint32_t(std::clamp(av + wb * (bv - av) + wc * (cv - av), 0.0, 255.0));
			};
			const uint32_t color = (channel(a.red, b.red, c.red) << 16) |
				(channel(a.green, b.green, c.green) << 8) | channel(a.blue, b.blue, c.blue);
			destination = (destination & 0xff000000u) | color;
			auxiliary = (auxiliary & ~0x0fff0fffu) | (z & 0xfff) | ((z & 0xfff000) << 4);
		}
	}
}

// RGB565, linear/repeat sampling, RGB decal mode.
uint32_t CRealImage2100::texture_color(double s, double t, uint32_t base,
	uint32_t width, uint32_t height, unsigned row_shift) const
{
	const double u = realimage_repeat_fraction(s) * width - 0.5,
		v = realimage_repeat_fraction(t) * height - 0.5;
	const int x = int(std::floor(u)), y = int(std::floor(v));
	const double fx = u - x, fy = v - y;
	auto texel = [&](int tx, int ty) {
		const uint32_t offset = base + ((uint32_t(ty) & (height - 1)) << row_shift) +
			((uint32_t(tx) & (width - 1)) << 1);
		return uint32_t(m_texture[offset]) | (uint32_t(m_texture[offset + 1]) << 8);
	};
	const uint32_t pixels[] = {texel(x, y), texel(x + 1, y), texel(x, y + 1), texel(x + 1, y + 1)};
	auto channel = [&](unsigned shift, uint32_t mask) {
		const double a = (pixels[0] >> shift) & mask, b = (pixels[1] >> shift) & mask,
			c = (pixels[2] >> shift) & mask, d = (pixels[3] >> shift) & mask;
		const double top = a + fx * (b - a), bottom = c + fx * (d - c);
		return uint32_t(std::clamp((top + fy * (bottom - top)) * 255 / mask, 0.0, 255.0));
	};
	return (channel(11, 31) << 16) | (channel(5, 63) << 8) | channel(0, 31);
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

uint32_t CRealImage2100::host_read()
{
	// Readback returns RGB directly, without destination ROP or write masks.
	const uint32_t value = readback_pixel(m_readback.word);
	if (++m_readback.word == m_readback.width * m_readback.height)
		m_readback = {};
	return value;
}

uint32_t CRealImage2100::readback_pixel(uint32_t word) const
{
	const uint32_t bank = m_readback.banks == 2 ? 1 : 0,
		x = m_readback.x + word % m_readback.width,
		y = m_readback.y + word / m_readback.width;
	return m_color[size_t(bank) * m_color_pixels + y * m_color_width + x];
}

void CRealImage2100::dma_command(uint32_t v)
{
	const uint32_t mode = v & ~DMACommandListCountMask;
	if (mode == 0xa1800000u || mode == 0xc0800000u)
	{
		dma_texture_upload(v);
		return;
	}
	if (mode == 0xc0400000u || mode == 0xa1000000u)
	{
		dma_command_list(v);
		return;
	}
	// Emulator staging limit, independent of driver buffer allocation.
	constexpr uint32_t DMABufferSize = 32768;
	const uint32_t destination = m_dma_regs[8], source = m_dma_regs[9],
		completion = m_dma_regs[12], words = v & 0xffff, bytes = words * 4;
	bool neutral = true;
	for (unsigned i : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 10u, 13u, 15u, 16u, 17u, 18u})
		neutral &= m_dma_regs[i] == 0;
	if ((v & 0xffff0000u) != 0xc4800000u || !words || bytes > DMABufferSize ||
		!neutral || m_dma_regs[14] != 8 ||
		!m_readback.width ||
		uint64_t(m_readback.width) * m_readback.height - m_readback.word < words ||
		((destination | source | completion) & 3) ||
		source < HostData || source > HostData + HostReadSize - bytes ||
		uint64_t(destination) + bytes > 0x100000000ull ||
		uint64_t(completion) + 4 > 0x100000000ull ||
		(uint64_t(completion) < uint64_t(destination) + bytes &&
			uint64_t(completion) + 4 > destination))
	{
		unimplemented(
			"REALimage DMA transfer (rejected; completion not written)", DMACommand, v, true);
		return;
	}
	std::array<uint8_t, DMABufferSize> data{};
	for (uint32_t word = 0; word < words; ++word)
	{
		const uint32_t pixel = readback_pixel(m_readback.word + word);
		for (unsigned lane = 0; lane < 4; ++lane)
			data[word * 4 + lane] = uint8_t(pixel >> (lane * 8));
	}
	// Commit FIFO progress only after payload and completion writes succeed.
	if (!m_dma_writer || !m_dma_writer(destination, data.data(), bytes, completion))
	{
		report("DMA_WRITE", destination, bytes, "PCI DMA write unavailable or rejected");
		return;
	}
	m_readback.word += words;
	if (m_readback.word == m_readback.width * m_readback.height)
		m_readback = {};
}

void CRealImage2100::dma_texture_upload(uint32_t v)
{
	const uint32_t completion = m_dma_regs[12];
	auto reject = [&]() {
		unimplemented("REALimage DMA texture upload (rejected; completion not written)",
			DMACommand, v, true);
	};
	auto source_range = [&](uint32_t address, uint32_t bytes) {
		return !((address | bytes) & 3) &&
			uint64_t(address) + bytes <= 0x100000000ull &&
			!(uint64_t(completion) < uint64_t(address) + bytes &&
				uint64_t(completion) + 4 > address);
	};
	bool neutral = true;
	for (unsigned i : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 10u, 13u, 15u, 16u, 17u, 18u})
		neutral &= m_dma_regs[i] == 0;
	if (!neutral || m_dma_regs[14] != 8 || m_dma_list_active || (completion & 3))
	{
		reject();
		return;
	}
	if (!m_dma_reader || !m_dma_completer)
	{
		report("DMA_READ", m_dma_regs[8], v, "PCI DMA upload callbacks unavailable");
		return;
	}
	// Stage the chain so a bad link cannot leave a partial upload.
	auto texture = m_texture;
	std::set<uint32_t> visited;
	uint32_t command = v, source = m_dma_regs[8], target = m_dma_regs[9],
		next = m_dma_regs[11];
	for (;;)
	{
		const uint32_t mode = command & ~DMACommandListCountMask,
			bytes = (command & DMACommandListCountMask) * 4;
		if ((mode != 0xa1800000u && mode != 0xc0800000u) || !bytes ||
			!source_range(source, bytes) || (target & 3) || target < 0x04000000u ||
			uint64_t(target) + bytes > 0x04000000ull + MaxTextureSize ||
			(mode == 0xa1800000u &&
				(!source_range(next, 16) || !visited.insert(next).second)))
		{
			reject();
			return;
		}
		std::vector<uint8_t> data(bytes);
		if (!m_dma_reader(source, data.data(), bytes, completion))
		{
			report("DMA_READ", source, bytes, "PCI DMA texture data unavailable or rejected");
			return;
		}
		for (uint32_t i = 0; i < bytes; i += 4)
		{
			const uint32_t offset = texture_offset(target - 0x04000000u + i);
			if (offset != 0xffffffffu)
				std::copy_n(data.data() + i, 4, texture.data() + offset);
		}
		if (mode == 0xc0800000u)
			break;
		std::array<uint8_t, 16> descriptor{};
		if (!m_dma_reader(next, descriptor.data(), descriptor.size(), completion))
		{
			report("DMA_READ", next, uint32_t(descriptor.size()),
				"PCI DMA descriptor unavailable or rejected");
			return;
		}
		auto word = [&](unsigned i) {
			const uint8_t* p = descriptor.data() + i * 4;
			return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
				(uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
		};
		command = word(0);
		source = word(1);
		target = word(2);
		next = word(3);
	}
	m_texture.swap(texture);
	if (!m_dma_completer(completion))
		report("DMA_COMPLETE", completion, v, "PCI DMA completion unavailable or rejected");
}

bool CRealImage2100::dma_list_target(uint32_t a) const
{
	if (a & 3)
		return false;
	if (vertex_register(a) || a == ContextLink || zero_context_register(a))
		return true;
	if (a >= HostData && a < HostData + HostDataSize)
		return true;
	if (a >= 0x01000000 && a < 0x03000000)
		return ((a & 0x1fff) >> 2) < m_color_width &&
			((a & 0x00ffffff) >> 13) < m_color_height;
	const int plane_index = plane_register(a);
	if (plane_index >= 0)
	{
		const uint32_t available = m_color_width == MaxColorWidth ? 255u : 15u;
		return plane_index != 24 || a == PlanePixelMask ||
			!(((a >> 13) & 255) & ~available);
	}
	// Other command-list targets use the existing register handlers.
	switch (a)
	{
	case ClipXMax: case ClipYMax: case ClipXMin: case ClipYMin:
	case GlobalControl0: case GlobalControl1: case GlobalControl2:
	case TextureBase:
	case PipelineControl0: case PipelineControl1:
	case PipelineControl2: case PipelineControl3:
	case PipelineControl4: case PipelineControl5:
	case DrawControl: case Foreground: case Background:
	case MonoPattern0: case MonoPattern1: case MonoPattern2: case MonoPattern3:
	case HostOrigin: case HostExtent: case HostCommand:
	case FillOrigin: case FillExtent: case FillCommand:
	case BlockSource: case BlockDestination: case BlockExtent: case BlockCommand:
		return true;
	default:
		return false;
	}
}

void CRealImage2100::dma_command_list(uint32_t v)
{
	const uint32_t completion = m_dma_regs[12];
	uint32_t command = v, source = m_dma_regs[8], initial_address = m_dma_regs[9],
		next = m_dma_regs[11];
	auto reject = [&]() {
		unimplemented("REALimage DMA command list (rejected; completion not written)",
			DMACommand, v, true);
	};
	auto source_range = [&](uint32_t address, uint32_t bytes) {
		return !((address | bytes) & 3) &&
			uint64_t(address) + bytes <= 0x100000000ull &&
			!(uint64_t(completion) < uint64_t(address) + bytes &&
				uint64_t(completion) + 4 > address);
	};
	bool neutral = true;
	for (unsigned i : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 10u, 13u, 15u, 16u, 17u, 18u})
		neutral &= m_dma_regs[i] == 0;
	if (m_dma_list_active || !neutral || m_dma_regs[14] != 8 || (completion & 3))
	{
		reject();
		return;
	}
	if (!m_dma_reader || !m_dma_completer)
	{
		report("DMA_READ", source, v, "PCI DMA command-list callbacks unavailable");
		return;
	}
	auto decode_word = [](const uint8_t* p) {
		return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
			(uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
	};
	std::vector<uint8_t> data;
	auto word_at = [&](uint32_t index) {
		return decode_word(data.data() + size_t(index) * 4);
	};
	struct Packet
	{
		uint32_t address, wait_mask, first, count;
	};
	std::vector<Packet> packets;
	std::set<uint32_t> visited;
	// Preflight every link within the existing staging budget before executing.
	for (;;)
	{
		const uint32_t mode = command & ~DMACommandListCountMask,
			words = command & DMACommandListCountMask, bytes = words * 4;
		if ((mode != 0xc0400000u && mode != 0xa1000000u) || !words ||
			bytes > DMACommandListMaxBytes - data.size() || !source_range(source, bytes) ||
			(initial_address & 3) || (mode == 0xa1000000u &&
				(!source_range(next, 16) || !visited.insert(next).second)))
		{
			reject();
			return;
		}
		const uint32_t first = uint32_t(data.size() / 4), end = first + words;
		data.resize(data.size() + bytes);
		if (!m_dma_reader(source, data.data() + size_t(first) * 4, bytes, completion))
		{
			report("DMA_READ", source, bytes, "PCI DMA read unavailable or rejected");
			return;
		}
		uint32_t cursor = first, address = initial_address;
		while (cursor < end)
		{
			const uint32_t control = word_at(cursor++), count = control & 0xffff;
			if (count > end - cursor ||
				uint64_t(address) + uint64_t(count) * 4 > 0x100000000ull)
			{
				reject();
				return;
			}
			if (!dma_list_target(address))
			{
				report("DMA_TARGET", address, control, "Unsupported DMA command-list target");
				reject();
				return;
			}
			for (uint32_t i = 0; i < count; ++i)
				if (!dma_list_target(address + i * 4))
				{
					report("DMA_TARGET", address + i * 4, word_at(cursor + i),
						"Unsupported DMA command-list target");
					reject();
					return;
				}
			packets.push_back({address, control & 0xffff0000u, cursor, count});
			cursor += count;
			if (cursor == end)
				break;
			address = word_at(cursor++);
			if (cursor == end)
			{
				reject();
				return;
			}
		}
		if (mode == 0xc0400000u)
			break;
		std::array<uint8_t, 16> descriptor{};
		if (!m_dma_reader(next, descriptor.data(), descriptor.size(), completion))
		{
			report("DMA_READ", next, uint32_t(descriptor.size()),
				"PCI DMA descriptor unavailable or rejected");
			return;
		}
		command = decode_word(descriptor.data());
		source = decode_word(descriptor.data() + 4);
		initial_address = decode_word(descriptor.data() + 8);
		next = decode_word(descriptor.data() + 12);
		// Captured chains retain the latched completion address at their terminal link.
		if ((command & ~DMACommandListCountMask) == 0xc0400000u && next != completion)
		{
			reject();
			return;
		}
	}
	struct ActiveList
	{
		bool& active;
		~ActiveList() { active = false; }
	} active{m_dma_list_active};
	m_dma_list_active = true;
	m_dma_list_rejected = false;
	for (const auto& packet : packets)
	{
		if (packet.wait_mask)
		{
			bool ready = false;
			for (uint32_t i = 0; i < StatusFrameReads; ++i)
				if (!(status_read() & packet.wait_mask))
				{
					ready = true;
					break;
				}
			if (!ready)
			{
				report("DMA_WAIT", packet.address, packet.wait_mask,
					"DMA command-list status wait did not complete");
				return;
			}
		}
		for (uint32_t i = 0; i < packet.count; ++i)
		{
			WriteMem(packet.address + i * 4, 32, word_at(packet.first + i));
			if (m_dma_list_rejected)
			{
				reject();
				return;
			}
		}
	}
	if (!m_dma_completer(completion))
		report("DMA_COMPLETE", completion, 0,
			"PCI DMA completion unavailable or rejected");
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
	if (!native_pixel_profile())
		return reject("Native pixel layout unsupported");
	if (!(m_dac_regs[0x0b] & 1) || !(m_dac_regs[0x0d] & 4))
		return reject("Native DAC disabled");
	// Each pixel's WID selects an RGB640 window attribute entry.
	std::array<bool, 16> supported_windows{};
	for (uint32_t i = 0; i < 16; ++i)
	{
		const uint32_t fb = 0x100 + i * 4, overlay = 0x200 + i * 4;
		supported_windows[i] = m_dac_regs[fb] == 8 && m_dac_regs[fb + 1] == 12 &&
			!m_dac_regs[fb + 2] && !m_dac_regs[fb + 3] &&
			m_dac_regs[overlay] == 4 && !m_dac_regs[overlay + 1] &&
			!m_dac_regs[overlay + 2] && m_dac_regs[overlay + 3] == 0x48;
	}
	// Unlike VGA, vertical display is a count, not a last-line index.
	const uint32_t horizontal = peek(TimingBase),
		overflow = peek(TimingBase + 4) >> 24,
		vertical = peek(TimingBase + 0x10),
		extension = (peek(TimingBase + 0x1c) >> 16) & 255;
	// RGB640 register 08 selects the driver's horizontal timing unit.
	const uint32_t horizontal_unit = m_dac_regs[0x08] == 0 ? 8 :
		m_dac_regs[0x08] == 1 && m_color_width == MaxColorWidth ? 16 : 0;
	const uint32_t width = ((((horizontal >> 8) & 255) |
		((extension & 0x40) << 2)) + 1) * horizontal_unit,
		vertical_extension = extension & 0x3f;
	// Whole extension patterns are established by both drivers' mode tables.
	const uint32_t height = ((vertical >> 16) & 255) |
		((overflow & 2) << 7) | ((overflow & 0x40) << 3) |
		(vertical_extension == 0x0d ? 1024u : 0u);
	if ((vertical_extension != 0 && vertical_extension != 8 && vertical_extension != 0x0d) ||
		!width || !height ||
		width > m_color_width || height > m_color_height)
		return reject("Native timing unsupported");
	Frame frame;
	frame.width = width;
	frame.height = height;
	frame.argb.resize(size_t(width) * height);
	// Board I/O bits 8..23 select the color bank for each WID.
	const uint32_t bank_select = (m_board_io >> 8) & 0xffff;
	if ((bank_select == 0 || bank_select == 0xffff) &&
		std::all_of(supported_windows.begin(), supported_windows.end(), [](bool v) { return v; }))
	{
		for (uint32_t y = 0; y < height; ++y)
		{
			const uint32_t* source = m_color.data() +
				(bank_select ? m_color_pixels : 0) + size_t(y) * m_color_width;
			uint32_t* destination = frame.argb.data() + size_t(y) * width;
			for (uint32_t x = 0; x < width; ++x)
				destination[x] = 0xff000000 | source[x];
		}
		composite_cursor(frame);
		return frame;
	}
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x)
		{
			const size_t offset = size_t(y) * m_color_width + x;
			const unsigned wid = (m_auxiliary[offset] >> 12) & 15;
			if (!supported_windows[wid])
				return reject("Native DAC window format unsupported");
			const size_t bank = (bank_select >> wid) & 1;
			frame.argb[size_t(y) * width + x] =
				0xff000000 | m_color[bank * m_color_pixels + offset];
		}
	composite_cursor(frame);
	return frame;
}

void CRealImage2100::composite_cursor(Frame& frame) const
{
	// NT uses the RGB640 64x64 Windows cursor mode.
	if (m_dac_regs[0x4b] != 0x0a)
		return;
	// Position uses twelve data bits and bit 15 as the sign.
	const int origin_x = int(m_dac_regs[0x40] | ((m_dac_regs[0x41] & 15) << 8)) -
		((m_dac_regs[0x41] & 0x80) ? 4096 : 0) - (m_dac_regs[0x44] & 63);
	const int origin_y = int(m_dac_regs[0x42] | ((m_dac_regs[0x43] & 15) << 8)) -
		((m_dac_regs[0x43] & 0x80) ? 4096 : 0) - (m_dac_regs[0x45] & 63);
	uint32_t colors[2];
	for (unsigned i = 0; i < 2; ++i)
	{
		const unsigned address = 0x4800 + 3 * (i + 1);
		colors[i] = 0xff000000u | (uint32_t(m_dac_regs[address]) << 16) |
			(uint32_t(m_dac_regs[address + 1]) << 8) | m_dac_regs[address + 2];
	}
	for (int cy = 0; cy < 64; ++cy)
	{
		const int y = origin_y + cy;
		if (y < 0 || uint32_t(y) >= frame.height)
			continue;
		for (int cx = 0; cx < 64; ++cx)
		{
			const int x = origin_x + cx;
			if (x < 0 || uint32_t(x) >= frame.width)
				continue;
			// Four pixels per byte, low pair first: colors 1/2, transparent, highlight.
			const unsigned code = (m_dac_regs[0x1000 + cy * 16 + cx / 4] >>
				(2 * (cx & 3))) & 3;
			uint32_t& pixel = frame.argb[size_t(y) * frame.width + unsigned(x)];
			if (code < 2)
				pixel = colors[code];
			else if (code == 3)
				pixel ^= 0x00808080u;
		}
	}
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

void CRealImage2100::configure_framebuffer(uint32_t ram_chips)
{
	if (ram_chips != 12 && ram_chips != 24)
		throw std::invalid_argument("Invalid REALimage framebuffer population");
	const uint32_t width = ram_chips == 24 ? MaxColorWidth : ColorWidth,
		height = ram_chips == 24 ? MaxColorHeight : ColorHeight;
	if (width == m_color_width && height == m_color_height)
		return;
	std::vector<uint32_t> color(size_t(width) * height * 2);
	std::vector<uint32_t> auxiliary(size_t(width) * height);
	m_color.swap(color);
	m_auxiliary.swap(auxiliary);
	m_color_width = width;
	m_color_height = height;
	m_color_pixels = width * height;
	m_board_straps = ram_chips == 24 ? 3 : 0;
	m_planes = {};
	m_clear_cache = {};
	m_pending = {};
	m_readback = {};
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
	  m_color(m_color_pixels * 2), m_auxiliary(m_color_pixels), m_texture(MinTextureSize)
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
		std::fill(m_auxiliary.begin(), m_auxiliary.end(), uint32_t(0));
		std::fill(m_texture.begin(), m_texture.end(), uint8_t(0));
	}
	m_dma_regs.fill(0);
	m_planes = {};
	m_clear_cache = {};
	m_pending = {};
	m_readback = {};
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
	if (m_dma_list_active)
		m_dma_list_rejected = true;
	if (m_diagnostic)
		m_diagnostic(Diagnostic{code, message, a, v, fatal});
}

void CRealImage2100::unimplemented(
	const char* what, uint32_t a, uint32_t v, bool write)
{
	if (m_dma_list_active)
		m_dma_list_rejected = true;
	if (m_unimplemented)
		m_unimplemented(what, a, v, write);
}

void CRealImage2100::unimplemented_once(
	const char* what, uint32_t a, uint32_t v, bool write)
{
	if (m_dma_list_active)
		m_dma_list_rejected = true;
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

// Selectors, latches, transfers, palette/DAC, shadow, VGA and color banks.
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
		const unsigned count = m_color_width == MaxColorWidth ?
			PlaneRegisterCount : LegacyPlaneRegisterCount;
		for (unsigned i = 0; i < count; ++i)
			put32(p, plane.regs[i]);
	}
	for (const auto& cache : m_clear_cache)
	{
		put32(p, cache.source);
		put32(p, cache.color);
		put32(p, cache.known);
	}
	put32(p, m_readback.x);
	put32(p, m_readback.y);
	put32(p, m_readback.width);
	put32(p, m_readback.height);
	put32(p, m_readback.word);
	put32(p, m_readback.banks);
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
	for (const uint32_t word : m_auxiliary)
		put32(p, word);
	p.write(reinterpret_cast<const char*>(m_texture.data()), m_texture.size());
	const auto bytes = p.str();
	put32(out, 0x30324952); // RI20
	put32(out, m_color_width == MaxColorWidth ? 13 : 12);
	put32(out, uint32_t(bytes.size()));
	put32(out, crc32(bytes));
	out.write(bytes.data(), bytes.size());
	if (!out)
		throw std::runtime_error("PowerStorm snapshot write failed");
}

void CRealImage2100::RestoreState(std::istream& in)
{
	const auto magic = get32(in), version = get32(in);
	if (magic != 0x30324952 || version < 9 || version > 13)
		throw std::runtime_error("Wrong REALimage snapshot version");
	const bool large = version == 11 || version == 13, packed_auxiliary = version >= 12;
	if (large != (m_color_width == MaxColorWidth))
		throw std::runtime_error("REALimage snapshot framebuffer mismatch");
	const uint32_t fixed_payload = FixedPayload + (version >= 10 ? 24 : 0) +
		(large ? (MaxColorPixels - ColorPixels) * 9 +
			PlaneCount * (PlaneRegisterCount - LegacyPlaneRegisterCount) * 4 : 0) +
		(packed_auxiliary ? m_color_pixels * 3 + 12 : 0);
	const auto size = get32(in), crc = get32(in);
	if (size < fixed_payload || size > MaxStateSize - 16)
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
		if ((plane.written & ~(large ? 0xffffcfffu : 0x0fffcfffu)) ||
			plane.unknown_masks > (large ? 255u : 15u))
			throw std::runtime_error("Invalid REALimage plane register mask");
		const unsigned registers = large ? PlaneRegisterCount : LegacyPlaneRegisterCount;
		for (unsigned i = 0; i < registers; ++i)
		{
			plane.regs[i] = get32(p);
			if (!(plane.written & (1u << i)) && plane.regs[i])
				throw std::runtime_error("Invalid REALimage plane register state");
		}
	}
	std::array<ClearCache, 3> clear_cache{};
	for (unsigned bank = 0; bank < (packed_auxiliary ? 3u : 2u); ++bank)
	{
		auto& cache = clear_cache[bank];
		cache.source = get32(p);
		cache.color = get32(p);
		cache.known = get32(p);
		if ((cache.source & ~0x07ff07ffu) || (bank < 2 && (cache.known & 0xff000000)) ||
			(cache.color & ~cache.known) || (!cache.known && cache.source))
			throw std::runtime_error("Invalid REALimage clear cache");
	}
	Pending readback;
	if (version >= 10)
	{
		readback.x = get32(p);
		readback.y = get32(p);
		readback.width = get32(p);
		readback.height = get32(p);
		readback.word = get32(p);
		readback.banks = get32(p);
		if ((readback.width && (pending.width || !readback.height ||
			readback.x >= m_color_width || readback.y >= m_color_height ||
			readback.width > m_color_width - readback.x ||
			readback.height > m_color_height - readback.y ||
			readback.word >= uint64_t(readback.width) * readback.height ||
			(readback.banks != 1 && readback.banks != 2))) ||
			(!readback.width && (readback.x || readback.y || readback.height ||
				readback.word || readback.banks)))
			throw std::runtime_error("Invalid REALimage readback state");
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
		fixed_payload + uint64_t(count) * 8 + texture_size - MinTextureSize != size)
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
	std::vector<uint32_t> color(m_color_pixels * 2);
	for (uint32_t& c : color)
	{
		c = get32(p);
		if (c & 0xff000000)
			throw std::runtime_error("Invalid REALimage color buffer");
	}
	std::vector<uint32_t> auxiliary(m_color_pixels);
	for (uint32_t& word : auxiliary)
	{
		if (packed_auxiliary)
			word = get32(p);
		else
		{
			const int id = p.get();
			if (id < 0 || id > 15)
				throw std::runtime_error("Invalid REALimage window ID buffer");
			word = uint32_t(id) << 12;
		}
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
	m_auxiliary.swap(auxiliary);
	m_texture.swap(texture);
	m_dma_regs = dma_regs;
	m_planes = planes;
	m_clear_cache = clear_cache;
	m_pending = pending;
	m_readback = readback;
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
