/* ES40 emulator.
 * Copyright (C) 2026 by the ES40 Emulator Project
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

#include "Permedia2.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>

static uint32_t swap32(uint32_t x)
{
	return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24);
}

static int32_t sx(uint32_t x, unsigned bits)
{
	const uint64_t range = uint64_t(1) << bits, sign = range >> 1;
	const uint64_t v = uint64_t(x) & (range - 1);
	return static_cast<int32_t>(
		(v & sign) ? static_cast<int64_t>(v) - static_cast<int64_t>(range)
				   : static_cast<int64_t>(v));
}

static int64_t fixed_integer(int64_t x)
{
	// A DDA's integer portion floors negative fractions; C++ division truncates.
	return x >= 0 ? x / 65536 : -((-x + 65535) / 65536);
}

static bool valid_width(unsigned bits)
{
	return bits == 8 || bits == 16 || bits == 32;
}

static uint32_t width_mask(unsigned bits)
{
	return bits == 32 ? 0xffffffffu : ((1u << bits) - 1);
}

static bool host_tag(uint32_t a)
{
	return a == CPermedia2::FBData || a == CPermedia2::FBSourceData ||
		a == CPermedia2::Color || a == CPermedia2::Depth ||
		a == CPermedia2::Stencil || a == CPermedia2::Texel0;
}

static float vertex_float(uint32_t bits)
{
	float value;
	static_assert(sizeof(value) == sizeof(bits), "IEEE vertex word size");
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

static uint32_t vertex_bits(float value)
{
	uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

// Register dispatch and indexed RAMDAC registers

// MMIO and queued graphics commands share the hardware's 8-byte register
// spacing. WriteMem enqueues graphics writes; service invokes their handlers
// only when each command reaches the FIFO head.
uint32_t CPermedia2::register_read(uint32_t address)
{
	if (address >= FIFO && address < 0x3000)
	{
		if (r(OutDMACount))
		{
			// Slave reads must not compete with an active PCI output transfer.
			signal_error(1u << 8);
			report(
				"OUTPUT_DMA_ACCESS",
				address,
				0,
				"CPU output FIFO access while output DMA is active");
			return 0;
		}
		if (m_output.empty())
		{
			signal_error(1u << 1);
			report(
				"EMPTY_OUTPUT",
				address,
				0,
				"Read from empty output FIFO; returned modeled open-bus zero");
			return 0;
		}
		const uint32_t value = m_output.front().value;
		m_output.pop_front();
		update_irq();
		return value;
	}
	switch (address)
	{
	case InFIFOSpace:
		return m_options.input_fifo_entries -
			static_cast<uint32_t>(m_input.size());
	case OutFIFOWords:
		return static_cast<uint32_t>(m_output.size());
	case FIFODiscon:
		return (r(FIFODiscon) & 0x7fffffffu) | (busy() ? 0x80000000u : 0);
	case DisplayData:
		return (r(DisplayData) & 12u) |
			3u; // No EDID device; pulled-up data/clock.
	case Reboot:
		return 0;
	case PaletteWrite:
		return m_palette_w / 3;
	case PaletteRead:
		return m_palette_r / 3;
	case PaletteData:
	{
		const uint8_t value = m_palette[m_palette_r];
		m_palette_r = (m_palette_r + 1) % 768;
		return value;
	}
	case IndexedData:
		return m_dac_map.read_byte(r(PaletteWrite) & 255);
	case CursorColorData:
	{
		const uint8_t value = m_cursor_colors[m_cursor_color_pos];
		m_cursor_color_pos = (m_cursor_color_pos + 1) % 12;
		return value;
	}
	case CursorRAM:
	{
		const uint8_t value = m_cursor[m_cursor_pos];
		m_cursor_pos = (m_cursor_pos + 1) % 1024;
		return value;
	}
	default:
		return r(address);
	}
}

bool CPermedia2::register_write(uint32_t address, uint32_t value)
{
	if (address >= FIFO && address < 0x3000)
		return packet_word(value);
	if (address >= V0Fixed && address < DeltaMode &&
		vertex_write(address, value))
		return true;
	switch (address)
	{
	case ResetStatus:
		reset();
		return true;
	case IntEnable:
		r(IntEnable) = value;
		update_irq();
		return true;
	case IntFlags:
		r(IntFlags) &= ~value;
		update_irq();
		return true;
	case ErrorFlags:
		r(ErrorFlags) &= ~value;
		return true;
	case InFIFOSpace:
	case OutFIFOWords:
	case LineCount:
	case Count:
		return false;
	case DMAAddress:
		if (r(DMACount))
		{
			report(
				"DMA_BUSY",
				DMAAddress,
				value,
				"DMA address changed while active",
				true);
			return false;
		}
		r(DMAAddress) = value & ~3u;
		m_dma_cursor = r(DMAAddress);
		return true;
	case DMACount:
		if (r(DMACount))
		{
			report(
				"DMA_BUSY",
				DMACount,
				value,
				"DMA restarted while active",
				true);
			return false;
		}
		if ((value & 0xffff) != 0 && (r(DMAControl) & ~0x10u) != 0)
		{
			report(
				"DMA_MODE",
				DMACount,
				value,
				"Only untransformed input DMA is implemented",
				true);
			return false;
		}
		r(DMACount) = value & 0xffff;
		m_dma_cursor = r(DMAAddress);
		return true;
	case OutDMACount:
		if (r(OutDMACount))
		{
			signal_error(1u << 10);
			report(
				"OUTPUT_DMA_BUSY",
				OutDMACount,
				value,
				"Output DMA count rewritten while active",
				true);
			return false;
		}
		// The readable address remains the last programmed start, including
		// when software prepares the next transfer while this one runs.
		r(OutDMACount) = value & 0xffff;
		m_out_dma_cursor = r(OutDMAAddress);
		return true;
	case ScreenBase:
		r(ScreenBase) = value & 0x1fffffu;
		if (r(VideoControl) & 1)
			r(VideoControl) |= 0x80;
		else
			m_active_screen_base = r(ScreenBase);
		return true;
	case VideoControl:
		r(VideoControl) = (value & ~0x80u) | (r(VideoControl) & 0x80u);
		return true;
	case Reboot:
		// SGRAM mode reload completes synchronously; it does not reset
		// graphics state or execute option-ROM firmware.
		r(Reboot) = 0;
		return true;
	case MemControl:
		r(MemControl) = value & 0x10u;
		return true;
	case BootAddress:
		r(BootAddress) = value & 0x3ffu;
		return true;
	case VClkCtl:
		r(VClkCtl) = value & 0x3ffu;
		return true;
	case FifoControl:
		// Threshold fields are RW; UNDERFLOW is write-one-to-clear.
		r(FifoControl) =
			(value & 0x1f1fu) | ((r(FifoControl) & 0x10000u) & ~value);
		return true;
	case VSConfiguration:
		if ((value & 7u) >= 1 && (value & 7u) <= 4)
		{
			report(
				"VIDEO_STREAM_MODE",
				VSConfiguration,
				value,
				"Active video-stream/GPBus modes are not implemented",
				true);
			return false;
		}
		r(VSConfiguration) = value & 0x1fffffffu;
		return true;
	case Sync:
	{
		const unsigned mode = (r(FilterMode) >> 10) & 3;
		const size_t words = ((mode & 1) ? 1u : 0u) + ((mode & 2) ? 1u : 0u);
		if (m_output.size() + words > 8)
			return false;
		bool intr = (value & 0x80000000u) != 0;
		if (mode & 1)
		{
			output_push(tag(Sync), intr);
			intr = false;
		}
		if (mode & 2)
			output_push(value, intr);
		r(Sync) = value;
		return true;
	}
	case SuspendUntilFrameBlank:
		m_waiting_frame = true;
		m_wait_frame = m_frame_number + 1;
		r(SuspendUntilFrameBlank) = value;
		return true;
	case DrawTriangle:
		r(DrawTriangle) = value;
		draw_triangle(value);
		return true;
	case RepeatTriangle:
	case DrawLine01:
	case DrawLine10:
	case RepeatLine:
		report(
			"UNSUPPORTED_COMMAND",
			address,
			value,
			"Setup-unit command not implemented",
			true);
		return true;
	case Continue:
	case ContinueNewLine:
	case ContinueNewDom:
	case ContinueNewSub:
		r(address) = value;
		continue_render(address, value);
		return true;
	case Config:
		r(FBReadMode) =
			(r(FBReadMode) & ~(ReadSource | ReadDestination | Packed)) |
			((value & 1) ? ReadSource : 0) |
			((value & 2) ? ReadDestination : 0) | ((value & 4) ? Packed : 0);
		r(FBWriteMode) = (r(FBWriteMode) & ~1u) | ((value >> 3) & 1);
		r(ColorDDAMode) = (r(ColorDDAMode) & ~1u) | ((value >> 4) & 1);
		r(LogicalOpMode) = (r(LogicalOpMode) & ~31u) | ((value >> 5) & 31);
		r(Config) = value;
		return true;
	case FBReadMode:
		r(FBWriteConfig) = value;
		m_relative_offset = sx(value >> 20, 3);
		r(FBReadMode) = value;
		return true;
	case PackedDataLimits:
		m_relative_offset = sx(value >> 29, 3);
		r(FBReadMode) = (r(FBReadMode) & ~0x700000u) |
			(uint32_t(m_relative_offset & 7) << 20);
		r(PackedDataLimits) = value;
		return true;
	case FBWindowBase:
		r(FBSourceBase) = r(FBWindowBase) = value;
		return true;
	case FBSourceDelta:
	{
		const int64_t origin_sign = (r(FBReadMode) & 0x10000) ? -1 : 1;
		const int64_t delta = int64_t(sx(value >> 16, 12)) *
				pitch_from_products(r(FBReadMode)) * origin_sign +
			sx(value, 12);
		r(FBSourceOffset) = uint32_t(delta) & 0xffffff;
		r(FBSourceDelta) = value;
		return true;
	}
	case TextureDownloadOffset:
		r(TextureDownloadOffset) = value & 0x3fffffu;
		return true;
	case TextureData:
	{
		const uint32_t offset = r(TextureDownloadOffset) & 0x3fffffu;
		const uint32_t destination = offset * 4;
		// Raw downloads bypass the rendering pipeline and FBWriteMode, but
		// retain the framebuffer's hardware write protection (SLAU011A,
		// sections 4.11.6/4.11.7; hardware reference section 3.3.6).
		if (destination >= VramSize && !m_warned[TextureDownloadOffset / 8])
		{
			m_warned[TextureDownloadOffset / 8] = true;
			report(
				"TEXTURE_DOWNLOAD_RANGE",
				TextureDownloadOffset,
				offset,
				"Out-of-VRAM texture data discarded; electrical aliasing "
				"unmodeled");
		}
		pixel_write(destination, 4, value, 0xffffffffu, r(FBHardwareWriteMask));
		r(TextureData) = value;
		r(TextureDownloadOffset) = (offset + 1) & 0x3fffffu;
		return true;
	}
	case Render:
		r(Render) = value;
		start_render(value);
		return true;
	case FBData:
	case FBSourceData:
	case BitMaskPattern:
		r(address) = value;
		report(
			"ORPHAN_PAYLOAD",
			address,
			value,
			"Host data outside a waiting Render is retained, not drawn");
		return true;
	// Direct RAMDAC ports retain byte semantics even for a PCI DWORD transfer.
	case PaletteWrite:
	{
		const uint8_t v = static_cast<uint8_t>(value);
		m_palette_w = uint32_t(v) * 3;
		r(PaletteWrite) = v;
		m_cursor_pos = ((m_dac[DACCursorControl] >> 2) & 3) * 256 + v;
		return true;
	}
	case PaletteRead:
	{
		const uint8_t v = static_cast<uint8_t>(value);
		m_palette_r = uint32_t(v) * 3;
		r(PaletteRead) = v;
		return true;
	}
	case PaletteData:
		m_palette[m_palette_w] = static_cast<uint8_t>(value);
		m_palette_w = (m_palette_w + 1) % 768;
		r(PaletteWrite) = m_palette_w / 3;
		return true;
	case IndexedData:
		m_dac_map.write_byte(
			r(PaletteWrite) & 255, static_cast<uint8_t>(value));
		return true;
	case CursorColorAddress:
		m_cursor_color_pos = (value % 4) * 3;
		r(CursorColorAddress) = value;
		return true;
	case CursorColorData:
		m_cursor_colors[m_cursor_color_pos] = static_cast<uint8_t>(value);
		m_cursor_color_pos = (m_cursor_color_pos + 1) % 12;
		return true;
	case CursorRAM:
		m_cursor[m_cursor_pos] = static_cast<uint8_t>(value);
		m_cursor_pos = (m_cursor_pos + 1) % 1024;
		r(PaletteWrite) = m_cursor_pos & 255;
		m_palette_w = (m_cursor_pos & 255) * 3;
		m_dac[DACCursorControl] = uint8_t(
			(m_dac[DACCursorControl] & ~12u) | ((m_cursor_pos >> 6) & 12));
		return true;
	default:
		r(address) = value;
		return true;
	}
}

// Indexed RAMDAC registers use the same byte address_map as the VGA CRTC.
void CPermedia2::dac_map(address_map& map)
{
	map(0, 255).lrw8(
		NAME([this](offs_t offset) {
			return m_dac[offset];
		}),
		NAME([this](offs_t offset, uint8_t value) {
			m_dac[offset] = value;
		}));
	map(DACCursorControl, DACCursorControl)
		.lw8(NAME([this](offs_t, uint8_t value) {
			m_dac[DACCursorControl] = value;
			m_cursor_pos = ((value >> 2) & 3) * 256 + (r(PaletteWrite) & 255);
		}));
	map(0x20, 0x28).lw8(NAME([this](offs_t offset, uint8_t value) {
		const unsigned index = 0x20 + offset;
		const unsigned base = 0x20 + (offset / 3) * 3;
		m_dac[index] = value;
		m_dac[DACPixelClockStatus] = (m_dac[base + 2] & 8) ? 0x10 : 0;
	}));
	map(0x30, 0x32).lw8(NAME([this](offs_t offset, uint8_t value) {
		m_dac[0x30 + offset] = value;
		m_dac[DACMemoryClockStatus] = (m_dac[0x32] & 8) ? 0x10 : 0;
	}));
	// Lock status follows an enabled programmed clock and is read-only.
	map(DACPixelClockStatus, DACPixelClockStatus).lw8(NAME([](offs_t, uint8_t) {
	}));
	map(DACMemoryClockStatus, DACMemoryClockStatus)
		.lw8(NAME([](offs_t, uint8_t) {
		}));
}

const char* CPermedia2::register_name(uint32_t a)
{
	if (a >= V0Fixed && a < DeltaMode)
	{
		static const char* const names[3][16] = {
			{ "V0S", "V0T", "V0Q", "V0Ks", "V0Kd", "V0R", "V0G", "V0B",
				"V0A", "V0F", "V0X", "V0Y", "V0Z", "REGISTER", "V0Color", "REGISTER" },
			{ "V1S", "V1T", "V1Q", "V1Ks", "V1Kd", "V1R", "V1G", "V1B",
				"V1A", "V1F", "V1X", "V1Y", "V1Z", "REGISTER", "V1Color", "REGISTER" },
			{ "V2S", "V2T", "V2Q", "V2Ks", "V2Kd", "V2R", "V2G", "V2B",
				"V2A", "V2F", "V2X", "V2Y", "V2Z", "REGISTER", "V2Color", "REGISTER" }
		};
		const unsigned vertex = ((a - V0Fixed) / 0x80) % 3;
		return names[vertex][((a - V0Fixed) / 8) & 15];
	}
	switch (a & 0xffff)
	{
#define R(x)                                                                   \
	case x:                                                                    \
		return #x;
		R(ResetStatus);
		R(IntEnable);
		R(IntFlags);
		R(InFIFOSpace);
		R(OutFIFOWords);
		R(DMAAddress);
		R(DMACount);
		R(ErrorFlags);
		R(VClkCtl);
		R(TestRegister);
		R(ApertureOne);
		R(ApertureTwo);
		R(DMAControl);
		R(FIFODiscon);
		R(ChipConfig);
		R(OutDMAAddress);
		R(OutDMACount);
		R(Reboot);
		R(MemControl);
		R(BootAddress);
		R(MemConfig);
		R(BypassWriteMask);
		R(FramebufferWriteMask);
		R(Count);
		R(FIFO);
		R(ScreenBase);
		R(ScreenStride);
		R(HTotal);
		R(HgEnd);
		R(HbEnd);
		R(HsStart);
		R(HsEnd);
		R(VTotal);
		R(VbEnd);
		R(VsStart);
		R(VsEnd);
		R(VideoControl);
		R(InterruptLine);
		R(DisplayData);
		R(LineCount);
		R(FifoControl);
		R(ScreenBaseRight);
		R(PaletteWrite);
		R(PaletteData);
		R(PixelMask);
		R(PaletteRead);
		R(CursorColorAddress);
		R(CursorColorData);
		R(IndexedData);
		R(CursorRAM);
		R(CursorXLow);
		R(CursorXHigh);
		R(CursorYLow);
		R(CursorYHigh);
		R(VSConfiguration);
		R(StartXDom);
		R(dXDom);
		R(StartXSub);
		R(dXSub);
		R(StartY);
		R(dY);
		R(RasterCount);
		R(Render);
		R(ContinueNewLine);
		R(ContinueNewDom);
		R(ContinueNewSub);
		R(Continue);
		R(BitMaskPattern);
		R(RasterizerMode);
		R(XLimits);
		R(YLimits);
		R(RectangleOrigin);
		R(RectangleSize);
		R(PackedDataLimits);
		R(ScissorMode);
		R(ScissorMin);
		R(ScissorMax);
		R(ScreenSize);
		R(AreaStippleMode);
		R(WindowOrigin);
		R(AreaStipplePattern0);
		R(AreaStipplePattern1);
		R(AreaStipplePattern2);
		R(AreaStipplePattern3);
		R(AreaStipplePattern4);
		R(AreaStipplePattern5);
		R(AreaStipplePattern6);
		R(AreaStipplePattern7);
		R(TextureAddressMode);
		R(SStart);
		R(dSdx);
		R(dSdyDom);
		R(TStart);
		R(dTdx);
		R(dTdyDom);
		R(TextureBaseAddress);
		R(TextureMapFormat);
		R(TextureDataFormat);
		R(Texel0);
		R(TextureReadMode);
		R(TextureLUTMode);
		R(TextureColorMode);
		R(FogMode);
		R(RStart);
		R(dRdx);
		R(dRdyDom);
		R(GStart);
		R(dGdx);
		R(dGdyDom);
		R(BStart);
		R(dBdx);
		R(dBdyDom);
		R(AStart);
		R(ColorDDAMode);
		R(ConstantColor);
		R(Color);
		R(AlphaTestMode);
		R(AntialiasMode);
		R(AlphaBlendMode);
		R(DitherMode);
		R(FBSoftwareWriteMask);
		R(LogicalOpMode);
		R(FBWriteData);
		R(LBReadMode);
		R(LBReadFormat);
		R(LBWindowBase);
		R(LBWriteFormat);
		R(Window);
		R(ZStartU);
		R(ZStartL);
		R(dZdxU);
		R(dZdxL);
		R(dZdyDomU);
		R(dZdyDomL);
		R(LBWriteMode);
		R(TextureData);
		R(TextureDownloadOffset);
		R(StencilMode);
		R(DepthMode);
		R(FBReadMode);
		R(FBSourceOffset);
		R(FBPixelOffset);
		R(FBColor);
		R(FBData);
		R(FBSourceData);
		R(FBWindowBase);
		R(FBWriteMode);
		R(FBHardwareWriteMask);
		R(FBBlockColor);
		R(FBReadPixel);
		R(FBWriteConfig);
		R(FilterMode);
		R(StatisticMode);
		R(Sync);
		R(SuspendUntilFrameBlank);
		R(FBSourceBase);
		R(FBSourceDelta);
		R(Config);
		R(YUVMode);
		R(DeltaMode);
		R(DrawTriangle);
		R(RepeatTriangle);
		R(DrawLine01);
		R(DrawLine10);
		R(RepeatLine);
#undef R
	default:
		return "REGISTER";
	}
}

uint32_t CPermedia2::ReadMem(uint32_t a, int dsize)
{
	if (!valid_width(dsize) || a >= RegisterBARSize || (a & (dsize / 8 - 1)))
	{
		report("ACCESS_WIDTH", a, dsize, "Invalid MMIO width or alignment");
		return 0xffffffffu;
	}
	const bool swap = (a & 0x10000) != 0;
	a &= 0xffff;
	if (swap && dsize != 32)
	{
		report(
			"SWAP_SUBWORD",
			a,
			dsize,
			"Subword upper MMIO alias not implemented");
		return width_mask(dsize);
	}
	if (a >= 0x5000 && a < 0x8000 && (a != VSConfiguration || swap))
	{
		report(
			"UNIMPLEMENTED_WINDOW",
			a,
			0,
			"Video streams, VGA proxy and reserved windows are not "
			"implemented");
		return width_mask(dsize);
	}
	if (a >= 0x2000 && a < 0x3000)
	{
		if (dsize != 32)
		{
			report("FIFO_WIDTH", a, dsize, "FIFO requires a 32-bit access");
			return width_mask(dsize);
		}
	}
	else if ((a & 7) != 0 || (dsize != 32 && !(a >= 0x4000 && a < 0x5000)))
	{
		report(
			"REGISTER_HOLE",
			a,
			dsize,
			"Unsupported register lane; no register alias or side effect");
		return width_mask(dsize);
	}
	uint32_t value = register_read(a);
	if (a >= 0x4000 && a < 0x5000)
		value &= 255;
	if (a >= 0x8000 && !m_warned[a / 8])
	{
		m_warned[a / 8] = true;
		report(
			"SHADOW_READ",
			a,
			value,
			"GP read returns modeled state, not verified hardware readback");
	}
	if (swap)
		value = swap32(value);
	return value & width_mask(dsize);
}

bool CPermedia2::WriteMem(uint32_t a, int dsize, uint32_t data)
{
	if (!valid_width(dsize) || a >= RegisterBARSize || (a & (dsize / 8 - 1)))
	{
		report("ACCESS_WIDTH", a, dsize, "Invalid MMIO width or alignment");
		return false;
	}
	const bool swap = (a & 0x10000) != 0;
	a &= 0xffff;
	if (a >= 0x5000 && a < 0x8000 && (a != VSConfiguration || swap))
	{
		report(
			"UNIMPLEMENTED_WINDOW",
			a,
			data,
			"Video-stream/VGA window write not implemented",
			true);
		return false;
	}
	if (swap)
	{
		if (dsize != 32)
		{
			report(
				"SWAP_SUBWORD",
				a,
				data,
				"Subword upper MMIO alias not implemented");
			return false;
		}
		data = swap32(data);
	}
	if (a >= 0x2000 && a < 0x3000)
	{
		if (dsize != 32)
		{
			report("FIFO_WIDTH", a, dsize, "FIFO requires a 32-bit access");
			return false;
		}
		return register_write(a, data);
	}
	if ((a & 7) != 0 || (dsize != 32 && !(a >= 0x4000 && a < 0x5000)))
	{
		report(
			"REGISTER_HOLE",
			a,
			data,
			"Ignored non-register lane; especially GP +4 is not an alias");
		return false;
	}
	if (a >= 0x8000)
	{
		if (m_input.size() == m_options.input_fifo_entries)
		{
			report(
				"INPUT_FULL",
				a,
				data,
				"Input FIFO full; write was not accepted");
			return false;
		}
		m_input.push_back({a, data});
		return true;
	}
	if (a >= 0x4000 && a < 0x5000)
		data &= 255;
	return register_write(a, data);
}

uint32_t CPermedia2::peek(uint32_t a) const
{
	if (a >= 0x10000 || (a & 7))
		throw std::out_of_range("unaligned canonical register");
	return r(a);
}

uint32_t CPermedia2::mem_read(unsigned ap, uint32_t a, int dsize)
{
	if (ap > 1 || !valid_width(dsize) || a > VramSize - dsize / 8)
	{
		report(
			"APERTURE_RANGE", a, dsize, "Invalid framebuffer aperture access");
		return 0xffffffff;
	}
	const uint32_t mode = r(ap ? ApertureTwo : ApertureOne);
	if (mode == 0x200)
	{
		// Only plain ROM routing is implemented. Combined formatting modes
		// are deliberately rejected pending a matching-device test.
		uint32_t v = 0;
		for (unsigned i = 0; i < static_cast<unsigned>(dsize) / 8; ++i)
			v |= uint32_t(m_rom_reader ? m_rom_reader((a + i) & 0xffffu) : 0xff)
				<< (i * 8);
		return v;
	}
	if ((mode & ~3u) || (mode & 3u) == 3)
	{
		report(
			"APERTURE_MODE",
			a,
			mode,
			"Reserved/packed/SVGA/combined aperture mode is not implemented",
			true);
		return width_mask(dsize);
	}
	const unsigned lane = (mode == 1) ? 3u : (mode == 2) ? 2u : 0u;
	uint32_t v = 0;
	for (unsigned i = 0; i < static_cast<unsigned>(dsize) / 8; ++i)
		v |= uint32_t(m_vram[(a + i) ^ lane]) << (i * 8);
	return v;
}

void CPermedia2::mem_write(unsigned ap, uint32_t a, int dsize, uint32_t data)
{
	if (ap > 1 || !valid_width(dsize) || a > VramSize - dsize / 8)
	{
		report(
			"APERTURE_RANGE", a, data, "Invalid framebuffer aperture access");
		return;
	}
	const uint32_t mode = r(ap ? ApertureTwo : ApertureOne);
	if (mode == 0x200)
	{
		report(
			"ROM_READ_ONLY",
			a,
			data,
			"ROM programming not implemented; no VRAM or firmware "
			"modification");
		return;
	}
	if ((mode & ~3u) || (mode & 3u) == 3)
	{
		report(
			"APERTURE_MODE",
			a,
			mode,
			"Reserved/packed/SVGA/combined aperture mode is not implemented",
			true);
		return;
	}
	const unsigned lane = (mode == 1) ? 3u : (mode == 2) ? 2u : 0u;
	for (unsigned i = 0; i < static_cast<unsigned>(dsize) / 8; ++i)
	{
		const uint32_t address = (a + i) ^ lane;
		const uint8_t mask = uint8_t(r(BypassWriteMask) >> ((address & 3) * 8));
		m_vram[address] =
			uint8_t((m_vram[address] & ~mask) | ((data >> (i * 8)) & mask));
	}
}

// Command processing and interrupts

bool CPermedia2::packet_word(uint32_t value)
{
	if (m_decoder.remaining == 0)
	{
		const uint32_t mode = (value >> 14) & 3;
		// Delta vertex/setup registers occupy groups 0x20..0x26, requiring
		// ten-bit tags (SLAU011A, Table 8-1), despite the nine-bit
		// description in section 2.3.4. Bits 10..13 remain reserved.
		if (mode == 3 || (value & 0x3c00u))
		{
			report(
				"FIFO_PACKET",
				FIFO,
				value,
				"Reserved FIFO packet format",
				true);
			return false;
		}
		m_decoder.mode = mode;
		m_decoder.tag = value & 0x3ff;
		m_decoder.mask = value >> 16;
		if (mode == 2)
		{
			m_decoder.tag &= 0x3f0;
			m_decoder.remaining = 0;
			for (uint32_t m = m_decoder.mask; m; m >>= 1)
				m_decoder.remaining += m & 1;
		}
		else
			m_decoder.remaining = (value >> 16) + 1;
		return true;
	}
	if (m_input.size() == m_options.input_fifo_entries)
	{
		report(
			"INPUT_FULL",
			FIFO,
			value,
			"Formatted FIFO data not accepted; decoder remains unchanged");
		return false;
	}
	uint32_t tagv = m_decoder.tag;
	if (m_decoder.mode == 2)
	{
		unsigned bit = 0;
		while (((m_decoder.mask >> bit) & 1) == 0 && bit < 16)
			++bit;
		tagv += bit;
		m_decoder.mask &= ~(1u << bit);
	}
	m_input.push_back({0x8000 + tagv * 8, value});
	--m_decoder.remaining;
	if (m_decoder.mode == 1)
		m_decoder.tag = (m_decoder.tag + 1) & 0x3ff;
	return true;
}

bool CPermedia2::execute(const Command& c)
{
	const uint32_t a = c.address, v = c.value;
	if (!register_write(a, v))
		return false;
	if (register_name(a) == std::string("REGISTER") && !m_warned[a / 8])
	{
		m_warned[a / 8] = true;
		report(
			"STATE_SHADOW",
			a,
			v,
			"State write retained; inspect capability documentation before "
			"assuming functional "
			"support");
	}
	// Unrecognized state writes are retained for trace/readback, not evidence of
	// support. Known non-2D modes are checked when a Render consumes state.
	return true;
}

void CPermedia2::output_push(uint32_t value, bool interrupt)
{
	if (m_output.size() >= 8)
		throw std::logic_error("output capacity invariant");
	m_output.push_back({value, interrupt});
	update_irq();
}

void CPermedia2::signal_error(uint32_t flags)
{
	r(ErrorFlags) |= flags;
	r(IntFlags) |= IRQ_ERROR;
	update_irq();
}

void CPermedia2::update_irq()
{
	if (!m_output.empty() && m_output.front().interrupt)
	{
		r(IntFlags) |= IRQ_SYNC;
		m_output.front().interrupt =
			false; // latch once on arrival at FIFO HEAD
	}
	const bool level = (r(IntFlags) & r(IntEnable)) != 0;
	if (level != m_irq)
	{
		m_irq = level;
		if (m_irq_callback)
			m_irq_callback(m_irq);
	}
}

bool CPermedia2::busy() const
{
	return m_job.active || !m_input.empty() || r(DMACount) != 0 ||
		r(OutDMACount) != 0 || m_waiting_frame;
}

size_t CPermedia2::service(size_t budget)
{
	size_t used = 0;
	while (used < budget && !m_halted)
	{
		bool progress = false;
		if (m_waiting_frame && m_frame_number >= m_wait_frame)
			m_waiting_frame = false;
		if (!m_waiting_frame)
		{
			if (m_job.active)
				progress = draw_step();
			else if (!m_input.empty())
			{
				const Command cmd = m_input.front();
				if (execute(cmd))
				{
					m_input.pop_front();
					progress = true;
				}
			}
		}
		if (m_halted)
			break;
		// Drain at most one output word per quantum, even while graphics is
		// blocked by a full output FIFO. Commit progress only after the PCI
		// callback succeeds, preserving the exact pending word on failure.
		if (r(OutDMACount) && !m_output.empty())
		{
			const uint32_t value = m_output.front().value;
			uint8_t b[4];
			for (unsigned i = 0; i < 4; ++i)
				b[i] = uint8_t(
					value >> (8 * ((r(DMAControl) & 0x10) ? 3 - i : i)));
			if (m_out_dma_cursor > UINT32_MAX - 3 || !m_dma_writer ||
				!m_dma_writer(m_out_dma_cursor, b, sizeof(b)))
			{
				signal_error(1u << 7);
				report(
					"DMA_WRITE",
					m_out_dma_cursor,
					4,
					"PCI DMA writer failed or bus mastering disabled",
					true);
				break;
			}
			m_output.pop_front();
			m_out_dma_cursor += 4;
			--r(OutDMACount);
			update_irq();
			// Output completion is polled through OutDMACount. The documented
			// DMA interrupt belongs to input DMA; Sync-at-head is independent.
			progress = true;
		}
		// Fetch at most one DMA word per scheduling quantum. Execution can stall
		// on payload/output space without preventing pending payload DMA arrival.
		if (r(DMACount) && m_input.size() < m_options.input_fifo_entries)
		{
			uint8_t b[4]{};
			if (!m_dma_reader || !m_dma_reader(m_dma_cursor, b, 4))
			{
				report(
					"DMA_READ",
					m_dma_cursor,
					4,
					"PCI DMA reader failed or bus mastering disabled",
					true);
				break;
			}
			const uint32_t value = uint32_t(b[0]) | (uint32_t(b[1]) << 8) |
				(uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
			if (!packet_word(value))
				break;
			m_dma_cursor += 4;
			--r(DMACount);
			progress = true;
			if (r(DMACount) == 0)
			{
				r(IntFlags) |= IRQ_DMA;
				update_irq();
			}
		}
		if (!progress)
			break;
		++r(Count);
		++used;
	}
	return used;
}

// Rasterizer and framebuffer

uint32_t CPermedia2::pitch_from_products(uint32_t mode)
{
	uint32_t out = 0;
	for (unsigned shift = 0; shift < 9; shift += 3)
	{
		const uint32_t p = (mode >> shift) & 7;
		if (p)
			out += 1u << (p + 4);
	}
	return out;
}

unsigned CPermedia2::render_bytes() const
{
	switch (r(FBReadPixel) & 7)
	{
	case 0:
		return 1;
	case 1:
		return 2;
	case 2:
		return 4;
	case 4:
		return 3;
	default:
		return 0;
	}
}

unsigned CPermedia2::texture_bytes() const
{
	switch ((r(TextureMapFormat) >> 19) & 7)
	{
	case 0:
		return 1;
	case 1:
		return 2;
	case 2:
		return 4;
	case 4:
		return 3;
	default:
		return 0;
	}
}

bool CPermedia2::validate_texture_block(uint32_t value)
{
	// Disabled texture reads leave the rasterizer's block mask unchanged
	// (SLAU011A, sections 4.4.6 and 4.9.7). Direct-index LUT fills generate
	// block colors without memory reads and still require validation.
	if (!(r(TextureReadMode) & 1) && (r(TextureLUTMode) & 3) != 3)
		return true;

	// Bound section 4.9.7's cached-font operation to forward rectangles and a
	// linear one-dimensional mask stream.
	if ((value & 0xc0) != PrimitiveRectangle ||
		(value & (PositiveX | PositiveY)) != (PositiveX | PositiveY) ||
		(value & (SyncMask | SyncHost)) || (r(FBReadMode) & Packed))
	{
		report(
			"TEXTURE_BLOCK_GEOMETRY",
			Render,
			value,
			"Textured block fills require a forward unpacked rectangle without "
			"host streams",
			true);
		return false;
	}
	const uint32_t read_mode = r(TextureReadMode);
	const unsigned bytes = texture_bytes();
	if (r(TextureAddressMode) != 1 || (read_mode & ~0x1fe1fu) ||
		(read_mode & 0x1f) != 0x0b || ((read_mode >> 9) & 15) > 11 ||
		((read_mode >> 13) & 15) > 11 || (r(TextureLUTMode) & 3) ||
		(r(TextureMapFormat) & ~0x3901ffu) ||
		(r(TextureDataFormat) & ~0x200u) ||
		(r(TextureBaseAddress) & 0xff000000u) || bytes == 0)
	{
		report(
			"TEXTURE_BLOCK_MODE",
			TextureReadMode,
			read_mode,
			"Only local linear repeat-addressed block masks without LUT, "
			"filtering or color conversion are implemented",
			true);
		return false;
	}
	// S/T have 18 significant fractional bits aligned above two unused low
	// bits, so the register representation has its integer boundary at bit 20.
	if ((r(SStart) & 0xfffffu) ||
		(r(dSdx) != 0x100000u && r(dSdx) != 0xfff00000u) || r(dSdyDom) != 0 ||
		r(TStart) != 0 || r(dTdx) != 0 || r(dTdyDom) != 0)
	{
		report(
			"TEXTURE_BLOCK_DDA",
			SStart,
			r(SStart),
			"Block masks require integral S, a unit S step per block and zero "
			"T/Y derivatives",
			true);
		return false;
	}
	if ((r(RectangleSize) & 0xffff) > 32 && bytes != 4)
	{
		report(
			"TEXTURE_BLOCK_WIDTH",
			RectangleSize,
			r(RectangleSize),
			"Block masks wider than 32 pixels require 32-bit texels",
			true);
		return false;
	}
	return true;
}

bool CPermedia2::validate_texture_copy(uint32_t value)
{
	// The initial indexed copy uses a linear CI8 map and one texel per native
	// framebuffer pixel (SLAU011A, sections 4.9.1/4.9.4/4.13.1).
	const uint32_t read_mode = r(TextureReadMode);
	const uint32_t format =
		((r(DitherMode) >> 2) & 15) | ((r(DitherMode) >> 12) & 16);
	const uint32_t logical_mode = r(LogicalOpMode);
	const unsigned logical_op = (logical_mode >> 1) & 15;
	const bool destination_op = (logical_mode & 1) &&
		logical_op != 0 && logical_op != 3 && logical_op != 12 &&
		logical_op != 15;
	// Non-dithered CI8 formatting preserves the texture index in either color
	// order. The ordinary logical-op unit then combines it with the destination
	// (SLAU011A, sections 4.9.4/4.15.1 and the DitherMode format table).
	if ((value & 0xc0) != PrimitiveTrapezoid ||
		(value & (FastFill | SyncMask | SyncHost | 1u)) ||
		r(TextureAddressMode) != 1 || r(TextureColorMode) != 7 ||
		r(TextureLUTMode) != 0 ||
		(r(TextureDataFormat) & ~0x20u) != 14 ||
		(r(TextureMapFormat) & ~0x1ffu) ||
		pitch_from_products(r(TextureMapFormat)) == 0 ||
		(r(TextureBaseAddress) & 0xff000000u) ||
		(read_mode & ~0x1fe01u) || !(read_mode & 1) ||
		((read_mode >> 9) & 15) > 11 || ((read_mode >> 13) & 15) > 11 ||
		render_bytes() != 1 || (r(ColorDDAMode) & 1) ||
		(r(DepthMode) & 1) || (r(LBWriteMode) & 1) ||
		((r(DitherMode) & 1) && format != 14) || (logical_mode & 0x20) ||
		(r(FBReadMode) & (ReadSource | Packed | 0x48000u)) ||
		(r(FBWriteConfig) & 0x40000u) ||
		((r(FBReadMode) & ReadDestination) &&
			((r(FBReadMode) ^ r(FBWriteConfig)) & 0x101ffu)) ||
		((destination_op || r(FBSoftwareWriteMask) != 0xffffffffu) &&
			!(r(FBReadMode) & ReadDestination)))
	{
		report("RENDER_MODE", Render, value,
			"Only linear nearest-clamped CI8 texture copies with native "
			"indexed framebuffer pixels are implemented", true);
		return false;
	}
	const int64_t left = int64_t(sx(r(StartXDom), 32)) / 65536;
	const int64_t right = int64_t(sx(r(StartXSub), 32)) / 65536;
	const int64_t top = int64_t(sx(r(StartY), 32)) / 65536;
	const uint32_t rows = r(RasterCount) & 0xffff;
	if (((r(StartXDom) | r(StartXSub) | r(StartY)) & 0xffff) ||
		r(dXDom) != 0 || r(dXSub) != 0 || r(dY) != 0x10000 ||
		right < left || right - left > 65535 || (r(RasterizerMode) & 0x30) ||
		((r(SStart) | r(TStart)) & 0xfffff) || r(dSdx) != 0x100000 ||
		r(dSdyDom) != 0 || r(dTdx) != 0 || r(dTdyDom) != 0x100000)
	{
		report("TEXTURE_COPY_GEOMETRY", Render, value,
			"Indexed texture copies require an integral forward trapezoid "
			"and unit S/T steps", true);
		return false;
	}
	// Limit clipping can alter rasterizer interpolation. Admit only a complete
	// footprint inside the limits until that continuation behavior is modeled.
	if ((r(RasterizerMode) & 0x40000) && rows != 0 && right != left &&
		(left < sx(r(XLimits), 12) || right > sx(r(XLimits) >> 16, 12) ||
			top < sx(r(YLimits), 12) ||
			top + rows > sx(r(YLimits) >> 16, 12)))
	{
		report("TEXTURE_COPY_LIMITS", RasterizerMode, r(RasterizerMode),
			"Indexed texture-copy footprint must lie inside active X/Y limits",
			true);
		return false;
	}
	if ((r(ScissorMode) & 2) &&
		(sx(r(WindowOrigin), 12) || sx(r(WindowOrigin) >> 16, 12)))
	{
		report("TEXTURE_COPY_SCISSOR", WindowOrigin, r(WindowOrigin),
			"Translated screen scissoring is not implemented for texture copies",
			true);
		return false;
	}
	return true;
}

bool CPermedia2::texture_copy_color(uint32_t& value)
{
	const unsigned width_bits = (r(TextureReadMode) >> 9) & 15;
	const unsigned height_bits = (r(TextureReadMode) >> 13) & 15;
	const unsigned pitch = pitch_from_products(r(TextureMapFormat));
	if (width_bits > 11 || height_bits > 11 || pitch == 0)
	{
		report("TEXTURE_COPY_LAYOUT", TextureMapFormat, r(TextureMapFormat),
			"Invalid active indexed texture layout", true);
		return false;
	}
	// Integral unit gradients let row/col preserve the complete texture DDA
	// position, including across snapshots, without additional hidden state.
	const int64_t s = int64_t(sx(r(SStart), 32)) / 0x100000 + m_job.col;
	const int64_t t = int64_t(sx(r(TStart), 32)) / 0x100000 + m_job.row;
	const int64_t sc = std::max<int64_t>(0,
		std::min<int64_t>((int64_t(1) << width_bits) - 1, s));
	const int64_t tc = std::max<int64_t>(0,
		std::min<int64_t>((int64_t(1) << height_bits) - 1, t));
	const int64_t address = int64_t(r(TextureBaseAddress)) + tc * pitch + sc;
	if (address < 0 || address >= int64_t(VramSize))
	{
		report("TEXTURE_COPY_RANGE", TextureBaseAddress, r(TextureBaseAddress),
			"Indexed texture source lies outside local VRAM", true);
		return false;
	}
	value = m_vram[static_cast<size_t>(address)];
	return true;
}

bool CPermedia2::load_texture_mask()
{
	// Recheck the mask layout before deriving a source address.
	if (!validate_texture_block(m_job.command))
		return false;
	const unsigned bytes = texture_bytes();
	// Section 4.9.7 advances S once per 32-pixel block. NT4's captured font
	// commands use dSdyDom=0; model that stream continuously across rows,
	// rather than restarting at SStart for each glyph scanline. This derives
	// progress from the existing Job, keeping snapshot layout unchanged.
	const uint64_t blocks_per_row = (uint64_t(m_job.columns) + 31) / 32;
	const int64_t block =
		int64_t(uint64_t(m_job.row) * blocks_per_row + m_job.col / 32);
	const int64_t s = int64_t(sx(r(SStart), 32)) / 0x100000 +
		block * (int64_t(sx(r(dSdx), 32)) / 0x100000);
	const int64_t width = int64_t(1) << ((r(TextureReadMode) >> 9) & 15);
	const int64_t texel = (s % width + width) % width;
	const int64_t address =
		(int64_t(r(TextureBaseAddress) & 0xffffffu) + texel) * bytes;
	if (address < 0 || address > int64_t(VramSize - bytes))
	{
		report(
			"TEXTURE_BLOCK_RANGE",
			TextureBaseAddress,
			r(TextureBaseAddress),
			"Block-mask source is outside local VRAM",
			true);
		return false;
	}
	uint32_t mask = pixel_read(address, bytes);
	if (r(TextureDataFormat) & 0x200)
	{
		// SpanFormat mirrors each byte; it does not reverse byte order or use
		// RasterizerMode's independent host BitMaskPattern controls.
		mask = ((mask & 0x55555555u) << 1) | ((mask >> 1) & 0x55555555u);
		mask = ((mask & 0x33333333u) << 2) | ((mask >> 2) & 0x33333333u);
		mask = ((mask & 0x0f0f0f0fu) << 4) | ((mask >> 4) & 0x0f0f0f0fu);
	}
	// Fetch once before writing any destination pixel. Source/destination
	// overlap and a snapshot within this block must retain the fetched mask.
	m_job.payload = mask;
	m_job.payload_left = std::min(32u, m_job.columns - m_job.col);
	return true;
}

bool CPermedia2::vertex_write(uint32_t address, uint32_t value)
{
	const unsigned parameter = ((address - V0Fixed) / 8) & 15;
	if (parameter == 13 || parameter == 15)
		return false;
	const bool fixed = address < V0Float;
	const uint32_t canonical = fixed ? address + (V0Float - V0Fixed) : address;
	const uint32_t vertex = canonical - parameter * 8;
	if (parameter == 14)
	{
		// Both address aliases carry the same packed color layout. Individual
		// component reads return the shared floating-point vertex storage.
		r(canonical) = r(canonical - (V0Float - V0Fixed)) = value;
		for (unsigned i = 0; i < 4; ++i)
		{
			const unsigned lane = (r(DeltaMode) & 0x40000) && i != 1 && i != 3
				? 2 - i : i;
			const uint32_t component = vertex_bits(float((value >> (lane * 8)) & 255) / 255);
			r(vertex + (5 + i) * 8) = component;
			r(vertex - (V0Float - V0Fixed) + (5 + i) * 8) = component;
		}
		return true;
	}
	float converted = vertex_float(value);
	if (fixed)
	{
		if (parameter == 10 || parameter == 11)
			converted = float(sx(value, 32) / 65536.0);
		else if (parameter == 3 || parameter == 4)
			converted = float(double(value & 0xffffffu) / 4194304.0);
		else if (parameter == 9)
			converted = float(sx(value, 32) / 4194304.0);
		else if (parameter <= 2)
			converted = float(sx(value, 32) / 1073741824.0);
		else
			converted = float(double(value & 0x7fffffffu) / 1073741824.0);
	}
	if (std::isfinite(converted))
	{
		if (parameter <= 2)
		{
			if (((r(DeltaMode) >> 14) & 3) == 1)
				converted = std::max(-1.0f, std::min(1.0f, converted));
		}
		else if (r(DeltaMode) & 0x2000)
		{
			const float minimum = parameter == 9 ? -512.0f
				: parameter == 10 || parameter == 11 ? -32768.0f : 0.0f;
			const float maximum = parameter == 3 ? 2.0f : parameter == 9 ? 512.0f
				: parameter == 10 || parameter == 11 ? std::nextafter(32768.0f, 0.0f) : 1.0f;
			converted = std::max(minimum, std::min(maximum, converted));
		}
	}
	// Fixed and floating ports name one vertex component. Setup sorting must
	// never alter these values; software reuses them for strips and fans.
	r(canonical) = r(canonical - (V0Float - V0Fixed)) = vertex_bits(converted);
	return true;
}

void CPermedia2::draw_triangle(uint32_t value)
{
	const uint32_t mode = r(DeltaMode);
	// Delta generates the ordinary rasterizer/color/depth register stream.
	// Texture, fog, setup-only and line setup require their own calculations.
	const uint32_t delta_controls = 3u | 0xcu | 0xc0u | 0x400u | 0x800u |
		0x2000u | 0xc000u | 0x20000u | 0x40000u | 0x80000u;
	if ((mode & ~delta_controls) || (mode & 3) == 1 || (mode & 3) == 3 ||
		(value & ~(0x40u | 1u | 0x10000u | 0x80000u | 0x100000u)) ||
		((mode & 0x20000) && ((mode | value) & 0x80000)) ||
		(value & 0xc0) != PrimitiveTrapezoid ||
		!(value & 0x10000) || !(mode & 0x400) ||
		(r(RasterizerMode) & 0x30) || (r(FBReadMode) & Packed))
	{
		report("DELTA_MODE", DrawTriangle, value,
			"Only untextured subpixel-corrected color/depth triangle setup with zero "
			"coordinate bias is implemented", true);
		return;
	}
	const bool smooth = (mode & 0x40) != 0;
	const bool depth = (mode & 0x80) != 0;
	const unsigned depth_format = (mode >> 2) & 3;
	if (((r(ColorDDAMode) & 3) == 3 && !smooth) ||
		((r(DepthMode) & 1) && ((r(DepthMode) >> 2) & 3) == 0 && !depth) ||
		(depth && (depth_format > 1 ||
			((r(DepthMode) & 1) && r(LBReadFormat) != (depth_format ? 0u : 3u)))))
	{
		report("DELTA_INTERPOLATION", DeltaMode, mode,
			"Triangle setup must supply the enabled color/depth DDAs and "
			"match the local-buffer depth width", true);
		return;
	}
	// The common driver also emits the Gamma-only Delta coordinate-bias bit
	// and a legacy strip-orientation bit in Draw. Decode the PM2 fields here;
	// combinations with enabled face culling remain outside this setup path.
	const uint32_t command = value & ~(0x80000u | 0x100000u);
	if (!validate_render(command, true))
		return;
	struct Vertex
	{
		double x, y, z, color[3], alpha;
	};
	Vertex vertex[3]{};
	for (unsigned i = 0; i < 3; ++i)
	{
		const uint32_t base = V0Float + i * 0x80;
		vertex[i].x = vertex_float(r(base + 10 * 8));
		vertex[i].y = vertex_float(r(base + 11 * 8));
		vertex[i].z = vertex_float(r(base + 12 * 8));
		vertex[i].alpha = vertex_float(r(base + 8 * 8));
		bool valid = std::isfinite(vertex[i].x) && std::isfinite(vertex[i].y) &&
			vertex[i].x >= -2048 && vertex[i].x < 2048 &&
			vertex[i].y >= -2048 && vertex[i].y < 2048;
		if (depth)
			valid = valid && std::isfinite(vertex[i].z) &&
				vertex[i].z >= 0 && vertex[i].z <= 1;
		if (smooth)
		{
			valid = valid && std::isfinite(vertex[i].alpha) &&
				vertex[i].alpha >= 0 && vertex[i].alpha <= 1;
			for (unsigned c = 0; c < 3; ++c)
			{
				vertex[i].color[c] = vertex_float(r(base + (5 + c) * 8));
				valid = valid && std::isfinite(vertex[i].color[c]) &&
					vertex[i].color[c] >= 0 && vertex[i].color[c] <= 1;
			}
		}
		if (!valid)
		{
			report("DELTA_VERTEX", base, r(base + 10 * 8),
				"Triangle vertex exceeds the modeled rasterizer/color/depth range",
				true);
			return;
		}
	}
	if (smooth && (vertex[0].alpha != vertex[1].alpha ||
		vertex[0].alpha != vertex[2].alpha))
	{
		report("DELTA_ALPHA", DrawTriangle, value,
			"Triangle setup with varying vertex alpha is not implemented", true);
		return;
	}
	const double area = (vertex[1].x - vertex[0].x) *
		(vertex[2].y - vertex[0].y) - (vertex[2].x - vertex[0].x) *
		(vertex[1].y - vertex[0].y);
	if (area == 0 || ((mode & 0x20000) &&
		((value & 0x100000) ? area < 0 : area > 0)))
	{
		m_job = {};
		return;
	}
	std::stable_sort(std::begin(vertex), std::end(vertex),
		[](const Vertex& a, const Vertex& b) { return a.y < b.y; });
	const Vertex& a = vertex[0];
	const Vertex& b = vertex[1];
	const Vertex& c = vertex[2];
	const double long_slope = (c.x - a.x) / (c.y - a.y);
	const double first_slope = b.y == a.y ? 0 : (b.x - a.x) / (b.y - a.y);
	const double second_slope = c.y == b.y ? 0 : (c.x - b.x) / (c.y - b.y);
	const auto raster_y = [](double y) {
		return fixed_integer(int64_t(std::trunc(y * 65536)) + 0x7fff);
	};
	const int64_t first_y = raster_y(a.y), middle_y = raster_y(b.y);
	const int64_t last_y = raster_y(c.y);
	const uint32_t first_rows = uint32_t(middle_y - first_y);
	const uint32_t second_rows = uint32_t(last_y - middle_y);
	if (first_rows == 0 && second_rows == 0)
	{
		m_job = {};
		return;
	}
	const bool correction = (value & 0x10000) && (mode & 0x400);
	const int64_t start_y = first_rows ? first_y : middle_y;
	const double sample_y = double(start_y) + 0.5;
	const double error_y = correction ? sample_y - a.y : 0;
	const double xdom = a.x + error_y * long_slope;
	const double xsub = first_rows ? a.x + error_y * first_slope
		: b.x + (correction ? sample_y - b.y : 0) * second_slope;
	const bool split = first_rows && second_rows;
	const double knee_x = split ? b.x +
		(correction ? double(middle_y) + 0.5 - b.y : 0) * second_slope : 0;
	// Pixel-center coverage uses the nearly-half bias before integer DDA
	// extraction. Sorting is private; the reusable vertex registers are intact.
	const auto coordinate = [](double x, int64_t& result) {
		// PM2's signed 12.15 DDAs are aligned to 16.16; bit zero is unused.
		const double fixed = std::trunc(x * 32768) * 2;
		if (!std::isfinite(fixed) || fixed < -134217728 || fixed > 134217726)
			return false;
		result = int64_t(fixed);
		return true;
	};
	int64_t start_dom, start_sub, delta_dom, delta_sub;
	int64_t next_sub = 0, next_delta = 0;
	// A half with no scanlines generates no continuation. Its unused edge
	// slope can exceed the DDA range even when the rendered half fits.
	if (!coordinate(xdom, start_dom) ||
		!coordinate(xsub, start_sub) ||
		!coordinate(long_slope, delta_dom) ||
		!coordinate(first_rows ? first_slope : second_slope, delta_sub) ||
		(split && (!coordinate(knee_x, next_sub) ||
			!coordinate(second_slope, next_delta))) ||
		start_y < -2048 || last_y > 2048 ||
		first_rows > 65535 || second_rows > 65535)
	{
		report("DELTA_RANGE", DrawTriangle, value,
			"Triangle setup exceeds the modeled fixed-point DDA range", true);
		return;
	}
	const auto edge_range = [](int64_t start, int64_t delta, uint32_t rows) {
		if (rows == 0)
			return true;
		const int64_t end = start + delta * (rows - 1);
		return start + 0x7fff >= -134217728 && start + 0x7fff < 134217728 &&
			end + 0x7fff >= -134217728 && end + 0x7fff < 134217728;
	};
	if (!edge_range(start_dom, delta_dom, first_rows + second_rows) ||
		!edge_range(start_sub, delta_sub, first_rows ? first_rows : second_rows) ||
		(split && !edge_range(next_sub, next_delta, second_rows)))
	{
		report("DELTA_RANGE", DrawTriangle, value,
			"Triangle edge exceeds the modeled rasterizer range", true);
		return;
	}
	// X limits accelerate rasterization but are not a true scissor (SLAU011A,
	// section 4.4.13). When the user scissor is enclosed by the limits, retain
	// the complete edge walk and interpolants; the scissor rejects fragments
	// before local-buffer or framebuffer writes. Other limit-crossing cases
	// still require the rasterizer's X termination behavior.
	if (r(RasterizerMode) & 0x40000)
	{
		const double left = std::min(a.x, std::min(b.x, c.x));
		const double right = std::max(a.x, std::max(b.x, c.x));
		const int32_t minimum = sx(r(XLimits), 12);
		const int32_t maximum = sx(r(XLimits) >> 16, 12);
		const bool scissored = (r(ScissorMode) & 1) &&
			sx(r(ScissorMin), 16) >= minimum &&
			sx(r(ScissorMax), 16) <= maximum;
		if (!scissored && (raster_y(left) < minimum || raster_y(right) > maximum))
		{
			report("DELTA_LIMITS", XLimits, r(XLimits),
				"Limit-crossing triangle setup requires a user scissor inside "
				"the active rasterizer X limits",
				true);
			return;
		}
	}
	const double determinant = (b.x - a.x) * (c.y - a.y) -
		(c.x - a.x) * (b.y - a.y);
	const double direction = determinant > 0 ? 1 : -1;
	int64_t colors[3]{}, dx[3]{}, dy[3]{}, z = 0, zdx = 0, zdy = 0;
	const auto interpolate = [&](double av, double bv, double cv, double scale,
		int64_t minimum, int64_t maximum, int64_t& start, int64_t& xstep,
		int64_t& ystep) {
		const double gradient_x = ((bv - av) * (c.y - a.y) -
			(cv - av) * (b.y - a.y)) / determinant;
		const double gradient_y = ((b.x - a.x) * (cv - av) -
			(c.x - a.x) * (bv - av)) / determinant;
		const double dominant = gradient_y + long_slope * gradient_x;
		const double values[3] = { (av + error_y * dominant) * scale,
			gradient_x * direction * scale, dominant * scale };
		int64_t fixed[3]{};
		for (unsigned i = 0; i < 3; ++i)
		{
			const double v = std::trunc(values[i] * 2048);
			if (!std::isfinite(v))
				return false;
			// Delta clamps its float-to-fixed conversions (3Dlabs, Hot Chips
			// 1996, page 23). Generated color/depth fields are signed 9.11 /
			// 17.11; this saturation is separate from vertex input clamping
			// and from the rasterizer's subsequent fixed-point arithmetic.
			fixed[i] = int64_t(std::max(double(minimum), std::min(double(maximum), v)));
		}
		start = fixed[0]; xstep = fixed[1]; ystep = fixed[2];
		return true;
	};
	bool valid = true;
	if (smooth)
		for (unsigned i = 0; i < 3; ++i)
			valid = valid && interpolate(a.color[i], b.color[i], c.color[i],
				255, -524288, 524287, colors[i], dx[i], dy[i]);
	if (depth)
		valid = valid && interpolate(a.z, b.z, c.z,
			depth_format ? 65535 : 32767, -134217728, 134217727, z, zdx, zdy);
	if (!valid)
	{
		report("DELTA_INTERPOLANT_RANGE", DrawTriangle, value,
			"Triangle interpolation produced a non-finite value",
			true);
		return;
	}
	// Delta supplies the edge bias in its generated coordinates. The
	// rasterizer's separate BiasCoordinates field therefore stays zero.
	r(StartXDom) = uint32_t(start_dom + 0x7fff);
	r(StartXSub) = uint32_t(start_sub + 0x7fff);
	r(StartY) = uint32_t(start_y * 65536);
	r(dXDom) = uint32_t(delta_dom);
	r(dXSub) = uint32_t(delta_sub);
	r(dY) = 65536;
	r(RasterCount) = first_rows ? first_rows : second_rows;
	if (smooth)
	{
		for (unsigned i = 0; i < 3; ++i)
		{
			r(RStart + i * 24) = uint32_t(colors[i]) << 4;
			r(dRdx + i * 24) = uint32_t(dx[i]) << 4;
			r(dRdyDom + i * 24) = uint32_t(dy[i]) << 4;
		}
		r(AStart) = uint32_t(std::trunc(a.alpha * 255 * 2048)) << 4;
	}
	if (depth)
	{
		r(ZStartU) = uint32_t(z >> 11) & 0x1ffff;
		r(ZStartL) = uint32_t(z) << 21;
		r(dZdxU) = uint32_t(zdx >> 11) & 0x1ffff;
		r(dZdxL) = uint32_t(zdx) << 21;
		r(dZdyDomU) = uint32_t(zdy >> 11) & 0x1ffff;
		r(dZdyDomL) = uint32_t(zdy) << 21;
	}
	r(Render) = command;
	start_render(command, true);
	if (m_job.active && split)
	{
		m_job.knee_rows = second_rows;
		m_job.knee_xsub = next_sub + 0x7fff;
		m_job.knee_dxsub = next_delta;
	}
}

bool CPermedia2::validate_interpolants(uint32_t value, bool setup)
{
	// Block-write fragments do not visit the color or local-buffer units.
	if (value & FastFill)
		return true;
	const bool gouraud = (r(ColorDDAMode) & 3) == 3;
	const bool depth = (r(DepthMode) & 1) != 0;
	const bool local = depth || (r(LBWriteMode) & 1);
	if (!setup && (gouraud || local) &&
		((r(RasterizerMode) & 0x40000) ||
			((r(ScissorMode) & 2) &&
				(sx(r(WindowOrigin), 12) || sx(r(WindowOrigin) >> 16, 12)))))
	{
		report("INTERPOLATION_CLIP", Render, value,
			"Rasterizer limits and translated screen scissoring are not "
			"implemented for color/depth interpolation", true);
		return false;
	}
	if ((r(ColorDDAMode) & 1) && (r(ColorDDAMode) & ~3u))
	{
		report("COLOR_DDA_MODE", ColorDDAMode, r(ColorDDAMode),
			"Unknown enabled color DDA control", true);
		return false;
	}
	if ((gouraud || local) &&
		((value & (SyncHost | SyncMask)) || (r(FBReadMode) & Packed) ||
			(r(FBReadMode) & ReadSource) || framebuffer_upload(value)))
	{
		report("INTERPOLATION_MODE", Render, value,
			"Host streams, packed pixels and framebuffer copies/uploads are "
			"not implemented with interpolated color or depth", true);
		return false;
	}
	if (gouraud && (value & 0xc0) == PrimitiveRectangle)
	{
		report("COLOR_DDA_PRIMITIVE", Render, value,
			"Gouraud rectangle interpolation is not implemented", true);
		return false;
	}
	if (!local)
		return true;
	const uint32_t mode = r(LBReadMode);
	const uint32_t read_format = r(LBReadFormat);
	const uint32_t write_format = r(LBWriteFormat);
	const unsigned function = (r(DepthMode) >> 4) & 7;
	const unsigned source = (r(DepthMode) >> 2) & 3;
	const bool write = (r(LBWriteMode) & 1) && !(r(Window) & 0x40000);
	if ((mode & ~(0x1ffu | 0x400u | 0x40000u)) ||
		pitch_from_products(mode) == 0 ||
		(read_format != 0 && read_format != 3) ||
		(write_format != 0 && write_format != 3) ||
		(write && read_format != write_format) ||
		(r(LBWriteMode) & ~1u) || (r(LBWindowBase) & 0xff000000u) ||
		(r(Window) & ~(0x18u | 0x40000u)) ||
		(depth && ((r(DepthMode) & ~0x7fu) || (source != 0 && source != 2))) ||
		(depth && function != 0 && function != 7 && !(mode & 0x400)))
	{
		report("DEPTH_MODE", DepthMode, r(DepthMode),
			"Only linear 15/16-bit local-buffer depth tests with fragment or "
			"constant-register depth are implemented", true);
		return false;
	}
	if (depth && source == 0 && (value & 0xc0) == PrimitiveRectangle)
	{
		report("DEPTH_PRIMITIVE", Render, value,
			"Depth DDA rectangle interpolation is not implemented", true);
		return false;
	}
	// Force-update interactions with a failed test or masked depth are outside
	// this first slice. The ordinary constant-depth clear is unambiguous.
	if (write && (r(Window) & 8) &&
		(!(r(Window) & 16) || !depth || source != 2 || function != 7 ||
			!(r(DepthMode) & 2)))
	{
		report("DEPTH_FORCE_MODE", Window, r(Window),
			"Forced local-buffer updates require a writable constant-depth "
			"clear with an Always comparison", true);
		return false;
	}
	return true;
}

uint32_t CPermedia2::interpolation_kind(uint32_t value) const
{
	if (value & FastFill)
		return 0;
	return ((r(ColorDDAMode) & 3) == 3 ? 1u : 0u) |
		((r(DepthMode) & 1) ? (((r(DepthMode) >> 2) & 3) == 0 ? 2u : 4u) : 0u);
}

void CPermedia2::load_interpolants(bool starts)
{
	if (starts)
		m_job.interpolation = interpolation_kind(m_job.command);
	// Color registers carry signed 9.11 in bits 23..4. Alpha uses the same
	// start format but is not interpolated (SLAU011A, sections 4.12/7.11).
	if (m_job.interpolation & 1)
	{
		for (unsigned i = 0; i < 3; ++i)
		{
			if (starts)
				m_job.color[i] = sx(r(RStart + i * 24) >> 4, 20);
			m_job.dcolor_dx[i] = sx(r(dRdx + i * 24) >> 4, 20);
			m_job.dcolor_dy[i] = sx(r(dRdyDom + i * 24) >> 4, 20);
		}
		if (starts)
		{
			const int32_t alpha = sx(r(AStart) >> 4, 20);
			m_job.alpha = static_cast<uint32_t>(
				std::max(0, std::min(255, alpha / 2048)));
		}
	}
	if (m_job.interpolation & 2)
	{
		// Upper words carry signed 17-bit integers; lower words carry eleven
		// left-justified fractional bits (SLAU011A, pp. 7-59..7-62).
		if (starts)
			m_job.z = int64_t(sx(r(ZStartU), 17)) * 2048 + (r(ZStartL) >> 21);
		m_job.dzdx = int64_t(sx(r(dZdxU), 17)) * 2048 + (r(dZdxL) >> 21);
		m_job.dzdy = int64_t(sx(r(dZdyDomU), 17)) * 2048 + (r(dZdyDomL) >> 21);
	}
}

void CPermedia2::step_interpolants()
{
	if (m_job.interpolation & 1)
		for (unsigned i = 0; i < 3; ++i)
			m_job.color[i] += m_job.dcolor_dy[i];
	if (m_job.interpolation & 2)
		m_job.z += m_job.dzdy;
}

uint32_t CPermedia2::fragment_color() const
{
	if (!(r(ColorDDAMode) & 2))
		return r(ConstantColor);
	uint32_t color = m_job.alpha << 24;
	for (unsigned i = 0; i < 3; ++i)
	{
		// X derivatives are increments along the span, from dominant toward
		// subordinate. Lines use only the dominant-edge derivative.
		int64_t component = m_job.color[i] +
			(m_job.primitive == PrimitiveTrapezoid
					? int64_t(m_job.col) * m_job.dcolor_dx[i]
					: 0);
		if (m_job.setup && (m_job.command & 0x10000))
		{
			const bool forward = m_job.xdom <= m_job.xsub;
			const int64_t first = fixed_integer(m_job.xdom) - (forward ? 0 : 1);
			const int64_t error = first * 65536 + 65535 - m_job.xdom;
			component += fixed_integer(error * m_job.dcolor_dx[i] *
				(forward ? 1 : -1));
		}
		const uint32_t byte = static_cast<uint32_t>(
			std::max<int64_t>(0, std::min<int64_t>(255, component / 2048)));
		color |= byte << (8 * i);
	}
	return color;
}

bool CPermedia2::depth_test(int32_t x, int32_t y)
{
	if (!(r(DepthMode) & 1))
		return true;
	// Keep malformed restored states bounded as well as newly issued draws.
	const uint32_t format = r(LBReadFormat);
	const unsigned pitch = pitch_from_products(r(LBReadMode));
	if ((format != 0 && format != 3) || pitch == 0)
	{
		report("DEPTH_LAYOUT", LBReadMode, r(LBReadMode),
			"Invalid active local-buffer depth layout", true);
		return false;
	}
	const uint32_t mask = format == 3 ? 0x7fff : 0xffff;
	int64_t z = 0;
	if (((r(DepthMode) >> 2) & 3) == 2)
		z = int64_t(r(Depth)) * 2048;
	else
		z = m_job.z + (m_job.primitive == PrimitiveTrapezoid
							 ? int64_t(m_job.col) * m_job.dzdx
							 : 0);
	if (m_job.setup && (m_job.command & 0x10000) &&
		((r(DepthMode) >> 2) & 3) == 0)
	{
		const bool forward = m_job.xdom <= m_job.xsub;
		const int64_t first = fixed_integer(m_job.xdom) - (forward ? 0 : 1);
		const int64_t error = first * 65536 + 65535 - m_job.xdom;
		z += fixed_integer(error * m_job.dzdx * (forward ? 1 : -1));
	}
	// Color saturation is specified; depth overflow is not. Reject values
	// outside the modeled depth range instead of inventing wrap or clamp rules.
	if (z < 0 || z > int64_t(mask) * 2048 + 2047)
	{
		report("DEPTH_RANGE", DepthMode, r(DepthMode),
			"Interpolated depth exceeds the local-buffer depth width", true);
		return false;
	}
	const int64_t sign = (r(LBReadMode) & 0x40000) ? -1 : 1;
	const int64_t pixel = int64_t(r(LBWindowBase)) +
		int64_t(y) * pitch * sign + x;
	const int64_t address = pixel * 2;
	if (address < 0 || address > int64_t(VramSize - 2))
	{
		report("DEPTH_ADDRESS", LBWindowBase, r(LBWindowBase),
			"Local-buffer fragment lies outside VRAM", true);
		return false;
	}
	const uint32_t old = pixel_read(address, 2);
	const uint32_t source = old & mask;
	const uint32_t fragment = static_cast<uint32_t>(z / 2048);
	bool pass = false;
	switch ((r(DepthMode) >> 4) & 7)
	{
	case 0:
		pass = false;
		break;
	case 1:
		pass = fragment < source;
		break;
	case 2:
		pass = fragment == source;
		break;
	case 3:
		pass = fragment <= source;
		break;
	case 4:
		pass = fragment > source;
		break;
	case 5:
		pass = fragment != source;
		break;
	case 6:
		pass = fragment >= source;
		break;
	case 7:
		pass = true;
		break;
	}
	if (pass && (r(DepthMode) & 2) && (r(LBWriteMode) & 1) &&
		!(r(Window) & 0x40000))
	{
		// Depth writes have their own controls; framebuffer software/hardware
		// masks do not replace DepthMode.WriteMask. Preserve non-depth planes.
		const uint32_t value = (old & ~mask) | fragment;
		m_vram[static_cast<size_t>(address)] = uint8_t(value);
		m_vram[static_cast<size_t>(address) + 1] = uint8_t(value >> 8);
	}
	return pass;
}

bool CPermedia2::packed_trapezoid(uint32_t command) const
{
	return (command & 0xc0) == PrimitiveTrapezoid && !(command & FastFill) &&
		(r(FBReadMode) & Packed);
}

bool CPermedia2::validate_packed_trapezoid(
	const Job& job, uint32_t address, uint32_t value)
{
	if (job.row >= job.rows)
		return true;
	const unsigned bytes = render_bytes();
	if (bytes != 1 && bytes != 2 && bytes != 4)
		return false;
	const int64_t scale = 4 / bytes;
	const bool host = (job.command & SyncHost) != 0;
	const int64_t alignment = host ? m_relative_offset : 0;
	const int64_t steps = job.rows - job.row - 1;
	const int64_t a = fixed_integer(job.xdom);
	const int64_t b = fixed_integer(job.xsub);
	const int64_t ae = fixed_integer(job.xdom + job.dxdom * steps);
	const int64_t be = fixed_integer(job.xsub + job.dxsub * steps);
	const int64_t y = fixed_integer(job.y);
	const int64_t ye = fixed_integer(job.y + job.dy * steps);
	const int64_t left = std::min(std::min(a, b), std::min(ae, be));
	const int64_t right = std::max(std::max(a, b), std::max(ae, be));
	const int64_t width = std::max(std::abs(job.xdom - job.xsub),
		std::abs(job.xdom + job.dxdom * steps -
			job.xsub - job.dxsub * steps));
	if (left * scale < INT32_MIN || right * scale > INT32_MAX ||
		left * scale + alignment < INT32_MIN ||
		right * scale + alignment > INT32_MAX ||
		std::min(y, ye) < INT32_MIN || std::max(y, ye) > INT32_MAX ||
		(width + 65535) / 65536 > 65535 ||
		(steps != 0 && job.dy != 65536 && job.dy != -65536))
	{
		report("PACKED_GEOMETRY", address, value,
			"Packed trapezoids require bounded coordinates and unit scanline steps",
			true);
		return false;
	}
	if (host && (job.xdom > job.xsub ||
		job.xdom + job.dxdom * steps > job.xsub + job.dxsub * steps))
	{
		report("PACKED_HOST_DIRECTION", address, value,
			"Reverse-X packed trapezoid host downloads are not implemented", true);
		return false;
	}
	// Limits belong to the rasterizer, before the framebuffer read unit
	// converts DWORD-group X coordinates into native pixels (PRM 4.4.13/6.4.1).
	// Host synchronization automatically disables these limits (PRM 4.4.13).
	if (!host && (r(RasterizerMode) & 0x40000) &&
		(left < sx(r(XLimits), 12) || right > sx(r(XLimits) >> 16, 12) ||
			std::min(y, ye) < sx(r(YLimits), 12) ||
			std::max(y, ye) >= sx(r(YLimits) >> 16, 12)))
	{
		report("PACKED_LIMITS", address, value,
			"Packed trapezoid footprint must lie inside active X/Y limits",
			true);
		return false;
	}
	return true;
}

bool CPermedia2::validate_render(uint32_t value, bool setup)
{
	// Render.Texture qualifies the texture units (SLAU011A, p. 4-79). Their
	// retained mode registers have no effect when this command leaves it clear.
	// Section 4.9.7 defines a separate block-fill mask path. Ordinary texture
	// reads are bounded to the linear indexed copy path validated below.
	const bool texture_block =
		(value & (FastFill | Texture)) == (FastFill | Texture);
	const bool texture_copy = (value & Texture) && !(value & FastFill);
	const uint32_t unsupported = r(AlphaBlendMode) | r(AlphaTestMode) |
		r(StencilMode) | r(FogMode) | r(AntialiasMode) |
		r(YUVMode);
	// Block-write fragments bypass these units and use raw FBBlockColor
	// (SLAU011A, sections 4.4.6 and 4.11.2). Their retained state is inactive.
	if (!(value & FastFill) && ((unsupported & 1) ||
		(r(LogicalOpMode) & ~63u)))
	{
		report(
			"RENDER_MODE",
			Render,
			value,
			"Unsupported blending, alpha/stencil test, fog, antialias, YUV, "
			"texture, dither or logical-operation mode",
			true);
		return false;
	}
	if (!(value & FastFill) && (r(DitherMode) & 3) == 3)
	{
		const uint32_t format =
			((r(DitherMode) >> 2) & 15) | ((r(DitherMode) >> 12) & 16);
		if (format != 16 || (r(DitherMode) & 0x800) || render_bytes() != 2)
		{
			report("DITHER_MODE", DitherMode, r(DitherMode),
				"Only ordered RGB565 dithering with native 16-bit pixels "
				"is implemented", true);
			return false;
		}
	}
	if (!validate_interpolants(value, setup))
		return false;
	if ((r(FBReadMode) | r(FBWriteConfig)) & 0x40000)
	{
		// Subpatch host downloads use native 16/32-bit pixels and a top-left
		// origin. Other patched reads, copies and render operations still need
		// their own addressing and pipeline rules.
		const uint32_t layout = 0x6000000u | 0x40000u | 0x10000u | 0x1ffu;
		if ((r(FBReadMode) & layout) != (r(FBWriteConfig) & layout) ||
			(r(FBWriteConfig) & (0x6000000u | 0x10000u)) != 0x2000000u ||
			!(value & SyncHost) || (value & (FastFill | Texture | SyncMask)) ||
			(r(FBReadMode) & (ReadSource | ReadDestination | Packed)) ||
			(r(ColorDDAMode) & 1) || !(r(FBWriteMode) & 1) ||
			(render_bytes() != 2 && render_bytes() != 4) ||
			pitch_from_products(r(FBWriteConfig)) == 0)
		{
			report("FB_PATCH_MODE", FBWriteConfig, r(FBWriteConfig),
				"Only top-left 16/32-bit Subpatch host downloads are implemented",
				true);
			return false;
		}
	}
	if (texture_block && !validate_texture_block(value))
		return false;
	if (texture_copy && !validate_texture_copy(value))
		return false;
	if ((value & SyncMask) && (value & SyncHost))
	{
		report(
			"DUAL_HOST_STREAM",
			Render,
			value,
			"Combined bitmask and host-color streams not implemented",
			true);
		return false;
	}
	const unsigned bytes = render_bytes();
	const bool depth_only = !(value & FastFill) && (r(DepthMode) & 1) &&
		!(r(FBWriteMode) & 1) &&
		!(r(FBReadMode) & (ReadSource | ReadDestination | Packed));
	if (!depth_only && (bytes == 0 ||
		pitch_from_products(r(FBWriteConfig)) == 0 ||
		((r(FBReadMode) & Packed) && (bytes == 3))))
	{
		report(
			"PIXEL_LAYOUT",
			Render,
			value,
			"Invalid pitch/pixel size or packed 24-bit mode",
			true);
		return false;
	}
	if ((r(FBReadMode) & Packed) && !(value & FastFill) &&
		(value & 0xc0) != PrimitiveRectangle)
	{
		const bool host = (value & SyncHost) != 0;
		if ((value & 0xc0) != PrimitiveTrapezoid ||
			(value & (SyncMask | Texture)) ||
			(host ? (r(FBReadMode) & ReadSource) != 0
				  : !(r(FBReadMode) & ReadSource)) ||
			(r(FBReadMode) & 0x8000) ||
			pitch_from_products(r(FBReadMode)) == 0 ||
			((value & 1) && (r(AreaStippleMode) & 1)) ||
			(r(ColorDDAMode) & 1) || (r(DitherMode) & 1) ||
			(r(LogicalOpMode) & 0x20) || (r(ScissorMode) & 3))
		{
			report("PACKED_PRIMITIVE", Render, value,
				"Only linear raw framebuffer-source copies and host downloads "
				"are implemented for packed trapezoids", true);
			return false;
		}
		const uint32_t mask = r(FBSoftwareWriteMask);
		const bool repeated = bytes == 4 ||
			(bytes == 2 && (mask & 0xffff) * 0x10001u == mask) ||
			(bytes == 1 && (mask & 0xff) * 0x1010101u == mask);
		if (!repeated || (mask != 0xffffffffu &&
			!(r(FBReadMode) & ReadDestination)))
		{
			report("PACKED_MASK", FBSoftwareWriteMask, mask,
				!repeated
					? "Nonreplicated packed trapezoid software masks are not implemented"
					: "Software writemasking requires framebuffer destination reads",
				true);
			return false;
		}
		if (host)
		{
			const uint32_t logical_mode = r(LogicalOpMode);
			const unsigned op = (logical_mode >> 1) & 15;
			const bool destination_op = (logical_mode & 1) &&
				op != 0 && op != 3 && op != 12 && op != 15;
			if ((destination_op && !(r(FBReadMode) & ReadDestination)) ||
				((r(FBReadMode) & ReadDestination) &&
					((r(FBReadMode) ^ r(FBWriteConfig)) & 0x101ffu)))
			{
				report("PACKED_HOST_READ", FBReadMode, r(FBReadMode),
					"Packed host logical operations require matching destination reads",
					true);
				return false;
			}
			if (std::abs(m_relative_offset) >= int(4 / bytes))
			{
				report("HOST_ALIGNMENT", FBReadMode, r(FBReadMode),
					"Whole-word packed trapezoid host alignment is not implemented",
					true);
				return false;
			}
		}
	}
	// FBColor destination reads go directly to Host Out, independently of
	// FBWriteMode.WriteEnable (SLAU011A, Table 4-18). Formatted uploads and
	// combined read/write pipelines require additional ordering/format rules.
	if (!(value & FastFill) && (r(FBReadMode) & 0x8000) &&
		(r(FBReadMode) & (ReadSource | ReadDestination)))
	{
		if (!framebuffer_upload(value) || (r(FBReadMode) & ReadSource) ||
			(r(DitherMode) & 1) || (r(FBWriteMode) & 1) ||
			(value & (FastFill | Texture | SyncMask | SyncHost)) ||
			((value & 1) && (r(AreaStippleMode) & 1)) ||
			((r(FBReadMode) & Packed) && (r(ScissorMode) & 3)) ||
			(!(r(FBReadMode) & Packed) && bytes != 4))
		{
			report(
				"FB_UPLOAD_MODE",
				FBReadMode,
				r(FBReadMode),
				"Only raw destination uploads with disabled framebuffer writes "
				"are implemented; "
				"formatting, packed scissoring, stippled and unpacked "
				"narrow-pixel uploads "
				"are unsupported",
				true);
			return false;
		}
	}
	const bool constant_fb_data = !framebuffer_upload(value) &&
		!(value & FastFill) && (r(LogicalOpMode) & 0x20);
	if (constant_fb_data)
	{
		// FBWriteData replaces the fragment color in the Logic Op unit. It cannot
		// be combined with logical operations or software writemasking
		// (SLAU011A, pp. 7-81/7-102); hardware writemasks still apply.
		if ((r(LogicalOpMode) & 1) || r(FBSoftwareWriteMask) != 0xffffffffu ||
			(r(DepthMode) & 1) || (r(ColorDDAMode) & 3) == 3)
		{
			report(
				"FB_CONSTANT_MODE",
				LogicalOpMode,
				r(LogicalOpMode),
				"Constant framebuffer data requires disabled logical "
				"operations "
				"and software writemasking, flat color and no depth test",
				true);
			return false;
		}
		if (bytes == 3)
		{
			report(
				"FB_CONSTANT_LAYOUT",
				FBReadPixel,
				r(FBReadPixel),
				"Constant framebuffer data in 24-bit pixel mode is not "
				"implemented",
				true);
			return false;
		}
	}
	// Restrict primitive-control flags to the subset modeled here. Do not
	// acknowledge unknown render features as if a primitive completed.
	// NT4 sets reserved bit 5 (SLAU011A, Table 4-6); retain it without effect.
	const uint32_t reserved_bit_5 = 0x20u;
	const uint32_t supported = 0xc0u | 1u | reserved_bit_5 | FastFill |
		SyncMask | SyncHost | PositiveX | PositiveY |
		((texture_block || texture_copy) ? Texture : 0) |
		(setup ? 0x10000u : 0);
	if (value & ~supported)
	{
		report(
			"RENDER_FLAGS", Render, value, "Unimplemented Render flag", true);
		return false;
	}
	// NT4 sets reserved bits 1 and 2 (SLAU011A, p. 7-82). Retain them
	// without effect; the actual host-upload control is bit 3.
	if (r(FBWriteMode) & ~7u)
	{
		report(
			"FB_WRITE_MODE",
			FBWriteMode,
			r(FBWriteMode),
			"Host readback or non-normal framebuffer write mode not "
			"implemented",
			true);
		return false;
	}
	if ((value & SyncHost) && (r(FBReadMode) & Packed) &&
		m_relative_offset != 0 &&
		((!packed_trapezoid(value) && !(value & PositiveX)) ||
			(r(DitherMode) & 1)))
	{
		report(
			"HOST_ALIGNMENT",
			FBReadMode,
			r(FBReadMode),
			"Nonzero packed host alignment requires forward X and raw "
			"color mode",
			true);
		return false;
	}
	if (!(value & FastFill) && (r(DitherMode) & 1))
	{
		const uint32_t fmt =
			((r(DitherMode) >> 2) & 15) | ((r(DitherMode) >> 12) & 16);
		if (fmt != 0 && fmt != 1 && fmt != 2 && fmt != 14 && fmt != 16)
		{
			report(
				"COLOR_FORMAT",
				DitherMode,
				r(DitherMode),
				"Formatted color mode not implemented",
				true);
			return false;
		}
	}
	const unsigned coordinate_bias = (r(RasterizerMode) >> 4) & 3;
	if (coordinate_bias == 3)
	{
		report(
			"RASTERIZER_BIAS",
			RasterizerMode,
			r(RasterizerMode),
			"Undefined coordinate bias encoding",
			true);
		return false;
	}
	if (value & (SyncMask | SyncHost))
	{
		// RasterizerMode retains independent bitmask and host-color controls
		// (SLAU011A, pp. 7-111/7-112). Drivers need not clear the inactive fields
		// when switching streams: draw_step applies only the selected controls.
		// FractionAdjust is retained here and acts only on ContinueNewLine.
		const uint32_t bitmask_controls =
			1u | 2u | 0x40u | 0x180u | 0x200u | 0x7c00u;
		const uint32_t host_controls = 0x18000u;
		// SyncOnHostData and SyncOnBitMask automatically disable X/Y limits,
		// regardless of the retained LimitsEnable bit (SLAU011A, sec. 4.4.13).
		const uint32_t limits_enable = 1u << 18;
		if (r(RasterizerMode) &
			~(bitmask_controls | host_controls | limits_enable | 0x3cu))
		{
			report(
				"RASTERIZER_MODE",
				RasterizerMode,
				r(RasterizerMode),
				"Host rasterizer option not implemented",
				true);
			return false;
		}
	}
	if ((value & FastFill) && (value & SyncMask) &&
		(r(RasterizerMode) & (0x40u | 0x200u | 0x7c00u)))
	{
		report(
			"BLOCK_MASK_MODE",
			RasterizerMode,
			r(RasterizerMode),
			"Block mask background/packing/offset combinations not implemented",
			true);
		return false;
	}
	return true;
}

void CPermedia2::start_render(uint32_t value, bool setup)
{
	m_job = {};
	if (!validate_render(value, setup))
		return;
	m_job.setup = setup;
	const unsigned bytes = render_bytes();
	const unsigned coordinate_bias = (r(RasterizerMode) >> 4) & 3;
	m_job.command = value;
	m_job.primitive = value & 0xc0;
	m_job.origin_x = sx(r(RectangleOrigin), 16);
	m_job.origin_y = sx(r(RectangleOrigin) >> 16, 16);
	m_job.xdom = sx(r(StartXDom), 32);
	m_job.xsub = sx(r(StartXSub), 32);
	m_job.y = sx(r(StartY), 32);
	m_job.dxdom = sx(r(dXDom), 32);
	m_job.dxsub = sx(r(dXSub), 32);
	m_job.dy = sx(r(dY), 32);
	load_interpolants(true);
	if (m_job.primitive == PrimitiveRectangle)
	{
		m_job.columns = r(RectangleSize) & 0xffff;
		m_job.rows = (r(RectangleSize) >> 16) & 0xffff;
		// Packed-copy coordinates are converted by the framebuffer read unit.
		// Block fills bypass that unit and keep the rasterizer's native pixels.
		if ((r(FBReadMode) & Packed) && !(value & FastFill) &&
			!framebuffer_upload(value))
		{
			m_job.origin_x *= static_cast<int32_t>(4 / bytes);
			m_job.columns *= 4 / bytes;
		}
	}
	else
	{
		m_job.rows =
			m_job.primitive == PrimitivePoint ? 1 : (r(RasterCount) & 0xffff);
		// Bias is added on DDA load, not on register writes or subsequent steps
		// (SLAU011A, p. 7-112). RectangleOrigin uses a separate integer path.
		const int64_t bias = coordinate_bias == 1 ? 0x8000
			: coordinate_bias == 2				  ? 0x7fff
												  : 0;
		m_job.xdom += bias;
		m_job.xsub += bias;
		m_job.y += bias;
		// Keep fractional X edges and line Y increments in the signed 16.16 DDAs.
		// Only address generation extracts their integer portions (SLAU011A,
		// pp. 4-27/4-31). Trapezoids advance whole scanlines; fractional dY is for
		// X-major lines (p. 7-58), and a single-row primitive never consumes it.
		if (m_job.primitive == PrimitiveTrapezoid && m_job.rows > 1 &&
			(m_job.dy % 65536))
		{
			report(
				"FRACTIONAL_DDA",
				Render,
				value,
				"Fractional trapezoid vertical increments are not implemented",
				true);
			return;
		}
		set_span();
		if (packed_trapezoid(value) &&
			!validate_packed_trapezoid(m_job, Render, value))
			return;
	}
	if (m_job.columns > 262140 || m_job.rows > 65535)
	{
		report("GEOMETRY", Render, value, "Invalid geometry", true);
		return;
	}
	// A trapezoid's first span can round to zero width while later spans widen.
	m_job.active = m_job.rows != 0 &&
		(m_job.columns != 0 || m_job.primitive == PrimitiveTrapezoid);
}

void CPermedia2::continue_render(uint32_t address, uint32_t value)
{
	if (m_job.setup)
	{
		report("SETUP_CONTINUATION", address, value,
			"Host continuations after setup-unit triangles are not implemented",
			true);
		return;
	}
	if ((m_job.command & Texture) && !(m_job.command & FastFill))
	{
		report("TEXTURE_COPY_CONTINUATION", address, value,
			"Texture-copy continuation requires retained S/T accumulators "
			"that are not implemented", true);
		return;
	}
	const bool line = address == ContinueNewLine;
	if (!line && (value & 0xfff) == 0 && !m_job.active &&
		m_job.primitive == PrimitiveRectangle && m_job.col == 0 &&
		m_job.row == m_job.rows)
	{
		// A zero scanline count emits no fragments; NewDom/NewSub still load
		// their selected edge (SLAU011A, pp. 7-19/7-20/7-22). Rectangle rendering
		// did not traverse the Start* DDAs, so there is no deferred edge step.
		if (address == ContinueNewDom || address == ContinueNewSub)
		{
			const unsigned coordinate_bias = (r(RasterizerMode) >> 4) & 3;
			if (coordinate_bias == 3)
			{
				report("RASTERIZER_BIAS", RasterizerMode, r(RasterizerMode),
					"Undefined coordinate bias encoding", true);
				return;
			}
			const int64_t bias = coordinate_bias == 1 ? 0x8000
				: coordinate_bias == 2 ? 0x7fff : 0;
			if (address == ContinueNewDom)
				m_job.xdom = int64_t(sx(r(StartXDom), 32)) + bias;
			else
				m_job.xsub = int64_t(sx(r(StartXSub), 32)) + bias;
		}
		m_job.dxdom = sx(r(dXDom), 32);
		m_job.dxsub = sx(r(dXSub), 32);
		m_job.dy = sx(r(dY), 32);
		m_job.row = m_job.col = m_job.rows = 0;
		// Keep the rectangle identity: this does not establish a nonzero
		// line/trapezoid continuation history.
		return;
	}
	if (m_job.active || m_job.col != 0 || m_job.row != m_job.rows ||
		(line ? m_job.primitive != PrimitiveLine || m_job.columns != 1
			  : m_job.primitive != PrimitiveTrapezoid))
	{
		report(
			"CONTINUATION_STATE",
			address,
			value,
			"Continuation requires a matching completed line or trapezoid",
			true);
		return;
	}
	if (m_job.command & (SyncHost | SyncMask))
	{
		report(
			"CONTINUATION_STREAM",
			address,
			value,
			"Synchronized continuation payload rules are not implemented",
			true);
		return;
	}
	if (!validate_render(m_job.command))
		return;
	if (m_job.interpolation != interpolation_kind(m_job.command))
	{
		report("CONTINUATION_INTERPOLATION", address, value,
			"Changing color/depth interpolation sources during a continuation "
			"chain is not implemented", true);
		return;
	}
	Job next = m_job;
	// Completed jobs retain the last rendered DDA position. Recover the
	// hardware's next endpoint using the OLD slopes, before loading new ones
	// (SLAU011A, pp. 2-4/7-19..7-22). A zero-count command has no deferred step.
	if (next.rows != 0)
	{
		next.xdom += next.dxdom;
		next.xsub += next.dxsub;
		next.y += next.dy;
		if (next.interpolation & 1)
			for (unsigned i = 0; i < 3; ++i)
				next.color[i] += next.dcolor_dy[i];
		if (next.interpolation & 2)
			next.z += next.dzdy;
	}
	if (line)
	{
		const unsigned adjustment = (r(RasterizerMode) >> 2) & 3;
		if (adjustment)
		{
			const int64_t fraction = adjustment == 1 ? 0
				: adjustment == 2					 ? 0x8000
													 : 0x7fff;
			// Replace only the fraction. In particular, negative values retain their
			// signed integer portion, and coordinate bias is not added a second time.
			next.xdom = fixed_integer(next.xdom) * 65536 + fraction;
			next.y = fixed_integer(next.y) * 65536 + fraction;
		}
	}
	else
	{
		const unsigned coordinate_bias = (r(RasterizerMode) >> 4) & 3;
		const int64_t bias = coordinate_bias == 1 ? 0x8000
			: coordinate_bias == 2				  ? 0x7fff
												  : 0;
		if (address == ContinueNewDom)
			next.xdom = int64_t(sx(r(StartXDom), 32)) + bias;
		else if (address == ContinueNewSub)
			next.xsub = int64_t(sx(r(StartXSub), 32)) + bias;
		// Continue retains both edges; every trapezoid continuation retains Y.
	}
	next.dxdom = sx(r(dXDom), 32);
	next.dxsub = sx(r(dXSub), 32);
	next.dy = sx(r(dY), 32);
	next.row = next.col = 0;
	next.rows =
		value & 0xfff; // The command count does not overwrite RasterCount.
	if (!line && next.rows > 1 && (next.dy % 65536))
	{
		report(
			"FRACTIONAL_DDA",
			address,
			value,
			"Fractional trapezoid vertical increments are not implemented",
			true);
		return;
	}

	// Chained continuations must remain representable by fragment coordinates
	// and the existing snapshot bounds. A 12-bit count times a signed 32-bit
	// increment is safe in int64; check both ends of each linear DDA interval.
	const auto valid_dda = [&next](int64_t position, int64_t delta) {
		const int64_t limit = int64_t(1) << 47;
		const int64_t endpoint = position + delta * int64_t(next.rows);
		return position >= -limit && position < limit && endpoint >= -limit &&
			endpoint < limit;
	};
	if (!valid_dda(next.xdom, next.dxdom) ||
		!valid_dda(next.xsub, next.dxsub) || !valid_dda(next.y, next.dy))
	{
		report(
			"CONTINUATION_RANGE",
			address,
			value,
			"Continuation coordinates exceed the modeled DDA range",
			true);
		return;
	}
	if (next.interpolation & 1)
		for (unsigned i = 0; i < 3; ++i)
			if (!valid_dda(next.color[i], sx(r(dRdyDom + i * 24) >> 4, 20)))
			{
				report("COLOR_RANGE", address, value,
					"Continued color DDA exceeds the modeled range", true);
				return;
			}
	const int64_t dzdy = int64_t(sx(r(dZdyDomU), 17)) * 2048 +
		(r(dZdyDomL) >> 21);
	if ((next.interpolation & 2) && !valid_dda(next.z, dzdy))
	{
		report("DEPTH_RANGE", address, value,
			"Continued depth DDA exceeds the modeled range", true);
		return;
	}
	if (packed_trapezoid(next.command) &&
		!validate_packed_trapezoid(next, address, value))
		return;
	m_job = next;
	load_interpolants(false);
	set_span();
	// Empty trapezoid spans still advance the DDAs and consume a scanline.
	m_job.active = m_job.rows != 0;
}

void CPermedia2::set_span()
{
	if (m_job.primitive != PrimitiveTrapezoid)
	{
		m_job.columns = 1;
		return;
	}
	const int64_t a = fixed_integer(m_job.xdom), b = fixed_integer(m_job.xsub);
	const int64_t width = a > b ? a - b : b - a;
	m_job.columns = static_cast<uint32_t>(std::min<int64_t>(width, 65535));
	// The rasterizer retains group coordinates. Expand only the emitted span;
	// multiplying a fractional DDA before taking its integer part changes edges.
	if (packed_trapezoid(m_job.command))
		m_job.columns *= 4 / render_bytes();
}

void CPermedia2::next_fragment()
{
	if (++m_job.col < m_job.columns)
		return;
	m_job.col = 0;
	if ((m_job.command & (FastFill | Texture)) == (FastFill | Texture) &&
		(r(TextureReadMode) & 1))
		m_job.payload_left = 0;
	if (++m_job.row >= m_job.rows)
	{
		if (m_job.setup && m_job.knee_rows)
		{
			// Delta's second trapezoid keeps the dominant-edge interpolants
			// and replaces only the subordinate edge (PRM 4.2.1/4.3.10).
			step_interpolants();
			m_job.xdom += m_job.dxdom;
			m_job.y += m_job.dy;
			m_job.xsub = m_job.knee_xsub;
			m_job.dxsub = m_job.knee_dxsub;
			m_job.rows = m_job.knee_rows;
			m_job.row = m_job.knee_rows = 0;
			r(StartXSub) = uint32_t(m_job.xsub);
			r(dXSub) = uint32_t(m_job.dxsub);
			r(ContinueNewSub) = m_job.rows;
			set_span();
			return;
		}
		// Leave completed jobs at the last rendered DDA position.
		// A continuation applies this final DDA step before reloading slopes.
		m_job.active = false;
		return;
	}
	step_interpolants();
	if (m_job.primitive != PrimitiveRectangle)
	{
		// Accumulate before extracting coordinates, retaining subpixel carry.
		// Signed 32-bit inputs over the modeled 16-bit Count fit these int64 DDAs.
		m_job.xdom += m_job.dxdom;
		m_job.xsub += m_job.dxsub;
		m_job.y += m_job.dy;
		set_span();
		// Empty spans consume work but must not produce a fragment.
	}
	if ((m_job.command & SyncHost) ||
		((m_job.command & SyncMask) && (r(RasterizerMode) & 0x200)))
		m_job.payload_left = 0;
}

bool CPermedia2::draw_block()
{
	const bool rectangle = m_job.primitive == PrimitiveRectangle;
	const bool forward = rectangle ? (m_job.command & PositiveX) != 0
		: m_job.xsub >= m_job.xdom;
	const int64_t first = rectangle
		? int64_t(m_job.origin_x) + (forward ? 0 : m_job.columns - 1)
		: fixed_integer(m_job.xdom) - (forward ? 0 : 1);
	const int64_t last = first + (forward ? int64_t(m_job.columns - 1)
		: -int64_t(m_job.columns - 1));
	const int64_t y = rectangle
		? int64_t(m_job.origin_y) + ((m_job.command & PositiveY)
			? m_job.row : m_job.rows - 1 - m_job.row)
		: fixed_integer(m_job.y);
	// Leave exceptional coordinates to the ordinary fragment path. The block
	// calculation must not introduce a different narrowing or address wrap.
	if (first < INT32_MIN || first > INT32_MAX ||
		last < INT32_MIN || last > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
		return false;
	int64_t left = INT32_MIN, right = int64_t(INT32_MAX) + 1;
	int64_t top = INT32_MIN, bottom = int64_t(INT32_MAX) + 1;
	if (r(ScissorMode) & 1)
	{
		left = sx(r(ScissorMin), 16);
		right = sx(r(ScissorMax), 16);
		top = sx(r(ScissorMin) >> 16, 16);
		bottom = sx(r(ScissorMax) >> 16, 16);
	}
	if (r(ScissorMode) & 2)
	{
		left = std::max<int64_t>(left, 0);
		right = std::min<int64_t>(right, r(ScreenSize) & 0xffff);
		top = std::max<int64_t>(top, 0);
		bottom = std::min<int64_t>(bottom, r(ScreenSize) >> 16);
	}
	const auto finish_span = [this]() {
		m_job.col = m_job.columns - 1;
		next_fragment();
	};
	if (y < top || y >= bottom || left >= right)
	{
		finish_span();
		return true;
	}
	int64_t x = first + (forward ? int64_t(m_job.col) : -int64_t(m_job.col));
	const int64_t begin = forward ? std::max(x, left) : std::min(x, right - 1);
	const int64_t end = forward ? std::min(last + 1, right) : std::max(last, left);
	if (forward ? begin >= end : begin < end)
	{
		finish_span();
		return true;
	}
	// Solid block writes have no color/depth interpolants or host payload.
	// True scissoring can therefore discard a prefix without stepping pixels.
	// Keep the original column index and span width for continuation/snapshots.
	m_job.col += uint32_t(forward ? begin - x : x - begin);
	x = begin;
	// SGRAM fills use 32 native pixels, with partial blocks at span edges
	// (SLAU011A, sections 3.3.3.2/4.4.6). Scissor and hardware masks still apply.
	const uint32_t lane = uint32_t(x) & 31;
	const uint32_t block = forward ? 32 - lane : lane + 1;
	const uint32_t count = uint32_t(std::min<int64_t>(block,
		forward ? end - x : x - end + 1));
	for (uint32_t i = 0; i < count; ++i)
	{
		emit_pixel(int32_t(x), int32_t(y), r(FBBlockColor), true);
		if (m_halted)
			return true;
		++m_job.col;
		x += forward ? 1 : -1;
	}
	// Scissor-rejected suffixes cannot affect another unit in this solid path.
	if (m_job.col == m_job.columns || (forward ? x >= end : x < end))
		finish_span();
	return true;
}

bool CPermedia2::draw_step()
{
	if (m_job.columns == 0)
	{
		next_fragment();
		return true;
	}
	const bool mask_stream = (m_job.command & SyncMask) != 0,
			   host_stream = (m_job.command & SyncHost) != 0;
	const bool texture_block =
		(m_job.command & (FastFill | Texture)) == (FastFill | Texture) &&
		(r(TextureReadMode) & 1);
	if ((m_job.command & FastFill) && !mask_stream && !host_stream &&
		!texture_block &&
		!((m_job.command & 1) && (r(AreaStippleMode) & 1)) &&
		(m_job.primitive == PrimitiveTrapezoid ||
			m_job.primitive == PrimitiveRectangle) && draw_block())
		return true;
	const unsigned bytes = render_bytes();
	if (texture_block && m_job.payload_left == 0 && !load_texture_mask())
		return false;
	if ((mask_stream || host_stream) && m_job.payload_left == 0)
	{
		if (m_input.empty())
			return false;
		const Command c = m_input.front();
		if (mask_stream ? c.address != BitMaskPattern : !host_tag(c.address))
		{
			// A non-data write aborts a rasterizer waiting for host input.
			// Keep the command queued for normal execution; pixels already
			// supplied by the host remain drawn.
			m_job.active = false;
			return true;
		}
		if (host_stream &&
			(c.address == Depth || c.address == Stencil || c.address == Texel0 ||
				(packed_trapezoid(m_job.command) && c.address == FBSourceData)))
		{
			report(
				"HOST_DATA",
				c.address,
				c.value,
				"This host-data source is not implemented for the active primitive",
				true);
			return false;
		}
		m_input.pop_front();
		m_job.payload = c.value;
		m_job.payload_tag = c.address;
		if (mask_stream)
		{
			const unsigned swap = (r(RasterizerMode) >> 7) & 3;
			if (swap & 1)
				m_job.payload = ((m_job.payload & 0xff00ff) << 8) |
					((m_job.payload >> 8) & 0xff00ff);
			if (swap & 2)
				m_job.payload = (m_job.payload << 16) | (m_job.payload >> 16);
			const unsigned skip = (r(RasterizerMode) >> 10) & 31;
			m_job.payload_left = 32;
			// BitMaskOffset selects the first position in a cyclic 32-bit word.
			// MirrorBitMask=0 consumes LSB first; =1 consumes MSB first.
			if (skip)
			{
				if (r(RasterizerMode) & 1)
					m_job.payload = (m_job.payload << skip) |
						(m_job.payload >> (32 - skip));
				else
					m_job.payload = (m_job.payload >> skip) |
						(m_job.payload << (32 - skip));
			}
		}
		else
		{
			const unsigned swap = (r(RasterizerMode) >> 15) & 3;
			if (swap & 1)
				m_job.payload = ((m_job.payload & 0xff00ff) << 8) |
					((m_job.payload >> 8) & 0xff00ff);
			if (swap & 2)
				m_job.payload = (m_job.payload << 16) | (m_job.payload >> 16);
			m_job.payload_left = (r(FBReadMode) & Packed) ? 4 / bytes : 1;
		}
		return true;
	}
	int32_t x = 0, y = 0;
	if (m_job.primitive == PrimitiveRectangle)
	{
		x = m_job.origin_x +
			static_cast<int32_t>(
				(m_job.command & PositiveX) ? m_job.col
											: m_job.columns - 1 - m_job.col);
		y = m_job.origin_y +
			static_cast<int32_t>(
				(m_job.command & PositiveY) ? m_job.row
											: m_job.rows - 1 - m_job.row);
	}
	else
	{
		y = static_cast<int32_t>(fixed_integer(m_job.y));
		x = static_cast<int32_t>(fixed_integer(m_job.xdom));
		if (packed_trapezoid(m_job.command))
			x *= static_cast<int32_t>(4 / bytes);
		if (m_job.primitive == PrimitiveTrapezoid)
			x += (m_job.xsub >= m_job.xdom)
				? static_cast<int32_t>(m_job.col)
				: -static_cast<int32_t>(m_job.col) - 1;
	}
	if (framebuffer_upload(m_job.command))
	{
		if (!upload_pixel(x, y))
			return false;
		next_fragment();
		return true;
	}
	const bool texture_copy =
		(m_job.command & Texture) && !(m_job.command & FastFill);
	// User/screen scissoring discards fragments before texture memory reads.
	if (texture_copy && clipped(x, y, false))
	{
		next_fragment();
		return true;
	}
	const bool dda = (r(ColorDDAMode) & 1) != 0;
	uint32_t color = dda ? fragment_color() : r(Color);
	bool raw = !dda, draw = true;
	if (m_job.command & FastFill)
	{
		color = r(FBBlockColor);
		raw = true;
	}
	if (texture_block)
	{
		draw = (m_job.payload & 1) != 0;
		m_job.payload >>= 1;
		--m_job.payload_left;
	}
	else if (texture_copy)
	{
		if (!texture_copy_color(color))
			return false;
		raw = true; // A CI8 texture in a CI8 framebuffer preserves its index.
	}
	else if (mask_stream)
	{
		const bool mirror = (r(RasterizerMode) & 1) != 0;
		bool bit = mirror ? (m_job.payload & 0x80000000u) != 0
						  : (m_job.payload & 1) != 0;
		if (r(RasterizerMode) & 2)
			bit = !bit;
		if (mirror)
			m_job.payload <<= 1;
		else
			m_job.payload >>= 1;
		--m_job.payload_left;
		if (!bit)
		{
			if (r(RasterizerMode) & 0x40)
			{
				color = r(Texel0);
				raw = true;
			}
			else
				draw = false;
		}
	}
	else if (host_stream)
	{
		color = m_job.payload;
		raw = m_job.payload_tag != Color;
		if (bytes < 4)
		{
			// Standard Color keeps all four components until formatting. Native
			// downloads consume one such word per fragment (PRM 4.11.5).
			if (raw || !(r(DitherMode) & 1) || (r(FBReadMode) & Packed))
				color &= (1u << (bytes * 8)) - 1;
			m_job.payload >>= bytes * 8;
		}
		--m_job.payload_left;
		// Packed downloads shift the destination stream in native pixels
		// (SLAU011A, section 6.4.2), including across DWORD boundaries.
		if ((r(FBReadMode) & Packed) &&
			(m_job.primitive == PrimitiveRectangle ||
				packed_trapezoid(m_job.command)))
			x += m_relative_offset;
	}
	else if (!(m_job.command & FastFill) && (r(FBReadMode) & ReadSource))
	{
		const int64_t sign = (r(FBReadMode) & 0x10000) ? -1 : 1;
		const int64_t src = int64_t(r(FBSourceBase)) +
			int64_t(y) * pitch_from_products(r(FBReadMode)) * sign + x +
			r(FBPixelOffset) + sx(r(FBSourceOffset), 24) -
			((r(FBReadMode) & Packed) ? m_relative_offset : 0);
		color = pixel_read(src * bytes, bytes);
		raw = true;
	}
	if (draw)
		emit_pixel(x, y, color, raw);
	if (m_halted)
		return false;
	next_fragment();
	return true;
}

bool CPermedia2::framebuffer_upload(uint32_t command) const
{
	// Block-write fragments bypass the framebuffer read unit, regardless of its
	// retained read enables and datatype (SLAU011A, section 4.4.6).
	return !(command & FastFill) &&
		(r(FBReadMode) & (0x8000u | ReadDestination)) ==
		(0x8000u | ReadDestination);
}

bool CPermedia2::upload_pixel(int32_t x, int32_t y)
{
	const unsigned bytes = render_bytes();
	const bool packed = (r(FBReadMode) & Packed) != 0;
	// Packed X addresses groups of a DWORD; pitch, base and pixel offset
	// remain in native-pixel units (SLAU011A, section 6.4.1). The packed
	// write limits and relative source alignment do not affect these reads.
	if (bytes == 0 || (packed && bytes == 3) || (!packed && bytes != 4))
	{
		report(
			"FB_UPLOAD_LAYOUT",
			FBReadPixel,
			r(FBReadPixel),
			"Unsupported restored framebuffer upload layout",
			true);
		return false;
	}
	const int64_t pixel_x = int64_t(x) * (packed ? 4 / bytes : 1);
	if (clipped(pixel_x, y, false))
		return true;
	const unsigned filter = (r(FilterMode) >> 8) & 3;
	const size_t words = (filter & 1) + ((filter >> 1) & 1);
	if (m_output.size() + words > 8)
		return false;
	const int64_t sign = (r(FBReadMode) & 0x10000) ? -1 : 1;
	const int64_t pixel = int64_t(r(FBWindowBase)) +
		int64_t(y) * pitch_from_products(r(FBReadMode)) * sign + pixel_x +
		r(FBPixelOffset);
	const uint32_t color = pixel_read(pixel * bytes, packed ? 4 : bytes);
	if (filter & 1)
		output_push(tag(FBColor));
	if (filter & 2)
		output_push(color);
	return true;
}

bool CPermedia2::clipped(int64_t x, int64_t y, bool packed_limits) const
{
	if (m_job.setup && (r(RasterizerMode) & 0x40000))
	{
		if (y < sx(r(YLimits), 12) || y >= sx(r(YLimits) >> 16, 12))
			return true;
	}
	if (r(ScissorMode) & 1)
	{
		if (x < sx(r(ScissorMin), 16) || y < sx(r(ScissorMin) >> 16, 16) ||
			x >= sx(r(ScissorMax), 16) || y >= sx(r(ScissorMax) >> 16, 16))
			return true;
	}
	if (r(ScissorMode) & 2)
	{
		const int64_t screen_x = x + (m_job.setup ? sx(r(WindowOrigin), 12) : 0);
		const int64_t screen_y = y +
			(m_job.setup ? sx(r(WindowOrigin) >> 16, 12) : 0);
		if (screen_x < 0 || screen_y < 0 ||
			screen_x >= static_cast<int32_t>(r(ScreenSize) & 0xffff) ||
			screen_y >= static_cast<int32_t>(r(ScreenSize) >> 16))
			return true;
	}
	if (packed_limits && (r(FBReadMode) & Packed))
	{
		const int32_t start = sx(r(PackedDataLimits) >> 16, 12);
		const int32_t end = sx(r(PackedDataLimits), 12);
		// Drivers may specify the two native-pixel edges in either order.
		if (x < std::min(start, end) || x >= std::max(start, end))
			return true;
	}
	return false;
}

uint32_t CPermedia2::pixel_read(int64_t address, unsigned bytes) const
{
	uint32_t value = 0;
	if (address < 0 || address > int64_t(VramSize - bytes))
		return 0;
	for (unsigned i = 0; i < bytes; ++i)
		value |= uint32_t(m_vram[static_cast<size_t>(address) + i]) << (8 * i);
	return value;
}

void CPermedia2::pixel_write(
	int64_t address, unsigned bytes, uint32_t value, uint32_t sw, uint32_t hw)
{
	if (address < 0 || address > int64_t(VramSize - bytes))
		return;
	for (unsigned i = 0; i < bytes; ++i)
	{
		const auto addr = static_cast<uint32_t>(address) + i;
		const uint8_t mask = uint8_t(
			(sw >> (i * 8)) & (hw >> ((addr & 3) * 8)) &
			(r(FramebufferWriteMask) >> ((addr & 3) * 8)));
		m_vram[addr] =
			uint8_t((m_vram[addr] & ~mask) | ((value >> (i * 8)) & mask));
	}
}

uint32_t CPermedia2::logical_op(unsigned op, uint32_t s, uint32_t d)
{
	switch (op & 15)
	{
	case 0:
		return 0;
	case 1:
		return s & d;
	case 2:
		return s & ~d;
	case 3:
		return s;
	case 4:
		return ~s & d;
	case 5:
		return d;
	case 6:
		return s ^ d;
	case 7:
		return s | d;
	case 8:
		return ~(s | d);
	case 9:
		return ~(s ^ d);
	case 10:
		return ~d;
	case 11:
		return s | ~d;
	case 12:
		return ~s;
	case 13:
		return ~s | d;
	case 14:
		return ~(s & d);
	default:
		return 0xffffffffu;
	}
}

uint32_t CPermedia2::format_color(uint32_t v, int32_t x, int32_t y) const
{
	if (!(r(DitherMode) & 1))
		return v;
	const uint32_t fmt =
		((r(DitherMode) >> 2) & 15) | ((r(DitherMode) >> 12) & 16);
	if (fmt == 14)
		return v & 255; // Color index, not a RAMDAC ColorMode enum
	const bool rgb = (r(DitherMode) & 0x400) != 0;
	// Internal color is AABBGGRR. RGB framebuffer order puts red in the
	// higher channel; BGR puts it in the lower one (SLAU011A, p. 7-36).
	const uint32_t red = v & 255, green = (v >> 8) & 255,
				   blue = (v >> 16) & 255;
	uint32_t low = rgb ? blue : red, high = rgb ? red : blue;
	uint32_t mid = green;
	if (r(DitherMode) & 2)
	{
		// Ordered color reduction adds the position-dependent fraction before
		// clamping and truncation. XY offsets select window-relative phase
		// (SLAU011A, section 4.14). The 2x2 pattern follows the GLINT color
		// formatter described in the 300SX architecture manual, section 13.1.1.
		static const uint8_t dither[2][2] = {{0, 2}, {3, 1}};
		const uint32_t dx = (uint32_t(x) + (r(DitherMode) >> 6)) & 1;
		const uint32_t dy = (uint32_t(y) + (r(DitherMode) >> 8)) & 1;
		const uint32_t fraction = dither[dy][dx];
		low = std::min<uint32_t>(255, low + fraction * 2);
		mid = std::min<uint32_t>(255, mid + fraction);
		high = std::min<uint32_t>(255, high + fraction * 2);
	}
	uint32_t alpha = v >> 24;
	if (r(DitherMode) & 0x1000)
		alpha = 0;
	if (r(DitherMode) & 0x2000)
		alpha = 0xf8;
	switch (fmt)
	{
	case 0:
		return low | (green << 8) | (high << 16) | (alpha << 24);
	case 1:
		return (low >> 3) | ((green >> 3) << 5) | ((high >> 3) << 10) |
			((alpha >> 7) << 15);
	case 2:
		return (low >> 4) | ((green >> 4) << 4) | ((high >> 4) << 8) |
			((alpha >> 4) << 12);
	case 16:
		return (low >> 3) | ((mid >> 2) << 5) | ((high >> 3) << 11);
	default:
		return 0; // start_render rejects unsupported active formats
	}
}

bool CPermedia2::emit_pixel(int32_t x, int32_t y, uint32_t value, bool raw)
{
	// Packed-copy edge limits do not trim block-write fragments. The rasterizer
	// defines their native-pixel area; ordinary scissoring still applies.
	if (clipped(x, y, !(m_job.command & FastFill)))
		return false;
	if ((m_job.command & 1) && (r(AreaStippleMode) & 1))
	{
		// Permedia 2 stipples are always 8x8 (SLAU011A, pp. 7-8..7-10).
		// Only three bits of each offset are used. Reserved and unused mode
		// fields are retained without effect, including those written by NT4.
		const uint32_t mode = r(AreaStippleMode);
		uint32_t ax = (uint32_t(x) + (mode >> 7)) & 7,
				 ay = (uint32_t(y) + (mode >> 12)) & 7;
		if (mode & (1u << 18))
			ax = 7 - ax;
		if (mode & (1u << 19))
			ay = 7 - ay;
		bool bit = ((r(AreaStipplePattern0 + ay * 8) >> ax) & 1) != 0;
		if (mode & (1u << 17))
			bit = !bit;
		if (!bit)
		{
			if (mode & (1u << 20))
			{
				value = r(Texel0);
				raw = true;
			}
			else
				return false;
		}
	}
	// Depth/local-buffer updates precede the optional framebuffer write.
	// Block fragments bypass both units (SLAU011A, section 4.4.6).
	if (!(m_job.command & FastFill) && !depth_test(x, y))
		return false;
	if (!(r(FBWriteMode) & 1))
		return false;
	const unsigned bytes = render_bytes();
	const int64_t sign = (r(FBWriteConfig) & 0x10000) ? -1 : 1;
	int64_t offset = int64_t(y) * pitch_from_products(r(FBWriteConfig)) *
		sign + x;
	if (r(FBWriteConfig) & 0x40000)
	{
		if (x < 0 || y < 0)
		{
			report("FB_PATCH_ADDRESS", FBWriteConfig, r(FBWriteConfig),
				"Negative Subpatch host coordinates are not implemented", true);
			return false;
		}
		// Subpatch stores 32x32 pixels in interleaved X/Y order. Complete
		// tile rows retain the configured pitch; the window base and pixel
		// offset are applied after converting the local coordinates.
		uint32_t within = 0;
		for (unsigned bit = 0; bit < 5; ++bit)
		{
			within |= ((uint32_t(x) >> bit) & 1) << (2 * bit);
			within |= ((uint32_t(y) >> bit) & 1) << (2 * bit + 1);
		}
		offset = int64_t(uint32_t(y) & ~31u) *
			pitch_from_products(r(FBWriteConfig)) +
			int64_t(uint32_t(x) & ~31u) * 32 + within;
	}
	const int64_t pixel = int64_t(r(FBWindowBase)) + offset + r(FBPixelOffset);
	const int64_t address = pixel * bytes;
	if (address < 0 || address > int64_t(VramSize - bytes))
	{
		if (!m_warned[FBWindowBase / 8])
		{
			m_warned[FBWindowBase / 8] = true;
			report(
				"DRAW_RANGE",
				FBWindowBase,
				uint32_t(pixel),
				"Out-of-VRAM fragment discarded; electrical aliasing "
				"unmodeled");
		}
		return false;
	}
	if (!(m_job.command & FastFill) && (r(LogicalOpMode) & 0x20))
	{
		// Native fragments use the low pixel of FBWriteData, as with data
		// produced by the Logic Op unit. Packed fragments select a byte lane
		// from its complete word. FastFill uses FBBlockColor instead.
		value = r(FBWriteData);
		if (r(FBReadMode) & Packed)
			value >>= (uint32_t(address) & 3) * 8;
	}
	else if (!raw)
		value = format_color(value, x, y);
	const uint32_t old = pixel_read(address, bytes);
	if (!(m_job.command & FastFill))
	{
		const uint32_t destination =
			(r(FBReadMode) & ReadDestination) ? old : 0;
		if (r(LogicalOpMode) & 1)
			value =
				logical_op((r(LogicalOpMode) >> 1) & 15, value, destination);
		value = (value & r(FBSoftwareWriteMask)) |
			(destination & ~r(FBSoftwareWriteMask));
	}
	pixel_write(address, bytes, value, 0xffffffffu, r(FBHardwareWriteMask));
	return true;
}

// Display and video timing

uint32_t CPermedia2::scanout_color(uint32_t raw, uint8_t format) const
{
	uint32_t red = 0, green = 0, blue = 0;
	const bool rgb = (m_dac[DACColorMode] & 0x20) != 0;
	auto pal = [&](uint32_t index, unsigned lane) -> uint32_t {
		uint32_t v = m_palette[(index & 255) * 3 + lane];
		if (!(m_dac[DACMiscControl] & 2))
		{
			v &= 63;
			v = (v << 2) | (v >> 4);
		}
		return v;
	};
	if (format == 0)
	{
		const uint32_t index = raw & r(PixelMask) & 255;
		return 0xff000000u | (pal(index, 0) << 16) | (pal(index, 1) << 8) |
			pal(index, 2);
	}
	// RGB places red at the high end; BGR places it at the low end. The 3:3:2
	// format keeps R/G/B widths of 3/3/2 in both orders (SLAU011A, Table 3-1).
	switch (format)
	{
	case 1:
		red = ((raw >> (rgb ? 5 : 0)) & 7) * 255 / 7;
		green = ((raw >> (rgb ? 2 : 3)) & 7) * 255 / 7;
		blue = ((raw >> (rgb ? 0 : 6)) & 3) * 255 / 3;
		break;
	case 4:
		red = ((raw >> (rgb ? 10 : 0)) & 31) * 255 / 31;
		green = ((raw >> 5) & 31) * 255 / 31;
		blue = ((raw >> (rgb ? 0 : 10)) & 31) * 255 / 31;
		break;
	case 5:
		red = ((raw >> (rgb ? 8 : 0)) & 15) * 17;
		green = ((raw >> 4) & 15) * 17;
		blue = ((raw >> (rgb ? 0 : 8)) & 15) * 17;
		break;
	case 6:
		red = ((raw >> (rgb ? 11 : 0)) & 31) * 255 / 31;
		green = ((raw >> 5) & 63) * 255 / 63;
		blue = ((raw >> (rgb ? 0 : 11)) & 31) * 255 / 31;
		break;
	case 8:
	case 9:
		red = (raw >> (rgb ? 16 : 0)) & 255;
		green = (raw >> 8) & 255;
		blue = (raw >> (rgb ? 0 : 16)) & 255;
		break;
	default:
		return 0xff000000u;
	}
	// RDColorMode bit 7 selects TrueColor palette bypass. With it clear, packed
	// components address the LUT. CI8 always uses the indexed path above.
	if (!(m_dac[DACColorMode] & 0x80))
	{
		red = pal(red, 0);
		green = pal(green, 1);
		blue = pal(blue, 2);
	}
	return 0xff000000u | (red << 16) | (green << 8) | blue;
}

CPermedia2::Frame CPermedia2::scanout(bool include_cursor) const
{
	Frame f;
	if (!(r(VideoControl) & 1))
		return f;
	const uint8_t format = m_dac[DACColorMode] & 15;
	unsigned bytes = 0;
	switch (format)
	{
	case 0:
	case 1:
		bytes = 1;
		break;
	case 4:
	case 5:
	case 6:
		bytes = 2;
		break;
	case 8:
		bytes = 4;
		break;
	case 9:
		bytes = 3;
		break;
	default:
		return f;
	}
	const uint32_t total = (r(HTotal) & 0x7ff) + 1, blank = r(HbEnd) & 0x7ff;
	const uint32_t vtotal = (r(VTotal) & 0x7ff) + 1, vblank = r(VbEnd) & 0x7ff;
	if (total <= blank || vtotal <= vblank)
		return f;
	const uint32_t wordbytes = (r(VideoControl) & 0x10000) ? 8u : 4u;
	f.width = (total - blank) * wordbytes / bytes;
	f.height = vtotal - vblank;
	const uint32_t stride = (r(ScreenStride) & 0x1fff) * 8;
	const uint64_t base = uint64_t(m_active_screen_base) * 8;
	if (!stride || !f.width || f.width > 4096 || f.height > 2048 ||
		base + uint64_t(f.height - 1) * stride + uint64_t(f.width) * bytes >
			VramSize)
		return {};
	f.argb.resize(size_t(f.width) * f.height);
	for (uint32_t y = 0; y < f.height; ++y)
		for (uint32_t x = 0; x < f.width; ++x)
			f.argb[size_t(y) * f.width + x] = scanout_color(
				pixel_read(
					static_cast<int64_t>(
						base + uint64_t(y) * stride + uint64_t(x) * bytes),
					bytes),
				format);
	if (include_cursor)
		composite_cursor(f);
	return f;
}

void CPermedia2::composite_cursor(Frame& frame) const
{
	const uint8_t control = m_dac[DACCursorControl];
	const unsigned mode = control & 3;
	if (mode == 0)
		return;

	// HRM sections 5.6-5.8: two 64x64 one-bit planes, eight bytes per row,
	// most-significant bit first. Small cursors select a quadrant in each plane.
	const bool large = (control & 0x40) != 0;
	const unsigned size = large ? 64 : 32;
	const unsigned selected = (control >> 4) & 3;
	const unsigned pattern_base =
		large ? 0 : (selected & 1) * 4 + (selected >> 1) * 256;
	// Position registers specify the selected cursor's bottom-right corner.
	const int origin_x =
		int(((r(CursorXHigh) & 255) << 8) | (r(CursorXLow) & 255)) - int(size);
	const int origin_y =
		int(((r(CursorYHigh) & 255) << 8) | (r(CursorYLow) & 255)) - int(size);

	// Table 5.4 indexes the planes as (plane1 << 1) | plane0. Zero means
	// transparent and -1 means complement; colors occupy slots1-3 (slot0 unused).
	static const int8_t colors[3][4] = {
		{0, 1, 2, 3}, {1, 2, 0, -1}, {0, 0, 1, 2}};
	uint32_t argb[4]{};
	for (unsigned i = 1; i < 4; ++i)
	{
		// Cursor colors are always 24-bit RGB, independent of main-palette width,
		// PixelMask, and the framebuffer's pixel format or component ordering.
		argb[i] = 0xff000000u | (uint32_t(m_cursor_colors[i * 3]) << 16) |
			(uint32_t(m_cursor_colors[i * 3 + 1]) << 8) |
			m_cursor_colors[i * 3 + 2];
	}
	for (unsigned cy = 0; cy < size; ++cy)
	{
		const int y = origin_y + int(cy);
		if (y < 0 || uint32_t(y) >= frame.height)
			continue;
		for (unsigned cx = 0; cx < size; ++cx)
		{
			const int x = origin_x + int(cx);
			if (x < 0 || uint32_t(x) >= frame.width)
				continue;
			const unsigned address = pattern_base + cy * 8 + cx / 8;
			const unsigned shift = 7 - (cx & 7);
			const unsigned planes = ((m_cursor[address] >> shift) & 1) |
				(((m_cursor[address + 0x200] >> shift) & 1) << 1);
			const int color = colors[mode - 1][planes];
			uint32_t& pixel = frame.argb[size_t(y) * frame.width + unsigned(x)];
			if (color > 0)
				pixel = argb[color];
			else if (color < 0)
				pixel ^=
					0x00ffffffu; // Preserve the host frame's opaque alpha channel.
		}
	}
}

void CPermedia2::write_ppm(std::ostream& out, const Frame& f)
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

void CPermedia2::advance_scanlines(uint32_t lines)
{
	// Explicitly virtual scanlines, not PLL cycles. Caller controls time.
	if (!(r(VideoControl) & 1))
		return;
	const uint32_t total = (r(VTotal) & 0x7ff) + 1;
	for (uint32_t n = 0; n < lines; ++n)
	{
		r(LineCount) = (r(LineCount) + 1) % total;
		if (r(LineCount) == 0)
		{
			++m_frame_number;
			r(IntFlags) |= IRQ_VBLANK;
			m_active_screen_base = r(ScreenBase);
			r(VideoControl) &= ~0x80u;
		}
		if (r(LineCount) == (r(InterruptLine) & 0x7ff))
			r(IntFlags) |= IRQ_SCANLINE;
	}
	update_irq();
}

// Device lifecycle

CPermedia2::CPermedia2() : CPermedia2(Options{})
{
}

CPermedia2::CPermedia2(const Options& o) : m_options(o), m_vram(VramSize)
{
	if (o.input_fifo_entries != 32 && o.input_fifo_entries != 256 &&
		o.input_fifo_entries != 258)
		throw std::invalid_argument(
			"Permedia2 input FIFO capacity must be 32, 256 or 258");
	dac_map(m_dac_map);
	reset();
}

void CPermedia2::reset(bool clear_vram)
{
	// PCI configuration belongs to the board wrapper and is NOT software-reset here.
	m_regs.fill(0);
	m_dac.fill(0);
	m_palette.fill(0);
	m_cursor.fill(0);
	m_cursor_colors.fill(0);
	m_input.clear();
	m_output.clear();
	m_decoder = {};
	m_job = {};
	m_warned.fill(false);
	m_palette_w = m_palette_r = m_cursor_color_pos = m_cursor_pos = 0;
	m_active_screen_base = m_dma_cursor = m_out_dma_cursor = 0;
	m_relative_offset = 0;
	m_frame_number = m_wait_frame = 0;
	m_waiting_frame = m_halted = false;
	m_last_fault.clear();
	r(ChipConfig) = m_options.chip_config;
	r(MemControl) = m_options.mem_control;
	r(MemConfig) = m_options.mem_config;
	r(BootAddress) = 0x31;
	r(VSConfiguration) = 0x1f0;
	r(FifoControl) = 0x1010;
	r(BypassWriteMask) = r(FramebufferWriteMask) = 0xffffffffu;
	r(FBSoftwareWriteMask) = r(FBHardwareWriteMask) = 0xffffffffu;
	r(PixelMask) = 255;
	if (clear_vram)
		std::fill(m_vram.begin(), m_vram.end(), uint8_t(0));
	if (m_irq)
	{
		m_irq = false;
		if (m_irq_callback)
			m_irq_callback(false);
	}
}

void CPermedia2::report(
	const char* code, uint32_t a, uint32_t v, const char* message, bool fatal)
{
	if (fatal)
	{
		m_halted = true;
		m_last_fault = std::string(code) + ": " + message;
	}
	if (m_diagnostic)
		m_diagnostic(Diagnostic{code, message, a, v, fatal});
}

// Snapshots

static void put32(std::ostream& s, uint32_t v)
{
	for (unsigned i = 0; i < 4; ++i)
		s.put(char(v >> (i * 8)));
}

static void put64(std::ostream& s, uint64_t v)
{
	put32(s, uint32_t(v));
	put32(s, uint32_t(v >> 32));
}

static uint32_t get32(std::istream& s)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < 4; ++i)
	{
		const int c = s.get();
		if (c < 0)
			throw std::runtime_error("Truncated PM2 snapshot");
		v |= uint32_t(c) << (8 * i);
	}
	return v;
}

static uint64_t get64(std::istream& s)
{
	const uint32_t lo = get32(s), hi = get32(s);
	return lo | (uint64_t(hi) << 32);
}

static int64_t signed64(uint64_t v)
{
	return v <= uint64_t(INT64_MAX) ? int64_t(v)
									: (-1 - static_cast<int64_t>(~v));
}

static int32_t signed32(uint32_t v)
{
	return v <= uint32_t(INT32_MAX) ? int32_t(v)
									: (-1 - static_cast<int32_t>(~v));
}

static bool getbool(std::istream& s)
{
	const auto v = get32(s);
	if (v > 1)
		throw std::runtime_error("Invalid snapshot Boolean");
	return v != 0;
}

static void require(bool p)
{
	if (!p)
		throw std::runtime_error("Invalid PM2 snapshot state");
}

void CPermedia2::SaveState(std::ostream& s) const
{
	s.write("PM2SNP01", 8);
	put32(s, 5);
	put32(s, VramSize);
	put32(s, m_options.chip_config);
	put32(s, m_options.mem_control);
	put32(s, m_options.mem_config);
	put32(s, m_options.input_fifo_entries);
	for (const auto v : m_regs)
		put32(s, v);
	for (const auto* a : {&m_dac})
		s.write(
			reinterpret_cast<const char*>(a->data()),
			static_cast<std::streamsize>(a->size()));
	s.write(
		reinterpret_cast<const char*>(m_palette.data()),
		static_cast<std::streamsize>(m_palette.size()));
	s.write(
		reinterpret_cast<const char*>(m_cursor_colors.data()),
		static_cast<std::streamsize>(m_cursor_colors.size()));
	s.write(
		reinterpret_cast<const char*>(m_cursor.data()),
		static_cast<std::streamsize>(m_cursor.size()));
	put32(s, m_palette_w);
	put32(s, m_palette_r);
	put32(s, m_cursor_color_pos);
	put32(s, m_cursor_pos);
	put32(s, m_active_screen_base);
	put32(s, m_dma_cursor);
	put32(s, uint32_t(m_relative_offset));
	put64(s, m_frame_number);
	put64(s, m_wait_frame);
	put32(s, m_halted);
	put32(s, m_waiting_frame);
	put32(s, m_decoder.mode);
	put32(s, m_decoder.tag);
	put32(s, m_decoder.remaining);
	put32(s, m_decoder.mask);
	put32(s, m_job.active);
	put32(s, m_job.command);
	put32(s, m_job.primitive);
	put32(s, m_job.row);
	put32(s, m_job.col);
	put32(s, m_job.rows);
	put32(s, m_job.columns);
	put32(s, m_job.payload);
	put32(s, m_job.payload_left);
	put32(s, m_job.payload_tag);
	put32(s, uint32_t(m_job.origin_x));
	put32(s, uint32_t(m_job.origin_y));
	for (const int64_t v :
		{m_job.xdom, m_job.xsub, m_job.y, m_job.dxdom, m_job.dxsub, m_job.dy})
		put64(s, uint64_t(v));
	for (const auto* a : {&m_job.color, &m_job.dcolor_dx, &m_job.dcolor_dy})
		for (const int64_t v : *a)
			put64(s, uint64_t(v));
	put32(s, m_job.alpha);
	put32(s, m_job.interpolation);
	for (const int64_t v : {m_job.z, m_job.dzdx, m_job.dzdy})
		put64(s, uint64_t(v));
	put32(s, static_cast<uint32_t>(m_input.size()));
	for (const auto& c : m_input)
	{
		put32(s, c.address);
		put32(s, c.value);
	}
	put32(s, static_cast<uint32_t>(m_output.size()));
	for (const auto& o : m_output)
	{
		put32(s, o.value);
		put32(s, o.interrupt);
	}
	put32(s, static_cast<uint32_t>(m_last_fault.size()));
	s.write(
		m_last_fault.data(), static_cast<std::streamsize>(m_last_fault.size()));
	s.write(
		reinterpret_cast<const char*>(m_vram.data()),
		static_cast<std::streamsize>(m_vram.size()));
	// The latched output DMA cursor differs from the independently
	// programmable OutDMAAddress register.
	put32(s, m_out_dma_cursor);
	put32(s, m_job.setup);
	put32(s, m_job.knee_rows);
	put64(s, uint64_t(m_job.knee_xsub));
	put64(s, uint64_t(m_job.knee_dxsub));
	if (!s)
		throw std::runtime_error("PM2 snapshot write failed");
}

void CPermedia2::restore_state(std::istream& s)
{
	char magic[8]{};
	s.read(magic, 8);
	require(std::string(magic, 8) == "PM2SNP01");
	require(get32(s) == 5);
	require(get32(s) == VramSize);
	m_options.chip_config = get32(s);
	m_options.mem_control = get32(s);
	m_options.mem_config = get32(s);
	m_options.input_fifo_entries = get32(s);
	require(
		m_options.input_fifo_entries == 32 ||
		m_options.input_fifo_entries == 256 ||
		m_options.input_fifo_entries == 258);
	for (auto& v : m_regs)
		v = get32(s);
	s.read(
		reinterpret_cast<char*>(m_dac.data()),
		static_cast<std::streamsize>(m_dac.size()));
	s.read(
		reinterpret_cast<char*>(m_palette.data()),
		static_cast<std::streamsize>(m_palette.size()));
	s.read(
		reinterpret_cast<char*>(m_cursor_colors.data()),
		static_cast<std::streamsize>(m_cursor_colors.size()));
	s.read(
		reinterpret_cast<char*>(m_cursor.data()),
		static_cast<std::streamsize>(m_cursor.size()));
	m_palette_w = get32(s);
	m_palette_r = get32(s);
	m_cursor_color_pos = get32(s);
	m_cursor_pos = get32(s);
	m_active_screen_base = get32(s);
	m_dma_cursor = get32(s);
	m_relative_offset = signed32(get32(s));
	m_frame_number = get64(s);
	m_wait_frame = get64(s);
	m_halted = getbool(s);
	m_waiting_frame = getbool(s);
	m_decoder.mode = get32(s);
	m_decoder.tag = get32(s);
	m_decoder.remaining = get32(s);
	m_decoder.mask = get32(s);
	m_job.active = getbool(s);
	m_job.command = get32(s);
	m_job.primitive = get32(s);
	m_job.row = get32(s);
	m_job.col = get32(s);
	m_job.rows = get32(s);
	m_job.columns = get32(s);
	m_job.payload = get32(s);
	m_job.payload_left = get32(s);
	m_job.payload_tag = get32(s);
	m_job.origin_x = signed32(get32(s));
	m_job.origin_y = signed32(get32(s));
	for (int64_t* p : {&m_job.xdom,
			 &m_job.xsub,
			 &m_job.y,
			 &m_job.dxdom,
			 &m_job.dxsub,
			 &m_job.dy})
		*p = signed64(get64(s));
	for (auto* a : {&m_job.color, &m_job.dcolor_dx, &m_job.dcolor_dy})
		for (int64_t& v : *a)
			v = signed64(get64(s));
	m_job.alpha = get32(s);
	m_job.interpolation = get32(s);
	for (int64_t* p : {&m_job.z, &m_job.dzdx, &m_job.dzdy})
		*p = signed64(get64(s));
	for (const int64_t v : m_job.color)
		require(v >= -(int64_t(1) << 47) && v < (int64_t(1) << 47));
	for (const auto* a : {&m_job.dcolor_dx, &m_job.dcolor_dy})
		for (const int64_t v : *a)
			require(v >= -(int64_t(1) << 19) && v < (int64_t(1) << 19));
	require(m_job.alpha <= 255);
	require(m_job.interpolation <= 5);
	require(m_job.z >= -(int64_t(1) << 47) && m_job.z < (int64_t(1) << 47));
	for (const int64_t v : {m_job.dzdx, m_job.dzdy})
		require(v >= -(int64_t(1) << 27) && v < (int64_t(1) << 27));
	require(
		m_palette_w < 768 && m_palette_r < 768 && m_cursor_color_pos < 12 &&
		m_cursor_pos < 1024);
	require(
		m_active_screen_base <= 0x1fffff && m_relative_offset >= -4 &&
		m_relative_offset <= 3);
	require(
		m_decoder.mode <= 2 && m_decoder.tag < 1024 &&
		m_decoder.remaining <= 65536 && m_decoder.mask <= 65535);
	if (m_decoder.mode == 2)
	{
		unsigned count = 0;
		for (auto m = m_decoder.mask; m; m >>= 1)
			count += m & 1;
		require(count == m_decoder.remaining);
	}
	require(
		m_job.rows <= 65535 && m_job.columns <= 262140 &&
		m_job.payload_left <= 32);
	require(
		m_job.primitive == 0 || m_job.primitive == 0x40 ||
		m_job.primitive == 0x80 || m_job.primitive == 0xc0);
	if (m_job.active)
		require(
			m_job.row < m_job.rows &&
			(m_job.columns == 0 || m_job.col < m_job.columns));
	for (const auto v : {m_job.xdom, m_job.xsub, m_job.y})
		require(v >= -(int64_t(1) << 47) && v < (int64_t(1) << 47));
	for (const auto v : {m_job.dxdom, m_job.dxsub, m_job.dy})
		require(v >= INT32_MIN && v <= INT32_MAX);
	require(
		m_job.origin_x >= -131072 && m_job.origin_x <= 131068 &&
		m_job.origin_y >= -32768 && m_job.origin_y <= 32767);
	require(r(DMACount) <= 65535 && r(OutDMACount) <= 65535);
	const uint32_t n = get32(s);
	require(n <= m_options.input_fifo_entries);
	m_input.clear();
	for (uint32_t i = 0; i < n; ++i)
	{
		Command c;
		c.address = get32(s);
		c.value = get32(s);
		require(
			c.address >= 0x8000 && c.address < 0x10000 && (c.address & 7) == 0);
		m_input.push_back(c);
	}
	const uint32_t no = get32(s);
	require(no <= 8);
	m_output.clear();
	for (uint32_t i = 0; i < no; ++i)
	{
		Output o;
		o.value = get32(s);
		o.interrupt = getbool(s);
		m_output.push_back(o);
	}
	const uint32_t len = get32(s);
	require(len <= 4096);
	m_last_fault.resize(len);
	s.read(m_last_fault.data(), len);
	s.read(
		reinterpret_cast<char*>(m_vram.data()),
		static_cast<std::streamsize>(m_vram.size()));
	m_out_dma_cursor = get32(s);
	m_job.setup = getbool(s);
	m_job.knee_rows = get32(s);
	m_job.knee_xsub = signed64(get64(s));
	m_job.knee_dxsub = signed64(get64(s));
	require(m_job.knee_rows <= 4096);
	require(m_job.knee_xsub >= -134217728 && m_job.knee_xsub < 134217728);
	require(m_job.knee_dxsub >= -134217728 && m_job.knee_dxsub <= 134217726);
	if (m_job.setup)
	{
		require(m_job.primitive == PrimitiveTrapezoid);
		require(!(m_job.command & (FastFill | SyncHost | SyncMask | Texture)));
		require(m_job.payload_left == 0);
		require(m_job.active || m_job.knee_rows == 0);
		require(m_job.dy == 65536);
		require(m_job.y % 65536 == 0);
		for (const int64_t delta : {m_job.dxdom, m_job.dxsub, m_job.knee_dxsub})
			require(delta >= -134217728 && delta <= 134217726 && delta % 2 == 0);
		require((r(RasterizerMode) & 0x30) == 0);
		const int64_t width = std::abs(fixed_integer(m_job.xdom) -
			fixed_integer(m_job.xsub));
		require(width <= 65535 && m_job.columns == uint32_t(width));
		require(m_job.columns != 0 || m_job.col == 0);
		const int64_t first_steps = m_job.active ? m_job.rows - m_job.row - 1 : 0;
		const int64_t steps = first_steps + m_job.knee_rows;
		const auto coordinate_range = [](int64_t position, int64_t delta,
			int64_t count) {
			const int64_t end = position + delta * count;
			return position >= -134217728 && position < 134217728 &&
				end >= -134217728 && end < 134217728;
		};
		require(coordinate_range(m_job.xdom, m_job.dxdom, steps));
		require(coordinate_range(m_job.xsub, m_job.dxsub, first_steps));
		require(coordinate_range(m_job.y, m_job.dy, steps));
		if (m_job.knee_rows)
			require(coordinate_range(m_job.knee_xsub, m_job.knee_dxsub,
				m_job.knee_rows - 1));
	}
	else
		require(m_job.knee_rows == 0);
	require(bool(s));
	m_warned.fill(false);
	m_irq = (r(IntFlags) & r(IntEnable)) != 0;
	// Active jobs must have valid layouts for the units they actually use.
	if (m_job.active)
	{
		require((m_job.command & 0xc0) == m_job.primitive);
		require(validate_render(m_job.command, m_job.setup));
		require(m_job.interpolation == interpolation_kind(m_job.command));
		if (packed_trapezoid(m_job.command))
		{
			require(validate_packed_trapezoid(m_job, Render, m_job.command));
			const int64_t width = std::abs(fixed_integer(m_job.xdom) -
				fixed_integer(m_job.xsub));
			const unsigned lanes = 4 / render_bytes();
			require(m_job.columns == uint64_t(width) * lanes);
			require(m_job.columns != 0 || m_job.col == 0);
			if (m_job.command & SyncHost)
			{
				const unsigned used = m_job.col % lanes;
				require(used == 0
					? m_job.payload_left == 0 || m_job.payload_left == lanes
					: m_job.payload_left == lanes - used);
				if (m_job.payload_left)
					require(m_job.columns != 0 &&
						(m_job.payload_tag == Color || m_job.payload_tag == FBData));
			}
			else
				require(m_job.payload_left == 0);
		}
		if ((m_job.command & Texture) && !(m_job.command & FastFill))
		{
			// The indexed copy derives texture positions from progress within
			// its original straight trapezoid. Restored geometry must match it.
			require(m_job.rows == (r(RasterCount) & 0xffff));
			require(m_job.xdom == sx(r(StartXDom), 32));
			require(m_job.xsub == sx(r(StartXSub), 32));
			require(m_job.y == int64_t(sx(r(StartY), 32)) +
				int64_t(m_job.row) * 65536);
			require(m_job.dxdom == 0 && m_job.dxsub == 0 && m_job.dy == 65536);
			require(m_job.columns == uint32_t((m_job.xsub - m_job.xdom) / 65536));
		}
	}
}

void CPermedia2::RestoreState(std::istream& s)
{
	CPermedia2 candidate;
	candidate.restore_state(s); // fail without altering live state or callbacks
	const bool old_irq = m_irq;
	// Only emulated state is replaced. Register-map lambdas stay bound to
	// this device and its externally supplied ROM/DMA/IRQ callbacks survive.
	using std::swap;
	swap(m_options, candidate.m_options);
	swap(m_regs, candidate.m_regs);
	swap(m_vram, candidate.m_vram);
	swap(m_dac, candidate.m_dac);
	swap(m_palette, candidate.m_palette);
	swap(m_cursor_colors, candidate.m_cursor_colors);
	swap(m_cursor, candidate.m_cursor);
	swap(m_palette_w, candidate.m_palette_w);
	swap(m_palette_r, candidate.m_palette_r);
	swap(m_cursor_color_pos, candidate.m_cursor_color_pos);
	swap(m_cursor_pos, candidate.m_cursor_pos);
	swap(m_active_screen_base, candidate.m_active_screen_base);
	swap(m_dma_cursor, candidate.m_dma_cursor);
	swap(m_out_dma_cursor, candidate.m_out_dma_cursor);
	swap(m_relative_offset, candidate.m_relative_offset);
	swap(m_frame_number, candidate.m_frame_number);
	swap(m_wait_frame, candidate.m_wait_frame);
	swap(m_input, candidate.m_input);
	swap(m_output, candidate.m_output);
	swap(m_halted, candidate.m_halted);
	swap(m_irq, candidate.m_irq);
	swap(m_waiting_frame, candidate.m_waiting_frame);
	swap(m_last_fault, candidate.m_last_fault);
	swap(m_warned, candidate.m_warned);
	swap(m_decoder, candidate.m_decoder);
	swap(m_job, candidate.m_job);
	if (old_irq != m_irq && m_irq_callback)
		m_irq_callback(m_irq);
}
