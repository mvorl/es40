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
 * Contains code to use a file as a disk image.
 *
 * $Id$
 *
 * X-1.23       Added BIN/CUE support.
 *              - Transparent: ISO/raw images and all existing OpenVMS disk
 *                images continue to work with zero code-path changes.
 *              - Activated automatically when filename ends in .cue
 *                (case-insensitive comparison, works on VMS/Win/Unix).
 *              - Supports single- and multi-file BIN/CUE layouts,
 *                MODE1/2048, MODE1/2352, MODE2/2336, MODE2/2352, AUDIO.
 *              - Falls back to raw-image handling if .cue parsing fails.
 *              - Multi-track TOC exposed via get_track() / get_track_for_lba()
 *                for use by the SCSI READ TOC handler in Disk.cpp.
 *
 * X-1.22       Camiel Vanderhoeven                             26-MAR-2008
 *      Support OpenVMS path names.
 *
 * X-1.21       Camiel Vanderhoeven                             14-MAR-2008
 *      Formatting.
 *
 * X-1.20       Camiel Vanderhoeven                             19-MAR-2008
 *      Use fopen_large to support files >2GB on linux.
 *
 * X-1.19       Camiel Vanderhoeven                             14-MAR-2008
 *   1. More meaningful exceptions replace throwing (int) 1.
 *   2. U64 macro replaces X64 macro.
 *
 * X-1.18       Camiel Vanderhoeven                             05-MAR-2008
 *      Multi-threading version.
 *
 * X-1.17       Camiel Vanderhoeven                             02-MAR-2008
 *      Natural way to specify large numeric values ("10M") in config file.
 *
 * X-1.16       David Leonard                                   20-FEB-2008
 *      Show disk creation progress.
 *
 * X-1.15       Camiel Vanderhoeven                             25-JAN-2008
 *      Create file if it doesn't exist and autocreate_size is specified.
 *
 * X-1.14       Camiel Vanderhoeven                             13-JAN-2008
 *      Use determine_layout instead of calc_cylinders.
 *
 * X-1.13       Brian Wheeler                                   09-JAN-2008
 *      Put filename in disk model number (without path).
 *
 * X-1.12       Camiel Vanderhoeven                             09-JAN-2008
 *      Save disk state to state file.
 *
 * X-1.11       Camiel Vanderhoeven                             06-JAN-2008
 *      Set default blocksize to 2048 for cd-rom devices.
 *
 * X-1.10       Camiel Vanderhoeven                             06-JAN-2008
 *      Support changing the block size (required for SCSI, ATAPI).
 *
 * X-1.9        Camiel Vanderhoeven                             04-JAN-2008
 *      64-bit file I/O.
 *
 * X-1.8        Camiel Vanderhoeven                             02-JAN-2008
 *      Cleanup.
 *
 * X-1.7        Camiel Vanderhoeven                             28-DEC-2007
 *      Throw exceptions rather than just exiting when errors occur.
 *
 * X-1.6        Camiel Vanderhoeven                             28-DEC-2007
 *      Keep the compiler happy.
 *
 * X-1.5        Camiel Vanderhoeven                             20-DEC-2007
 *      Close files and free memory when the emulator shuts down.
 *
 * X-1.4        Camiel Vanderhoeven                             18-DEC-2007
 *      Byte-sized transfers for SCSI controller.
 *
 * X-1.3        Brian Wheeler                                   17-DEC-2007
 *      Changed last cylinder number.
 *
 * X-1.2        Brian Wheeler                                   16-DEC-2007
 *      Fixed case of StdAfx.h.
 *
 * X-1.1        Camiel Vanderhoeven                             12-DEC-2007
 *      Initial version in CVS.
 **/

#include "StdAfx.h"
#include "DiskFile.h"

#if defined(HAVE_SDL)
#include "gui/sdl_media.h"
#endif

#include <ctype.h>
#include <exception>
#include <string>
#include <string.h>

CDiskFileMediaMailbox::CDiskFileMediaMailbox(const std::string& label,
    bool floppy, bool initial_read_only) :
    device_label(label), floppy_device(floppy),
    read_only(initial_read_only), guest_locked(false), active(true)
{
}

bool CDiskFileMediaMailbox::request_image_change(
    const char* path, bool force_locked) noexcept
{
    try
    {
        if (!path || !*path)
            return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (!active)
            return false;
        pending_actions.push_back(
            { EDiskFileMediaAction::ChangeImage, path, false, force_locked });
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool CDiskFileMediaMailbox::request_read_only_toggle() noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!active || !floppy_device)
            return false;

        // Queue absolute state rather than relative toggle. 
        // Repeated requests remain deterministic regardless of FDC state.
        const bool desired = !read_only.load(std::memory_order_relaxed);
        pending_actions.push_back(
            { EDiskFileMediaAction::SetReadOnly, std::string(), desired,
              false });
        read_only.store(desired, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool CDiskFileMediaMailbox::take_pending_actions(
    std::deque<SDiskFileMediaAction>& actions) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!active || pending_actions.empty())
            return false;
        actions.swap(pending_actions);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void CDiskFileMediaMailbox::reconcile_read_only(bool actual_value) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(mutex);
        bool projected_value = actual_value;
        for (std::deque<SDiskFileMediaAction>::const_iterator it =
                 pending_actions.begin(); it != pending_actions.end(); ++it)
            if (it->type == EDiskFileMediaAction::SetReadOnly)
                projected_value = it->read_only;
        read_only.store(projected_value, std::memory_order_release);
    }
    catch (...)
    {
    }
}

