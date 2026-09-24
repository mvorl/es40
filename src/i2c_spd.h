/* ES40 emulator.
 * Copyright (C) 2026 by the ES40 Emulator Project
 * Copyright (C) 2025 by Kisara Development LLC.
 * All rights reserved.
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

#pragma once
#include <cstdint>
#include <vector>
#include <memory>

// SPD EEPROM image for one registered-ECC PC100 SDR SDRAM DIMM of dimm_mb MB.
std::vector<uint8_t> build_sdram_spd(uint32_t dimm_mb, bool registered_ecc = true);

/* ------------ I2C abstract device (open-drain bus) ------------ */
class I2CDevice {
public:
  virtual ~I2CDevice() = default;

  // START/STOP edges (SDA falling/rising while SCL is high)
  virtual void on_start();
  virtual void on_stop();

  // Clock edges; sda_line is the current bus level of SDA at this instant.
  virtual void on_scl_rise(bool sda_line);
  virtual void on_scl_fall(bool sda_line);

  // Open-drain pulls: return true to pull the line LOW (0), false to release (1).
  virtual bool pull_sda_low() const;  // ACK/data 0-bit
  virtual bool pull_scl_low() const;  // clock stretching (unused by SPD)
};

/* ------------ I2C bus (wired-AND SCL/SDA) ------------ */
class I2CBus {
public:
  I2CBus();

  void attach(const std::shared_ptr<I2CDevice>& dev);

  // Host drives via MPD (1 = release/pull-up, 0 = pull low).
  void drive_from_host(bool scl_release, bool sda_release);

  // Current bus levels (after wired-AND with devices).
  bool scl() const;
  bool sda() const;

private:
  bool host_scl_;  // host driver: 1=released, 0=low
  bool host_sda_;
  bool line_scl_;  // actual line level after wired-AND (1=high)
  bool line_sda_;
  std::vector<std::shared_ptr<I2CDevice>> devs_;

  void recompute_lines();            // apply open-drain from all devices

  // Helpers to fan out bus events
  void notify_start();
  void notify_stop();
  void notify_scl_rise();
  void notify_scl_fall();
};

/* ------------ Minimal read-only 24C02 for SPD (0x50-0x57) ------------ */
class Eeprom24C02 : public I2CDevice {
public:
  explicit Eeprom24C02(uint8_t addr7, const std::vector<uint8_t>& image);

  // I2CDevice overrides
  void on_start() override;
  void on_stop() override;
  void on_scl_rise(bool sda_line) override;
  void on_scl_fall(bool sda_line) override;
  bool pull_sda_low() const override;

private:
  const uint8_t addr7_;
  std::vector<uint8_t> mem_;

  enum State { IDLE, ADDR, WORD, XMIT, RECV_ACK } st_;
  enum AckPhase { NONE, ACK_ADDR, ACK_WORD, ACK_DATA } ack_phase_;

  uint8_t shreg_;     // shift register
  uint8_t bitpos_;    // 0..8 (8 bits then ACK/NACK)
  uint8_t wordptr_;   // current memory pointer
  bool    rw_;        // 0=write, 1=read
  bool    ack_pull_;  // pulls SDA low when we ACK or transmit a '0' bit
  bool    addr_match_;

  // Internal helpers
  void enter_addr();
  void enter_word();
  void enter_xmit();
  void enter_idle();

  void prepare_next_tx_bit();  // while SCL is low
};
