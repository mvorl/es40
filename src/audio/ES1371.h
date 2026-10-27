/* ES40 emulator.
 * Copyright (C) 2026 by Cacodemon345
 * Copyright (C) 2026 by gdwnldsKSC
 * Copyright (c) 2005 Vassili Karpov (malc) from QEMU
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
// ES1371 adaptation of the ES1370 port to ES40 by Cacodemon345.
#ifndef ES40_ES1371_H
#define ES40_ES1371_H
#include "StdAfx.h"
#include "system/System.h"
#include "emu/SystemComponent.h"
#include "emu/PCIDevice.h"

#include "HostAudio.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
class CES1371 : public CPCIDevice, public CRunnable
{
public:
  virtual int   SaveState(FILE* f) override;
  virtual int   RestoreState(FILE* f) override;
  virtual void  check_state() override;
  void run() override;
  virtual void  init();
  void ResetPCI() override;
  void config_write_custom(int func, u32 address, int dsize,
    u32 old_data, u32 new_data, u32 data) override;
  virtual void  start_threads() override;
  virtual void  stop_threads() override;

  virtual void  WriteMem_Bar(int func, int bar, u32 address, int dsize, u32 data);
  virtual u32   ReadMem_Bar(int func, int bar, u32 address, int dsize);

  CES1371(CConfigurator* cfg, class CSystem* c, int pcibus, int pcidev);
  virtual ~CES1371();

private:
  struct chan {
    uint32_t shift;
    uint32_t leftover;
    uint32_t scount;
    uint32_t frame_addr;
    uint32_t frame_cnt;
    uint32_t halted;
    uint32_t freq;
    // Worker-only refill pacing: unspent byte budget (16.16) and last accrual time.
    int64_t credit_q16;
    std::chrono::steady_clock::time_point pace_last;
  };

  struct ES1371State {
    host_audio::DeviceId audio_be_out;
    host_audio::DeviceId audio_be_in;
    struct chan chan[3];
    host_audio::Stream dac_voice[2];
    host_audio::Stream adc_voice;

    uint32_t ctl;
    uint32_t status;
    uint32_t mempage;
    uint32_t codec;
    uint32_t sctl;
    uint32_t src, src_phase, legacy, uart_control, uart_test;
    uint32_t phantom_addr, phantom_count;
    uint16_t src_ram[128];
    uint16_t ac97[64];
  } state;

  // Protected by the system bus mutex
  bool audio_running = false;

  CThread* m_audio_thread = nullptr;
  std::atomic<bool> m_audio_stop{true}, m_audio_failed{false}, m_audio_wake{false};
  std::atomic<bool> m_audio_enabled[3]{};
  std::mutex m_audio_wait_mutex;
  std::condition_variable m_audio_wait;

  // Host period: m_out_period_frames at m_out_freq Hz.
  int m_out_freq = 0;
  int m_out_period_frames = 0;
  // Queued playback lead (config "latency_ms").
  unsigned m_latency_ms = 40;
  host_audio::Backend m_audio_backend{};

  enum ServiceResult { SERVICE_IDLE, SERVICE_MORE, SERVICE_BUSY };

  void reset_audio_request(size_t channel, bool enabled, bool clear);
  void request_audio(size_t channel);
  ServiceResult service_audio_channel(size_t channel,
    std::chrono::steady_clock::time_point now);
  uint32_t chan_rate_bps(size_t channel) const;
  int dac_pull_bytes(uint32_t rate_bps) const;
  int dac_lead_bytes(size_t channel, uint32_t rate_bps) const;

  struct ES1371SavedState {
    uint32_t ctl, status, mempage, codec, sctl;
    uint32_t src, src_phase, legacy, uart_control, uart_test;
    uint32_t phantom_addr, phantom_count;
    uint16_t src_ram[128];
    uint16_t ac97[64];
    struct {
      uint32_t leftover, scount, frame_addr, frame_cnt, halted;
    } channel[3];
  };

  void unbind_voices();

  struct chan_bits {
    uint32_t ctl_en;
    uint32_t stat_int;
    uint32_t sctl_pause;
    uint32_t sctl_inten;
    uint32_t sctl_fmt;
    uint32_t sctl_sh_fmt;
    uint32_t sctl_loopsel;
  };

  static const chan_bits es1371_chan_bits[3];
  uint32_t channel_rate(size_t channel) const;
  bool channel_enabled(size_t channel, uint32_t ctl, uint32_t sctl) const;
  void reset_codec();
  void write_codec(uint32_t command);
  int apply_volume(size_t channel, uint8_t* data, int bytes);
  void es1371_update_status(ES1371State* s, uint32_t new_status);
  void es1371_reset(ES1371State* s);
  void es1371_maybe_lower_irq(ES1371State* s, uint32_t sctl);
  void es1371_update_voices(ES1371State* s, uint32_t ctl, uint32_t sctl,
    bool force = false);
  uint32_t es1371_fixup(ES1371State* s, uint32_t addr);
  void es1371_write(void* opaque, u64 addr, uint32_t val, uint32_t mask);
  uint64_t es1371_read(void* opaque, u64 addr, unsigned size);
  int es1371_transfer_audio(ES1371State* s, struct chan* d, int loop_sel, int max, bool* irq);
  int es1371_run_channel(ES1371State* s, size_t chan, int free_or_avail);

  static void es1371_dac_callback_dac1(void* userdata);
  static void es1371_dac_callback_dac2(void* userdata);
  static void es1371_dac_callback_adc(void* userdata);
};

#endif // ES40_ES1371_H
