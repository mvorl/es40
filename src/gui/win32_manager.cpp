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
enum win32_manager_control { manager_open = 100, manager_start, manager_stop };

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

static filesystem::path manager_log_directory()
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)))
        throw runtime_error("Unable to locate the local application data folder.");
    unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(raw, CoTaskMemFree);
    filesystem::path directory = filesystem::path(folder.get()) / L"ES40" / L"Workstation" / L"logs";
    filesystem::create_directories(directory);
    return directory;
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

class win32_manager_window
{
    HWND window = nullptr, host = nullptr, open_button = nullptr, start_button = nullptr;
    HWND stop_button = nullptr, config_text = nullptr, status = nullptr, output = nullptr, output_label = nullptr;
    filesystem::path executable, configuration, log;
    win32_session session;
    wstring failure, last_output;
    unsigned sequence = 0;
    bool closing = false;

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id = 0)
    {
        HWND result = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        if (!result) throw runtime_error("Unable to create a manager control.");
        SendMessageW(result, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return result;
    }

    void fit_display()
    {
        HWND display = session.display();
        if (!display || GetParent(display) != host) return;
        RECT bounds = {};
        if (GetClientRect(host, &bounds))
            SetWindowPos(display, nullptr, 0, 0, bounds.right, bounds.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void layout()
    {
        if (!host) return;
        RECT bounds = {};
        GetClientRect(window, &bounds);
        const int width = max(640L, bounds.right), height = max(440L, bounds.bottom);
        MoveWindow(open_button, 12, 12, 80, 28, TRUE);
        MoveWindow(config_text, 104, 12, width - 296, 28, TRUE);
        MoveWindow(start_button, width - 180, 12, 78, 28, TRUE);
        MoveWindow(stop_button, width - 90, 12, 78, 28, TRUE);
        MoveWindow(status, 12, 49, width - 24, 22, TRUE);
        MoveWindow(host, 12, 78, width - 24, height - 246, TRUE);
        MoveWindow(output_label, 12, height - 158, width - 24, 20, TRUE);
        MoveWindow(output, 12, height - 134, width - 24, 122, TRUE);
        fit_display();
    }

    void refresh()
    {
        if (!window) return;
        EnableWindow(open_button, !session.running() && !closing);
        EnableWindow(start_button, !configuration.empty() && !session.running() && !closing);
        EnableWindow(stop_button, session.running() && !session.stopping());
        const wchar_t* state = closing ? L"Stopping before close..." :
            session.stopping() ? L"Stopping..." : session.running() ?
            (session.display() ? L"Running" : L"Starting...") : !failure.empty() ? L"Error" :
            configuration.empty() ? L"Choose an existing configuration." : L"Powered off";
        SetWindowTextW(status, state);
        InvalidateRect(host, nullptr, TRUE);
    }

    void update_output()
    {
        wstring text = failure;
        if (!text.empty()) text += L"\r\n";
        text += manager_read_log(log);
        if (text == last_output) return;
        last_output = text;
        SetWindowTextW(output, text.c_str());
        SendMessageW(output, EM_SETSEL, (WPARAM)text.size(), (LPARAM)text.size());
        SendMessageW(output, EM_SCROLLCARET, 0, 0);
    }

    void report(const char* message) noexcept
    {
        session.send(win32_manager_release);
        try { failure = manager_text(message); refresh(); update_output(); } catch (...) {}
        MessageBoxW(window, failure.empty() ? L"An unexpected manager error occurred." : failure.c_str(),
            L"ES40 Workstation", MB_OK | MB_ICONERROR);
    }

    void select_configuration(const filesystem::path& path)
    {
        if (session.running() || closing) return;
        filesystem::path selected = filesystem::absolute(path).lexically_normal();
        if (!filesystem::is_regular_file(selected)) throw runtime_error("The configuration file is missing.");
        configuration = selected;
        failure.clear(); log.clear();
        SetWindowTextW(config_text, configuration.c_str());
        refresh(); update_output();
    }

    void open_configuration()
    {
        if (session.running() || closing) return;
        vector<wchar_t> path(32768, 0);
        OPENFILENAMEW dialog = { sizeof(dialog) };
        dialog.hwndOwner = window;
        dialog.lpstrTitle = L"Open machine configuration";
        dialog.lpstrFilter = L"ES40 configuration (*.cfg)\0*.cfg\0All files\0*.*\0";
        dialog.lpstrFile = path.data(); dialog.nMaxFile = (DWORD)path.size();
        dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&dialog)) select_configuration(path.data());
        else if (CommDlgExtendedError()) throw runtime_error("Unable to open the file chooser.");
    }

    void start()
    {
        if (configuration.empty() || session.running() || closing) return;
        failure.clear();
        log = manager_log_directory() / (L"session-" + to_wstring(GetCurrentProcessId()) + L"-" +
            to_wstring(GetTickCount64()) + L"-" + to_wstring(++sequence) + L".log");
        session.start(executable, configuration, log, host);
        refresh(); update_output();
    }

    void close()
    {
        if (!session.running()) { DestroyWindow(window); return; }
        session.send(win32_manager_release);
        if (closing)
        {
            if (MessageBoxW(window, L"The machine is still stopping. Force it to exit? Unsaved guest data may be lost.",
                L"Close ES40 Workstation", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                session.force_stop();
            return;
        }
        if (MessageBoxW(window, L"Stop the running machine and close ES40 Workstation?",
            L"Close ES40 Workstation", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        closing = true;
        session.stop();
        refresh();
    }

    LRESULT event(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) session.send(win32_manager_release);
            layout(); return 0;
        case WM_ENTERSIZEMOVE: session.send(win32_manager_release); return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = { 680, 490 }; return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam))
            {
            case manager_open: open_configuration(); break;
            case manager_start: start(); break;
            case manager_stop: session.stop(); refresh(); break;
            }
            return 0;
        case WM_TIMER:
            if (session.poll())
            {
                if (session.exit_code()) failure = L"Emulator exited with code " + to_wstring(session.exit_code()) + L".";
                refresh();
            }
            update_output();
            if (closing && !session.running()) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE: close(); return 0;
        case WM_QUERYENDSESSION: session.stop(); return TRUE;
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
        auto* self = reinterpret_cast<win32_manager_window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<win32_manager_window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->host = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        try
        {
            switch (message)
            {
            case win32_manager_display_ready:
                if (self->session.attach_display((HWND)wparam, (DWORD)lparam))
                { self->fit_display(); self->refresh(); }
                return 0;
            case win32_manager_display_size: return 0;
            case WM_SIZE: self->fit_display(); return 0;
            case WM_SETFOCUS:
                if (self->session.display() && GetParent(self->session.display()) == hwnd)
                    SetFocus(self->session.display());
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
                const wchar_t* text = self->session.running() ? L"Starting machine..." : L"Machine is powered off";
                DrawTextW(dc, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, old); EndPaint(hwnd, &paint); return 0;
            }
            case WM_NCDESTROY: self->host = nullptr; break;
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
        int argc = 0;
        struct arguments_guard
        {
            LPWSTR* values;
            ~arguments_guard() { if (values) LocalFree(values); }
        } arguments{CommandLineToArgvW(GetCommandLineW(), &argc)};
        if (!arguments.values || argc < 2 || argc > 3)
            throw runtime_error("Usage: es40.exe --manager [configuration.cfg]");
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
            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 760, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!window) throw runtime_error("Unable to create the manager window.");
        open_button = control(L"BUTTON", L"&Open...", WS_TABSTOP | BS_PUSHBUTTON, manager_open);
        config_text = control(L"EDIT", L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL);
        start_button = control(L"BUTTON", L"&Start", WS_TABSTOP | BS_PUSHBUTTON, manager_start);
        stop_button = control(L"BUTTON", L"S&top", WS_TABSTOP | BS_PUSHBUTTON, manager_stop);
        status = control(L"STATIC", L"", SS_LEFT);
        output_label = control(L"STATIC", L"Session output", SS_LEFT);
        output = control(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
        host = CreateWindowExW(0, win32_manager_host_class, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, window, nullptr, GetModuleHandleW(nullptr), this);
        if (!host || !SetTimer(window, 1, 250, nullptr)) throw runtime_error("Unable to initialize the console host.");
        layout(); refresh(); ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window);
        if (argc == 3)
        {
            try { select_configuration(arguments.values[2]); }
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