void CDiskFileMediaMailbox::deactivate() noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(mutex);
        active = false;
        pending_actions.clear();
    }
    catch (...)
    {
    }
}

// ===========================================================================
//  CDiskFile: constructor / destructor
// ===========================================================================

CDiskFile::CDiskFile(CConfigurator* cfg, CSystem* sys, CDiskController* c,
    int idebus, int idedev) : CDisk(cfg, sys, c, idebus, idedev)
{
    // Initialise bin/cue state before anything else so that the destructor
    // is always safe to call even if the constructor throws.
    is_bincue        = false;
    tracks           = nullptr;
    track_count      = 0;
    current_lba      = 0;
    logical_byte_pos = 0;
    floppy_device    = myCfg->get_myParent() != nullptr &&
                        myCfg->get_myParent()->get_class_id() == c_floppy;
    char* configured_filename = myCfg->get_text_value("file");
    const bool start_without_media = (floppy_device || cdrom()) &&
                                     (!configured_filename ||
                                      !*configured_filename);
    if (!configured_filename && !start_without_media)
    {
        FAILURE_1(Configuration, "%s: Disk has no file attached!\n", devid_string);
    }

    if (!start_without_media && !reload_file(configured_filename))
        FAILURE_1(Runtime, "%s: Could not mount configured disk image", devid_string);
    state.scsi.media_changed = 0;

    const char* default_model = floppy_device ?
        "3.5-inch 1.44 MB floppy" : "DEC RRD42 CD-ROM";
    model_number = myCfg->get_text_value("model_number",
        start_without_media ? default_model : configured_filename);

    // Advance model_number pointer past any directory component.
    char* p = model_number;
#if defined(_WIN32)
    char sep = '\\';
#elif defined(__VMS)
    char sep = ']';
#else
    char sep = '/';
#endif
    while (*p)
    {
        if (*p == sep)
            model_number = p + 1;
        p++;
    }

    if (cdrom() || floppy_device)
    {
        const char* kind = floppy_device ? "Floppy" : "CD-ROM";
        const char* device = devid_string ? devid_string : "unnamed device";
        media_mailbox = std::make_shared<CDiskFileMediaMailbox>(
            std::string(kind) + ": " + device, floppy_device, read_only);
#if defined(HAVE_SDL)
        sdl_register_removable_disk(media_mailbox);
#endif
    }

    if (start_without_media)
        printf("%s: %s drive has no media.\n", devid_string,
               floppy_device ? "3.5-inch 1.44 MB floppy" : "CD-ROM");
    else
        printf("%s: Mounted file %s, %" PRId64 " %zu-byte blocks, "
               "%" PRId64 "/%ld/%ld.\n",
               devid_string, filename.c_str(),
               byte_size / state.block_size, state.block_size,
               cylinders, heads, sectors);
}

CDiskFile::~CDiskFile(void)
{
    printf("%s: Closing file.\n", devid_string);

    if (media_mailbox)
    {
        media_mailbox->deactivate();
#if defined(HAVE_SDL)
        sdl_unregister_removable_disk(media_mailbox);
#endif
    }

    // Close any open bin/cue file handles and free the track array.
    if (is_bincue)
    {
        close_bin_handles();
        if (tracks)
        {
            free(tracks);
            tracks = nullptr;
        }
    }

    // Close the plain-file handle (nullptr when bin/cue is active).
    if (handle)
    {
        fclose(handle);
        handle = nullptr;
    }
}


// ===========================================================================
//  reload_file  –  entry point for both initial load and media change
// ===========================================================================

bool CDiskFile::reload_file(const char* _filename)
{
    return load_file_transactional(_filename, true, true);
}

bool CDiskFile::change_media(const char* _filename)
{
    return load_file_transactional(_filename, false, false);
}

void CDiskFile::media_lock_changed(bool locked)
{
    if (media_mailbox)
        media_mailbox->update_media_lock(locked);
}

bool CDiskFile::set_read_only(bool desired_read_only)
{
    if (desired_read_only == read_only)
        return true;

    // BIN/CUE media are read-only in this backend.
    if (is_bincue)
        return false;
    if (!handle || filename.empty())
        return false;

    if (desired_read_only && fflush(handle) != 0)
        return false;

    FILE* replacement = fopen_large(filename.c_str(),
                                   desired_read_only ? "rb" : "rb+");
    if (!replacement)
        return false;

    if (fseek_large(replacement, state.byte_pos, SEEK_SET) != 0)
    {
        fclose(replacement);
        return false;
    }

    FILE* old_handle = handle;
    handle = replacement;
    read_only = desired_read_only;
    fclose(old_handle);
    return true;
}

