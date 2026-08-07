/* ES40 emulator.
 * Copyright (C) 2026 by the ES40 Emulator Project
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

#include "../StdAfx.h"

#if defined(HAVE_SDL)

#if defined(min)
#undef min
#endif
#if defined(max)
#undef max
#endif

#include "sdl_media.h"
#include "../DiskFile.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

static const SDL_DialogFileFilter cdrom_filters[] = {
    { "CD-ROM images", "iso;cue" },
    { "All files",     "*"       }
};

static const SDL_DialogFileFilter floppy_filters[] = {
    { "Floppy images", "img;ima;vfd;dsk;flp;bin" },
    { "All files",     "*"                       }
};

struct SBlankFloppyFormat
{
    const char* label;
    size_t image_size;
};

static const SBlankFloppyFormat blank_floppy_formats[] = {
    { "360 KB (40 tracks, 9 sectors)",   360 * 1024 },
    { "720 KB (80 tracks, 9 sectors)",   720 * 1024 },
    { "1.2 MB (80 tracks, 15 sectors)", 1200 * 1024 },
    { "1.44 MB (80 tracks, 18 sectors)", 1440 * 1024 },
    { "2.88 MB (80 tracks, 36 sectors)", 2880 * 1024 }
};

static std::atomic<bool> file_dialog_open(false);
static std::mutex removable_disks_mutex;
static std::vector<std::shared_ptr<CDiskFileMediaMailbox> > removable_disks;

enum EMediaPopupMode
{
    MEDIA_POPUP_CLOSED,
    MEDIA_POPUP_DEVICES,
    MEDIA_POPUP_FLOPPY,
    MEDIA_POPUP_FLOPPY_SIZE,
    MEDIA_POPUP_CDROM,
    MEDIA_POPUP_CD_LOCKED,
    MEDIA_POPUP_MESSAGE
};

struct SMediaPopupState
{
    EMediaPopupMode mode = MEDIA_POPUP_CLOSED;
    SDL_Window* parent = nullptr;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_WindowID window_id = 0;
    std::vector<std::shared_ptr<CDiskFileMediaMailbox> > devices;
    std::shared_ptr<CDiskFileMediaMailbox> device;
    std::string pending_path;
    bool pending_eject = false;
    std::string message;
    std::vector<SDL_FRect> rows;
    int selected = 0;
    int device_selection = 0;
    int locked_return_selection = 0;
    int first_visible = 0;
    int width = 0;
    int height = 0;
};

static SMediaPopupState media_popup;
static SDL_WindowID retired_popup_id = 0;
static bool swallowed_key_releases[SDL_SCANCODE_COUNT] = {};
static bool swallow_parent_pointer_until_release = false;
static std::mutex pending_lock_confirmation_mutex;
static std::shared_ptr<CDiskFileMediaMailbox>
    pending_lock_confirmation_device;
static std::string pending_lock_confirmation_path;
static bool pending_lock_confirmation_eject = false;

void sdl_register_removable_disk(
    const std::shared_ptr<CDiskFileMediaMailbox>& mailbox) noexcept
{
    try
    {
        if (!mailbox)
            return;
        std::lock_guard<std::mutex> lock(removable_disks_mutex);
        removable_disks.push_back(mailbox);
    }
    catch (...)
    {
    }
}

void sdl_unregister_removable_disk(
    const std::shared_ptr<CDiskFileMediaMailbox>& mailbox) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(removable_disks_mutex);
        for (std::vector<std::shared_ptr<CDiskFileMediaMailbox> >::iterator it =
                 removable_disks.begin(); it != removable_disks.end(); ++it)
        {
            if (*it == mailbox)
            {
                removable_disks.erase(it);
                break;
            }
        }
    }
    catch (...)
    {
    }
}

static std::vector<std::shared_ptr<CDiskFileMediaMailbox> >
snapshot_removable_disks()
{
    std::lock_guard<std::mutex> lock(removable_disks_mutex);
    return removable_disks;
}

static bool popup_visible()
{
    return media_popup.mode != MEDIA_POPUP_CLOSED &&
           media_popup.window != nullptr;
}

static int item_count()
{
    if (media_popup.mode == MEDIA_POPUP_DEVICES)
        return (int)media_popup.devices.size() + 1;
    if (media_popup.mode == MEDIA_POPUP_FLOPPY)
        return 5;
    if (media_popup.mode == MEDIA_POPUP_FLOPPY_SIZE)
        return (int)SDL_arraysize(blank_floppy_formats) + 1;
    if (media_popup.mode == MEDIA_POPUP_CDROM)
        return 3;
    if (media_popup.mode == MEDIA_POPUP_CD_LOCKED)
        return 2;
    if (media_popup.mode == MEDIA_POPUP_MESSAGE)
        return 1;
    return 0;
}

static void destroy_popup_window()
{
    if (media_popup.renderer)
    {
        SDL_DestroyRenderer(media_popup.renderer);
        media_popup.renderer = nullptr;
    }
    if (media_popup.window)
    {
        retired_popup_id = media_popup.window_id;
        SDL_DestroyWindow(media_popup.window);
        media_popup.window = nullptr;
    }
    media_popup.window_id = 0;
    media_popup.rows.clear();
}

static void close_popup()
{
    destroy_popup_window();
    media_popup.mode = MEDIA_POPUP_CLOSED;
    media_popup.devices.clear();
    media_popup.device.reset();
    media_popup.pending_path.clear();
    media_popup.pending_eject = false;
    media_popup.message.clear();
    media_popup.selected = 0;
    media_popup.device_selection = 0;
    media_popup.locked_return_selection = 0;
    media_popup.first_visible = 0;
}

static std::string elide_text(const std::string& text, int max_characters)
{
    if (max_characters <= 0)
        return std::string();
    if ((int)text.size() <= max_characters)
        return text;
    if (max_characters <= 3)
        return text.substr(0, (size_t)max_characters);
    return text.substr(0, (size_t)max_characters - 3) + "...";
}

static void draw_text(float x, float y, const std::string& text,
    float factor, int max_characters, Uint8 red, Uint8 green, Uint8 blue)
{
    float old_scale_x = 1.0f;
    float old_scale_y = 1.0f;
    SDL_GetRenderScale(media_popup.renderer, &old_scale_x, &old_scale_y);
    SDL_SetRenderScale(media_popup.renderer,
                       old_scale_x * factor, old_scale_y * factor);
    SDL_SetRenderDrawColor(media_popup.renderer, red, green, blue, 255);
    const std::string displayed = elide_text(text, max_characters);
    SDL_RenderDebugText(media_popup.renderer,
                        x / factor, y / factor, displayed.c_str());
    SDL_SetRenderScale(media_popup.renderer, old_scale_x, old_scale_y);
}

static void render_popup()
{
    if (!popup_visible() || !media_popup.renderer)
        return;

    const float text_scale = media_popup.width >= 480 ? 1.5f : 1.0f;
    const float title_scale = media_popup.width >= 480 ? 1.75f : 1.25f;
    const float row_height = media_popup.width >= 480 ? 58.0f : 44.0f;
    const float header_height = media_popup.width >= 480 ? 72.0f : 56.0f;
    const float footer_height = media_popup.width >= 480 ? 38.0f : 28.0f;
    const int count = item_count();
    const int visible_count = std::max(1,
        (int)(((float)media_popup.height - header_height - footer_height) /
              row_height));

    if (media_popup.selected < media_popup.first_visible)
        media_popup.first_visible = media_popup.selected;
    if (media_popup.selected >= media_popup.first_visible + visible_count)
        media_popup.first_visible =
            media_popup.selected - visible_count + 1;
    media_popup.first_visible = std::max(0, std::min(
        media_popup.first_visible, std::max(0, count - visible_count)));

    SDL_SetRenderDrawColor(media_popup.renderer, 24, 29, 39, 255);
    SDL_RenderClear(media_popup.renderer);
    SDL_FRect top_accent = { 0.0f, 0.0f, (float)media_popup.width, 3.0f };
    SDL_SetRenderDrawColor(media_popup.renderer, 76, 139, 245, 255);
    SDL_RenderFillRect(media_popup.renderer, &top_accent);

    std::string title;
    std::string subtitle;
    if (media_popup.mode == MEDIA_POPUP_DEVICES)
    {
        title = "Change removable media";
        subtitle = "Choose a configured drive";
    }
    else if (media_popup.mode == MEDIA_POPUP_FLOPPY)
    {
        title = "Floppy media";
        subtitle = media_popup.device ? media_popup.device->label() :
            "Selected floppy drive";
    }
    else if (media_popup.mode == MEDIA_POPUP_FLOPPY_SIZE)
    {
        title = "Create blank floppy image";
        subtitle = "Choose an unformatted raw-image capacity";
    }
    else if (media_popup.mode == MEDIA_POPUP_CDROM)
    {
        title = "CD-ROM media";
        subtitle = media_popup.device ? media_popup.device->label() :
            "Selected CD-ROM drive";
    }
    else if (media_popup.mode == MEDIA_POPUP_CD_LOCKED)
    {
        title = "CD-ROM media locked";
        if (media_popup.pending_eject)
            subtitle = "The guest locked this CD-ROM; eject requires an override.";
        else if (!media_popup.pending_path.empty())
            subtitle = "The guest locked this CD-ROM while the file selector was open.";
        else
            subtitle = media_popup.device ?
                media_popup.device->label() + " is locked by the guest." :
                "The selected CD-ROM is locked by the guest.";
    }
    else
    {
        title = "Change removable media";
        subtitle = media_popup.message;
    }

    const int title_chars = std::max(1,
        (int)(((float)media_popup.width - 36.0f) / (8.0f * title_scale)));
    const int body_chars = std::max(1,
        (int)(((float)media_popup.width - 44.0f) / (8.0f * text_scale)));
    draw_text(20.0f, 16.0f, title, title_scale, title_chars,
              242, 246, 255);
    draw_text(20.0f, header_height - 24.0f, subtitle,
              text_scale, body_chars, 157, 169, 189);

    media_popup.rows.clear();
    const int last = std::min(count,
        media_popup.first_visible + visible_count);
    for (int index = media_popup.first_visible; index < last; index++)
    {
        const int visible = index - media_popup.first_visible;
        SDL_FRect row = {
            14.0f,
            header_height + row_height * (float)visible,
            (float)media_popup.width - 28.0f,
            row_height - 6.0f
        };
        media_popup.rows.push_back(row);

        if (index == media_popup.selected)
        {
            SDL_SetRenderDrawColor(media_popup.renderer, 51, 103, 190, 255);
            SDL_RenderFillRect(media_popup.renderer, &row);
            SDL_SetRenderDrawColor(media_popup.renderer, 107, 163, 255, 255);
            SDL_RenderRect(media_popup.renderer, &row);
        }
        else
        {
            SDL_SetRenderDrawColor(media_popup.renderer, 35, 42, 55, 255);
            SDL_RenderFillRect(media_popup.renderer, &row);
        }

        std::string label;
        std::string image_label;
        bool device_row = false;
        if (media_popup.mode == MEDIA_POPUP_DEVICES)
        {
            if (index < (int)media_popup.devices.size())
            {
                const std::shared_ptr<CDiskFileMediaMailbox>& mailbox =
                    media_popup.devices[(size_t)index];
                label = mailbox ? mailbox->label() :
                    "Unnamed removable drive";
                image_label = std::string("Image: ") +
                    (mailbox ? mailbox->mounted_image_name() : "<none>");
                device_row = true;
            }
            else
                label = "Cancel";
        }
        else if (media_popup.mode == MEDIA_POPUP_FLOPPY)
        {
            if (index == 0)
                label = "Change image...";
            else if (index == 1)
                label = "Create blank image...";
            else if (index == 2)
                label = "Eject";
            else if (index == 3)
                label = media_popup.device &&
                    media_popup.device->displayed_read_only() ?
                    "Make writable" : "Make read-only";
            else
                label = media_popup.devices.size() > 1 ? "Back" : "Cancel";
        }
        else if (media_popup.mode == MEDIA_POPUP_FLOPPY_SIZE)
        {
            if (index < (int)SDL_arraysize(blank_floppy_formats))
                label = blank_floppy_formats[index].label;
            else
                label = "Back";
        }
        else if (media_popup.mode == MEDIA_POPUP_CDROM)
        {
            if (index == 0)
                label = "Change image...";
            else if (index == 1)
                label = "Eject";
            else
                label = media_popup.devices.size() > 1 ? "Back" : "Cancel";
        }
        else if (media_popup.mode == MEDIA_POPUP_CD_LOCKED)
            label = index == 0 ?
                (media_popup.pending_eject ?
                    "Force eject anyway" : "Force change anyway...") :
                "Back";
        else
            label = "Close";

        if (device_row)
        {
            const float label_y = row.y +
                (media_popup.width >= 480 ? 7.0f : 4.0f);
            const float image_y = row.y +
                (media_popup.width >= 480 ? 32.0f : 22.0f);
            draw_text(row.x + 14.0f, label_y, label, text_scale, body_chars,
                      index == media_popup.selected ? 255 : 218,
                      index == media_popup.selected ? 255 : 225,
                      index == media_popup.selected ? 255 : 239);
            draw_text(row.x + 14.0f, image_y, image_label, 1.0f,
                      std::max(1, ((int)row.w - 28) / 8),
                      index == media_popup.selected ? 220 : 157,
                      index == media_popup.selected ? 232 : 169,
                      index == media_popup.selected ? 255 : 189);
        }
        else
        {
            draw_text(row.x + 14.0f,
                      row.y + (row.h - 8.0f * text_scale) * 0.5f,
                      label, text_scale, body_chars,
                      index == media_popup.selected ? 255 : 218,
                      index == media_popup.selected ? 255 : 225,
                      index == media_popup.selected ? 255 : 239);
        }
    }

    const bool back_available =
        ((media_popup.mode == MEDIA_POPUP_FLOPPY ||
          media_popup.mode == MEDIA_POPUP_CDROM) &&
         media_popup.devices.size() > 1) ||
        media_popup.mode == MEDIA_POPUP_FLOPPY_SIZE ||
        media_popup.mode == MEDIA_POPUP_CD_LOCKED;
    const char* footer = back_available ?
        "Arrows select   Enter open   Esc back" :
        "Arrows select   Enter open   Esc cancel";
    draw_text(20.0f, (float)media_popup.height - footer_height + 11.0f,
              footer, 1.0f,
              std::max(1, (media_popup.width - 40) / 8),
              133, 146, 166);
    SDL_RenderPresent(media_popup.renderer);
}

static bool size_popup_for_content()
{
    int parent_width = 640;
    int parent_height = 480;
    SDL_GetWindowSize(media_popup.parent, &parent_width, &parent_height);

    media_popup.width = std::max(260, std::min(560, parent_width - 24));
    const int row_height = media_popup.width >= 480 ? 58 : 44;
    const int header_height = media_popup.width >= 480 ? 72 : 56;
    const int footer_height = media_popup.width >= 480 ? 38 : 28;
    const int available_rows = std::max(1,
        (parent_height - 24 - header_height - footer_height) / row_height);
    const int visible_rows = std::max(1,
        std::min(item_count(), std::min(available_rows, 8)));
    media_popup.height = header_height + footer_height +
        row_height * visible_rows;

    return true;
}

static void sync_popup_size()
{
    if (!media_popup.window || !media_popup.renderer)
        return;
    int width = media_popup.width;
    int height = media_popup.height;
    if (SDL_GetWindowSize(media_popup.window, &width, &height) &&
        width > 0 && height > 0)
    {
        media_popup.width = width;
        media_popup.height = height;
    }
    SDL_SetRenderLogicalPresentation(media_popup.renderer,
        media_popup.width, media_popup.height,
        SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

static bool create_popup_window()
{
    if (!media_popup.parent)
        return false;

    size_popup_for_content();
    int parent_width = 640;
    int parent_height = 480;
    SDL_GetWindowSize(media_popup.parent, &parent_width, &parent_height);
    const int x = std::max(0, (parent_width - media_popup.width) / 2);
    const int y = std::max(0, (parent_height - media_popup.height) / 2);

    media_popup.window = SDL_CreatePopupWindow(media_popup.parent,
        x, y, media_popup.width, media_popup.height,
        SDL_WINDOW_POPUP_MENU | SDL_WINDOW_HIGH_PIXEL_DENSITY |
        SDL_WINDOW_HIDDEN);
    if (!media_popup.window)
    {
        SDL_Log("Could not create media selector popup: %s", SDL_GetError());
        return false;
    }

    media_popup.window_id = SDL_GetWindowID(media_popup.window);
    if (media_popup.window_id == retired_popup_id)
        retired_popup_id = 0;
    media_popup.renderer = SDL_CreateRenderer(media_popup.window, nullptr);
    if (!media_popup.renderer)
    {
        SDL_Log("Could not create media selector renderer: %s", SDL_GetError());
        destroy_popup_window();
        return false;
    }
    sync_popup_size();
    render_popup();
    if (!SDL_ShowWindow(media_popup.window))
    {
        SDL_Log("Could not show media selector popup: %s", SDL_GetError());
        destroy_popup_window();
        return false;
    }
    // Some backends discard presents made while a window is hidden.
    render_popup();
    return true;
}

static void update_popup_content()
{
    if (!popup_visible())
        return;
    destroy_popup_window();
    if (!create_popup_window())
        close_popup();
}

static void show_message(const char* message)
{
    media_popup.mode = MEDIA_POPUP_MESSAGE;
    media_popup.message = message ? message : "Unknown media-selector error.";
    media_popup.selected = 0;
    media_popup.first_visible = 0;
    if (!media_popup.window)
        create_popup_window();
    else
        update_popup_content();
}

static bool defer_lock_confirmation(
    const std::shared_ptr<CDiskFileMediaMailbox>& mailbox,
    const char* path, bool eject = false) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(pending_lock_confirmation_mutex);
        pending_lock_confirmation_path = path ? path : "";
        pending_lock_confirmation_device = mailbox;
        pending_lock_confirmation_eject = eject;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void sdl_media_pump() noexcept
{
    try
    {
        std::shared_ptr<CDiskFileMediaMailbox> mailbox;
        std::string path;
        bool eject = false;
        {
            std::lock_guard<std::mutex> lock(
                pending_lock_confirmation_mutex);
            if (!pending_lock_confirmation_device)
                return;
            mailbox.swap(pending_lock_confirmation_device);
            path.swap(pending_lock_confirmation_path);
            eject = pending_lock_confirmation_eject;
            pending_lock_confirmation_eject = false;
        }

        if (!media_popup.parent || !mailbox)
            return;

        media_popup.devices = snapshot_removable_disks();
        media_popup.device_selection = 0;
        for (size_t index = 0; index < media_popup.devices.size(); index++)
            if (media_popup.devices[index] == mailbox)
            {
                media_popup.device_selection = (int)index;
                break;
            }
        media_popup.device = mailbox;
        media_popup.pending_path.swap(path);
        media_popup.pending_eject = eject;
        media_popup.locked_return_selection = eject ? 1 : 0;
        media_popup.mode = MEDIA_POPUP_CD_LOCKED;
        media_popup.selected = 1;
        media_popup.first_visible = 0;
        if (!media_popup.window)
            create_popup_window();
        else
            update_popup_content();
    }
    catch (...)
    {
        close_popup();
    }
}

struct SMediaFileDialogContext
{
    std::shared_ptr<CDiskFileMediaMailbox> mailbox;
    bool force_locked = false;
    size_t blank_floppy_size = 0;
};

static void SDLCALL media_file_callback(void* userdata,
                                         const char* const* filelist,
                                         int filter)
{
    (void)filter;
    std::unique_ptr<SMediaFileDialogContext> context(
        static_cast<SMediaFileDialogContext*>(userdata));
    file_dialog_open.store(false, std::memory_order_release);

    if (!filelist)
    {
        SDL_Log("Could not show media file selector: %s", SDL_GetError());
        return;
    }
    if (!*filelist)
        return;
    if (!**filelist)
    {
        SDL_Log("The user selected an empty file.");
        return;
    }
    if (!context || !context->mailbox)
        return;

    if (context->blank_floppy_size != 0)
    {
        if (!context->mailbox->request_blank_floppy(
                filelist[0], context->blank_floppy_size))
            SDL_Log("The selected floppy drive is no longer available.");
        return;
    }

    if (!context->mailbox->is_floppy() &&
        context->mailbox->media_locked() && !context->force_locked)
    {
        if (!defer_lock_confirmation(context->mailbox, filelist[0]))
            SDL_Log("Could not prepare the locked-media confirmation.");
        return;
    }

    if (!context->mailbox->request_image_change(
            filelist[0], context->force_locked))
        SDL_Log("The selected removable-media device is no longer available.");
}

static void show_file_dialog(
    const std::shared_ptr<CDiskFileMediaMailbox>& mailbox,
    bool force_locked = false)
{
    if (!mailbox || !media_popup.parent ||
        file_dialog_open.exchange(true, std::memory_order_acq_rel))
        return;

    std::unique_ptr<SMediaFileDialogContext> context(
        new (std::nothrow) SMediaFileDialogContext);
    if (!context)
    {
        file_dialog_open.store(false, std::memory_order_release);
        show_message("Could not allocate the file-dialog request.");
        return;
    }
    context->mailbox = mailbox;
    context->force_locked = force_locked;

    const SDL_DialogFileFilter* filters = mailbox->is_floppy() ?
        floppy_filters : cdrom_filters;
    const int filter_count = mailbox->is_floppy() ?
        (int)SDL_arraysize(floppy_filters) :
        (int)SDL_arraysize(cdrom_filters);
    SDL_ShowOpenFileDialog(media_file_callback, context.release(),
                           media_popup.parent, filters, filter_count,
                           nullptr, false);
}

static void show_blank_floppy_dialog(
    const std::shared_ptr<CDiskFileMediaMailbox>& mailbox,
    size_t image_size)
{
    if (!mailbox || !mailbox->is_floppy() || image_size == 0 ||
        !media_popup.parent ||
        file_dialog_open.exchange(true, std::memory_order_acq_rel))
        return;

    std::unique_ptr<SMediaFileDialogContext> context(
        new (std::nothrow) SMediaFileDialogContext);
    if (!context)
    {
        file_dialog_open.store(false, std::memory_order_release);
        show_message("Could not allocate the blank-image request.");
        return;
    }
    context->mailbox = mailbox;
    context->blank_floppy_size = image_size;

    SDL_ShowSaveFileDialog(media_file_callback, context.release(),
                           media_popup.parent, floppy_filters,
                           (int)SDL_arraysize(floppy_filters),
                           "blank.img");
}

static void open_device(
    const std::shared_ptr<CDiskFileMediaMailbox>& mailbox)
{
    if (!mailbox)
        return;
    if (media_popup.mode == MEDIA_POPUP_DEVICES)
        media_popup.device_selection = media_popup.selected;
    media_popup.device = mailbox;
    media_popup.pending_path.clear();
    media_popup.pending_eject = false;
    media_popup.mode = mailbox->is_floppy() ?
        MEDIA_POPUP_FLOPPY : MEDIA_POPUP_CDROM;
    media_popup.selected = 0;
    media_popup.first_visible = 0;
    if (!media_popup.window)
        create_popup_window();
    else
        update_popup_content();
}

void sdl_select_media(SDL_Window* window) noexcept
{
    try
    {
        if (file_dialog_open.load(std::memory_order_acquire))
            return;
        if (popup_visible())
        {
            close_popup();
            return;
        }

        swallow_parent_pointer_until_release = false;
        media_popup.parent = window;
        media_popup.devices = snapshot_removable_disks();
        if (media_popup.devices.empty())
        {
            show_message("No removable media devices are configured.");
            return;
        }
        media_popup.mode = MEDIA_POPUP_DEVICES;
        media_popup.selected = 0;
        media_popup.device_selection = 0;
        media_popup.first_visible = 0;
        create_popup_window();
    }
    catch (...)
    {
        close_popup();
        SDL_Log("Could not open the removable-media selector.");
    }
}

static void move_selection(int delta)
{
    const int count = item_count();
    if (count <= 0)
        return;
    media_popup.selected = (media_popup.selected + delta + count) % count;
    render_popup();
}

static void cancel_or_go_back();

static void activate_selection()
{
    if (media_popup.mode == MEDIA_POPUP_DEVICES)
    {
        if (media_popup.selected >= 0 &&
            media_popup.selected < (int)media_popup.devices.size())
            open_device(media_popup.devices[(size_t)media_popup.selected]);
        else
            close_popup();
        return;
    }

    if (media_popup.mode == MEDIA_POPUP_FLOPPY)
    {
        std::shared_ptr<CDiskFileMediaMailbox> mailbox = media_popup.device;
        if (!mailbox)
        {
            close_popup();
            return;
        }
        if (media_popup.selected == 0)
        {
            close_popup();
            show_file_dialog(mailbox);
        }
        else if (media_popup.selected == 1)
        {
            media_popup.mode = MEDIA_POPUP_FLOPPY_SIZE;
            media_popup.selected = 3;
            media_popup.first_visible = 0;
            update_popup_content();
        }
        else if (media_popup.selected == 2)
        {
            if (!mailbox->request_eject())
                SDL_Log("The selected floppy drive is no longer available.");
            close_popup();
        }
        else if (media_popup.selected == 3)
        {
            mailbox->request_read_only_toggle();
            close_popup();
        }
        else
            cancel_or_go_back();
        return;
    }
    if (media_popup.mode == MEDIA_POPUP_FLOPPY_SIZE)
    {
        std::shared_ptr<CDiskFileMediaMailbox> mailbox = media_popup.device;
        if (mailbox && media_popup.selected >= 0 &&
            media_popup.selected < (int)SDL_arraysize(blank_floppy_formats))
        {
            const size_t image_size =
                blank_floppy_formats[media_popup.selected].image_size;
            close_popup();
            show_blank_floppy_dialog(mailbox, image_size);
        }
        else
            cancel_or_go_back();
        return;
    }
    if (media_popup.mode == MEDIA_POPUP_CDROM)
    {
        std::shared_ptr<CDiskFileMediaMailbox> mailbox = media_popup.device;
        if (!mailbox)
        {
            close_popup();
            return;
        }

        if (media_popup.selected == 0)
        {
            if (mailbox->media_locked())
            {
                media_popup.pending_path.clear();
                media_popup.pending_eject = false;
                media_popup.locked_return_selection = 0;
                media_popup.mode = MEDIA_POPUP_CD_LOCKED;
                media_popup.selected = 1;
                media_popup.first_visible = 0;
                update_popup_content();
            }
            else
            {
                close_popup();
                show_file_dialog(mailbox);
            }
        }
        else if (media_popup.selected == 1)
        {
            if (mailbox->media_locked())
            {
                media_popup.pending_path.clear();
                media_popup.pending_eject = true;
                media_popup.locked_return_selection = 1;
                media_popup.mode = MEDIA_POPUP_CD_LOCKED;
                media_popup.selected = 1;
                media_popup.first_visible = 0;
                update_popup_content();
            }
            else
            {
                if (!mailbox->request_eject())
                    SDL_Log("The selected CD-ROM device is no longer available.");
                close_popup();
            }
        }
        else
            cancel_or_go_back();
        return;
    }
    if (media_popup.mode == MEDIA_POPUP_CD_LOCKED)
    {
        std::shared_ptr<CDiskFileMediaMailbox> mailbox = media_popup.device;
        if (media_popup.selected == 0 && mailbox)
        {
            std::string path;
            path.swap(media_popup.pending_path);
            const bool eject = media_popup.pending_eject;
            close_popup();
            if (eject)
            {
                if (!mailbox->request_eject(true))
                    SDL_Log("The selected CD-ROM device is no longer available.");
            }
            else if (path.empty())
                show_file_dialog(mailbox, true);
            else if (!mailbox->request_image_change(path.c_str(), true))
                SDL_Log("The selected CD-ROM device is no longer available.");
        }
        else
            cancel_or_go_back();
        return;
    }
    if (media_popup.mode == MEDIA_POPUP_MESSAGE)
        close_popup();
}

static void cancel_or_go_back()
{
    if (media_popup.mode == MEDIA_POPUP_FLOPPY_SIZE)
    {
        media_popup.mode = MEDIA_POPUP_FLOPPY;
        media_popup.selected = 1;
        media_popup.first_visible = 0;
        update_popup_content();
        return;
    }
    if (media_popup.mode == MEDIA_POPUP_CD_LOCKED)
    {
        media_popup.mode = MEDIA_POPUP_CDROM;
        media_popup.pending_path.clear();
        media_popup.pending_eject = false;
        media_popup.selected = std::max(0, std::min(
            media_popup.locked_return_selection, item_count() - 1));
        media_popup.first_visible = 0;
        update_popup_content();
        return;
    }
    if ((media_popup.mode == MEDIA_POPUP_FLOPPY ||
         media_popup.mode == MEDIA_POPUP_CDROM) &&
        media_popup.devices.size() > 1)
    {
        media_popup.mode = MEDIA_POPUP_DEVICES;
        media_popup.device.reset();
        media_popup.pending_path.clear();
        media_popup.selected = std::max(0, std::min(
            media_popup.device_selection, item_count() - 1));
        media_popup.first_visible = 0;
        update_popup_content();
        return;
    }
    close_popup();
}

static bool point_in_rect(float x, float y, const SDL_FRect& rect)
{
    return x >= rect.x && x < rect.x + rect.w &&
           y >= rect.y && y < rect.y + rect.h;
}

static SDL_WindowID event_window_id(const SDL_Event& event)
{
    if (event.type >= SDL_EVENT_WINDOW_FIRST &&
        event.type <= SDL_EVENT_WINDOW_LAST)
        return event.window.windowID;
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return event.key.windowID;
    case SDL_EVENT_MOUSE_MOTION:
        return event.motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.windowID;
    case SDL_EVENT_MOUSE_WHEEL:
        return event.wheel.windowID;
    default:
        return 0;
    }
}

bool sdl_media_handle_event(const SDL_Event* source_event) noexcept
{
    try
    {
        if (!source_event)
            return false;

        if (swallow_parent_pointer_until_release &&
            (source_event->type == SDL_EVENT_MOUSE_MOTION ||
             source_event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             source_event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
             source_event->type == SDL_EVENT_MOUSE_WHEEL))
        {
            if (source_event->type == SDL_EVENT_MOUSE_BUTTON_UP)
                swallow_parent_pointer_until_release = false;
            return true;
        }

        if (source_event->type == SDL_EVENT_KEY_DOWN ||
            source_event->type == SDL_EVENT_KEY_UP)
        {
            const SDL_Scancode scancode = source_event->key.scancode;
            if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT &&
                swallowed_key_releases[scancode])
            {
                if (source_event->type == SDL_EVENT_KEY_UP)
                {
                    swallowed_key_releases[scancode] = false;
                    return true;
                }
                const bool navigation_repeat = popup_visible() &&
                    source_event->key.repeat &&
                    (source_event->key.key == SDLK_UP ||
                     source_event->key.key == SDLK_DOWN);
                if (!navigation_repeat)
                    return true;
            }
        }

        const SDL_WindowID event_id = event_window_id(*source_event);
        if (event_id != 0 && event_id == retired_popup_id)
            return true;
        if (!popup_visible())
            return false;

        if (event_id != media_popup.window_id)
        {
            const bool keyboard_event =
                source_event->type == SDL_EVENT_KEY_DOWN ||
                source_event->type == SDL_EVENT_KEY_UP;
            const bool pointer_event =
                source_event->type == SDL_EVENT_MOUSE_MOTION ||
                source_event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                source_event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
                source_event->type == SDL_EVENT_MOUSE_WHEEL;
            if (source_event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            {
                swallow_parent_pointer_until_release = true;
                close_popup();
            }
            if (pointer_event)
                return true;
            if (!keyboard_event)
                return false;
        }

        if (source_event->type >= SDL_EVENT_WINDOW_FIRST &&
            source_event->type <= SDL_EVENT_WINDOW_LAST)
        {
            switch (source_event->type)
            {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            case SDL_EVENT_WINDOW_HIDDEN:
                if (source_event->type == SDL_EVENT_WINDOW_FOCUS_LOST)
                    swallow_parent_pointer_until_release = true;
                close_popup();
                break;
            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_SHOWN:
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                sync_popup_size();
                render_popup();
                break;
            default:
                break;
            }
            return true;
        }

        if (source_event->type == SDL_EVENT_KEY_DOWN)
        {
            const SDL_Scancode scancode = source_event->key.scancode;
            if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT)
                swallowed_key_releases[scancode] = true;
            switch (source_event->key.key)
            {
            case SDLK_UP:       move_selection(-1); break;
            case SDLK_DOWN:     move_selection(1); break;
            case SDLK_TAB:
                move_selection((source_event->key.mod & SDL_KMOD_SHIFT) ?
                    -1 : 1);
                break;
            case SDLK_HOME:     media_popup.selected = 0; render_popup(); break;
            case SDLK_END:      media_popup.selected = item_count() - 1; render_popup(); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:    activate_selection(); break;
            case SDLK_ESCAPE:   cancel_or_go_back(); break;
            case SDLK_F11:
                if (source_event->key.mod & SDL_KMOD_CTRL)
                    close_popup();
                break;
            default: break;
            }
            return true;
        }
        if (source_event->type == SDL_EVENT_KEY_UP)
            return true;

        if (source_event->type == SDL_EVENT_MOUSE_WHEEL)
        {
            float delta = source_event->wheel.y;
            if (source_event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                delta = -delta;
            if (delta > 0.0f)
                move_selection(-1);
            else if (delta < 0.0f)
                move_selection(1);
            return true;
        }

        if (source_event->type == SDL_EVENT_MOUSE_MOTION ||
            source_event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            source_event->type == SDL_EVENT_MOUSE_BUTTON_UP)
        {
            SDL_Event event = *source_event;
            SDL_ConvertEventToRenderCoordinates(media_popup.renderer, &event);
            const float x = source_event->type == SDL_EVENT_MOUSE_MOTION ?
                event.motion.x : event.button.x;
            const float y = source_event->type == SDL_EVENT_MOUSE_MOTION ?
                event.motion.y : event.button.y;
            int hovered = -1;
            for (size_t i = 0; i < media_popup.rows.size(); i++)
                if (point_in_rect(x, y, media_popup.rows[i]))
                {
                    hovered = media_popup.first_visible + (int)i;
                    break;
                }

            if (hovered >= 0 && hovered < item_count() &&
                hovered != media_popup.selected)
            {
                media_popup.selected = hovered;
                render_popup();
            }
            if (source_event->type == SDL_EVENT_MOUSE_BUTTON_UP &&
                event.button.button == SDL_BUTTON_LEFT && hovered >= 0)
                activate_selection();
            return true;
        }

        return true;
    }
    catch (...)
    {
        close_popup();
        return true;
    }
}

void sdl_media_shutdown() noexcept
{
    close_popup();
    try
    {
        std::lock_guard<std::mutex> lock(pending_lock_confirmation_mutex);
        pending_lock_confirmation_device.reset();
        pending_lock_confirmation_path.clear();
        pending_lock_confirmation_eject = false;
    }
    catch (...)
    {
    }
    media_popup.parent = nullptr;
    retired_popup_id = 0;
    swallow_parent_pointer_until_release = false;
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++)
        swallowed_key_releases[i] = false;
}

#endif // defined(HAVE_SDL)
