/* ES40 emulator.
 * Copyright (C) 2026 by gdwnldsKSC
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

#if defined(_WIN32)
#include "win32_media.h"
#include "storage/DiskFile.h"
#include <commdlg.h>
#include <windowsx.h>
#include <algorithm>
#include <string>
#include <vector>

namespace
{
const char media_class[] = "ES40 Media";
const char cd_filters[] = "CD-ROM images (*.iso;*.cue)\0*.iso;*.cue\0All files\0*.*\0";
const char floppy_filters[] =
    "Floppy images (*.img;*.ima;*.vfd;*.dsk;*.flp;*.bin)\0*.img;*.ima;*.vfd;*.dsk;*.flp;*.bin\0All files\0*.*\0";
const struct { const char* label; size_t bytes; } blank_formats[] = {
    { "360 KB (40 tracks, 9 sectors)", 360 * 1024 },
    { "720 KB (80 tracks, 9 sectors)", 720 * 1024 },
    { "1.2 MB (80 tracks, 15 sectors)", 1200 * 1024 },
    { "1.44 MB (80 tracks, 18 sectors)", 1440 * 1024 },
    { "2.88 MB (80 tracks, 36 sectors)", 2880 * 1024 }
};

bool pointer_down()
{
    return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_MBUTTON) |
             GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
}

bool pointer_press(UINT msg)
{
    return msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN ||
           msg == WM_LBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK;
}
}

struct win32_media_selector::state : std::enable_shared_from_this<state>
{
    enum class mode { closed, devices, floppy, size, cdrom, locked, message };
    mode page = mode::closed;
    HWND parent = nullptr, window = nullptr, dialog = nullptr;
    bool dialog_open = false, cancel_dialog = false, stopping = false;
    bool swallow_pointer = false, pending_eject = false;
    int selected = 0, device_selection = 0, locked_selection = 0, first_visible = 0;
    int width = 0, height = 0, row_height = 0, header_height = 0, footer_height = 0;
    std::vector<std::shared_ptr<CDiskFileMediaMailbox>> devices;
    std::shared_ptr<CDiskFileMediaMailbox> device;
    std::string pending_path, message;
    std::vector<RECT> rows;
    RECT reset_button = {};
    HFONT title_font = nullptr, body_font = nullptr, detail_font = nullptr;
    std::function<bool(UINT, WPARAM, LPARAM)> key_handler;
    std::function<void()> release_input, reset;

    ~state()
    {
        if (title_font) DeleteObject(title_font);
        if (body_font) DeleteObject(body_font);
        if (detail_font) DeleteObject(detail_font);
    }

    int item_count() const
    {
        switch (page)
        {
        case mode::devices: return (int)devices.size() + 1;
        case mode::floppy: return 5;
        case mode::size: return 6;
        case mode::cdrom: return 3;
        case mode::locked: return 2;
        case mode::message: return 1;
        default: return 0;
        }
    }
    int selection_count() const { return item_count() + (page == mode::devices ? 1 : 0); }

    void close_popup(bool restore_focus = true)
    {
        HWND old = window;
        window = nullptr;
        page = mode::closed;
        devices.clear(); device.reset(); pending_path.clear(); rows.clear();
        message.clear(); reset_button = {};
        pending_eject = false;
        if (old)
        {
            const bool had_focus = GetFocus() == old;
            DestroyWindow(old);
            if (release_input) release_input();
            if (restore_focus && had_focus && IsWindow(parent)) SetFocus(parent);
        }
    }

    void failed_popup(const char* reason) noexcept
    {
        printf("%%WIN32-W-MEDIA: Could not open the removable-media selector: %s\n", reason);
        // Setup can fail after the native window exists. Retire it and the
        // selection before returning to the guest, so the next toggle can retry.
        try { close_popup(); }
        catch (...) { printf("%%WIN32-W-MEDIA: Could not release media-selector input.\n"); }
    }

    void close()
    {
        cancel_dialog = true;
        if (dialog) PostMessage(dialog, WM_COMMAND, IDCANCEL, 0);
        close_popup();
    }

    void layout()
    {
        RECT bounds = { 0, 0, 640, 480 };
        GetClientRect(parent, &bounds);
        width = (std::max)(260, (std::min)(560, (int)bounds.right - 24));
        row_height = width >= 480 ? 58 : 44;
        header_height = width >= 480 ? 72 : 56;
        footer_height = width >= 480 ? 38 : 28;
        int available = (std::max)(1, ((int)bounds.bottom - 24 - header_height - footer_height) / row_height);
        int visible = (std::max)(1, (std::min)(item_count(), (std::min)(available, 8)));
        height = header_height + footer_height + row_height * visible;
        if (selected < item_count())
        {
            if (selected < first_visible) first_visible = selected;
            if (selected >= first_visible + visible) first_visible = selected - visible + 1;
        }
        first_visible = (std::max)(0, (std::min)(first_visible, (std::max)(0, item_count() - visible)));
        rows.clear();
        for (int i = first_visible; i < (std::min)(item_count(), first_visible + visible); ++i)
        {
            int y = header_height + (i - first_visible) * row_height;
            rows.push_back({ 14, y, width - 14, y + row_height - 6 });
        }
        reset_button = { width - 110, height - footer_height + 3, width - 14, height - 4 };
    }

    void update()
    {
        if (!window) return;
        layout();
        RECT bounds = {}; GetClientRect(parent, &bounds);
        POINT origin = { (std::max)(0, ((int)bounds.right - width) / 2),
                         (std::max)(0, ((int)bounds.bottom - height) / 2) };
        ClientToScreen(parent, &origin);
        SetWindowPos(window, nullptr, origin.x, origin.y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        if (window) InvalidateRect(window, nullptr, FALSE);
    }

    void create_popup()
    {
        if (stopping || !IsWindow(parent)) { close_popup(false); return; }
        WNDCLASSA wc = {};
        wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = media_class;
        if (!RegisterClassA(&wc))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS)
                FAILURE_1(Win32, "Unable to register media selector window class (error %lu)", error);
        }
        layout();
        window = CreateWindowExA(WS_EX_TOOLWINDOW, media_class, "ES40 Media", WS_POPUP,
            0, 0, width, height, parent, nullptr, wc.hInstance, this);
        if (!window) FAILURE_1(Win32, "Unable to create media selector window (error %lu)", GetLastError());
        update();
        // Native callbacks can already have dismissed a failed popup.
        if (window) ShowWindow(window, SW_SHOW);
        if (window) SetFocus(window);
    }

    void show_message(const std::string& text)
    {
        message = text; page = mode::message; selected = first_visible = 0;
        if (window) update(); else create_popup();
    }

    void queued(bool accepted)
    {
        if (accepted) close_popup();
        else show_message("The selected removable-media device is no longer available.");
    }

    void locked(bool eject, std::string path = {})
    {
        pending_eject = eject; pending_path = std::move(path);
        locked_selection = eject ? 1 : 0;
        page = mode::locked; selected = 1; first_visible = 0;
        if (window) update(); else create_popup();
    }

    static UINT_PTR CALLBACK dialog_hook(HWND hwnd, UINT msg, WPARAM, LPARAM lparam)
    {
        auto* self = reinterpret_cast<state*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (msg == WM_INITDIALOG)
        {
            auto* options = reinterpret_cast<OPENFILENAMEA*>(lparam);
            self = reinterpret_cast<state*>(options->lCustData);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            self->dialog = GetParent(hwnd);
            if (self->stopping || self->cancel_dialog)
                PostMessage(self->dialog, WM_COMMAND, IDCANCEL, 0);
        }
        else if (msg == WM_DESTROY && self) self->dialog = nullptr;
        return 0;
    }

    void file_dialog(std::shared_ptr<CDiskFileMediaMailbox> mailbox,
                     bool force = false, size_t blank_size = 0)
    {
        if (!mailbox || dialog_open || stopping || !IsWindow(parent)) return;
        // Nested native modal loops can dispatch emulator teardown. Retain this
        // state and an independent hidden owner until the common dialog returns.
        auto keep_alive = shared_from_this();
        std::vector<char> path(32768, 0);
        if (blank_size) lstrcpyA(path.data(), "blank.img");
        RECT bounds = {}; GetWindowRect(parent, &bounds);
        HWND owner = CreateWindowExA(WS_EX_TOOLWINDOW, "STATIC", "ES40 media selector", WS_POPUP,
            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        if (!owner) { show_message("Could not prepare the file dialog."); return; }
        struct dialog_owner
        {
            state& selector;
            HWND window;
            void finish()
            {
                selector.dialog = nullptr;
                if (window) { DestroyWindow(window); window = nullptr; }
                selector.dialog_open = false;
            }
            ~dialog_owner() { finish(); }
        } owner_guard = { *this, owner };
        dialog_open = true; cancel_dialog = false;
        close_popup(false);
        OPENFILENAMEA options = {};
        options.lStructSize = sizeof(options); options.hwndOwner = owner;
        options.lpstrFilter = mailbox->is_floppy() ? floppy_filters : cd_filters;
        options.nFilterIndex = 1; options.lpstrFile = path.data(); options.nMaxFile = (DWORD)path.size();
        options.lpstrTitle = blank_size ? "Create blank floppy image" : "Change removable media";
        options.lpstrDefExt = blank_size ? "img" : nullptr;
        options.Flags = OFN_EXPLORER | OFN_ENABLEHOOK | OFN_ENABLESIZING | OFN_HIDEREADONLY |
            OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (blank_size ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        options.lpfnHook = dialog_hook; options.lCustData = (LPARAM)this;
        BOOL accepted = blank_size ? GetSaveFileNameA(&options) : GetOpenFileNameA(&options);
        DWORD error = accepted ? 0 : CommDlgExtendedError();
        owner_guard.finish();
        swallow_pointer = swallow_pointer || pointer_down();
        if (release_input) release_input();
        if (stopping || cancel_dialog || !IsWindow(parent)) return;
        SetActiveWindow(parent); SetFocus(parent);
        if (error) { show_message("Could not open file selector (error " + std::to_string(error) + ")."); return; }
        if (!accepted || !path[0]) return;
        if (blank_size) queued(mailbox->request_blank_floppy(path.data(), blank_size));
        else if (!mailbox->is_floppy() && mailbox->media_locked() && !force)
        {
            devices = disk_file_removable_media(); device = mailbox; device_selection = 0;
            auto found = std::find(devices.begin(), devices.end(), mailbox);
            if (found != devices.end()) device_selection = (int)(found - devices.begin());
            locked(false, path.data());
        }
        else queued(mailbox->request_image_change(path.data(), force));
    }

    void back()
    {
        if (page == mode::size) { page = mode::floppy; selected = 1; }
        else if (page == mode::locked)
        {
            page = mode::cdrom; selected = locked_selection;
            pending_path.clear(); pending_eject = false;
        }
        else if (page == mode::floppy || page == mode::cdrom)
        {
            page = mode::devices; device.reset(); pending_path.clear();
            selected = (std::min)(device_selection, item_count() - 1);
        }
        else { close_popup(); return; }
        first_visible = 0; update();
    }

    void activate()
    {
        if (page == mode::devices)
        {
            if (selected < (int)devices.size())
            {
                device_selection = selected; device = devices[selected];
                if (!device) { close_popup(); return; }
                page = device->is_floppy() ? mode::floppy : mode::cdrom;
                selected = first_visible = 0; update();
            }
            else
            {
                const bool reset_selected = selected == item_count();
                auto request_reset = reset;
                close_popup();
                if (reset_selected && request_reset) request_reset();
            }
        }
        else if (page == mode::floppy && device)
        {
            if (selected == 0) file_dialog(device);
            else if (selected == 1) { page = mode::size; selected = 3; first_visible = 0; update(); }
            else if (selected == 2) queued(device->request_eject());
            else if (selected == 3) queued(device->request_read_only_toggle());
            else back();
        }
        else if (page == mode::size && device)
        {
            if (selected < 5) file_dialog(device, false, blank_formats[selected].bytes);
            else back();
        }
        else if (page == mode::cdrom && device)
        {
            if (selected < 2)
            {
                if (device->media_locked()) locked(selected == 1);
                else if (selected == 0) file_dialog(device);
                else queued(device->request_eject());
            }
            else back();
        }
        else if (page == mode::locked && device)
        {
            if (selected == 0)
            {
                if (pending_eject) queued(device->request_eject(true));
                else if (pending_path.empty()) file_dialog(device, true);
                else queued(device->request_image_change(pending_path.c_str(), true));
            }
            else back();
        }
        else close_popup();
    }

    void move(int delta)
    {
        int count = selection_count();
        if (count) { selected = (selected + delta + count) % count; update(); }
    }

    int hit(int x, int y) const
    {
        POINT point = { x, y };
        if (page == mode::devices && PtInRect(&reset_button, point)) return item_count();
        for (size_t i = 0; i < rows.size(); ++i)
            if (PtInRect(&rows[i], point)) return first_visible + (int)i;
        return -1;
    }

    void text(HDC dc, RECT area, const std::string& value, HFONT font, COLORREF color, UINT align = DT_VCENTER)
    {
        HGDIOBJ previous = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
        DrawTextA(dc, value.c_str(), (int)value.size(), &area,
                  DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | align);
        SelectObject(dc, previous);
    }

    void draw(HDC dc)
    {
        if (!title_font) title_font = CreateFontA(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");
        if (!body_font) body_font = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");
        if (!detail_font) detail_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");
        RECT all = { 0, 0, width, height };
        FillRect(dc, &all, GetSysColorBrush(COLOR_WINDOW));
        FrameRect(dc, &all, GetSysColorBrush(COLOR_WINDOWFRAME));
        std::string title = "Change removable media", subtitle;
        switch (page)
        {
        case mode::devices: subtitle = devices.empty() ? "No removable media devices are configured." : "Choose a configured drive"; break;
        case mode::floppy: title = "Floppy media"; subtitle = device->label(); break;
        case mode::size: title = "Create blank floppy image"; subtitle = "Choose an unformatted raw-image capacity"; break;
        case mode::cdrom: title = "CD-ROM media"; subtitle = device->label(); break;
        case mode::locked:
            title = "CD-ROM media locked";
            subtitle = pending_eject ? "The guest locked this CD-ROM; eject requires an override." :
                (!pending_path.empty() ? "The guest locked this CD-ROM while the file selector was open." :
                    (device ? device->label() + " is locked by the guest." : "The selected CD-ROM is locked by the guest."));
            break;
        default: subtitle = message; break;
        }
        text(dc, { 20, 9, width - 20, header_height - 26 }, title, title_font, GetSysColor(COLOR_WINDOWTEXT));
        text(dc, { 20, header_height - 26, width - 20, header_height - 5 }, subtitle, detail_font, GetSysColor(COLOR_WINDOWTEXT));
        for (size_t i = 0; i < rows.size(); ++i)
        {
            int index = first_visible + (int)i;
            RECT row = rows[i];
            bool active = index == selected;
            FillRect(dc, &row, GetSysColorBrush(active ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
            COLORREF color = GetSysColor(active ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
            std::string label, image;
            if (page == mode::devices)
            {
                if (index < (int)devices.size())
                {
                    label = devices[index]->label();
                    image = "Image: " + devices[index]->mounted_image_name();
                }
                else label = "Cancel";
            }
            else if (page == mode::floppy)
            {
                const char* labels[] = { "Change image...", "Create blank image...", "Eject", "", "Back" };
                label = index == 3 ? (device->displayed_read_only() ? "Make writable" : "Make read-only") : labels[index];
            }
            else if (page == mode::size) label = index < 5 ? blank_formats[index].label : "Back";
            else if (page == mode::cdrom)
            {
                const char* labels[] = { "Change image...", "Eject", "Back" }; label = labels[index];
            }
            else if (page == mode::locked) label = index == 0 ? (pending_eject ? "Force eject anyway" : "Force change anyway...") : "Back";
            else label = "Close";
            row.left += 12; row.right -= 12;
            if (image.empty()) text(dc, row, label, body_font, color);
            else
            {
                RECT line = row; line.bottom = line.top + (row.bottom - row.top) / 2;
                text(dc, line, label, body_font, color);
                line.top = line.bottom; line.bottom = row.bottom;
                text(dc, line, image, detail_font, color);
            }
        }
        RECT footer = { 20, height - footer_height, width - 20, height };
        if (page == mode::devices)
        {
            bool active = selected == item_count();
            FillRect(dc, &reset_button, GetSysColorBrush(active ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
            text(dc, reset_button, "Reset VM", detail_font,
                GetSysColor(active ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT), DT_CENTER | DT_VCENTER);
            footer.right = reset_button.left - 8;
        }
        text(dc, footer, width >= 480 ? "Arrows/Tab  Enter  Esc" : "Tab Enter Esc", detail_font, GetSysColor(COLOR_WINDOWTEXT));
    }

    LRESULT event(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
            key_handler && key_handler(msg, wparam, lparam)) return 0;
        switch (msg)
        {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            switch (wparam)
            {
            case VK_UP: move(-1); break;
            case VK_DOWN: move(1); break;
            case VK_TAB: move(GetKeyState(VK_SHIFT) & 0x8000 ? -1 : 1); break;
            case VK_HOME: selected = 0; update(); break;
            case VK_END: selected = selection_count() - 1; update(); break;
            case VK_RETURN: case VK_SPACE: if (!(lparam & (1L << 30))) activate(); break;
            case VK_ESCAPE: if (!(lparam & (1L << 30))) back(); break;
            case VK_F4: if (GetKeyState(VK_MENU) & 0x8000) close_popup(); break;
            }
            return 0;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: return 0;
        case WM_MOUSEWHEEL: if (GET_WHEEL_DELTA_WPARAM(wparam)) move(GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1); return 0;
        case WM_MOUSEMOVE: case WM_LBUTTONUP:
        {
            int index = hit(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (index >= 0)
            {
                if (selected != index) { selected = index; update(); }
                if (msg == WM_LBUTTONUP) activate();
            }
            return 0;
        }
        case WM_SYSCOMMAND:
            // The focused media popup needs the same display-power policy as
            // guest windows; the application owns the execution-state request.
            if ((wparam & 0xfff0) == SC_SCREENSAVE || (wparam & 0xfff0) == SC_MONITORPOWER)
                return 0;
            break;
        case WM_CLOSE: close_popup(); return 0;
        case WM_KILLFOCUS:
            swallow_pointer = swallow_pointer || pointer_down();
            close_popup(false); return 0;
        case WM_SHOWWINDOW: if (!wparam && window == hwnd) close_popup(false); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint; HDC dc = BeginPaint(hwnd, &paint);
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP bitmap = buffer ? CreateCompatibleBitmap(dc, width, height) : nullptr;
            HGDIOBJ old = bitmap ? SelectObject(buffer, bitmap) : nullptr;
            try { draw(bitmap ? buffer : dc); }
            catch (...)
            {
                if (bitmap) { SelectObject(buffer, old); DeleteObject(bitmap); }
                if (buffer) DeleteDC(buffer);
                EndPaint(hwnd, &paint); throw;
            }
            if (bitmap)
            {
                BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, old); DeleteObject(bitmap);
            }
            if (buffer) DeleteDC(buffer);
            EndPaint(hwnd, &paint); return 0;
        }
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        auto* raw = reinterpret_cast<state*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            raw = static_cast<state*>(reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)raw);
        }
        if (!raw) return DefWindowProcA(hwnd, msg, wparam, lparam);
        auto self = raw->shared_from_this();
        if (msg == WM_NCDESTROY)
        {
            if (self->window == hwnd) { self->window = nullptr; self->page = mode::closed; }
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcA(hwnd, msg, wparam, lparam);
        }
        try { return self->event(hwnd, msg, wparam, lparam); }
        catch (...)
        {
            printf("%%WIN32-W-MEDIA: Media selector operation failed.\n");
            try { self->close(); } catch (...) {}
            return 0;
        }
    }
};

win32_media_selector::win32_media_selector(
    std::function<bool(UINT, WPARAM, LPARAM)> key_handler,
    std::function<void()> release_input, std::function<void()> reset)
    : impl(std::make_shared<state>())
{
    impl->key_handler = std::move(key_handler);
    impl->release_input = std::move(release_input);
    impl->reset = std::move(reset);
}

win32_media_selector::~win32_media_selector() { shutdown(); }
void win32_media_selector::shutdown() noexcept
{
    impl->stopping = true;
    try { impl->close(); }
    catch (...) { printf("%%WIN32-W-MEDIA: Could not release media-selector input.\n"); }
    impl->key_handler = {}; impl->release_input = {}; impl->reset = {};
}
void win32_media_selector::close() { impl->close(); }
bool win32_media_selector::active() const { return impl->window || impl->dialog_open; }
HWND win32_media_selector::parent() const { return impl->parent; }
void win32_media_selector::reconcile_input()
{
    if (!pointer_down()) impl->swallow_pointer = false;
}

void win32_media_selector::toggle(HWND parent)
{
    if (impl->stopping || impl->dialog_open) return;
    if (impl->window) { impl->close_popup(); return; }
    try
    {
        impl->parent = parent;
        impl->devices = disk_file_removable_media();
        impl->page = state::mode::devices;
        impl->selected = impl->device_selection = impl->first_visible = 0;
        impl->swallow_pointer = false;
        impl->create_popup();
    }
    catch (const CException& error) { impl->failed_popup(error.message().c_str()); }
    catch (const std::exception& error) { impl->failed_popup(error.what()); }
    catch (...) { impl->failed_popup("Unknown media-selector error."); }
}

bool win32_media_selector::filter_message(UINT msg, WPARAM, LPARAM)
{
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && impl->swallow_pointer)
    {
        if (!pointer_down()) impl->swallow_pointer = false;
        return true;
    }
    if (!active()) return false;
    if (pointer_press(msg) && !impl->dialog_open)
    {
        impl->swallow_pointer = true;
        impl->close_popup(false);
    }
    return true;
}
#endif
