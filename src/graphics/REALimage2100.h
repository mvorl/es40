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

#if !defined(INCLUDED_REALimage2100_H_)
#define INCLUDED_REALimage2100_H_

#include "address_map.h"
#include <array>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

/** REALimage 2100 native transport and legacy VGA storage.
 * No native command processor, framebuffer layout or accelerator is modeled.
 * Unknown native registers are shadowed with a one-time diagnostic, as in
 * CPermedia2.
 */
class CRealImage2100
{
public:
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

  static constexpr uint32_t VGAMemorySize = 0x40000;
  // ROM file offsets 0x378a, 0x37a1 and 0x37b8 load these indirect addresses.
  static constexpr uint32_t DACIndexLow = 0x00838010, DACIndexHigh = 0x00838014,
                            DACIndexedData = 0x00838018;
  static constexpr uint32_t DACRegisterCount = 0x10000;
  // Bounds shadow storage; BAR0 decodes far more than any register file.
  static constexpr uint32_t MaxShadowRegisters = 4096;

  using DiagnosticCallback = std::function<void(const Diagnostic&)>;
  using UnimplementedCallback = std::function<void(
    const char* what, uint32_t address, uint32_t value, bool write)>;

  CRealImage2100();
  CRealImage2100(const CRealImage2100&) = delete;
  CRealImage2100& operator=(const CRealImage2100&) = delete;
  void reset(bool clear_vga_memory = false);
  // Widths are BITS, like CPermedia2/CRV200 and CPCIDevice callbacks.
  uint32_t ReadMem(uint32_t address, int dsize = 32);
  void WriteMem(uint32_t address, int dsize, uint32_t data);
  // BAR2 index/data pair.
  uint32_t io_read(uint32_t offset, int dsize);
  void io_write(uint32_t offset, int dsize, uint32_t data);
  // BAR1 aperture; its native memory layout is not modeled.
  uint32_t mem_read(uint32_t address, int dsize);
  void mem_write(uint32_t address, int dsize, uint32_t data);

  uint32_t io_index() const { return m_io_index; }

  uint16_t dac_index() const { return m_dac_index; }

  void set_diagnostic_callback(DiagnosticCallback fn)
  {
    m_diagnostic = std::move(fn);
  }

  void set_unimplemented_callback(UnimplementedCallback fn)
  {
    m_unimplemented = std::move(fn);
  }

  // Borrowed by CVGA. No claim that native BAR1 aliases this storage.
  uint8_t* vram_data() { return m_vga_memory.data(); }

  const std::vector<uint8_t>& vram() const { return m_vga_memory; }

  uint32_t palette_color(unsigned i) const { return m_palette[i & 255]; }

  void set_palette_color(unsigned i, uint32_t c)
  {
    m_palette[i & 255] = c & 0xffffff;
  }

  uint8_t palette_read_index() const { return m_palette_read; }

  uint8_t palette_write_index() const { return m_palette_write; }

  void set_palette_read_index(uint8_t i) { m_palette_read = i; }

  void set_palette_write_index(uint8_t i) { m_palette_write = i; }

  // Versioned little-endian snapshot. Loading validates before committing.
  void SaveState(std::ostream&) const;
  void RestoreState(std::istream&);
  static void write_ppm(std::ostream&, const Frame&);

private:
  void dac_port_map(address_map& map);
  void report(
    const char* code, uint32_t address, uint32_t value, const char* message,
    bool fatal = false);
  void unimplemented(const char* what, uint32_t a, uint32_t v, bool write);
  // Once per register; the set is bounded like the shadow.
  void unimplemented_once(const char* what, uint32_t a, uint32_t v, bool write);

  address_map m_dac_ports{0x20};
  std::vector<uint8_t> m_vga_memory;
  std::array<uint32_t, 256> m_palette{};
  // IBM RAMDAC indexed registers. Storage only: the BIOS writes but never reads.
  std::vector<uint8_t> m_dac_regs;
  // Unknown native registers, keyed by dword-aligned address.
  std::map<uint32_t, uint32_t> m_shadow;
  std::set<uint32_t> m_warned;
  bool m_aperture_warned = false, m_shadow_full_warned = false;
  uint32_t m_io_index = 0;
  uint16_t m_dac_index = 0;
  uint8_t m_palette_read = 0, m_palette_write = 0;
  DiagnosticCallback m_diagnostic;
  UnimplementedCallback m_unimplemented;
};

#endif // INCLUDED_REALimage2100_H_
