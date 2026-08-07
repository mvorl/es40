/* ES40 emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 *
 * WWW    : http://sourceforge.net/projects/es40
 * E-mail : camiel@camicom.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might serve
 * the general public.
 */

/**
 * \file
 * Contains definitions to use a file as a disk image.
 *
 * $Id$
 *
 * X-1.8        Added BIN/CUE support via private extension methods.
 *              No changes to public interface or existing behaviour.
 *
 * X-1.7        Camiel Vanderhoeven                             09-JAN-2008
 *      Save disk state to state file.
 *
 * X-1.6        Camiel Vanderhoeven                             06-JAN-2008
 *      Support changing the block size (required for SCSI, ATAPI).
 *
 * X-1.5        Camiel Vanderhoeven                             04-JAN-2008
 *      64-bit file I/O.
 *
 * X-1.4        Camiel Vanderhoeven                             02-JAN-2008
 *      Comments.
 *
 * X-1.3        Camiel Vanderhoeven                             28-DEC-2007
 *      Keep the compiler happy.
 *
 * X-1.2        Camiel Vanderhoeven                             20-DEC-2007
 *      Close files and free memory when the emulator shuts down.
 *
 * X-1.1        Camiel Vanderhoeven                             12-DEC-2007
 *      Initial version in CVS.
 **/

#if !defined(__DISKFILE_H__)
#define __DISKFILE_H__

#include "Disk.h"
#include "DiskFileBinCue.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

enum class EDiskFileMediaAction
{
  ChangeImage,
  SetReadOnly
};

struct SDiskFileMediaAction
{
  EDiskFileMediaAction type;
  std::string path;
  bool read_only;
};

/**
 * Thread-safe mailbox shared with the UI.
 */
class CDiskFileMediaMailbox
{
public:
  CDiskFileMediaMailbox(const std::string& label, bool floppy,
                        bool initial_read_only);

  bool request_image_change(const char* path) noexcept;
  bool request_read_only_toggle() noexcept;
  bool take_pending_actions(std::deque<SDiskFileMediaAction>& actions) noexcept;
  void deactivate() noexcept;
  void reconcile_read_only(bool actual_value) noexcept;

  const std::string& label() const { return device_label; }

private:
  std::string device_label;
  bool floppy_device;
  std::atomic<bool> read_only;
  bool active;
  std::mutex mutex;
  std::deque<SDiskFileMediaAction> pending_actions;
};

/**
 * \brief Emulated disk that uses an image file.
 *
 * Supports raw/ISO images (existing behaviour, unchanged) and BIN/CUE
 * images (new). BIN/CUE support is activated automatically when the
 * configured filename ends in .cue (case-insensitive). If the .cue
 * cannot be parsed the code falls back to treating the file as a raw
 * image so that no existing configuration is broken.
 **/
class CDiskFile : public CDisk
{
public:
  CDiskFile(CConfigurator* cfg, CSystem* sys, CDiskController* c,
            int idebus, int idedev);
  virtual         ~CDiskFile(void);

  virtual bool    seek_byte(off_t_large byte);
  virtual size_t  read_bytes(void* dest, size_t bytes);
  virtual size_t  write_bytes(void* src, size_t bytes);
  virtual void    flush();

  bool            reload_file(const char* filename);
  bool            change_media(const char* filename);
  bool            set_read_only(bool read_only);
  virtual void    service_pending_media_actions() override;
  FILE*           get_handle() { return handle; }

  // ---------------------------------------------------------------
  // BIN/CUE query interface (safe to call even for ISO images;
  // returns sane defaults when bin/cue is not active).
  // ---------------------------------------------------------------
  bool            is_bincue_image()  const { return is_bincue; }
  int             get_track_count()  const { return track_count; }

  /**
   * \brief Return a pointer to track \a idx, or nullptr if out of range.
   *
   * Valid indices are 0 .. get_track_count()-1.
   * Returns nullptr when bin/cue is not active.
   **/
  const CueTrack* get_track(int idx) const
  {
    if (!is_bincue || !tracks || idx < 0 || idx >= track_count)
      return nullptr;
    return &tracks[idx];
  }

  /**
   * \brief Return the track that contains \a lba, or nullptr.
   **/
  const CueTrack* get_track_for_lba(long lba) const;

protected:
  FILE*       handle   = nullptr;
  std::string filename;

private:
  bool            floppy_device = false;
  std::shared_ptr<CDiskFileMediaMailbox> media_mailbox;

  // ------------------------------------------------------------------
  // BIN/CUE internal state
  // All members are initialised in the constructor and reset by
  // reset_bincue_state().  Nothing here is touched by the ISO/raw path.
  // ------------------------------------------------------------------
  bool            is_bincue;          ///< True while a .cue is loaded
  CueTrack*       tracks;             ///< Heap-allocated track array
  int             track_count;        ///< Elements in tracks[]
  long            current_lba;        ///< Logical LBA of current position
  off_t_large     logical_byte_pos;   ///< Logical byte position (LBA * 2048)

  // Internal helpers ------------------------------------------------
  bool            load_file_transactional(const char* filename,
                                           bool allow_autocreate,
                                           bool allow_cue_fallback);
  void            reset_bincue_state();
  bool            try_parse_cue(const char* cue_path);
  bool            open_bin_files();
  void            close_bin_handles();

  CueTrackMode    parse_mode_string(const char* mode_str,
                                    int& sector_size,
                                    int& data_offset,
                                    int& data_size);

  bool            lba_to_file_position(long lba,
                                       int& track_idx,
                                       off_t_large& file_offset) const;

                                       // MSF <-> LBA helpers (static: no instance state required)
                                       static long     msf_to_lba(int m, int s, int f);
                                       static void     lba_to_msf(long lba, int& m, int& s, int& f);
                                       static bool     has_cue_extension(const char* path);
                                       static char     path_separator();
};

#endif // !defined(__DISKFILE_H__)