bool CDiskFile::load_file_transactional(const char* _filename,
                                        bool allow_autocreate,
                                        bool allow_cue_fallback)
{
    if (!_filename || !*_filename)
        return false;

    // Allocate the persistent path before touching the mounted image.
    // failure here is harmless.
    std::string new_filename(_filename);

    // Keep the mounted image mounted until we've opened the replacement
    // this way, a failed selection returns cleanly without changing anything
    struct MountedState
    {
        FILE* handle;
        bool is_bincue;
        CueTrack* tracks;
        int track_count;
        long current_lba;
        off_t_large logical_byte_pos;
        off_t_large byte_size;
        off_t_large cylinders;
        long heads;
        long sectors;
        off_t_large byte_pos;
        int media_changed;
    } old = {
        handle, is_bincue, tracks, track_count, current_lba,
        logical_byte_pos, byte_size, cylinders, heads, sectors,
        state.byte_pos, state.scsi.media_changed
    };

    if (old.handle && !read_only && fflush(old.handle) != 0)
    {
        printf("%s: Could not flush the current image before media change.\n",
               devid_string);
        return false;
    }

    handle           = nullptr;
    is_bincue        = false;
    tracks           = nullptr;
    track_count      = 0;
    current_lba      = 0;
    logical_byte_pos = 0;
    byte_size        = 0;
    state.byte_pos   = 0;

    auto discard_current = [this]()
    {
        reset_bincue_state();
        if (handle)
        {
            fclose(handle);
            handle = nullptr;
        }
    };

    auto restore_old = [this, &old, &discard_current]()
    {
        discard_current();
        handle                   = old.handle;
        is_bincue                = old.is_bincue;
        tracks                   = old.tracks;
        track_count              = old.track_count;
        current_lba              = old.current_lba;
        logical_byte_pos         = old.logical_byte_pos;
        byte_size                = old.byte_size;
        cylinders                = old.cylinders;
        heads                    = old.heads;
        sectors                  = old.sectors;
        state.byte_pos           = old.byte_pos;
        state.scsi.media_changed = old.media_changed;
    };

    auto discard_old = [&old]()
    {
        if (old.tracks)
        {
            for (int i = 0; i < old.track_count; i++)
            {
                if (old.tracks[i].fileHandle)
                    fclose(old.tracks[i].fileHandle);
            }
            free(old.tracks);
        }
        if (old.handle)
            fclose(old.handle);
    };

    try
    {
        bool loaded = false;

        // ---- detect .cue extension (case-insensitive, cross-platform) ----
        if (has_cue_extension(_filename))
        {
            printf("%s: Detected .cue extension, attempting BIN/CUE parse...\n",
                   devid_string);

            if (try_parse_cue(_filename))
            {
                // Expose total user-data bytes (LBAs * 2048) to SCSI.
                is_bincue = true;
                byte_size = 0;
                for (int i = 0; i < track_count; i++)
                {
                    long track_lbas = tracks[i].endLBA - tracks[i].startLBA;
                    if (track_lbas > 0)
                        byte_size += (off_t_large)track_lbas * 2048;
                }

                state.byte_pos   = 0;
                logical_byte_pos = 0;
                current_lba      = 0;
                sectors          = 32;
                heads            = 8;
                loaded = true;

                printf("%s: BIN/CUE loaded: %d track(s), %" PRId64
                       " logical bytes.\n",
                       devid_string, track_count, byte_size);
            }
            else
            {
                if (!allow_cue_fallback)
                {
                    restore_old();
                    return false;
                }

                // Preserve the fallback for raw whose names happen to end in .cue.
                printf("%s: BIN/CUE parse failed, falling back to raw image.\n",
                       devid_string);
            }
        }

        if (!loaded)
        {
            if (read_only)
                handle = fopen(_filename, "rb");
            else
                handle = fopen_large(_filename, "rb+");

            if (!handle)
            {
                printf("%s: Could not open file %s!\n", devid_string, _filename);

                int sz = allow_autocreate ?
                    myCfg->get_num_value("autocreate_size", false, 0) /
                    1024 / 1024 : 0;
                if (!sz)
                {
                    restore_old();
                    return false;
                }

                handle = fopen_large(_filename, "wb");
                if (!handle)
                {
                    restore_old();
                    return false;
                }

                void* crt_buf = calloc(1024, 1024);
                if (!crt_buf)
                {
                    restore_old();
                    return false;
                }

                printf("%s: writing %d 1kB blocks:   0%%\b\b\b\b",
                       devid_string, sz);

                int lastpc = 0;
                bool write_ok = true;
                for (int a = 0; a < sz; a++)
                {
                    if (fwrite(crt_buf, 1024, 1024, handle) != 1024)
                    {
                        write_ok = false;
                        break;
                    }

                    int pc = a * 100 / sz;
                    if (pc != lastpc)
                    {
                        printf("%3d\b\b\b", pc);
                        lastpc = pc;
                    }

                    fflush(stdout);
                }

                if (!write_ok || fflush(handle) != 0)
                {
                    free(crt_buf);
                    restore_old();
                    return false;
                }

                printf("100%%\n");
                int close_result = fclose(handle);
                handle = nullptr;
                free(crt_buf);
                if (close_result != 0)
                {
                    restore_old();
                    return false;
                }

                if (read_only)
                    handle = fopen_large(_filename, "rb");
                else
                    handle = fopen_large(_filename, "rb+");

                if (!handle)
                {
                    restore_old();
                    return false;
                }

                printf("%s: %d MB file %s created.\n",
                       devid_string, sz, _filename);
            }

            if (fseek_large(handle, 0, SEEK_END) != 0)
            {
                restore_old();
                return false;
            }
            byte_size = ftell_large(handle);
            if (byte_size < (off_t_large)state.block_size ||
                fseek_large(handle, 0, SEEK_SET) != 0)
            {
                restore_old();
                return false;
            }
            state.byte_pos = ftell_large(handle);
            if (state.byte_pos != 0)
            {
                restore_old();
                return false;
            }

            sectors = 32;
            heads   = 8;
        }

        if (byte_size < (off_t_large)state.block_size)
        {
            printf("%s: Image %s is empty, too small, or its size could not be read.\n",
                   devid_string, _filename);
            restore_old();
            return false;
        }
        determine_layout();
    }
    catch (...)
    {
        restore_old();
        throw;
    }

    filename.swap(new_filename);
    discard_old();
    state.scsi.media_changed = -1;
    return true;
}

