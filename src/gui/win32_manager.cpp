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

using namespace std;

static constexpr wchar_t manager_class[] = L"ES40.Workstation";
enum win32_manager_control
{
    manager_add = 100, manager_start, manager_stop, manager_remove, manager_library,
    manager_settings, manager_new, manager_copy, manager_restart, manager_force_stop,
    manager_media, manager_capture, manager_release, manager_ctrl_alt_delete,
    manager_fullscreen, manager_fit, manager_displays, manager_exit
};
static constexpr wchar_t manager_config_filter[] = L"ES40 configuration (*.cfg)\0*.cfg\0All files\0*.*\0";
static constexpr wchar_t manager_rom_filter[] = L"Firmware images (*.exe;*.rom;*.bin)\0*.exe;*.rom;*.bin\0All files\0*.*\0";

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

class win32_manager_window;

struct win32_manager_machine
{
    win32_manager_window* owner = nullptr;
    filesystem::path configuration, log;
    HWND host = nullptr;
    win32_session session;
    win32_settings_session settings;
    wstring failure;
    bool restart = false;

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
    HWND window = nullptr, library = nullptr, library_label = nullptr, empty = nullptr;
    HWND new_button = nullptr, add_button = nullptr, remove_button = nullptr, settings_button = nullptr, start_button = nullptr;
    HWND copy_button = nullptr, stop_button = nullptr, config_text = nullptr, status = nullptr, output = nullptr, output_label = nullptr;
    HWND restart_button = nullptr, force_stop_button = nullptr;
    filesystem::path executable, store;
    vector<unique_ptr<win32_manager_machine>> machines;
    wstring last_output;
    int selection = -1;
    unsigned sequence = 0;
    bool closing = false, changing_selection = false;

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
        const HMENU file = popup(L"&File"), machine = popup(L"&Machine"), view = popup(L"&View");
        entry(file, manager_new, L"&New machine...");
        entry(file, manager_add, L"&Add configuration...");
        entry(file, manager_copy, L"&Copy configuration...");
        entry(file, manager_remove, L"&Remove from library");
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
        if (!SetMenu(window, menu.value)) throw runtime_error("Unable to attach the manager menu.");
        menu.value = nullptr;
    }

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id = 0)
    {
        HWND result = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        if (!result) throw runtime_error("Unable to create a manager control.");
        SendMessageW(result, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
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
        if (!library || !empty) return;
        RECT bounds = {};
        GetClientRect(window, &bounds);
        const int width = max(880L, bounds.right), height = max(440L, bounds.bottom);
        const int left = 284, content_width = width - left - 12;
        MoveWindow(new_button, 12, 12, 125, 28, TRUE);
        MoveWindow(add_button, 147, 12, 125, 28, TRUE);
        MoveWindow(remove_button, 12, 48, 125, 28, TRUE);
        MoveWindow(settings_button, 147, 48, 125, 28, TRUE);
        MoveWindow(copy_button, 12, 84, 260, 28, TRUE);
        MoveWindow(library_label, 12, 121, 260, 22, TRUE);
        MoveWindow(library, 12, 150, 260, height - 162, TRUE);
        MoveWindow(config_text, left, 12, content_width - 212, 28, TRUE);
        MoveWindow(start_button, width - 212, 12, 94, 28, TRUE);
        MoveWindow(stop_button, width - 106, 12, 94, 28, TRUE);
        MoveWindow(restart_button, width - 212, 46, 94, 28, TRUE);
        MoveWindow(force_stop_button, width - 106, 46, 94, 28, TRUE);
        MoveWindow(status, left, 49, content_width - 212, 22, TRUE);
        MoveWindow(empty, left, 96, content_width, 60, TRUE);
        ShowWindow(empty, selected() ? SW_HIDE : SW_SHOWNA);
        for (const auto& item : machines)
        {
            MoveWindow(item->host, left, 78, content_width, height - 246, TRUE);
            ShowWindow(item->host, item.get() == selected() ? SW_SHOWNA : SW_HIDE);
            fit_display(*item);
        }
        MoveWindow(output_label, left, height - 158, content_width, 20, TRUE);
        MoveWindow(output, left, height - 134, content_width, 122, TRUE);
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
        const bool ready = console_ready();
        for (int id : {manager_media, manager_capture, manager_release, manager_ctrl_alt_delete,
            manager_fullscreen, manager_fit, manager_displays}) enabled(id, ready);
        DrawMenuBar(window);
        const wchar_t* state = closing ? L"Stopping before close..." :
            item ? item->state() : L"Create a machine or add an existing configuration.";
        SetWindowTextW(status, state);
        SetWindowTextW(config_text, item ? item->configuration.c_str() : L"");
        for (size_t i = 0; i < machines.size(); ++i)
        {
            LVITEMW row = {};
            row.iItem = (int)i; row.iSubItem = 1;
            row.pszText = const_cast<LPWSTR>(machines[i]->state());
            SendMessageW(library, LVM_SETITEMTEXTW, i, (LPARAM)&row);
            InvalidateRect(machines[i]->host, nullptr, TRUE);
        }
    }

    void update_output()
    {
        wstring text;
        if (const auto* item = selected())
        {
            text = item->failure;
            if (!text.empty()) text += L"\r\n";
            text += manager_read_log(item->log);
        }
        if (text == last_output) return;
        last_output = text;
        SetWindowTextW(output, text.c_str());
        SendMessageW(output, EM_SETSEL, (WPARAM)text.size(), (LPARAM)text.size());
        SendMessageW(output, EM_SCROLLCARET, 0, 0);
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
        case WM_GETMINMAXINFO:
        {
            RECT bounds = { 0, 0, 880, 440 };
            auto& size = reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize;
            size = { 920, 490 };
            if (AdjustWindowRectEx(&bounds, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), GetMenu(hwnd) != nullptr,
                (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE)))
                size = { max(size.x, bounds.right - bounds.left), max(size.y, bounds.bottom - bounds.top) };
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
            case manager_exit: close(); break;
            }
            return 0;
        case WM_NOTIFY:
        {
            const auto* notice = reinterpret_cast<NMHDR*>(lparam);
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
                if (index >= 0 && index < (int)machines.size()) { select(index); start(); }
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
            case win32_manager_display_size: return 0;
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
                SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(190, 190, 190));
                HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
                const wchar_t* text = item->session.running() ? L"Starting machine..." : L"Machine is powered off";
                DrawTextW(dc, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
    ~win32_manager_window() { if (IsWindow(window)) DestroyWindow(window); }

    int run()
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
        if (!arguments.values || argc < 2 || argc > 3)
            throw runtime_error("Usage: es40.exe --manager [configuration.cfg]");
        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES };
        if (!InitCommonControlsEx(&controls)) throw runtime_error("Unable to initialize the machine list.");
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
        window = CreateWindowExW(0, manager_class, L"ES40 Workstation", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1160, 760, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!window) throw runtime_error("Unable to create the manager window.");
        new_button = control(L"BUTTON", L"&New...", WS_TABSTOP | BS_PUSHBUTTON, manager_new);
        add_button = control(L"BUTTON", L"&Add...", WS_TABSTOP | BS_PUSHBUTTON, manager_add);
        remove_button = control(L"BUTTON", L"&Remove", WS_TABSTOP | BS_PUSHBUTTON, manager_remove);
        settings_button = control(L"BUTTON", L"S&ettings...", WS_TABSTOP | BS_PUSHBUTTON, manager_settings);
        copy_button = control(L"BUTTON", L"&Copy configuration...", WS_TABSTOP | BS_PUSHBUTTON, manager_copy);
        library_label = control(L"STATIC", L"Machines", SS_LEFT);
        library = control(WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS, manager_library);
        ListView_SetExtendedListViewStyle(library, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = 130;
        column.pszText = const_cast<LPWSTR>(L"Machine");
        SendMessageW(library, LVM_INSERTCOLUMNW, 0, (LPARAM)&column);
        column.cx = 120; column.pszText = const_cast<LPWSTR>(L"State");
        SendMessageW(library, LVM_INSERTCOLUMNW, 1, (LPARAM)&column);
        config_text = control(L"EDIT", L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL);
        start_button = control(L"BUTTON", L"&Start", WS_TABSTOP | BS_PUSHBUTTON, manager_start);
        stop_button = control(L"BUTTON", L"S&top", WS_TABSTOP | BS_PUSHBUTTON, manager_stop);
        restart_button = control(L"BUTTON", L"Restart", WS_TABSTOP | BS_PUSHBUTTON, manager_restart);
        force_stop_button = control(L"BUTTON", L"Force Stop", WS_TABSTOP | BS_PUSHBUTTON, manager_force_stop);
        status = control(L"STATIC", L"", SS_LEFT);
        output_label = control(L"STATIC", L"Session output", SS_LEFT);
        output = control(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
        empty = control(L"STATIC", L"Create a new machine or add an existing configuration to the library.", SS_CENTER);
        create_menus();
        if (!SetTimer(window, 1, 250, nullptr)) throw runtime_error("Unable to initialize session updates.");
        layout(); refresh(); ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window);
        try
        {
            for (const auto& configuration : win32_library_load(store)) add_configuration(configuration, false);
            if (!machines.empty()) select(0);
        }
        catch (const exception& error) { report(error.what()); }
        if (argc == 3)
        {
            try { add_configuration(arguments.values[2]); }
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
                 message.wParam == L'v' || message.wParam == L'V'))
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

int win32_manager_main()
{
    try { win32_manager_window manager; return manager.run(); }
    catch (const exception& error)
    {
        try { MessageBoxW(nullptr, manager_text(error.what()).c_str(), L"ES40 Workstation", MB_OK | MB_ICONERROR); }
        catch (...) { MessageBoxW(nullptr, L"Unable to start ES40 Workstation.", L"ES40 Workstation", MB_OK | MB_ICONERROR); }
    }
    catch (...) { MessageBoxW(nullptr, L"Unable to start ES40 Workstation.", L"ES40 Workstation", MB_OK | MB_ICONERROR); }
    return 1;
}
#endif
