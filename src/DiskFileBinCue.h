/* ES40 Emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 *
 * WWW    : https://github.com/ES40-Emu/es40
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
 *
 */

/**
 * \file
 * Contains definitions for BIN/CUE CD image support within CDiskFile.
 *
 * X-1.1        Initial implementation.
 *              Supports: single/multi-file BIN/CUE images,
 *              MODE1/2048, MODE1/2352, MODE2/2336, MODE2/2352, AUDIO tracks.
 *              Falls back gracefully to raw image on parse failure.
 *              Cross-platform: OpenVMS, Windows, Linux, macOS.
 **/

#if !defined(__DISKFILEBINCUE_H__)
#define __DISKFILEBINCUE_H__

/**
 * \brief Track modes parsed from a .cue sheet.
 *
 * These correspond directly to the mode strings found in .cue files.
 * sectorSize, dataOffset and dataSize are populated by parse_mode_string().
 *
 * Layout of a 2352-byte raw CD sector:
 *   [Sync: 12][Header: 4][Sub-header: 8][User data: 2048][EDC/ECC: 280]
 *
 * MODE1/2352:  dataOffset=16  (skip Sync+Header)
 * MODE2/2352:  dataOffset=24  (skip Sync+Header+Sub-header)
 * MODE2/2336:  dataOffset=8   (skip Sub-header only; no Sync/Header)
 * AUDIO:       dataOffset=0   (raw PCM, entire sector is payload)
 * MODE1/2048:  dataOffset=0   (cooked, no raw header present)
 **/
enum CueTrackMode
{
  TRACK_MODE_UNKNOWN   = 0, ///< Not yet determined / parse error
  TRACK_MODE_AUDIO,         ///< AUDIO  - 2352 bytes, no header
  TRACK_MODE1_2048,         ///< MODE1/2048 - 2048 bytes cooked
  TRACK_MODE1_2352,         ///< MODE1/2352 - 2352 bytes raw
  TRACK_MODE2_2336,         ///< MODE2/2336 - 2336 bytes raw
  TRACK_MODE2_2352          ///< MODE2/2352 - 2352 bytes raw
};

/**
 * \brief Describes a single track as parsed from a .cue sheet.
 *
 * Fixed-size char arrays are used throughout rather than std::string to
 * avoid any potential ABI issues on older OpenVMS toolchains and to keep
 * the structure trivially copyable / zero-initializable with memset.
 **/
#define BINCUE_MAX_PATH 512

struct CueTrack
{
  int          number;                    ///< Track number, 1-based
  CueTrackMode mode;                      ///< Parsed track mode
  int          sectorSize;                ///< Raw bytes per sector in .bin
  int          dataOffset;                ///< Header bytes to skip per sector
  int          dataSize;                  ///< Usable payload bytes per sector
  long         startLBA;                  ///< First LBA of this track
  long         endLBA;                    ///< First LBA of next track (exclusive)
  long         pregapLBA;                 ///< Pregap length in sectors (informational)
  char         filename[BINCUE_MAX_PATH]; ///< Resolved path to .bin file
  FILE*        fileHandle;                ///< Open read handle (nullptr if closed)
  off_t_large  fileOffset;                ///< Byte offset in .bin where track begins
};

#endif // !defined(__DISKFILEBINCUE_H__)