void CDiskFile::check_state()
{
    if (!cdrom() || !media_mailbox)
        return;

    std::lock_guard<std::recursive_mutex> lock(media_action_mutex);
    if (scsi_get_phase(0) == SCSI_PHASE_FREE)
        service_pending_media_actions();
}

void CDiskFile::scsi_select_me(int bus)
{
    std::lock_guard<std::recursive_mutex> lock(media_action_mutex);
    CDisk::scsi_select_me(bus);
}

void CDiskFile::service_pending_media_actions()
{
    std::lock_guard<std::recursive_mutex> lock(media_action_mutex);

    if (!media_mailbox)
        return;

    std::deque<SDiskFileMediaAction> actions;
    if (!media_mailbox->take_pending_actions(actions))
        return;

    for (std::deque<SDiskFileMediaAction>::const_iterator it = actions.begin();
         it != actions.end(); ++it)
    {
        if (it->type == EDiskFileMediaAction::SetReadOnly)
        {
            const bool changed = set_read_only(it->read_only);
            media_mailbox->reconcile_read_only(read_only);
            printf("%s: Floppy is %s%s.\n", devid_string,
                read_only ? "read-only" : "writable",
                changed ? "" : " (unchanged: backing file could not be reopened)");
            continue;
        }

        if (cdrom() && state.scsi.locked && !it->force_locked)
        {
            printf("%s: Media change refused because the guest locked the CD-ROM.\n",
                devid_string);
            continue;
        }
        if (cdrom() && state.scsi.locked && it->force_locked)
            printf("%s: Forcing operator-approved change of guest-locked CD-ROM.\n",
                devid_string);

        bool changed = false;
        try
        {
            changed = change_media(it->path.c_str());
        }
        catch (const std::exception& error)
        {
            printf("%s: Media change failed: %s\n",
                devid_string, error.what());
        }
        catch (...)
        {
            printf("%s: Media change failed with an unknown error.\n",
                devid_string);
        }
        printf("%s: Media change %s: %s\n", devid_string,
            changed ? "complete" : "failed", it->path.c_str());
    }

    media_mailbox->reconcile_read_only(read_only);
}


// ===========================================================================
//  CDisk virtual interface overrides
// ===========================================================================

/**
 * \brief Seek to an absolute logical byte position.
 *
 * For BIN/CUE images the position is logical (LBA * 2048); the physical
 * file seek is deferred to read_bytes() so that we can translate across
 * track boundaries without seeking unnecessarily.
 *
 * For plain images the behaviour is identical to the original code.
 **/
bool CDiskFile::seek_byte(off_t_large byte)
{
    if (!media_present())
        return false;

    if (is_bincue)
    {
        if (byte >= byte_size)
        {
            FAILURE_1(InvalidArgument,
                      "%s: BIN/CUE seek beyond end of image!\n", devid_string);
        }

        logical_byte_pos = byte;
        current_lba      = (long)(byte / 2048);
        state.byte_pos   = byte;
        return true;
    }

    if (!handle)
        return false;

    // --- original plain-file path (unchanged) ---
    if (byte >= byte_size)
    {
        FAILURE_1(InvalidArgument,
                  "%s: Seek beyond end of file!\n", devid_string);
    }

    fseek_large(handle, byte, SEEK_SET);
    state.byte_pos = ftell_large(handle);
    return true;
}

/**
 * \brief Read \a bytes bytes from the current position into \a dest.
 *
 * For BIN/CUE images: translates logical byte/LBA positions to physical
 * file offsets, extracting only user-data bytes and skipping raw sector
 * headers. Correctly crosses track and file boundaries.
 *
 * For audio tracks the full raw sector (2352 bytes) is treated as payload
 * because there is no header to skip.
 *
 * For plain images the behaviour is identical to the original code.
 **/
