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
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std;

static constexpr wchar_t manager_class[] = L"ES40.Workstation";
enum win32_manager_control { manager_add = 100, manager_start, manager_stop, manager_remove, manager_library };

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
    wstring failure;

    ~win32_manager_machine() { if (IsWindow(host)) DestroyWindow(host); }

    const wchar_t* state() const
    {
        if (session.stopping()) return L"Stopping";
        if (session.running()) return session.display() ? L"Running" : L"Starting";
        return failure.empty() ? L"Powered off" : L"Error";
    }
};

class win32_manager_window
{
    HWND window = nullptr, library = nullptr, library_label = nullptr, empty = nullptr;
    HWND add_button = nullptr, remove_button = nullptr, start_button = nullptr;
    HWND stop_button = nullptr, config_text = nullptr, status = nullptr, output = nullptr, output_label = nullptr;
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
        MoveWindow(add_button, 12, 12, 80, 28, TRUE);
        MoveWindow(remove_button, 102, 12, 90, 28, TRUE);
        MoveWindow(library_label, 12, 49, 260, 22, TRUE);
        MoveWindow(library, 12, 78, 260, height - 90, TRUE);
        MoveWindow(config_text, left, 12, content_width - 180, 28, TRUE);
        MoveWindow(start_button, width - 180, 12, 78, 28, TRUE);
        MoveWindow(stop_button, width - 90, 12, 78, 28, TRUE);
        MoveWindow(status, left, 49, content_width, 22, TRUE);
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
        EnableWindow(add_button, !closing);
        EnableWindow(remove_button, item && !item->session.running() && !closing);
        EnableWindow(start_button, item && !item->session.running() && !closing);
        EnableWindow(stop_button, item && item->session.running() && !item->session.stopping());
        const wchar_t* state = closing ? L"Stopping before close..." :
            item ? item->state() : L"Add an existing configuration.";
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
        if (!item || item->session.running() || closing) return;
        release_input();
        if (MessageBoxW(window, L"Remove this machine from the library? Its files will remain on disk.",
            L"Remove machine", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        win32_library_update(store, item->configuration, false);
        machines.erase(machines.begin() + selection);
        selection = machines.empty() ? -1 : min(selection, (int)machines.size() - 1);
        rebuild_library(); select(selection);
    }

    void choose_configuration()
    {
        if (closing) return;
        release_input();
        vector<wchar_t> path(32768, 0);
        OPENFILENAMEW dialog = { sizeof(dialog) };
        dialog.hwndOwner = window;
        dialog.lpstrTitle = L"Add machine configuration";
        dialog.lpstrFilter = L"ES40 configuration (*.cfg)\0*.cfg\0All files\0*.*\0";
        dialog.lpstrFile = path.data(); dialog.nMaxFile = (DWORD)path.size();
        dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&dialog)) add_configuration(path.data());
        else if (CommDlgExtendedError()) throw runtime_error("Unable to open the file chooser.");
    }

    void start()
    {
        auto* item = selected();
        if (!item || item->session.running() || closing) return;
        item->failure.clear(); item->log.clear();
        try
        {
            filesystem::create_directories(store / L"logs");
            item->log = store / L"logs" / (L"session-" + to_wstring(GetCurrentProcessId()) + L"-" +
                to_wstring(GetTickCount64()) + L"-" + to_wstring(++sequence) + L".log");
            item->session.start(executable, item->configuration, item->log, item->host);
        }
        catch (const exception& error)
        {
            item->failure = manager_text(error.what());
            refresh(); update_output();
            throw;
        }
        refresh(); update_output();
    }

    void close()
    {
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
        for (const auto& item : machines) item->session.stop();
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
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = { 920, 490 }; return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam))
            {
            case manager_add: choose_configuration(); break;
            case manager_remove: remove_configuration(); break;
            case manager_start: start(); break;
            case manager_stop:
                if (auto* item = selected()) item->session.stop();
                refresh(); break;
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
                if (!item->session.poll()) continue;
                if (item->session.exit_code())
                    item->failure = L"Emulator exited with code " + to_wstring(item->session.exit_code()) + L".";
                changed = true;
            }
            if (changed) refresh();
            update_output();
            if (closing && !running()) DestroyWindow(hwnd);
            return 0;
        }
        case WM_CLOSE: close(); return 0;
        case WM_QUERYENDSESSION:
            for (const auto& item : machines) item->session.stop();
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
        add_button = control(L"BUTTON", L"&Add...", WS_TABSTOP | BS_PUSHBUTTON, manager_add);
        remove_button = control(L"BUTTON", L"&Remove", WS_TABSTOP | BS_PUSHBUTTON, manager_remove);
        library_label = control(L"STATIC", L"Machines", SS_LEFT);
        library = control(WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS, manager_library);
        ListView_SetExtendedListViewStyle(library, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = 150;
        column.pszText = const_cast<LPWSTR>(L"Machine");
        SendMessageW(library, LVM_INSERTCOLUMNW, 0, (LPARAM)&column);
        column.cx = 100; column.pszText = const_cast<LPWSTR>(L"State");
        SendMessageW(library, LVM_INSERTCOLUMNW, 1, (LPARAM)&column);
        config_text = control(L"EDIT", L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL);
        start_button = control(L"BUTTON", L"&Start", WS_TABSTOP | BS_PUSHBUTTON, manager_start);
        stop_button = control(L"BUTTON", L"S&top", WS_TABSTOP | BS_PUSHBUTTON, manager_stop);
        status = control(L"STATIC", L"", SS_LEFT);
        output_label = control(L"STATIC", L"Session output", SS_LEFT);
        output = control(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
        empty = control(L"STATIC", L"Add an existing configuration to the machine library.", SS_CENTER);
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
