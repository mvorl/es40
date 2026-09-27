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

#if !defined(INCLUDED_Permedia2_H_)
#define INCLUDED_Permedia2_H_

#include "address_map.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

class CPermedia2
{
public:
  struct Options
  {
    uint32_t chip_config = 0, mem_control = 0, mem_config = 0;
    // HRM Issue 6 says 256; SLAU011A contains conflicting 32-entry text.
    uint32_t input_fifo_entries = 256;
  };

  struct Diagnostic
  {
    std::string code, message;
    uint32_t address, value;
    bool fatal;
  };

  struct Frame
  {
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> argb;
  };

  // Numeric hardware interface: TI TVP4020 PRM SLAU011A and HRM Issue 6.
  static constexpr uint32_t
    ResetStatus = 0x0000,
    IntEnable = 0x0008, IntFlags = 0x0010, InFIFOSpace = 0x0018,
    OutFIFOWords = 0x0020, DMAAddress = 0x0028, DMACount = 0x0030,
    ErrorFlags = 0x0038, VClkCtl = 0x0040, TestRegister = 0x0048,
    ApertureOne = 0x0050, ApertureTwo = 0x0058, DMAControl = 0x0060,
    FIFODiscon = 0x0068, ChipConfig = 0x0070, OutDMAAddress = 0x0080,
    OutDMACount = 0x0088, Reboot = 0x1000, MemControl = 0x1040,
    BootAddress = 0x1080, MemConfig = 0x10c0, BypassWriteMask = 0x1100,
    FramebufferWriteMask = 0x1140, Count = 0x1180, FIFO = 0x2000,
    ScreenBase = 0x3000, ScreenStride = 0x3008, HTotal = 0x3010, HgEnd = 0x3018,
    HbEnd = 0x3020, HsStart = 0x3028, HsEnd = 0x3030, VTotal = 0x3038,
    VbEnd = 0x3040, VsStart = 0x3048, VsEnd = 0x3050, VideoControl = 0x3058,
    InterruptLine = 0x3060, DisplayData = 0x3068, LineCount = 0x3070,
    FifoControl = 0x3078, ScreenBaseRight = 0x3080, PaletteWrite = 0x4000,
    PaletteData = 0x4008, PixelMask = 0x4010, PaletteRead = 0x4018,
    CursorColorAddress = 0x4020, CursorColorData = 0x4028, IndexedData = 0x4050,
    CursorRAM = 0x4058, CursorXLow = 0x4060, CursorXHigh = 0x4068,
    CursorYLow = 0x4070, CursorYHigh = 0x4078, VSConfiguration = 0x5800,
    StartXDom = 0x8000, dXDom = 0x8008, StartXSub = 0x8010, dXSub = 0x8018,
    StartY = 0x8020, dY = 0x8028, RasterCount = 0x8030, Render = 0x8038,
    ContinueNewLine = 0x8040, ContinueNewDom = 0x8048, ContinueNewSub = 0x8050,
    Continue = 0x8058, BitMaskPattern = 0x8068, RasterizerMode = 0x80a0,
    RectangleOrigin = 0x80d0, RectangleSize = 0x80d8, PackedDataLimits = 0x8150,
    ScissorMode = 0x8180, ScissorMin = 0x8188, ScissorMax = 0x8190,
    ScreenSize = 0x8198, AreaStippleMode = 0x81a0, WindowOrigin = 0x81c8,
    AreaStipplePattern0 = 0x8200, AreaStipplePattern1 = 0x8208,
    AreaStipplePattern2 = 0x8210, AreaStipplePattern3 = 0x8218,
    AreaStipplePattern4 = 0x8220, AreaStipplePattern5 = 0x8228,
    AreaStipplePattern6 = 0x8230, AreaStipplePattern7 = 0x8238,
    TextureAddressMode = 0x8380, SStart = 0x8388, dSdx = 0x8390,
    dSdyDom = 0x8398, TStart = 0x83a0, dTdx = 0x83a8, dTdyDom = 0x83b0,
    TextureBaseAddress = 0x8580, TextureMapFormat = 0x8588,
    TextureDataFormat = 0x8590, Texel0 = 0x8600, TextureReadMode = 0x8670,
    TextureLUTMode = 0x8678, TextureColorMode = 0x8680, FogMode = 0x8690,
    RStart = 0x8780, ColorDDAMode = 0x87e0, ConstantColor = 0x87e8,
    Color = 0x87f0, AlphaTestMode = 0x8800, AntialiasMode = 0x8808,
    AlphaBlendMode = 0x8810, DitherMode = 0x8818, FBSoftwareWriteMask = 0x8820,
    LogicalOpMode = 0x8828, FBWriteData = 0x8830, LBReadMode = 0x8880,
    LBWriteMode = 0x88c0, TextureData = 0x88e8, TextureDownloadOffset = 0x88f0,
    StencilMode = 0x8988, Stencil = 0x8998, DepthMode = 0x89a0,
    Depth = 0x89a8, FBReadMode = 0x8a80,
    FBSourceOffset = 0x8a88, FBPixelOffset = 0x8a90, FBColor = 0x8a98,
    FBData = 0x8aa0, FBSourceData = 0x8aa8, FBWindowBase = 0x8ab0,
    FBWriteMode = 0x8ab8, FBHardwareWriteMask = 0x8ac0, FBBlockColor = 0x8ac8,
    FBReadPixel = 0x8ad0, FBWriteConfig = 0x8ae8, FilterMode = 0x8c00,
    StatisticMode = 0x8c08, Sync = 0x8c40, SuspendUntilFrameBlank = 0x8c78,
    FBSourceBase = 0x8d80, FBSourceDelta = 0x8d88, Config = 0x8d90,
    YUVMode = 0x8f00, DeltaMode = 0x9300, DrawTriangle = 0x9308;