size_t CDiskFile::read_bytes(void* dest, size_t bytes)
{
    if (!media_present())
        return 0;

    if (is_bincue)
    {
        size_t total_read = 0;
        u8*    out        = static_cast<u8*>(dest);

        while (bytes > 0)
        {
            int         track_idx = 0;
            off_t_large file_off  = 0;

            if (!lba_to_file_position(current_lba, track_idx, file_off))
            {
                // LBA is beyond all tracks: pad with zeros and stop.
                printf("%s: BIN/CUE read past end of image at LBA %ld\n",
                       devid_string, current_lba);
                break;
            }

            const CueTrack* trk = &tracks[track_idx];

            // Byte offset within the current logical 2048-byte sector.
            long   offset_in_sector = (long)(logical_byte_pos % 2048);

            // How many logical bytes remain in this sector.
            size_t can_read = (size_t)(2048 - offset_in_sector);
            if (can_read > bytes)
                can_read = bytes;

            // Physical position: start of raw sector + header + intra-sector offset.
            off_t_large phys = file_off
                             + (off_t_large)trk->dataOffset
                             + (off_t_large)offset_in_sector;

            if (fseek_large(trk->fileHandle, phys, SEEK_SET) != 0)
            {
                printf("%s: BIN/CUE fseek failed for track %d\n",
                       devid_string, trk->number);
                break;
            }

            size_t r = fread(out, 1, can_read, trk->fileHandle);
            if (r == 0)
            {
                printf("%s: BIN/CUE fread returned 0 for track %d\n",
                       devid_string, trk->number);
                break;
            }

            out              += r;
            total_read       += r;
            bytes            -= r;
            logical_byte_pos += (off_t_large)r;
            state.byte_pos    = logical_byte_pos;
            current_lba       = (long)(logical_byte_pos / 2048);
        }

        return total_read;
    }

    if (!handle)
        return 0;

    // --- original plain-file path (unchanged) ---
    size_t r = fread(dest, 1, bytes, handle);
    state.byte_pos = ftell_large(handle);
    return r;
}

/**
 * \brief Write \a bytes bytes from \a src at the current position.
 *
 * BIN/CUE images are always read-only (write returns 0).
 * Plain image behaviour is identical to the original code.
 **/
size_t CDiskFile::write_bytes(void* src, size_t bytes)
{
    if (!media_present())
        return 0;

    if (is_bincue)
    {
        // BIN/CUE images are treated as read-only media.
        return 0;
    }

    if (read_only)
        return 0;

    if (!handle)
        return 0;

    size_t r = fwrite(src, 1, bytes, handle);
    state.byte_pos = ftell_large(handle);
    return r;
}

/**
 * \brief Flush write buffers.
 *
 * BIN/CUE images are read-only so there is nothing to flush.
 * Plain image behaviour is identical to the original code.
 **/
void CDiskFile::flush()
{
    if (is_bincue)
        return;

    if (handle && !read_only)
        fflush(handle);
}


// ===========================================================================
//  BIN/CUE query helpers (public)
// ===========================================================================

/**
 * \brief Return the track that covers \a lba, or nullptr.
 **/
const CueTrack* CDiskFile::get_track_for_lba(long lba) const
{
    if (!is_bincue || !tracks)
        return nullptr;

    for (int i = 0; i < track_count; i++)
    {
        if (lba >= tracks[i].startLBA && lba < tracks[i].endLBA)
            return &tracks[i];
    }

    return nullptr;
}


// ===========================================================================
//  BIN/CUE private helpers
// ===========================================================================

/**
 * \brief Return the platform path separator character.
 *
 * Used when resolving .bin paths relative to the .cue file's directory.
 **/
char CDiskFile::path_separator()
{
#if defined(_WIN32)
    return '\\';
#elif defined(__VMS)
    return ']';
#else
    return '/';
#endif
}

/**
 * \brief Return true when \a path ends with ".cue" (case-insensitive).
 **/
bool CDiskFile::has_cue_extension(const char* path)
{
    if (!path) return false;

    size_t len = strlen(path);
    if (len < 4) return false;

    const char* ext = path + len - 4;
    return (tolower((unsigned char)ext[0]) == '.' &&
            tolower((unsigned char)ext[1]) == 'c' &&
            tolower((unsigned char)ext[2]) == 'u' &&
            tolower((unsigned char)ext[3]) == 'e');
}

/**
 * \brief Convert MSF address to LBA.
 **/
long CDiskFile::msf_to_lba(int m, int s, int f)
{
    return (long)(m * 60 + s) * 75 + f;
}

/**
 * \brief Convert LBA to MSF address.
 **/
void CDiskFile::lba_to_msf(long lba, int& m, int& s, int& f)
{
    f    = (int)(lba % 75); lba /= 75;
    s    = (int)(lba % 60); lba /= 60;
    m    = (int)lba;
}

/**
 * \brief Release all bin/cue state, leaving the object ready to load
 *        a fresh image (either bin/cue or plain).
 **/
void CDiskFile::reset_bincue_state()
{
    if (tracks)
    {
        close_bin_handles();
        free(tracks);
        tracks = nullptr;
    }

    is_bincue        = false;
    track_count      = 0;
    current_lba      = 0;
    logical_byte_pos = 0;
}

