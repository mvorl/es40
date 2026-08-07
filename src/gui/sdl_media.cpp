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
#include <string>
#include <vector>

extern std::vector<CDiskFile*> cd_diskfiles;

static const SDL_DialogFileFilter cdrom_filters[] = {
    { "CD-ROM images", "iso;cue" },
    { "All files",     "*"       }
};

static std::atomic<bool> file_dialog_open(false);

enum EMediaPopupMode
{
    MEDIA_POPUP_CLOSED,
    MEDIA_POPUP_DEVICES,
    MEDIA_POPUP_MESSAGE
};

struct SMediaPopupState
{
    EMediaPopupMode mode = MEDIA_POPUP_CLOSED;
    SDL_Window* parent = nullptr;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_WindowID window_id = 0;
    std::vector<CDiskFile*> devices;
    std::string message;
    std::vector<SDL_FRect> rows;
    int selected = 0;
    int first_visible = 0;
    int width = 0;
    int height = 0;
};

static SMediaPopupState media_popup;
static SDL_WindowID retired_popup_id = 0;
static bool swallowed_key_releases[SDL_SCANCODE_COUNT] = {};
static bool swallow_parent_pointer_until_release = false;

static bool popup_visible()
{
    return media_popup.mode != MEDIA_POPUP_CLOSED &&
           media_popup.window != nullptr;
}

static int item_count()
{
    if (media_popup.mode == MEDIA_POPUP_DEVICES)
        return (int)media_popup.devices.size() + 1;
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
    media_popup.message.clear();
    media_popup.selected = 0;
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
    const float row_height = media_popup.width >= 480 ? 42.0f : 32.0f;
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
        subtitle = "Choose a configured CD-ROM drive";
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
        if (media_popup.mode == MEDIA_POPUP_DEVICES)
        {
            if (index < (int)media_popup.devices.size())
            {
                CDiskFile* disk = media_popup.devices[(size_t)index];
                label = disk && disk->devid_string ? disk->devid_string :
                    "Unnamed CD-ROM drive";
            }
            else
                label = "Cancel";
        }
        else
            label = "Close";

        draw_text(row.x + 14.0f,
                  row.y + (row.h - 8.0f * text_scale) * 0.5f,
                  label, text_scale, body_chars,
                  index == media_popup.selected ? 255 : 218,
                  index == media_popup.selected ? 255 : 225,
                  index == media_popup.selected ? 255 : 239);
    }

    const char* footer = "Arrows select   Enter open   Esc cancel";
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
    const int row_height = media_popup.width >= 480 ? 42 : 32;
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

static void SDLCALL media_file_callback(void* userdata,
                                         const char* const* filelist,
                                         int filter)
{
    (void)filter;
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
    CDiskFile* disk = static_cast<CDiskFile*>(userdata);
    if (!disk)
        return;

    char* filename = SDL_strdup(filelist[0]);
    if (!filename)
    {
        SDL_Log("Could not allocate the selected CD-ROM image path.");
        return;
    }
    SDL_Log("Change CD-ROM image file to %s", filename);
    disk->reload_file(filename);
    SDL_free(filename);
}

static void show_file_dialog(CDiskFile* disk)
{
    if (!disk || !media_popup.parent ||
        file_dialog_open.exchange(true, std::memory_order_acq_rel))
        return;

    SDL_ShowOpenFileDialog(media_file_callback, disk, media_popup.parent,
                           cdrom_filters, SDL_arraysize(cdrom_filters),
                           nullptr, false);
}

static void open_device(CDiskFile* disk)
{
    if (!disk)
        return;
    close_popup();
    show_file_dialog(disk);
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
        media_popup.devices = cd_diskfiles;
        if (media_popup.devices.empty())
        {
            show_message("No file-backed CD-ROM drives are configured.");
            return;
        }
        media_popup.mode = MEDIA_POPUP_DEVICES;
        media_popup.selected = 0;
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

    if (media_popup.mode == MEDIA_POPUP_MESSAGE)
        close_popup();
}

static void cancel_or_go_back()
{
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
            if (source_event->type == SDL_EVENT_KEY_DOWN)
            {
                const SDL_Scancode scancode = source_event->key.scancode;
                if (scancode >= 0 && scancode < SDL_SCANCODE_COUNT)
                    swallowed_key_releases[scancode] = true;
            }
            if (source_event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            {
                swallow_parent_pointer_until_release = true;
                close_popup();
            }
            if (keyboard_event || pointer_event)
                return true;
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
    media_popup.parent = nullptr;
    retired_popup_id = 0;
    swallow_parent_pointer_until_release = false;
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++)
        swallowed_key_releases[i] = false;
}

#endif // defined(HAVE_SDL)