  static constexpr uint32_t PrimitiveLine = 0, PrimitiveTrapezoid = 0x40,
                            PrimitivePoint = 0x80, PrimitiveRectangle = 0xc0,
                            FastFill = 8, SyncMask = 0x800, SyncHost = 0x1000,
                            Texture = 0x2000, PositiveX = 0x200000,
                            PositiveY = 0x400000, ReadSource = 0x200,
                            ReadDestination = 0x400, Packed = 0x80000;

  static constexpr uint32_t IRQ_DMA = 1, IRQ_SYNC = 2, IRQ_ERROR = 8,
                            IRQ_VBLANK = 0x10, IRQ_SCANLINE = 0x20;

  static constexpr uint8_t DACCursorControl = 6, DACColorMode = 0x18,
                           DACModeControl = 0x19, DACMiscControl = 0x1e,
                           DACPixelClockStatus = 0x29,
                           DACMemoryClockStatus = 0x33;

  static constexpr uint32_t tag(uint32_t offset)
  {
    return (offset - 0x8000u) / 8u;
  }

  static constexpr uint32_t VramSize = 8 * 1024 * 1024,
                            RegisterBARSize = 0x20000;
  using ROMReader = std::function<uint8_t(uint32_t)>;
  using DMAReader = std::function<bool(uint32_t, uint8_t*, size_t)>;
  using DMAWriter = std::function<bool(uint32_t, const uint8_t*, size_t)>;
  using IRQCallback = std::function<void(bool)>;
  using DiagnosticCallback = std::function<void(const Diagnostic&)>;
  CPermedia2();
  explicit CPermedia2(const Options& options);
  CPermedia2(const CPermedia2&) = delete;
  CPermedia2& operator=(const CPermedia2&) = delete;
  void reset(bool clear_vram = false);
  uint32_t ReadMem(uint32_t address, int dsize = 32);
  bool WriteMem(uint32_t address, int dsize, uint32_t data);
  uint32_t mem_read(unsigned aperture, uint32_t address, int dsize = 32);
  void mem_write(unsigned aperture, uint32_t address, int dsize, uint32_t data);
  // No access-driven hidden execution: embedding must call service explicitly.
  size_t service(size_t budget = 4096);
  // Deterministic *scanline* advancement; no host-wall-clock dependency in core.
  void advance_scanlines(uint32_t lines = 1);
  bool busy() const;

  bool halted() const { return m_halted; }

  bool irq_asserted() const { return m_irq; }

  uint32_t peek(uint32_t offset) const;

  size_t input_size() const { return m_input.size(); }

  size_t output_size() const { return m_output.size(); }

  const std::string& last_fault() const { return m_last_fault; }

  const std::vector<uint8_t>& vram() const { return m_vram; }

  uint8_t* vram_data() { return m_vram.data(); }

  uint32_t palette_color(uint32_t index) const
  {
    return scanout_color(index, 0);
  }

  void set_rom_reader(ROMReader fn) { m_rom_reader = std::move(fn); }