/**
 * \brief Parse a .cue sheet and populate the tracks[] array.
 *
 * Returns true on success (tracks[] is allocated and populated,
 * all .bin files are open).  Returns false on any error; on failure
 * the tracks[] array is freed and the object is left in a clean state.
 *
 * Design notes
 * ------------
 * - Two-pass approach: first pass counts TRACK keywords so we can
 *   allocate exactly the right amount of memory with a single calloc,
 *   avoiding any std::vector dependency.
 * - Fixed-size char arrays throughout: no heap allocations inside the
 *   track structures, no C++ exceptions.
 * - Robust against DOS/Unix/Mac line endings (\r\n, \n, \r).
 * - Path joining uses path_separator() for cross-platform correctness.
 * - OpenVMS note: VMS path syntax is  DEVICE:[DIR]FILE.EXT  The last
 *   ']' is the directory-close character used as path_separator() on VMS,
 *   which means get_cue_directory() correctly strips to the directory
 *   portion (DEVICE:[DIR]) that can be prefixed to relative .bin names.
 **/
bool CDiskFile::try_parse_cue(const char* cue_path)
{
    // ------------------------------------------------------------------
    // Open the .cue file
    // ------------------------------------------------------------------
    FILE* f = fopen(cue_path, "r");
    if (!f)
    {
        printf("%s: Cannot open .cue file: %s\n", devid_string, cue_path);
        return false;
    }

    // ------------------------------------------------------------------
    // Resolve the directory that contains the .cue file.
    // .bin filenames in the cue sheet are interpreted relative to this.
    // ------------------------------------------------------------------
    char cue_dir[BINCUE_MAX_PATH] = "";
    {
        const char  sep      = path_separator();
        const char* last_sep = nullptr;
        const char* p        = cue_path;
        while (*p)
        {
            if (*p == sep) last_sep = p;
            p++;
        }

        if (last_sep)
        {
            size_t dir_len = (size_t)(last_sep - cue_path);
            if (dir_len >= sizeof(cue_dir))
                dir_len = sizeof(cue_dir) - 1;
            memcpy(cue_dir, cue_path, dir_len);
            cue_dir[dir_len] = '\0';
        }
        // If last_sep is nullptr the .cue is in the CWD; cue_dir stays "".
    }

    // ------------------------------------------------------------------
    // First pass: count TRACK keywords to size the allocation.
    // ------------------------------------------------------------------
    char line[512];
    int  num_tracks = 0;

    while (fgets(line, (int)sizeof(line), f))
    {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "TRACK ", 6) == 0)
            num_tracks++;
    }

    if (num_tracks == 0)
    {
        printf("%s: .cue contains no TRACK entries.\n", devid_string);
        fclose(f);
        return false;
    }

    // ------------------------------------------------------------------
    // Allocate track array (zero-initialised).
    // ------------------------------------------------------------------
    tracks = static_cast<CueTrack*>(calloc((size_t)num_tracks, sizeof(CueTrack)));
    if (!tracks)
    {
        printf("%s: Out of memory allocating %d track descriptors.\n",
               devid_string, num_tracks);
        fclose(f);
        return false;
    }

    // ------------------------------------------------------------------
    // Second pass: populate track array.
    // ------------------------------------------------------------------
    rewind(f);

    int  current_idx  = -1;   // index into tracks[] for current TRACK block
    char current_bin[BINCUE_MAX_PATH] = ""; // most-recently-seen FILE path

    while (fgets(line, (int)sizeof(line), f))
    {
        // Strip leading whitespace.
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;

        // Strip trailing whitespace / line endings.
        char* end = p + strlen(p);
        while (end > p &&
               (*(end-1) == '\r' || *(end-1) == '\n' || *(end-1) == ' '))
            --end;
        *end = '\0';

        if (*p == '\0' || *p == ';' || *p == '#')
            continue; // blank or comment

        // ---- FILE "filename.bin" BINARY ---------------------------
        if (strncmp(p, "FILE ", 5) == 0)
        {
            char* q1 = strchr(p, '"');
            char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;

            if (!q1 || !q2)
            {
                // Some tools omit quotes; grab token after "FILE ".
                char raw[BINCUE_MAX_PATH] = "";
                sscanf(p + 5, "%511s", raw);
                q1 = raw - 1;   // reuse pointer trick won't work here
                // Simpler: just copy raw directly.
                strncpy(current_bin, raw, BINCUE_MAX_PATH - 1);
                current_bin[BINCUE_MAX_PATH - 1] = '\0';
            }
            else
            {
                size_t name_len = (size_t)(q2 - q1 - 1);
                if (name_len >= BINCUE_MAX_PATH)
                    name_len = BINCUE_MAX_PATH - 1;
                memcpy(current_bin, q1 + 1, name_len);
                current_bin[name_len] = '\0';
            }

            // Resolve to absolute path when cue_dir is known and the
            // filename is not already absolute.
            //
            // Absolute detection:
            //   Windows  – starts with '\' or "X:"
            //   VMS      – starts with a letter followed by ':' (device)
            //   Unix     – starts with '/'
            bool is_absolute = false;
#if defined(_WIN32)
            is_absolute = (current_bin[0] == '\\' ||
                           (current_bin[0] != '\0' && current_bin[1] == ':'));
#elif defined(__VMS)
            is_absolute = (current_bin[0] != '\0' && current_bin[1] == ':');
#else
            is_absolute = (current_bin[0] == '/');
#endif
            if (!is_absolute && cue_dir[0] != '\0')
            {
                char tmp[BINCUE_MAX_PATH];
                snprintf(tmp, sizeof(tmp), "%s%c%s",
                         cue_dir, path_separator(), current_bin);
                strncpy(current_bin, tmp, BINCUE_MAX_PATH - 1);
                current_bin[BINCUE_MAX_PATH - 1] = '\0';
            }
        }

        // ---- TRACK nn MODE ----------------------------------------
        else if (strncmp(p, "TRACK ", 6) == 0)
        {
            current_idx++;
            if (current_idx >= num_tracks)
            {
                // Shouldn't happen; guard against corrupted cue files.
                printf("%s: More TRACKs found than counted – stopping parse.\n",
                       devid_string);
                break;
            }

            CueTrack* trk = &tracks[current_idx];

            char mode_str[32] = "";
            int  track_num    = 0;
            sscanf(p + 6, "%d %31s", &track_num, mode_str);

            trk->number = track_num;
            trk->mode   = parse_mode_string(mode_str,
                                             trk->sectorSize,
                                             trk->dataOffset,
                                             trk->dataSize);

            strncpy(trk->filename, current_bin, BINCUE_MAX_PATH - 1);
            trk->filename[BINCUE_MAX_PATH - 1] = '\0';

            // startLBA / endLBA filled in by INDEX lines below.
            trk->startLBA   = 0;
            trk->endLBA     = 0;
            trk->pregapLBA  = 0;
            trk->fileOffset = 0;
            trk->fileHandle = nullptr;

            printf("%s:   Track %02d  %s  sectorSize=%d  dataOffset=%d\n",
                   devid_string, track_num, mode_str,
                   trk->sectorSize, trk->dataOffset);
        }

        // ---- INDEX nn mm:ss:ff ------------------------------------
        else if (strncmp(p, "INDEX ", 6) == 0)
        {
            if (current_idx < 0) continue;

            int index_num = 0, m = 0, s = 0, fr = 0;
            if (sscanf(p + 6, "%d %d:%d:%d", &index_num, &m, &s, &fr) == 4)
            {
                long lba = msf_to_lba(m, s, fr);
                if (index_num == 0)
                    tracks[current_idx].pregapLBA = lba;
                else if (index_num == 1)
                    tracks[current_idx].startLBA  = lba;
                // Higher index values (index points) are ignored.
            }
        }

        // ---- PREGAP mm:ss:ff --------------------------------------
        else if (strncmp(p, "PREGAP ", 7) == 0)
        {
            if (current_idx < 0) continue;

            int m = 0, s = 0, fr = 0;
            if (sscanf(p + 7, "%d:%d:%d", &m, &s, &fr) == 3)
                tracks[current_idx].pregapLBA = msf_to_lba(m, s, fr);
        }

        // ---- Other keywords (TITLE, PERFORMER, …) -----------------
        // Silently ignored; we only care about structural keywords.
    }

    fclose(f);

    track_count = current_idx + 1;

    if (track_count <= 0)
    {
        printf("%s: .cue parse produced no usable tracks.\n", devid_string);
        free(tracks);
        tracks = nullptr;
        track_count = 0;
        return false;
    }

    // ------------------------------------------------------------------
    // Compute endLBA for every track except the last.
    // The last track's endLBA is determined from the .bin file size after
    // we open the files below.
    // ------------------------------------------------------------------
    for (int i = 0; i < track_count - 1; i++)
        tracks[i].endLBA = tracks[i + 1].startLBA;

    // ------------------------------------------------------------------
    // Open all .bin files and finalise fileOffset / last endLBA.
    // ------------------------------------------------------------------
    if (!open_bin_files())
    {
        // open_bin_files() already printed an error and cleaned up.
        free(tracks);
        tracks      = nullptr;
        track_count = 0;
        return false;
    }

    return true;
}

