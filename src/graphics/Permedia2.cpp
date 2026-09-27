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
		a == CPermedia2::Color;
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
		report(
			"UNSUPPORTED_COMMAND",
			DrawTriangle,
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
		if (mode == 3 || (value & 0x3e00u))
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
		m_decoder.tag = value & 0x1ff;
		m_decoder.mask = value >> 16;
		if (mode == 2)
		{
			m_decoder.tag &= 0x1f0;
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
		m_decoder.tag = (m_decoder.tag + 1) & 0x1ff;
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
	// Bound section 4.9.7's cached-font operation to forward rectangles and a
	// linear one-dimensional mask stream. Other texture pipelines stay fatal.
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

bool CPermedia2::load_texture_mask()
{
	// RestoreState accepts the serialized Job without re-running Render.
	// Recheck this new path before deriving addresses from restored modes.
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

bool CPermedia2::validate_render(uint32_t value)
{
	// Render.Texture qualifies the texture units (SLAU011A, p. 4-79). Their
	// retained mode registers have no effect when this command leaves it clear.
	// Section 4.9.7 defines a separate block-fill mask path; ordinary color
	// texturing remains unsupported.
	const bool texture_block =
		(value & (FastFill | Texture)) == (FastFill | Texture);
	const uint32_t unsupported = r(AlphaBlendMode) | r(AlphaTestMode) |
		r(DepthMode) | r(StencilMode) | r(FogMode) | r(AntialiasMode) |
		r(YUVMode);
	if ((unsupported & 1) || ((value & Texture) && !texture_block) ||
		(r(LBWriteMode) & 1) || (r(ColorDDAMode) & 2) ||
		(r(FBReadMode) & 0x40000) || (r(FBWriteConfig) & 0x40000) ||
		(r(DitherMode) & 2) || (r(LogicalOpMode) & ~63u))
	{
		report(
			"RENDER_MODE",
			Render,
			value,
			"Active 3D, interpolated, patched, dithered or unsupported logical "
			"mode",
			true);
		return false;
	}
	if (texture_block && !validate_texture_block(value))
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
	if (bytes == 0 || pitch_from_products(r(FBWriteConfig)) == 0 ||
		((r(FBReadMode) & Packed) && (bytes == 3)))
	{
		report(
			"PIXEL_LAYOUT",
			Render,
			value,
			"Invalid pitch/pixel size or packed 24-bit mode",
			true);
		return false;
	}
	// FBColor destination reads go directly to Host Out, independently of
	// FBWriteMode.WriteEnable (SLAU011A, Table 4-18). Formatted uploads and
	// combined read/write pipelines require additional ordering/format rules.
	if ((r(FBReadMode) & 0x8000) &&
		(r(FBReadMode) & (ReadSource | ReadDestination)))
	{
		if (!framebuffer_upload() || (r(FBReadMode) & ReadSource) ||
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
	const bool constant_fb_data = !framebuffer_upload() &&
		!(value & FastFill) && (r(LogicalOpMode) & 0x20);
	if (constant_fb_data)
	{
		// FBWriteData replaces the fragment color in the Logic Op unit. It cannot
		// be combined with logical operations or software writemasking
		// (SLAU011A, pp. 7-81/7-102); hardware writemasks still apply.
		if ((r(LogicalOpMode) & 1) || r(FBSoftwareWriteMask) != 0xffffffffu)
		{
			report(
				"FB_CONSTANT_MODE",
				LogicalOpMode,
				r(LogicalOpMode),
				"Constant framebuffer data requires disabled logical "
				"operations "
				"and software writemasking",
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
		(texture_block ? Texture : 0);
	if (value & ~supported)
	{
		report(
			"RENDER_FLAGS", Render, value, "Unimplemented Render flag", true);
		return false;
	}
	// NT4 sets reserved bit 2 (SLAU011A, p. 7-82); retain it without effect.
	if (r(FBWriteMode) & ~(1u | 0x4u))
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
	if ((value & SyncHost) &&
		((r(FBReadMode) & Packed) && m_relative_offset != 0))
	{
		report(
			"HOST_ALIGNMENT",
			PackedDataLimits,
			r(PackedDataLimits),
			"Nonzero packed host alignment is not yet implemented",
			true);
		return false;
	}
	if (r(DitherMode) & 1)
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

void CPermedia2::start_render(uint32_t value)
{
	m_job = {};
	if (!validate_render(value))
		return;
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
	if (m_job.primitive == PrimitiveRectangle)
	{
		m_job.columns = r(RectangleSize) & 0xffff;
		m_job.rows = (r(RectangleSize) >> 16) & 0xffff;
		if ((r(FBReadMode) & Packed) && !framebuffer_upload())
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
		if (r(FBReadMode) & Packed)
		{
			report(
				"PACKED_PRIMITIVE",
				Render,
				value,
				"Packed line/trapezoid not implemented",
				true);
			return;
		}
		set_span();
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
	const bool line = address == ContinueNewLine;
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
	if (r(FBReadMode) & Packed)
	{
		report(
			"PACKED_PRIMITIVE",
			address,
			value,
			"Packed line/trapezoid not implemented",
			true);
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
	m_job = next;
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
}

void CPermedia2::next_fragment()
{
	if (++m_job.col < m_job.columns)
		return;
	m_job.col = 0;
	if ((m_job.command & (FastFill | Texture)) == (FastFill | Texture))
		m_job.payload_left = 0;
	if (++m_job.row >= m_job.rows)
	{
		// Leave completed jobs at the last rendered DDA position.
		// A continuation applies this final DDA step before reloading slopes.
		m_job.active = false;
		return;
	}
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
		(m_job.command & (FastFill | Texture)) == (FastFill | Texture);
	const unsigned bytes = render_bytes();
	if (texture_block && m_job.payload_left == 0 && !load_texture_mask())
		return false;
	if ((mask_stream || host_stream) && m_job.payload_left == 0)
	{
		if (m_input.empty())
			return false;
		const Command c = m_input.front();
		if (mask_stream ? c.address != BitMaskPattern : !host_tag(c.address))
			return false;
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
		if (m_job.primitive == PrimitiveTrapezoid)
			x += (m_job.xsub >= m_job.xdom)
				? static_cast<int32_t>(m_job.col)
				: -static_cast<int32_t>(m_job.col) - 1;
	}
	if (framebuffer_upload())
	{
		if (!upload_pixel(x, y))
			return false;
		next_fragment();
		return true;
	}
	const bool dda = (r(ColorDDAMode) & 1) != 0;
	uint32_t color = dda ? r(ConstantColor) : r(Color);
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
			color &= (1u << (bytes * 8)) - 1;
			m_job.payload >>= bytes * 8;
		}
		--m_job.payload_left;
	}
	else if (r(FBReadMode) & ReadSource)
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
	next_fragment();
	return true;
}

bool CPermedia2::framebuffer_upload() const
{
	return (r(FBReadMode) & (0x8000u | ReadDestination)) ==
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
	if (r(ScissorMode) & 1)
	{
		if (x < sx(r(ScissorMin), 16) || y < sx(r(ScissorMin) >> 16, 16) ||
			x >= sx(r(ScissorMax), 16) || y >= sx(r(ScissorMax) >> 16, 16))
			return true;
	}
	if (r(ScissorMode) & 2)
	{
		if (x < 0 || y < 0 ||
			x >= static_cast<int32_t>(r(ScreenSize) & 0xffff) ||
			y >= static_cast<int32_t>(r(ScreenSize) >> 16))
			return true;
	}
	if (packed_limits && (r(FBReadMode) & Packed))
	{
		const int32_t start =
			static_cast<int32_t>((r(PackedDataLimits) >> 16) & 0xfff);
		const int32_t end = static_cast<int32_t>(r(PackedDataLimits) & 0xfff);
		if (x < start || x >= end)
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

uint32_t CPermedia2::format_color(uint32_t v) const
{
	if (!(r(DitherMode) & 1))
		return v;
	const uint32_t fmt =
		((r(DitherMode) >> 2) & 15) | ((r(DitherMode) >> 12) & 16);
	if (fmt == 14)
		return v & 255; // Color index, not a RAMDAC ColorMode enum
	const bool rgb = (r(DitherMode) & 0x400) != 0;
	const uint32_t red = rgb ? (v & 255) : ((v >> 16) & 255),
				   green = (v >> 8) & 255,
				   blue = rgb ? ((v >> 16) & 255) : (v & 255);
	uint32_t alpha = v >> 24;
	if (r(DitherMode) & 0x1000)
		alpha = 0;
	if (r(DitherMode) & 0x2000)
		alpha = 0xf8;
	switch (fmt)
	{
	case 0:
		return red | (green << 8) | (blue << 16) | (alpha << 24);
	case 1:
		return (red >> 3) | ((green >> 3) << 5) | ((blue >> 3) << 10) |
			((alpha >> 7) << 15);
	case 2:
		return (red >> 4) | ((green >> 4) << 4) | ((blue >> 4) << 8) |
			((alpha >> 4) << 12);
	case 16:
		return (red >> 3) | ((green >> 2) << 5) | ((blue >> 3) << 11);
	default:
		return 0; // start_render rejects unsupported active formats
	}
}

bool CPermedia2::emit_pixel(int32_t x, int32_t y, uint32_t value, bool raw)
{
	if (!(r(FBWriteMode) & 1) || clipped(x, y))
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
	const unsigned bytes = render_bytes();
	const int64_t sign = (r(FBWriteConfig) & 0x10000) ? -1 : 1;
	const int64_t pixel = int64_t(r(FBWindowBase)) +
		int64_t(y) * pitch_from_products(r(FBWriteConfig)) * sign + x +
		r(FBPixelOffset);
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
		// Model the raw word on framebuffer byte lanes, consistent with the
		// required replication of 8-bit colors across all bytes and 16-bit
		// colors across both halfwords (SLAU011A, p. 7-81).
		// FastFill uses FBBlockColor instead.
		value = r(FBWriteData) >> ((uint32_t(address) & 3) * 8);
	}
	else if (!raw)
		value = format_color(value);
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
	const int origin_x =
		int(((r(CursorXHigh) & 255) << 8) | (r(CursorXLow) & 255)) - 64;
	const int origin_y =
		int(((r(CursorYHigh) & 255) << 8) | (r(CursorYLow) & 255)) - 64;

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
	if (o.input_fifo_entries != 32 && o.input_fifo_entries != 256)
		throw std::invalid_argument(
			"Permedia2 input FIFO capacity must be 32 or 256");
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
	put32(s, 3);
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
	if (!s)
		throw std::runtime_error("PM2 snapshot write failed");
}

void CPermedia2::restore_state(std::istream& s)
{
	char magic[8]{};
	s.read(magic, 8);
	require(std::string(magic, 8) == "PM2SNP01");
	require(get32(s) == 3);
	require(get32(s) == VramSize);
	m_options.chip_config = get32(s);
	m_options.mem_control = get32(s);
	m_options.mem_config = get32(s);
	m_options.input_fifo_entries = get32(s);
	require(
		m_options.input_fifo_entries == 32 ||
		m_options.input_fifo_entries == 256);
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
	require(
		m_palette_w < 768 && m_palette_r < 768 && m_cursor_color_pos < 12 &&
		m_cursor_pos < 1024);
	require(
		m_active_screen_base <= 0x1fffff && m_relative_offset >= -4 &&
		m_relative_offset <= 3);
	require(
		m_decoder.mode <= 2 && m_decoder.tag < 512 &&
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
	require(bool(s));
	m_warned.fill(false);
	m_irq = (r(IntFlags) & r(IntEnable)) != 0;
	// Device-generated active jobs must have a supported nonzero pixel layout.
	if (m_job.active)
		require(
			render_bytes() != 0 && pitch_from_products(r(FBWriteConfig)) != 0);
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
