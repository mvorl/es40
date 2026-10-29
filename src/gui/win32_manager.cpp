/* ES40 native machine manager.
 * Copyright (C) 2026 by the ES40 Emulator Project
 * SPDX-License-Identifier: BSD-1-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
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
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "win32_session.h"
#include "win32_library.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <climits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

using namespace std;

static constexpr wchar_t manager_class[] = L"ES40.Workstation";
enum win32_manager_control
{
    manager_add = 100, manager_start, manager_stop, manager_remove, manager_library,
    manager_settings, manager_new, manager_copy, manager_restart, manager_force_stop,
    manager_media, manager_capture, manager_release, manager_ctrl_alt_delete,
    manager_fullscreen, manager_fit, manager_displays, manager_exit,
    manager_tabs, manager_folder, manager_open_log, manager_about
};
static constexpr wchar_t manager_config_filter[] = L"ES40 configuration (*.cfg)\0*.cfg\0All files\0*.*\0";
static constexpr wchar_t manager_rom_filter[] = L"Firmware images (*.exe;*.rom;*.bin)\0*.exe;*.rom;*.bin\0All files\0*.*\0";

static RECT manager_work_area(HWND window)
{
    MONITORINFO monitor = { sizeof(monitor) };
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return monitor.rcWork;
    return { 0, 0, GetSystemMetrics(SM_CXFULLSCREEN), GetSystemMetrics(SM_CYFULLSCREEN) };
}

static wstring manager_text(const string& text)
{
    if (text.empty()) return {};
    UINT page = CP_UTF8;
    int count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (!count) { page = CP_ACP; count = MultiByteToWideChar(page, 0, text.data(), (int)text.size(), nullptr, 0); }
    wstring result(count, L'\0');
    if (count) MultiByteToWideChar(page, 0, text.data(), (int)text.size(), result.data(), count);
    return result;
}

static filesystem::path manager_store_directory()
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)))
        throw runtime_error("Unable to locate the local application data folder.");
    unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(raw, CoTaskMemFree);
    return filesystem::path(folder.get()) / L"ES40" / L"Workstation";
}

static filesystem::path manager_choose_file(HWND owner, const wchar_t* title, const wchar_t* filter,
    bool save = false, const wchar_t* initial = L"AlphaServer.cfg")
{
    vector<wchar_t> path(32768, 0);
    if (save) wcscpy_s(path.data(), path.size(), initial);
    OPENFILENAMEW dialog = { sizeof(dialog) };
    dialog.hwndOwner = owner; dialog.lpstrTitle = title; dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data(); dialog.nMaxFile = (DWORD)path.size();
    dialog.lpstrDefExt = save ? L"cfg" : nullptr;
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
        (save ? OFN_NOTESTFILECREATE : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))
        return filesystem::absolute(path.data()).lexically_normal();
    const DWORD error = CommDlgExtendedError();
    if (error) throw runtime_error("Unable to open the file chooser (code " + to_string(error) + ").");
    return {};
}

static string manager_config_bytes(const wstring& text)
{
    if (text.empty()) return {};
    if (text.size() > INT_MAX) throw runtime_error("The configuration is too large.");
    const bool unicode = GetACP() == CP_UTF8;
    const DWORD flags = unicode ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL substituted = FALSE;
    BOOL* fallback = unicode ? nullptr : &substituted;
    const int count = WideCharToMultiByte(CP_ACP, flags, text.data(), (int)text.size(), nullptr, 0, nullptr, fallback);
    string bytes(count, '\0');
    if (!count || !WideCharToMultiByte(CP_ACP, flags, text.data(), (int)text.size(),
        bytes.data(), count, nullptr, fallback) || substituted)
        throw runtime_error("Choose paths representable in the active Windows code page.");
    return bytes;
}

static void manager_require_unused_path(const filesystem::path& path)
{
    if (filesystem::exists(filesystem::symlink_status(path)))
        throw filesystem::filesystem_error("Choose a different machine filename or folder; this path already exists",
            path, make_error_code(errc::file_exists));
}

static void manager_require_new_paths(const filesystem::path& configuration)
{
    manager_require_unused_path(configuration);
    const wstring base = configuration.stem().wstring();
    for (const auto* suffix : { L".img", L"-flash.rom", L"-dpr.rom", L"-toy.rom" })
    {
        const filesystem::path path = configuration.parent_path() / (base + suffix);
        if (win32_library_same_configuration(configuration, path))
            throw runtime_error("The configuration filename conflicts with its disk or state files. Choose a .cfg filename.");
        manager_require_unused_path(path);
    }
}

static string manager_read_configuration(const filesystem::path& path)
{
    if (!filesystem::is_regular_file(path)) throw runtime_error("The source configuration file is missing.");
    struct file_guard
    {
        HANDLE value;
        ~file_guard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        throw system_error((int)GetLastError(), system_category(), "Unable to read the source configuration");
    string bytes;
    char buffer[8192];
    for (;;)
    {
        DWORD count = 0;
        if (!ReadFile(file.value, buffer, sizeof(buffer), &count, nullptr))
            throw system_error((int)GetLastError(), system_category(), "Unable to read the source configuration");
        if (!count) return bytes;
        if (bytes.size() > (size_t)INT_MAX - count) throw runtime_error("The source configuration is too large.");
        bytes.append(buffer, count);
    }
}

static void manager_write_configuration(const filesystem::path& configuration, const string& bytes, bool check_companions)
{
    if (check_companions) manager_require_new_paths(configuration);
    else manager_require_unused_path(configuration);
    struct temporary_file
    {
        filesystem::path path;
        HANDLE handle = INVALID_HANDLE_VALUE;
        bool created = false;
        ~temporary_file()
        {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            if (created) DeleteFileW(path.c_str());
        }
    } temporary;
    const auto fail = [](const char* message, DWORD error)
    { throw system_error((int)error, system_category(), message); };
    const wstring stem = configuration.filename().wstring() + L".manager-" +
        to_wstring(GetCurrentProcessId()) + L"-" + to_wstring(GetTickCount64());
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        temporary.path = configuration.parent_path() / (stem + L"-" + to_wstring(attempt) + L".tmp");
        temporary.handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary.handle != INVALID_HANDLE_VALUE) { temporary.created = true; break; }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            fail("Unable to create the machine configuration", error);
    }
    if (!temporary.created) throw runtime_error("Unable to choose a temporary configuration filename.");
    size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD written = 0;
        if (!WriteFile(temporary.handle, bytes.data() + offset, (DWORD)(bytes.size() - offset), &written, nullptr))
            fail("Unable to write the machine configuration", GetLastError());
        if (!written) fail("Unable to write the machine configuration", ERROR_WRITE_FAULT);
        offset += written;
    }
    if (!FlushFileBuffers(temporary.handle)) fail("Unable to flush the machine configuration", GetLastError());
    if (!CloseHandle(temporary.handle)) fail("Unable to close the machine configuration", GetLastError());
    temporary.handle = INVALID_HANDLE_VALUE;
    // Recheck paths before publishing the configuration.
    if (check_companions) manager_require_new_paths(configuration);
    else manager_require_unused_path(configuration);
    if (!MoveFileExW(temporary.path.c_str(), configuration.c_str(), MOVEFILE_WRITE_THROUGH))
        fail("Unable to save the new machine configuration", GetLastError());
    temporary.created = false;
}

static wstring manager_read_log(const filesystem::path& path)
{
    if (path.empty()) return {};
    struct file_guard
    {
        HANDLE value;
        ~file_guard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size = {}, offset = {};
    if (!GetFileSizeEx(file.value, &size)) return {};
    const DWORD count = (DWORD)min<LONGLONG>(size.QuadPart, 64 * 1024);
    offset.QuadPart = size.QuadPart - count;
    if (!SetFilePointerEx(file.value, offset, nullptr, FILE_BEGIN)) return {};
    string bytes(count, '\0');
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), count, &read, nullptr)) return {};
    bytes.resize(read);
    const wstring decoded = manager_text(bytes);
    wstring text = offset.QuadPart ? L"[Earlier output omitted]\r\n" : L"";
    for (size_t i = 0; i < decoded.size(); ++i)
    {
        if (decoded[i] == L'\n' && (!i || decoded[i - 1] != L'\r')) text += L'\r';
        text += decoded[i];
    }
    return text;
}

static wstring manager_configuration_preview(const filesystem::path& path)
{
    struct file_guard
    {
        HANDLE value;
        ~file_guard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return L"Unable to read the configuration file.";
    const DWORD limit = 128 * 1024;
    string bytes(limit + 1, '\0');
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), (DWORD)bytes.size(), &read, nullptr))
        return L"Unable to read the configuration file.";
    const int length = (int)min(read, limit);
    const int count = MultiByteToWideChar(CP_ACP, 0, bytes.data(), length, nullptr, 0);
    wstring decoded(count, L'\0'), text;
    if (count) MultiByteToWideChar(CP_ACP, 0, bytes.data(), length, decoded.data(), count);
    for (size_t i = 0; i < decoded.size(); ++i)
    {
        if (decoded[i] == L'\n' && (!i || decoded[i - 1] != L'\r')) text += L'\r';
        text += decoded[i];
    }
    if (read > limit) text += L"\r\n[Configuration preview truncated]";
    return text;
}

class win32_manager_window;

struct win32_manager_machine
{
    win32_manager_window* owner = nullptr;
    filesystem::path configuration, log;
    HWND host = nullptr;
    win32_session session;
    win32_settings_session settings;
    wstring failure;
    unsigned width = 0, height = 0;
    bool restart = false, completed = false;

    ~win32_manager_machine() { if (IsWindow(host)) DestroyWindow(host); }

    const wchar_t* state() const
    {
        if (settings.running()) return L"Editing settings";
        if (restart) return L"Restarting";
        if (session.stopping()) return L"Stopping";
        if (session.running()) return session.display() ? L"Running" : L"Starting";
        return failure.empty() ? L"Powered off" : L"Error";
    }
};

class win32_manager_window
{
    HWND window = nullptr, library = nullptr, empty = nullptr, status = nullptr, output = nullptr;
    HWND tabs = nullptr, details = nullptr, heading = nullptr, subtitle = nullptr;
    HFONT font = nullptr, heading_font = nullptr, log_font = nullptr;
    HBRUSH background = nullptr;
    COLORREF background_color = RGB(245, 247, 250), text_color = RGB(35, 48, 66);
    filesystem::path executable, store;
    vector<unique_ptr<win32_manager_machine>> machines;
    vector<HWND> buttons;
    wstring last_output, last_details, config_preview;
    int selection = -1, page = 0, dpi = 96;
    unsigned sequence = 0;
    bool closing = false, changing_selection = false, preview_dirty = true;

    int px(int value) const { return MulDiv(value, dpi, 96); }

    win32_manager_machine* selected() const
    { return selection >= 0 && selection < (int)machines.size() ? machines[selection].get() : nullptr; }

    bool running() const
    { return any_of(machines.begin(), machines.end(), [](const auto& item) { return item->session.running(); }); }

    void release_input()
    { for (const auto& item : machines) item->session.send(win32_manager_release); }

    bool console_ready() const
    {
        const auto* item = selected();
        return item && item->session.running() && item->session.display() && !item->session.stopping() && !closing;
    }

    void console_command(win32_manager_action action)
    {
        if (!console_ready()) return;
        auto* item = selected();
        if (action == win32_manager_capture || action == win32_manager_media ||
            action == win32_manager_fullscreen || action == win32_manager_reset_size) select_page(0);
        if (action == win32_manager_capture)
        {
            const HWND display = item->session.display();
            DWORD pid = 0;
            if (!GetWindowThreadProcessId(display, &pid) || pid != item->session.process_id()) return;
            const HWND root = GetAncestor(display, GA_ROOT);
            if (IsIconic(root)) ShowWindow(root, SW_RESTORE);
            SetForegroundWindow(root);
            SetFocus(display);
        }
        if (!item->session.send(action)) throw runtime_error("Unable to send the command to the machine console.");
    }

    void create_menus()
    {
        struct menu_guard
        {
            HMENU value = CreateMenu();
            ~menu_guard() { if (value) DestroyMenu(value); }
        } menu;
        if (!menu.value) throw runtime_error("Unable to create the manager menu.");
        const auto popup = [&](const wchar_t* title)
        {
            HMENU child = CreatePopupMenu();
            if (!child) throw runtime_error("Unable to create a manager submenu.");
            if (!AppendMenuW(menu.value, MF_POPUP, (UINT_PTR)child, title))
            { DestroyMenu(child); throw runtime_error("Unable to attach a manager submenu."); }
            return child;
        };
        const auto entry = [](HMENU menu, UINT id, const wchar_t* text)
        {
            if (!AppendMenuW(menu, text ? MF_STRING : MF_SEPARATOR, id, text))
                throw runtime_error("Unable to add a manager menu item.");
        };
        const HMENU file = popup(L"&File"), machine = popup(L"&Machine"), view = popup(L"&View"), help = popup(L"&Help");
        entry(file, manager_new, L"&New machine...");
        entry(file, manager_add, L"&Add configuration...");
        entry(file, manager_copy, L"&Copy configuration...");
        entry(file, manager_remove, L"&Remove from library");
        entry(file, 0, nullptr);
        entry(file, manager_folder, L"Open machine &folder");
        entry(file, manager_open_log, L"Open session &log");
        entry(file, 0, nullptr);
        entry(file, manager_exit, L"E&xit");
        entry(machine, manager_start, L"&Start");
        entry(machine, manager_stop, L"S&top");
        entry(machine, manager_restart, L"&Restart");
        entry(machine, manager_force_stop, L"&Force Stop...");
        entry(machine, 0, nullptr);
        entry(machine, manager_settings, L"Machine s&ettings...");
        entry(machine, manager_media, L"Removable &media...");
        entry(machine, 0, nullptr);
        entry(machine, manager_capture, L"Capture &input");
        entry(machine, manager_release, L"Re&lease input");
        entry(machine, manager_ctrl_alt_delete, L"Send Ctrl+Alt+&Delete");
        entry(view, manager_fullscreen, L"&Full screen");
        entry(view, manager_fit, L"Fit console to &window");
        entry(view, manager_displays, L"Show &additional displays");
        entry(help, manager_about, L"&About ES40 Workstation");
        if (!SetMenu(window, menu.value)) throw runtime_error("Unable to attach the manager menu.");
        menu.value = nullptr;
    }

    void update_colors()
    {
        HIGHCONTRASTW contrast = { sizeof(contrast) };
        const bool high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
            (contrast.dwFlags & HCF_HIGHCONTRASTON);
        background_color = high_contrast ? GetSysColor(COLOR_WINDOW) : RGB(245, 247, 250);
        text_color = high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : RGB(35, 48, 66);
        HBRUSH brush = CreateSolidBrush(background_color);
        if (!brush) throw runtime_error("Unable to create the manager background.");
        if (background) DeleteObject(background);
        background = brush;
        if (window) RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void initialize_fonts()
    {
        if (HDC dc = GetDC(nullptr))
        {
            const int value = GetDeviceCaps(dc, LOGPIXELSX);
            if (value > 0) dpi = value;
            ReleaseDC(nullptr, dc);
        }
        const auto create = [&](int size, int weight, const wchar_t* face)
        {
            return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        };
        font = create(14, FW_NORMAL, L"Segoe UI");
        heading_font = create(22, FW_SEMIBOLD, L"Segoe UI");
        log_font = create(14, FW_NORMAL, L"Consolas");
        if (!font || !heading_font || !log_font) throw runtime_error("Unable to create the manager fonts.");
    }

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id = 0, DWORD extended = 0)
    {
        HWND result = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        if (!result) throw runtime_error("Unable to create a manager control.");
        SendMessageW(result, WM_SETFONT, (WPARAM)font, TRUE);
        return result;
    }

    void fit_display(win32_manager_machine& item)
    {
        HWND display = item.session.display();
        if (!display || GetParent(display) != item.host) return;
        RECT bounds = {};
        if (GetClientRect(item.host, &bounds))
            SetWindowPos(display, nullptr, 0, 0, bounds.right, bounds.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void layout()
    {
        if (!library || !empty || !status) return;
        RECT bounds = {};
        GetClientRect(window, &bounds);
        const int width = bounds.right, height = bounds.bottom;
        SendMessageW(status, WM_SIZE, 0, 0);
        RECT footer = {};
        GetWindowRect(status, &footer);
        const int footer_height = footer.bottom - footer.top;
        const int margin = px(16), sidebar = px(256);
        MoveWindow(heading, margin, px(14), max(1, width - 2 * margin), px(30), TRUE);
        MoveWindow(subtitle, margin, px(48), max(1, width - 2 * margin), px(24), TRUE);
        int x = margin, y = px(82);
        for (HWND button : buttons)
        {
            const int button_width = px(GetDlgCtrlID(button) == manager_fullscreen ? 108 : 86);
            if (x > margin && x + button_width > width - margin) { x = margin; y += px(38); }
            MoveWindow(button, x, y, button_width, px(30), TRUE);
            x += button_width + px(8);
        }
        const int top = y + px(44), content_height = max(1, height - top - footer_height - margin);
        MoveWindow(library, margin, top, sidebar, content_height, TRUE);
        const int left = sidebar + 2 * margin, content_width = max(1, width - left - margin);
        SetWindowPos(tabs, HWND_BOTTOM, left, top, content_width, content_height, SWP_NOACTIVATE);
        RECT content = {};
        GetClientRect(tabs, &content);
        TabCtrl_AdjustRect(tabs, FALSE, &content);
        MapWindowPoints(tabs, window, reinterpret_cast<POINT*>(&content), 2);
        const int page_width = max(1L, content.right - content.left), page_height = max(1L, content.bottom - content.top);
        MoveWindow(empty, content.left + px(24), content.top + px(60), max(1, page_width - px(48)),
            max(1, page_height - px(80)), TRUE);
        ShowWindow(empty, selected() ? SW_HIDE : SW_SHOWNA);
        for (const auto& item : machines)
        {
            MoveWindow(item->host, content.left, content.top, page_width, page_height, TRUE);
            ShowWindow(item->host, item.get() == selected() && page == 0 ? SW_SHOWNA : SW_HIDE);
            fit_display(*item);
        }
        MoveWindow(details, content.left, content.top, page_width, page_height, TRUE);
        MoveWindow(output, content.left, content.top, page_width, page_height, TRUE);
        ShowWindow(details, selected() && page == 1 ? SW_SHOWNA : SW_HIDE);
        ShowWindow(output, selected() && page == 2 ? SW_SHOWNA : SW_HIDE);
    }

    void select_page(int index)
    {
        if (index < 0 || index > 2) return;
        if (page != index)
        {
            if (auto* item = selected()) item->session.send(win32_manager_release);
            SetFocus(tabs);
        }
        page = index;
        TabCtrl_SetCurSel(tabs, page);
        preview_dirty = true;
        layout(); update_details(); update_output();
    }

    void update_details()
    {
        if (page != 1) return;
        wstring text;
        if (const auto* item = selected())
        {
            if (preview_dirty)
            {
                config_preview = manager_configuration_preview(item->configuration);
                preview_dirty = false;
            }
            text = L"Machine: " + item->configuration.stem().wstring() + L"\r\nState: " + item->state() +
                L"\r\nConfiguration: " + item->configuration.wstring() +
                L"\r\nWorking folder: " + item->configuration.parent_path().wstring() +
                L"\r\n\r\nGraphics: Native Win32\r\nDisplay #0: Embedded console\r\nAdditional displays: Separate windows\r\n";
            if (item->session.running()) text += L"\r\nProcess ID: " + to_wstring(item->session.process_id()) + L"\r\n";
            else if (item->completed) text += L"\r\nLast exit code: " + to_wstring(item->session.exit_code()) + L"\r\n";
            if (!item->failure.empty()) text += L"\r\n" + item->failure + L"\r\n";
            if (!item->log.empty()) text += L"\r\nSession log: " + item->log.wstring() + L"\r\n";
            text += L"\r\nConfiguration preview (use Settings to edit):\r\n\r\n" + config_preview;
        }
        if (text == last_details) return;
        last_details = text;
        SetWindowTextW(details, text.c_str());
    }

    void refresh()
    {
        if (!window) return;
        const auto* item = selected();
        const HMENU menu = GetMenu(window);
        const auto enabled = [&](int id, bool value)
        {
            if (HWND button = GetDlgItem(window, id)) EnableWindow(button, value);
            if (menu) EnableMenuItem(menu, id, MF_BYCOMMAND | (value ? MF_ENABLED : MF_GRAYED));
        };
        enabled(manager_new, !closing);
        enabled(manager_add, !closing);
        enabled(manager_copy, item && !item->session.running() && !item->settings.running() && !closing);
        enabled(manager_remove, item && !item->session.running() && !item->settings.running() && !closing);
        enabled(manager_start, item && !item->session.running() && !item->settings.running() && !closing);
        enabled(manager_settings, item && !item->session.running() && !closing);
        enabled(manager_stop, item && item->session.running() && (!item->session.stopping() || item->restart));
        enabled(manager_restart, item && item->session.running() && !item->session.stopping() && !closing);
        enabled(manager_force_stop, item && item->session.running());
        enabled(manager_folder, item != nullptr);
        error_code log_error;
        enabled(manager_open_log, item && !item->log.empty() && filesystem::is_regular_file(item->log, log_error));
        const bool ready = console_ready();
        for (int id : {manager_media, manager_capture, manager_release, manager_ctrl_alt_delete,
            manager_fullscreen, manager_fit, manager_displays}) enabled(id, ready);
        DrawMenuBar(window);
        const wstring title = item ? item->configuration.stem().wstring() + L"  /  " + item->state() : L"AlphaServer virtual machines";
        SetWindowTextW(subtitle, title.c_str());
        for (size_t i = 0; i < machines.size(); ++i)
        {
            LVITEMW row = {};
            row.iItem = (int)i; row.iSubItem = 1;
            row.pszText = const_cast<LPWSTR>(machines[i]->state());
            SendMessageW(library, LVM_SETITEMTEXTW, i, (LPARAM)&row);
            InvalidateRect(machines[i]->host, nullptr, TRUE);
        }
        refresh_status();
        update_details();
    }

    void refresh_status()
    {
        wstring text = L"Create a machine or add an existing configuration.";
        if (const auto* item = selected())
        {
            text = item->configuration.stem().wstring() + L"  |  " + item->state();
            if (item->session.running() && item->width && item->height)
                text += L"  |  Display #0: " + to_wstring(item->width) + L" x " + to_wstring(item->height);
            if (console_ready()) text += L"  |  Machine > Capture input, or use the configured capture hotkey.";
        }
        if (closing) text = L"Stopping before close. Machine > Force Stop remains available.";
        SendMessageW(status, SB_SETTEXTW, 0, (LPARAM)text.c_str());
    }

    void update_output()
    {
        if (page != 2) return;
        wstring text;
        if (const auto* item = selected())
        {
            text = item->failure;
            if (!text.empty()) text += L"\r\n";
            text += manager_read_log(item->log);
            if (text.empty()) text = item->session.running() ? L"Waiting for session output..." :
                item->log.empty() ? L"Start this machine to create a session log." : L"No session output is available.";
        }
        if (text == last_output) return;
        last_output = text;
        SetWindowTextW(output, text.c_str());
        SendMessageW(output, EM_SETSEL, (WPARAM)text.size(), (LPARAM)text.size());
        SendMessageW(output, EM_SCROLLCARET, 0, 0);
    }

    void open_machine_path(bool log)
    {
        const auto* item = selected();
        if (!item) return;
        release_input();
        const auto path = log ? item->log : item->configuration.parent_path();
        if (log && (path.empty() || !filesystem::is_regular_file(path)))
            throw runtime_error("No session log is available for this machine.");
        const wstring argument = L"\"" + path.wstring() + L"\"";
        const auto result = ShellExecuteW(window, L"open", log ? L"notepad.exe" : path.c_str(),
            log ? argument.c_str() : nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)result <= 32) throw runtime_error(log ? "Unable to open the session log." : "Unable to open the machine folder.");
    }

    void report(const char* message) noexcept
    {
        release_input();
        try { MessageBoxW(window, manager_text(message).c_str(), L"ES40 Workstation", MB_OK | MB_ICONERROR); }
        catch (...) { MessageBoxW(window, L"An unexpected manager error occurred.", L"ES40 Workstation", MB_OK | MB_ICONERROR); }
    }

    void rebuild_library()
    {
        changing_selection = true;
        struct rebuild_guard
        {
            bool& changing;
            HWND list;
            ~rebuild_guard() { SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE); changing = false; }
        } guard{changing_selection, library};
        SendMessageW(library, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(library);
        for (size_t i = 0; i < machines.size(); ++i)
        {
            const wstring name = machines[i]->configuration.stem().wstring();
            LVITEMW row = {};
            row.mask = LVIF_TEXT; row.iItem = (int)i;
            row.pszText = const_cast<LPWSTR>(name.c_str());
            if (SendMessageW(library, LVM_INSERTITEMW, 0, (LPARAM)&row) < 0)
                throw runtime_error("Unable to display the machine library.");
        }
        if (selection >= 0)
            ListView_SetItemState(library, selection, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    void select(int index)
    {
        if (auto* old = selected()) old->session.send(win32_manager_release);
        selection = index >= 0 && index < (int)machines.size() ? index : -1;
        changing_selection = true;
        ListView_SetItemState(library, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (selection >= 0)
        {
            ListView_SetItemState(library, selection, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(library, selection, FALSE);
        }
        changing_selection = false;
        preview_dirty = true;
        layout(); refresh(); update_output();
    }

    void add_configuration(const filesystem::path& path, bool save = true)
    {
        if (closing) return;
        const filesystem::path configuration = filesystem::absolute(path).lexically_normal();
        if (save && !filesystem::is_regular_file(configuration)) throw runtime_error("The configuration file is missing.");
        for (size_t i = 0; i < machines.size(); ++i)
        {
            if (!win32_library_same_configuration(machines[i]->configuration, configuration)) continue;
            if (save) win32_library_update(store, configuration, true);
            select((int)i);
            return;
        }
        auto item = make_unique<win32_manager_machine>();
        item->owner = this; item->configuration = configuration;
        item->host = CreateWindowExW(0, win32_manager_host_class, L"", WS_CHILD | WS_TABSTOP |
            WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, window, nullptr, GetModuleHandleW(nullptr), item.get());
        if (!item->host) throw runtime_error("Unable to create the console host.");
        machines.reserve(machines.size() + 1);
        if (save) win32_library_update(store, configuration, true);
        if (auto* old = selected()) old->session.send(win32_manager_release);
        machines.push_back(move(item));
        selection = (int)machines.size() - 1;
        rebuild_library(); select(selection);
    }

    void remove_configuration()
    {
        auto* item = selected();
        if (!item || item->session.running() || item->settings.running() || closing) return;
        release_input();
        if (MessageBoxW(window, L"Remove this machine from the library? Its files will remain on disk.",
            L"Remove machine", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        win32_library_update(store, item->configuration, false);
        machines.erase(machines.begin() + selection);
        selection = machines.empty() ? -1 : min(selection, (int)machines.size() - 1);
        rebuild_library(); select(selection);
    }

    bool add_created_configuration(const filesystem::path& configuration, const wchar_t* title)
    {
        try { add_configuration(configuration); return true; }
        catch (const exception& error)
        {
            const wstring message = L"The configuration was created at:\r\n" + configuration.wstring() +
                L"\r\n\r\nUnable to finish adding it to the library. Use Add to select it.\r\n\r\n" + manager_text(error.what());
            MessageBoxW(window, message.c_str(), title, MB_OK | MB_ICONERROR);
            return false;
        }
    }

    void choose_configuration()
    {
        if (closing) return;
        release_input();
        const auto path = manager_choose_file(window, L"Add machine configuration", manager_config_filter);
        if (!path.empty() && IsWindow(window)) add_configuration(path);
    }

    void new_machine()
    {
        if (closing) return;
        release_input();
        const auto configuration = manager_choose_file(window, L"New machine - choose its configuration file",
            manager_config_filter, true);
        if (configuration.empty() || closing || !IsWindow(window)) return;
        manager_config_bytes(configuration.wstring());
        manager_require_new_paths(configuration);
        const auto firmware = manager_choose_file(window, L"Choose the AlphaServer ES40 SRM firmware", manager_rom_filter);
        if (firmware.empty() || closing || !IsWindow(window)) return;
        const auto video_rom = manager_choose_file(window, L"Choose the S3 VGA BIOS", manager_rom_filter);
        if (video_rom.empty() || closing || !IsWindow(window)) return;
        if (!filesystem::is_regular_file(firmware) || !filesystem::is_regular_file(video_rom))
            throw runtime_error("A selected firmware image is no longer available.");
        const auto quote = [](const wstring& value)
        {
            wstring result = L"\"";
            for (wchar_t c : value) { if (c == L'"') result += L'"'; result += c; }
            return result + L'"';
        };
        const wstring base = configuration.stem().wstring();
        const wstring text = L"// Created by ES40 Workstation. Paths resolve relative to this file.\r\n"
            L"gui = win32 { video.linear = true; }\r\n"
            L"sys0 = tsunami\r\n{\r\n  memory.bits = 29;\r\n  rom.srm = " + quote(firmware.wstring()) +
            L";\r\n  rom.flash = " + quote(base + L"-flash.rom") + L";\r\n  rom.dpr = " + quote(base + L"-dpr.rom") +
            L";\r\n  rom.toy = " + quote(base + L"-toy.rom") +
            L";\r\n  cpu0 = ev68cb { speed = 500M; }\r\n"
            L"  pci0.7 = ali { vga_console = true; }\r\n  pci0.19 = ali_usb {}\r\n  pci0.17 = ali_pmu {}\r\n"
            L"  pci0.2 = s3 { rom = " + quote(video_rom.wstring()) + L"; }\r\n"
            L"  pci0.15 = ali_ide\r\n  {\r\n    disk0.0 = file { file = " + quote(base + L".img") +
            L"; autocreate_size = 2G; }\r\n    disk1.0 = file { cdrom = true; }\r\n  }\r\n}\r\n";
        manager_write_configuration(configuration, manager_config_bytes(text), true);
        if (!add_created_configuration(configuration, L"New machine")) return;
        MessageBoxW(window, L"Machine created: one CPU, 512 MB RAM, S3 graphics, a 2 GB disk, and an empty CD-ROM drive.\n\n"
            L"The disk is created on first start. Use Settings to change the hardware.",
            L"New machine", MB_OK | MB_ICONINFORMATION);
    }

    void copy_configuration()
    {
        const auto* item = selected();
        if (!item || item->session.running() || item->settings.running() || closing) return;
        release_input();
        const filesystem::path source = item->configuration;
        win32_configuration_lock lock(source);
        const auto configuration = manager_choose_file(window, L"Copy configuration (disk images are not copied)",
            manager_config_filter, true, L"Copy.cfg");
        if (configuration.empty() || closing || !IsWindow(window)) return;
        manager_config_bytes(configuration.wstring());
        if (win32_library_same_configuration(source, configuration))
            throw runtime_error("Choose a new filename for the configuration copy.");
        manager_require_unused_path(configuration);
        if (MessageBoxW(window, L"This copies only the configuration. Disks and firmware/state files are not copied.\n\n"
            L"Saved paths stay unchanged. Relative paths resolve from the destination folder and may refer to different files. "
            L"Review storage and state paths in Settings before running both machines.\n\nCopy the configuration?",
            L"Copy configuration", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        if (closing || !IsWindow(window)) return;
        manager_write_configuration(configuration, manager_read_configuration(source), false);
        add_created_configuration(configuration, L"Copy configuration");
    }

    void start(win32_manager_machine& item)
    {
        if (item.session.running() || item.settings.running() || closing) return;
        item.restart = false;
        item.completed = false;
        item.width = item.height = 0;
        item.failure.clear(); item.log.clear();
        try
        {
            filesystem::create_directories(store / L"logs");
            item.log = store / L"logs" / (L"session-" + to_wstring(GetCurrentProcessId()) + L"-" +
                to_wstring(GetTickCount64()) + L"-" + to_wstring(++sequence) + L".log");
            item.session.start(executable, item.configuration, item.log, item.host);
        }
        catch (const exception& error)
        {
            item.failure = manager_text(error.what());
            refresh(); update_output();
            throw;
        }
        refresh(); update_output();
    }

    void start()
    { if (auto* item = selected()) start(*item); }

    void stop(bool restart = false)
    {
        auto* item = selected();
        if (!item || !item->session.running() || (restart && (item->session.stopping() || closing))) return;
        item->restart = restart;
        item->session.stop();
        refresh();
    }

    void force_stop()
    {
        auto* item = selected();
        if (!item || !item->session.running()) return;
        const DWORD pid = item->session.process_id();
        const auto log = item->log;
        release_input();
        if (MessageBoxW(window, L"Force stop this machine? Unsaved guest data may be lost. Use this if Stop does not complete.",
            L"Force Stop", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        if (!item->session.running() || item->session.process_id() != pid || item->log != log) return;
        item->restart = false;
        refresh();
        item->session.force_stop();
        refresh();
    }

    void edit_settings()
    {
        auto* item = selected();
        if (!item || item->session.running() || closing) return;
        release_input();
        if (item->settings.running()) { item->settings.activate(); return; }
        item->settings.start(executable.parent_path() / L"es40-cfg-win32.exe", item->configuration);
        item->failure.clear();
        refresh(); update_output();
    }

    void close()
    {
        for (const auto& item : machines)
        {
            if (!item->settings.running()) continue;
            release_input();
            MessageBoxW(window, L"Save or cancel the open machine settings before closing ES40 Workstation.",
                L"Machine settings are open", MB_OK | MB_ICONINFORMATION);
            item->settings.activate();
            return;
        }
        if (!running()) { DestroyWindow(window); return; }
        release_input();
        if (closing)
        {
            if (MessageBoxW(window, L"Machines are still stopping. Force them to exit? Unsaved guest data may be lost.",
                L"Close ES40 Workstation", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                for (const auto& item : machines) item->session.force_stop();
            return;
        }
        if (MessageBoxW(window, L"Stop all running machines and close ES40 Workstation?",
            L"Close ES40 Workstation", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        closing = true;
        for (const auto& item : machines) { item->restart = false; item->session.stop(); }
        refresh();
    }

    LRESULT event(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) release_input();
            layout(); return 0;
        case WM_ENTERSIZEMOVE: release_input(); return 0;
        case WM_ENTERMENULOOP: release_input(); return 0;
        case WM_INITMENUPOPUP: refresh(); return 0;
        case WM_SYSCOMMAND:
            if (((wparam & 0xfff0) == SC_SCREENSAVE || (wparam & 0xfff0) == SC_MONITORPOWER) && running()) return 0;
            break;
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE: update_colors(); break;
        case WM_CTLCOLORSTATIC:
            if ((HWND)lparam == heading || (HWND)lparam == subtitle || (HWND)lparam == empty)
            { SetBkColor((HDC)wparam, background_color); SetTextColor((HDC)wparam, text_color); return (LRESULT)background; }
            if ((HWND)lparam == details || (HWND)lparam == output)
            { SetBkColor((HDC)wparam, GetSysColor(COLOR_WINDOW)); SetTextColor((HDC)wparam, GetSysColor(COLOR_WINDOWTEXT)); return (LRESULT)GetSysColorBrush(COLOR_WINDOW); }
            break;
        case WM_ERASEBKGND:
        {
            RECT bounds = {}; GetClientRect(hwnd, &bounds);
            FillRect((HDC)wparam, &bounds, background ? background : GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }
        case WM_GETMINMAXINFO:
        {
            RECT bounds = { 0, 0, px(980), px(540) };
            auto& size = reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize;
            size = { px(1010), px(590) };
            if (AdjustWindowRectEx(&bounds, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), GetMenu(hwnd) != nullptr,
                (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE)))
                size = { max(size.x, bounds.right - bounds.left), max(size.y, bounds.bottom - bounds.top) };
            const RECT work = manager_work_area(hwnd);
            size = { min(size.x, work.right - work.left), min(size.y, work.bottom - work.top) };
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wparam))
            {
            case manager_new: new_machine(); break;
            case manager_copy: copy_configuration(); break;
            case manager_add: choose_configuration(); break;
            case manager_remove: remove_configuration(); break;
            case manager_settings: edit_settings(); break;
            case manager_start: start(); break;
            case manager_stop: stop(); break;
            case manager_restart: stop(true); break;
            case manager_force_stop: force_stop(); break;
            case manager_media: console_command(win32_manager_media); break;
            case manager_capture: console_command(win32_manager_capture); break;
            case manager_release: console_command(win32_manager_release); break;
            case manager_ctrl_alt_delete: console_command(win32_manager_ctrl_alt_delete); break;
            case manager_fullscreen: console_command(win32_manager_fullscreen); break;
            case manager_fit: console_command(win32_manager_reset_size); break;
            case manager_displays: console_command(win32_manager_show_displays); break;
            case manager_folder: open_machine_path(false); break;
            case manager_open_log: open_machine_path(true); break;
            case manager_about:
                release_input();
                MessageBoxW(window, L"ES40 Workstation\nNative Windows management for the AlphaServer ES40 emulator.\n\n"
                    L"Create or add a machine, edit its hardware in Settings, then select Start. "
                    L"Display #0 stays in Console; additional displays open separately.\n\n"
                    L"Use the Machine menu to capture or release input, and View for full screen. "
                    L"Console hotkeys follow the machine configuration.\n\n"
                    L"Shut down the guest OS before Stop. Stop ends emulation and flushes storage.",
                    L"About ES40 Workstation", MB_OK | MB_ICONINFORMATION);
                break;
            case manager_exit: close(); break;
            }
            return 0;
        case WM_NOTIFY:
        {
            const auto* notice = reinterpret_cast<NMHDR*>(lparam);
            if (notice->idFrom == manager_tabs && notice->code == TCN_SELCHANGE)
            { select_page(TabCtrl_GetCurSel(tabs)); return 0; }
            if (notice->idFrom != manager_library || changing_selection) break;
            if (notice->code == LVN_ITEMCHANGED)
            {
                const auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
                if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED))
                {
                    const int index = ListView_GetNextItem(library, -1, LVNI_SELECTED);
                    if (index != selection) select(index);
                }
            }
            else if (notice->code == NM_DBLCLK)
            {
                const int index = reinterpret_cast<NMITEMACTIVATE*>(lparam)->iItem;
                if (index >= 0 && index < (int)machines.size()) { select(index); select_page(0); start(); }
            }
            return 0;
        }
        case WM_TIMER:
        {
            bool changed = false;
            for (const auto& item : machines)
            {
                if (item->session.poll())
                {
                    item->completed = true;
                    item->width = item->height = 0;
                    const bool restart = item->restart;
                    item->restart = false;
                    if (item->session.exit_code())
                        item->failure = L"Emulator exited with code " + to_wstring(item->session.exit_code()) + L".";
                    if (restart && !closing)
                    {
                        try { start(*item); }
                        catch (const exception& error) { item->failure = manager_text(error.what()); }
                    }
                    changed = true;
                }
                if (item->settings.poll())
                {
                    if (item.get() == selected()) preview_dirty = true;
                    if (item->settings.exit_code())
                        item->failure = L"Settings editor exited with code " + to_wstring(item->settings.exit_code()) + L".";
                    changed = true;
                }
            }
            if (changed) refresh();
            update_output();
            if (closing && !running()) DestroyWindow(hwnd);
            return 0;
        }
        case WM_CLOSE: close(); return 0;
        case WM_QUERYENDSESSION:
            for (const auto& item : machines) if (item->settings.running()) return FALSE;
            for (const auto& item : machines) { item->restart = false; item->session.stop(); }
            return TRUE;
        case WM_DESTROY:
            KillTimer(hwnd, 1); window = nullptr; PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* self = reinterpret_cast<win32_manager_window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<win32_manager_window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        try { return self->event(hwnd, message, wparam, lparam); }
        catch (const exception& error) { self->report(error.what()); }
        catch (...) { self->report("An unexpected manager error occurred."); }
        return 0;
    }

    static LRESULT CALLBACK host_procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* item = reinterpret_cast<win32_manager_machine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            item = static_cast<win32_manager_machine*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            item->host = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)item);
        }
        if (!item) return DefWindowProcW(hwnd, message, wparam, lparam);
        auto* self = item->owner;
        try
        {
            switch (message)
            {
            case win32_manager_display_ready:
                if (item->session.attach_display((HWND)wparam, (DWORD)lparam))
                { self->fit_display(*item); self->refresh(); }
                return 0;
            case win32_manager_display_size:
                if (item->session.running() && item->session.display())
                { item->width = (unsigned)wparam; item->height = (unsigned)lparam; self->refresh_status(); }
                return 0;
            case WM_SIZE: self->fit_display(*item); return 0;
            case WM_SETFOCUS:
                if (item->session.display() && GetParent(item->session.display()) == hwnd)
                    SetFocus(item->session.display());
                return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT paint;
                HDC dc = BeginPaint(hwnd, &paint);
                RECT bounds = {}; GetClientRect(hwnd, &bounds);
                FillRect(dc, &bounds, (HBRUSH)GetStockObject(BLACK_BRUSH));
                SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(185, 195, 208));
                HGDIOBJ old = SelectObject(dc, self->font);
                const wchar_t* text = item->session.running() ? L"Starting machine...\nThe primary display will appear here." :
                    item->failure.empty() ? L"This machine is powered off.\nSelect Start to open its console." :
                    L"The machine could not continue.\nOpen Session log for details.";
                RECT text_bounds = bounds;
                DrawTextW(dc, text, -1, &text_bounds, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
                bounds.top = max(0L, (bounds.bottom - text_bounds.bottom) / 2);
                DrawTextW(dc, text, -1, &bounds, DT_CENTER | DT_WORDBREAK);
                SelectObject(dc, old); EndPaint(hwnd, &paint); return 0;
            }
            case WM_NCDESTROY: item->host = nullptr; break;
            }
        }
        catch (const exception& error) { self->report(error.what()); return 0; }
        catch (...) { self->report("An unexpected console error occurred."); return 0; }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

public:
    ~win32_manager_window()
    {
        if (IsWindow(window)) DestroyWindow(window);
        machines.clear();
        if (font) DeleteObject(font);
        if (heading_font) DeleteObject(heading_font);
        if (log_font) DeleteObject(log_font);
        if (background) DeleteObject(background);
    }

    int run(const char* initial_config)
    {
        DWORD console[2] = {};
        if (GetConsoleProcessList(console, 2) == 1 && console[0] == GetCurrentProcessId()) FreeConsole();
        vector<wchar_t> path(32768);
        DWORD length = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
        if (!length || length >= path.size()) throw runtime_error("Unable to locate the emulator executable.");
        executable = path.data();
        store = manager_store_directory();
        int argc = 0;
        struct arguments_guard
        {
            LPWSTR* values;
            ~arguments_guard() { if (values) LocalFree(values); }
        } arguments{CommandLineToArgvW(GetCommandLineW(), &argc)};
        if (!arguments.values || (!initial_config && (argc < 2 || argc > 3 || wcscmp(arguments.values[1], L"--manager"))))
            throw runtime_error("Usage: es40.exe --manager [configuration.cfg]");
        initialize_fonts(); update_colors();
        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES };
        if (!InitCommonControlsEx(&controls)) throw runtime_error("Unable to initialize the manager controls.");
        for (const auto& definition : { pair<const wchar_t*, WNDPROC>(manager_class, procedure),
            {win32_manager_host_class, host_procedure} })
        {
            WNDCLASSEXW type = { sizeof(type) };
            type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = definition.second;
            type.lpszClassName = definition.first;
            type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            type.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
            type.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                throw runtime_error("Unable to register the manager window.");
        }
        const RECT work = manager_work_area(nullptr);
        const int width = min((LONG)px(1240), work.right - work.left), height = min((LONG)px(820), work.bottom - work.top);
        window = CreateWindowExW(0, manager_class, L"ES40 Workstation", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
            width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!window) throw runtime_error("Unable to create the manager window.");
        heading = control(L"STATIC", L"ES40 Workstation", SS_LEFT);
        SendMessageW(heading, WM_SETFONT, (WPARAM)heading_font, TRUE);
        subtitle = control(L"STATIC", L"AlphaServer virtual machines", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX);
        const pair<int, const wchar_t*> actions[] = {
            {manager_new, L"&New..."}, {manager_add, L"&Add..."}, {manager_settings, L"S&ettings..."},
            {manager_start, L"&Start"}, {manager_stop, L"S&top"}, {manager_restart, L"Restart"},
            {manager_media, L"Media..."}, {manager_fullscreen, L"Full screen"}
        };
        for (const auto& action : actions)
            buttons.push_back(control(L"BUTTON", action.second, WS_TABSTOP | BS_PUSHBUTTON, action.first));
        library = control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS, manager_library, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(library, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = px(145);
        column.pszText = const_cast<LPWSTR>(L"Virtual machine");
        SendMessageW(library, LVM_INSERTCOLUMNW, 0, (LPARAM)&column);
        column.cx = px(95); column.pszText = const_cast<LPWSTR>(L"State");
        SendMessageW(library, LVM_INSERTCOLUMNW, 1, (LPARAM)&column);
        tabs = control(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, manager_tabs);
        for (const auto* title : { L"Console", L"Machine details", L"Session log" })
        {
            TCITEMW tab = {};
            tab.mask = TCIF_TEXT; tab.pszText = const_cast<LPWSTR>(title);
            if (SendMessageW(tabs, TCM_INSERTITEMW, TabCtrl_GetItemCount(tabs), (LPARAM)&tab) < 0)
                throw runtime_error("Unable to create the manager tabs.");
        }
        details = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
        output = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL);
        SendMessageW(details, EM_SETLIMITTEXT, 512 * 1024, 0);
        SendMessageW(output, EM_SETLIMITTEXT, 256 * 1024, 0);
        SendMessageW(output, WM_SETFONT, (WPARAM)log_font, TRUE);
        empty = control(L"STATIC", L"Welcome to ES40 Workstation\r\n\r\nCreate a machine or add an existing configuration.\r\n\r\n"
            L"Each machine has its own console, settings, and session log.\r\nDisplay #0 stays here; additional displays open separately.", SS_CENTER);
        status = control(STATUSCLASSNAMEW, L"Ready", SBARS_SIZEGRIP);
        create_menus();
        if (!SetTimer(window, 1, 250, nullptr)) throw runtime_error("Unable to initialize session updates.");
        layout(); refresh(); ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window);
        try
        {
            for (const auto& configuration : win32_library_load(store)) add_configuration(configuration, false);
            if (!machines.empty()) select(0);
            else if (!initial_config && argc == 2)
            {
                for (const auto& directory : { filesystem::current_path(), executable.parent_path() })
                {
                    const filesystem::path candidate = directory / L"es40.cfg";
                    if (!filesystem::is_regular_file(candidate) || filesystem::file_size(candidate) == 0) continue;
                    add_configuration(candidate);
                    break;
                }
            }
        }
        catch (const exception& error) { report(error.what()); }
        if (initial_config || argc == 3)
        {
            try { add_configuration(initial_config ? filesystem::path(initial_config) : filesystem::path(arguments.values[2])); }
            catch (const exception& error) { report(error.what()); }
        }
        MSG message = {};
        int result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
        {
            // Guest keystrokes bypass manager dialog navigation.
            DWORD focus_pid = 0; GetWindowThreadProcessId(GetFocus(), &focus_pid);
            if (focus_pid == GetCurrentProcessId() && message.message == WM_SYSCHAR &&
                (message.wParam == L'f' || message.wParam == L'F' ||
                 message.wParam == L'm' || message.wParam == L'M' ||
                 message.wParam == L'v' || message.wParam == L'V' ||
                 message.wParam == L'h' || message.wParam == L'H'))
            {
                SendMessageW(window, WM_SYSCOMMAND, SC_KEYMENU, message.wParam);
                continue;
            }
            if (focus_pid != GetCurrentProcessId() || !IsDialogMessageW(window, &message))
            { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (result < 0) throw runtime_error("Unable to read manager window messages.");
        return (int)message.wParam;
    }
};

int win32_manager_main(const char* initial_config)
{
    try { win32_manager_window manager; return manager.run(initial_config); }
    catch (const exception& error)
    {
        try { MessageBoxW(nullptr, manager_text(error.what()).c_str(), L"ES40 Workstation", MB_OK | MB_ICONERROR); }
        catch (...) { MessageBoxW(nullptr, L"Unable to start ES40 Workstation.", L"ES40 Workstation", MB_OK | MB_ICONERROR); }
    }
    catch (...) { MessageBoxW(nullptr, L"Unable to start ES40 Workstation.", L"ES40 Workstation", MB_OK | MB_ICONERROR); }
    return 1;
}
#endif