/**
 * \brief Open the .bin file for every track and compute physical offsets.
 *
 * Returns false and closes any already-opened handles on failure.
 *
 * Note on multi-file CUEs: each track may reference a different .bin file.
 * In the common single-file case all tracks share the same path but we
 * still open one handle per track for simplicity; OS file caching means
 * this has no practical cost.
 *
 * fileOffset for each track is the byte offset within its .bin file at
 * which the track's raw sector data begins (startLBA * sectorSize for
 * single-bin images; 0 for per-track bin files).
 **/
bool CDiskFile::open_bin_files()
{
    // Detect whether this is a single-bin or multi-bin layout by checking
    // whether all tracks share the same filename.
    bool single_bin = true;
    for (int i = 1; i < track_count; i++)
    {
        if (strcmp(tracks[0].filename, tracks[i].filename) != 0)
        {
            single_bin = false;
            break;
        }
    }

    for (int i = 0; i < track_count; i++)
    {
        CueTrack* trk = &tracks[i];

        trk->fileHandle = fopen_large(trk->filename, "rb");
        if (!trk->fileHandle)
        {
            printf("%s: Cannot open BIN file: %s\n",
                   devid_string, trk->filename);
            close_bin_handles();
            return false;
        }

        if (single_bin)
        {
            // All tracks live in one .bin; each track starts at
            // startLBA * sectorSize bytes into the file.
            trk->fileOffset = (off_t_large)trk->startLBA * trk->sectorSize;
        }
        else
        {
            // Per-track .bin: track data starts at byte 0 of its file.
            trk->fileOffset = 0;
        }

        // For the last track, determine endLBA from the file size.
        if (i == track_count - 1 && trk->endLBA == 0)
        {
            fseek_large(trk->fileHandle, 0, SEEK_END);
            off_t_large bin_bytes = ftell_large(trk->fileHandle);

            long raw_sectors;
            if (single_bin)
            {
                // The file may contain data for all tracks; compute
                // how many sectors belong to the last track.
                off_t_large preceding_bytes =
                    (off_t_large)trk->startLBA * trk->sectorSize;
                off_t_large last_track_bytes = bin_bytes - preceding_bytes;
                raw_sectors = (long)(last_track_bytes / trk->sectorSize);
            }
            else
            {
                raw_sectors = (long)(bin_bytes / trk->sectorSize);
            }

            trk->endLBA = trk->startLBA + raw_sectors;
        }

        printf("%s:   Track %02d  LBA %ld..%ld  file=%s  offset=%" PRId64 "\n",
               devid_string, trk->number,
               trk->startLBA, trk->endLBA,
               trk->filename, trk->fileOffset);
    }

    return true;
}