  void set_dma_reader(DMAReader fn) { m_dma_reader = std::move(fn); }

  void set_dma_writer(DMAWriter fn) { m_dma_writer = std::move(fn); }

  void set_irq_callback(IRQCallback fn) { m_irq_callback = std::move(fn); }

  void set_diagnostic_callback(DiagnosticCallback fn)
  {
    m_diagnostic = std::move(fn);
  }

  Frame scanout(bool include_cursor = true) const;
  static void write_ppm(std::ostream&, const Frame&);
  // Versioned little-endian snapshot. Loading validates into a temporary object.
  void SaveState(std::ostream&) const;
  void RestoreState(std::istream&);
  static uint32_t pitch_from_products(uint32_t mode);
  static uint32_t logical_op(unsigned op, uint32_t source, uint32_t dest);
  static const char* register_name(uint32_t address);

private:
  struct Command
  {
    uint32_t address = 0, value = 0;
  };

  struct Output
  {
    uint32_t value = 0;
    bool interrupt = false;
  };

  struct Decoder
  {
    uint32_t mode = 0, tag = 0, remaining = 0, mask = 0;
  } m_decoder;

  struct Job
  {
    bool active = false;
    uint32_t command = 0, primitive = 0, row = 0, col = 0, rows = 0,
             columns = 0;
    uint32_t payload = 0, payload_left = 0, payload_tag = 0;
    int32_t origin_x = 0, origin_y = 0;
    int64_t xdom = 0, xsub = 0, y = 0, dxdom = 0, dxsub = 0, dy = 0;
  } m_job;

  Options m_options;
  std::array<uint32_t, 8192> m_regs{}; // 64K canonical space, 8-byte slots.
  std::vector<uint8_t> m_vram;
  std::array<uint8_t, 256> m_dac{};
  std::array<uint8_t, 768> m_palette{};
  std::array<uint8_t, 12> m_cursor_colors{};
  std::array<uint8_t, 1024> m_cursor{};
  uint32_t m_palette_w = 0, m_palette_r = 0, m_cursor_color_pos = 0,
           m_cursor_pos = 0;
  uint32_t m_active_screen_base = 0, m_dma_cursor = 0, m_out_dma_cursor = 0;
  int32_t m_relative_offset = 0;
  uint64_t m_frame_number = 0, m_wait_frame = 0;
  std::deque<Command> m_input;
  std::deque<Output> m_output;
  bool m_halted = false, m_irq = false, m_waiting_frame = false;
  std::string m_last_fault;
  ROMReader m_rom_reader;
  DMAReader m_dma_reader;
  DMAWriter m_dma_writer;
  IRQCallback m_irq_callback;
  DiagnosticCallback m_diagnostic;
  std::array<bool, 8192> m_warned{};
  address_map m_dac_map{256};

  uint32_t& r(uint32_t a) { return m_regs[a / 8]; }

  uint32_t r(uint32_t a) const { return m_regs[a / 8]; }

  void report(const char*, uint32_t, uint32_t, const char*, bool fatal = false);
  void update_irq();
  void signal_error(uint32_t flags);
  void output_push(uint32_t value, bool interrupt = false);
  bool packet_word(uint32_t value);
  bool execute(const Command&);
  bool validate_render(uint32_t value);
  bool validate_texture_block(uint32_t value);
  bool load_texture_mask();
  void start_render(uint32_t value);
  void continue_render(uint32_t address, uint32_t value);
  bool draw_step();
  bool framebuffer_upload() const;
  bool upload_pixel(int32_t x, int32_t y);
  void next_fragment();
  void set_span();
  bool emit_pixel(int32_t x, int32_t y, uint32_t value, bool raw);
  uint32_t pixel_read(int64_t address, unsigned bytes) const;
  void pixel_write(
    int64_t address, unsigned bytes, uint32_t value, uint32_t swmask,
    uint32_t hwmask);
  unsigned render_bytes() const;
  unsigned texture_bytes() const;
  bool clipped(int64_t x, int64_t y, bool packed_limits = true) const;
  uint32_t format_color(uint32_t value) const;
  uint32_t scanout_color(uint32_t raw, uint8_t format) const;
  void composite_cursor(Frame&) const;
  void restore_state(std::istream&);
  uint32_t register_read(uint32_t address);
  bool register_write(uint32_t address, uint32_t value);
  void dac_map(address_map& map);
};

#endif // INCLUDED_Permedia2_H_
