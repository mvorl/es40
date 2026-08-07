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

/*
 * Kisara Development LLC contribution notice
 * Copyright (C) 2025 Kisara Development LLC.
 *
 * This contribution is being open sourced from our commercial Intel Itanium 2
 * emulation platform. Kisara Development LLC has removed the former
 * proprietary license notice and all associated proprietary restrictions. It
 * is now distributed under the BSD-1-Clause license above.
 *
 * No warranty, maintenance, support, update, or other service obligation is
 * provided. The code is supplied "AS IS" and remains subject to the warranty
 * disclaimer and limitation of liability stated above.
 */

/**
 * \file
 * Contains the definitions for the emulated LSI Logic 53C1020 controller.
 */
#if !defined(INCLUDED_LSI53C1020_H_)
#define INCLUDED_LSI53C1020_H_

#include "PCIDevice.h"
#include "DiskController.h"
#include "SCSIDevice.h"

#include <ctime>
#include <string>
#include <vector>

/**
 * \brief LSI Logic 53C1020 Fusion-MPT SCSI disk controller.
 *
 * The 53C1020 is a single-channel Ultra320 controller using Fusion-MPT
 * doorbells and message queues.  Its optional 512 KiB flash may contain an
 * x86 option ROM and IOC firmware; neither image is required by the emulated
 * IOC.
 *
 * This is not a native Alpha boot device: SRM has no Fusion-MPT driver and
 * cannot boot from it.  AlphaBIOS option-ROM discovery is verified, but booting
 * an operating system through SCSIBIOS is not.  A guest operating system may
 * use the controller through its own Fusion-MPT driver.
 */