/**
 * \brief Close all open .bin file handles without freeing the array.
 **/
void CDiskFile::close_bin_handles()
{
    if (!tracks) return;

    for (int i = 0; i < track_count; i++)
    {
        if (tracks[i].fileHandle)
        {
            fclose(tracks[i].fileHandle);
            tracks[i].fileHandle = nullptr;
        }
    }
}

/**
 * \brief Parse a cue track-mode string and populate sector geometry.
 *
 * Returns the matching enum value. Falls back to TRACK_MODE1_2352
 * for unrecognised strings and prints a warning.
 *
 * Sector geometry reference
 * -------------------------
 * AUDIO        : 2352 / offset 0  / data 2352  (raw PCM)
 * MODE1/2048   : 2048 / offset 0  / data 2048  (cooked)
 * MODE1/2352   : 2352 / offset 16 / data 2048
 * MODE2/2336   : 2336 / offset 8  / data 2048
 * MODE2/2352   : 2352 / offset 24 / data 2048
 **/
CueTrackMode CDiskFile::parse_mode_string(const char* s,
                                           int& sector_size,
                                           int& data_offset,
                                           int& data_size)
{
    if (strcmp(s, "AUDIO") == 0)
    {
        sector_size = 2352; data_offset = 0;  data_size = 2352;
        return TRACK_MODE_AUDIO;
    }
    if (strcmp(s, "MODE1/2048") == 0)
    {
        sector_size = 2048; data_offset = 0;  data_size = 2048;
        return TRACK_MODE1_2048;
    }
    if (strcmp(s, "MODE1/2352") == 0)
    {
        sector_size = 2352; data_offset = 16; data_size = 2048;
        return TRACK_MODE1_2352;
    }
    if (strcmp(s, "MODE2/2336") == 0)
    {
        sector_size = 2336; data_offset = 8;  data_size = 2048;
        return TRACK_MODE2_2336;
    }
    if (strcmp(s, "MODE2/2352") == 0)
    {
        sector_size = 2352; data_offset = 24; data_size = 2048;
        return TRACK_MODE2_2352;
    }

    // Unrecognised mode: warn and default to MODE1/2352.
    printf("CDiskFile: Unknown track mode '%s', defaulting to MODE1/2352\n", s);
    sector_size = 2352; data_offset = 16; data_size = 2048;
    return TRACK_MODE1_2352;
}

/**
 * \brief Translate a logical LBA into a physical track index and byte offset.
 *
 * \param lba         Logical block address to look up.
 * \param track_idx   Receives the index into tracks[].
 * \param file_offset Receives the byte offset in tracks[track_idx].fileHandle
 *                    at which the *raw* sector for \a lba begins (before the
 *                    dataOffset header skip).
 * \return true on success, false if lba is out of range.
 **/
bool CDiskFile::lba_to_file_position(long lba,
                                      int& track_idx,
                                      off_t_large& file_offset) const
{
    for (int i = 0; i < track_count; i++)
    {
        if (lba >= tracks[i].startLBA && lba < tracks[i].endLBA)
        {
            track_idx = i;
            long sector_in_track = lba - tracks[i].startLBA;
            file_offset = tracks[i].fileOffset
                        + (off_t_large)sector_in_track * tracks[i].sectorSize;
            return true;
        }
    }

    return false;
}