class CLSI53C1020 : public CPCIDevice,
  public CDiskController,
  public CSCSIDevice
{
public:
  virtual int   SaveState(FILE* f);
  virtual int   RestoreState(FILE* f);
  virtual void  check_state();

  virtual void  init();
  virtual void  start_threads();
  virtual void  stop_threads();
  virtual void  ResetPCI();

  virtual void  WriteMem_Bar(int func, int bar, u32 address, int dsize,
    u32 data);
  virtual u32   ReadMem_Bar(int func, int bar, u32 address, int dsize);

  virtual void  register_disk(class CDisk* dsk, int bus, int dev);

  CLSI53C1020(CConfigurator* cfg, class CSystem* c, int pcibus, int pcidev);
  virtual       ~CLSI53C1020();

private:
  enum
  {
    LSI_MAX_TARGETS = 16,
    LSI_REPLY_FIFO_DEPTH = 128,
    LSI_HANDSHAKE_MAX_DWORDS = 32,
    LSI_REPLY_MAX_WORDS = 2 + (LSI_REPLY_FIFO_DEPTH * 2),
    LSI_MEMORY0_SIZE = 1024,
    LSI_SHARED_RAM_OFFSET = 0x80,
    LSI_SHARED_RAM_SIZE = LSI_MEMORY0_SIZE - LSI_SHARED_RAM_OFFSET,
    LSI_DIAG_BUFFER_SIZE = 4096
  };

  struct SLSI_sge
  {
    u64 addr;
    u32 len;
  };

  struct SLSI_scsi_result
  {
    u32 data_in_len;
    u32 data_out_used;
    u8  status;
    bool data_out_short;
  };

  void  load_flash();
  bool  load_flash_image(const std::string& path);
  bool  load_option_rom(const std::string& path);
  bool  load_ioc_firmware(const std::string& path);
  void  rebuild_flash_views();
  void  mark_flash_dirty();
  bool  save_flash();
  void  flush_flash();

  u8    read_b_flash(u32 address) const;
  void  write_b_flash(u32 address, u8 value);
  u8    read_b_diag(u32 address) const;
  void  write_b_diag(u32 address, u8 value);

  u32   read_l_doorbell() const;
  void  write_l_doorbell(u32 value);
  u32   read_l_host_int_status() const;
  void  write_l_host_int_status();
  u32   read_l_diag(u32 address) const;
  void  write_l_diag(u32 address, u32 value);

  u32   peek_l_register(u32 reg, bool io_space) const;
  u32   read_l_register(u32 reg, bool io_space);
  void  write_l_register(u32 reg, u32 value, bool io_space);

  void  clear_transport();
  void  clear_ioc_init_state();
  void  message_unit_reset();
  void  chip_reset();
  void  enter_fault(u16 fault_code);
  void  release_scsi_bus();
  void  reset_scsi_target(u8 target);
  void  reset_scsi_bus();

  bool  dma_read(u64 address, u8* data, u32 bytes);
  bool  dma_write(u64 address, const u8* data, u32 bytes);
  void  post_address_reply(const u8* frame, u32 frame_bytes);
  void  post_handshake_reply(const u8* frame, u32 frame_bytes);
  void  post_event(u32 event, u32 event_data0);
  void  advance_handshake_reply();

  u32   build_default_reply(const u8* request, u8* reply, u16 iocstatus);
  u32   execute_ioc_facts(const u8* request, u8* reply);
  u32   execute_port_facts(const u8* request, u8* reply);
  u32   execute_ioc_init(const u8* request, u8* reply);
  u32   execute_port_enable(const u8* request, u8* reply);
  u32   execute_event_notification(const u8* request, u8* reply);
  u32   execute_fw_download(const u8* request, u8* reply);
  u32   execute_fw_upload(const u8* request, u8* reply);

  int   collect_sgl(const u8* frame, u32 sge_offset,
    u8 chain_offset_dwords, SLSI_sge* out, u32 max_entries);
  void  init_scsi_io_reply(const u8* request, u8* reply);
  void  release_scsi_target(u8 target);
  bool  execute_scsi_command(u8 target, const u8* cdb, u8 cdb_len,
    const u8* data_out, u32 data_out_len, u8* data_in, u32 data_in_cap,
    SLSI_scsi_result& result);
  u32   request_sense(u8 target, u8* sense, u32 sense_cap);
  u32   execute_scsi_io(const u8* request, u8* reply);

  u32   build_config_page(u8 type, u8 number, u32 page_address,
    bool factory_default, u8* data);
  void  store_config_page(u8 type, u8 number, u32 page_address,
    const u8* data, u32 bytes);
  u32   execute_config(const u8* request, u8* reply);

  void  trace(const char* fmt, ...) const;
  void  trace_message(const char* via, const u8* request, const u8* reply,
    u32 reply_bytes) const;
  u32   execute(const u8* request, u8* reply);
  void  execute_handshake();
  void  execute_request(u32 mfa);

  void  eval_interrupts();

  CMutex* myRegLock;
  bool trace_enabled;
  u32 trace_last_his;
  u32 trace_last_doorbell;
  bool trace_his_valid;
  bool trace_doorbell_valid;

  std::vector<u8> flash;
  std::vector<u8> download_image;
  std::string flash_path;
  u32 option_rom_offset;
  u32 option_rom_size;
  u32 ioc_firmware_size;
  u16 firmware_product_id;
  u32 firmware_version;
  u8 download_image_type;
  bool download_active;
  bool flash_dirty;
  bool flash_views_dirty;
  std::time_t flash_last_dirty;

  /// The state structure contains all elements that need to be saved to the statefile.
  struct SLSI_state
  {
    u8  ioc_state;
    u16 fault_code;

    bool hs_receiving;
    u8   hs_expected_dwords;
    u8   hs_received_dwords;
    u32  hs_buffer[LSI_HANDSHAKE_MAX_DWORDS];

    bool hs_replying;
    u16  hs_reply_len_words;
    u16  hs_reply_pos;
    bool hs_word_consumed;
    bool hs_int_cleared;
    u16  hs_reply_words[LSI_REPLY_MAX_WORDS];

    bool doorbell_interrupt;
    u32  host_interrupt_mask;

    u8   write_sequence_pos;
    bool diag_write_enable;
    u8   host_diag_bits;
    u32  test_base;
    u32  diag_rw_addr;
    u8   shared_ram[LSI_SHARED_RAM_SIZE];
    u8   diag_buffer[LSI_DIAG_BUFFER_SIZE];
    u32  flash_csr;
    u8   flash_mode;

    u32 reply_post_fifo[LSI_REPLY_FIFO_DEPTH];
    u32 reply_post_head;
    u32 reply_post_count;
    u32 reply_free_fifo[LSI_REPLY_FIFO_DEPTH];
    u32 reply_free_head;
    u32 reply_free_count;

    u8   who_init;
    u16  reply_frame_size;
    u32  host_mfa_high_addr;
    u32  sense_buffer_high_addr;
    bool port_enabled;
    bool events_enabled;
    u32  event_msg_context;
    u32  event_context;
    bool bus_reset_event_pending;
    bool taskmgmt_mirror_pending;
    u8   taskmgmt_reply_frame[0x18];

    u32 scsi_port1_cfg[2];
    u32 scsi_dev1_params[LSI_MAX_TARGETS];
    u32 scsi_dev1_cfg[LSI_MAX_TARGETS];

    bool irq_asserted;
  } state;
};

#endif // !defined(INCLUDED_LSI53C1020_H_)
