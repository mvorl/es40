/* ES40 native machine sessions.
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
#include <cstdint>
#include <cwchar>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace std;

struct win32_session_handle
{
    HANDLE value = nullptr;
    explicit win32_session_handle(HANDLE handle = nullptr) : value(handle) {}
    win32_session_handle(const win32_session_handle&) = delete;
    win32_session_handle& operator=(const win32_session_handle&) = delete;
    ~win32_session_handle() { reset(); }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    HANDLE release() { HANDLE handle = value; value = nullptr; return handle; }
    void reset(HANDLE handle = nullptr)
    {
        if (*this) CloseHandle(value);
        value = handle;
    }
};

static void session_error(const char* message, DWORD error = GetLastError())
{
    throw std::system_error(static_cast<int>(error), std::system_category(), message);
}

static std::wstring session_quote(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value)
    {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

static void session_check_path(const std::wstring& path)
{
    if (GetACP() == CP_UTF8) return;
    BOOL substituted = FALSE;
    int count = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
        path.data(), (int)path.size(), nullptr, 0, nullptr, &substituted);
    std::string bytes(count, '\0');
    if (!count || !WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
        path.data(), (int)path.size(), bytes.data(), count, nullptr, &substituted) || substituted)
        throw std::runtime_error("The configuration path is not representable in the Windows code page.");
}

static std::wstring session_mutex_name(const filesystem::path& configuration)
{
    std::wstring canonical = filesystem::weakly_canonical(configuration).wstring();
    CharLowerBuffW(canonical.data(), (DWORD)canonical.size());
    uint64_t identity = 14695981039346656037ULL;
    for (wchar_t c : canonical) { identity ^= (uint16_t)c; identity *= 1099511628211ULL; }
    std::wostringstream name;
    name << L"Local\\ES40.Workstation.Config." << std::hex << identity;
    return name.str();
}

win32_configuration_lock::win32_configuration_lock(const filesystem::path& configuration)
{
    const std::wstring name = session_mutex_name(configuration);
    win32_session_handle candidate(CreateMutexW(nullptr, FALSE, name.c_str()));
    const DWORD error = GetLastError();
    if (!candidate) session_error("Unable to lock the machine configuration", error);
    if (error == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("This machine is already running or its settings are open.");
    handle = candidate.release();
}

win32_configuration_lock::~win32_configuration_lock()
{
    if (handle) CloseHandle(handle);
}

static std::vector<wchar_t> session_environment(HWND host)
{
    struct nocase
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const
        { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };
    std::map<std::wstring, std::wstring, nocase> values;
    struct environment_guard
    {
        LPWCH block = GetEnvironmentStringsW();
        ~environment_guard() { if (block) FreeEnvironmentStringsW(block); }
    } source;
    if (!source.block) session_error("Unable to read the process environment");
    for (const wchar_t* entry = source.block; *entry; entry += wcslen(entry) + 1)
    {
        const wchar_t* separator = wcschr(entry + (*entry == L'=' ? 1 : 0), L'=');
        if (separator) values[std::wstring(entry, separator)] = separator + 1;
    }
    values[L"ES40_MANAGER_HOST"] = std::to_wstring(reinterpret_cast<uintptr_t>(host));
    values[L"ES40_MANAGER_PID"] = std::to_wstring(GetCurrentProcessId());
    values[L"ES40_GUI"] = L"win32";
    values.erase(L"ES40_RESTORE"); // Restore requests belong to individual sessions.
    std::vector<wchar_t> result;
    for (const auto& entry : values)
    {
        const std::wstring line = entry.first + L"=" + entry.second;
        result.insert(result.end(), line.begin(), line.end());
        result.push_back(0);
    }
    result.push_back(0);
    return result;
}

struct win32_session::state
{
    std::unique_ptr<win32_configuration_lock> config_lock;
    win32_session_handle process, job;
    HWND host = nullptr, display = nullptr;
    DWORD pid = 0, exit_code = 0;
    bool stopping = false;

    ~state() { clear(); }
    void clear()
    {
        job.reset();
        process.reset();
        config_lock.reset();
        host = display = nullptr;
        pid = 0;
        stopping = false;
    }
};

win32_session::win32_session() : impl(std::make_unique<state>()) {}
win32_session::~win32_session() = default;
bool win32_session::running() const { return !!impl->process; }
bool win32_session::stopping() const { return impl->stopping; }
DWORD win32_session::process_id() const { return impl->pid; }
DWORD win32_session::exit_code() const { return impl->exit_code; }
HWND win32_session::display() const { return impl->display; }

void win32_session::start(const filesystem::path& executable, const filesystem::path& configuration,
    const filesystem::path& log, HWND host)
{
    if (running()) throw std::logic_error("The machine session is already active.");
    DWORD host_pid = 0;
    wchar_t host_class[64] = {};
    if (!GetWindowThreadProcessId(host, &host_pid) || host_pid != GetCurrentProcessId() ||
        !GetClassNameW(host, host_class, 64) || wcscmp(host_class, win32_manager_host_class))
        throw std::runtime_error("The machine console host is invalid.");
    const filesystem::path program = filesystem::absolute(executable).lexically_normal();
    const filesystem::path config = filesystem::absolute(configuration).lexically_normal();
    const filesystem::path output = filesystem::absolute(log).lexically_normal();
    const filesystem::path directory = config.parent_path();
    if (!filesystem::is_regular_file(config)) throw std::runtime_error("The configuration file is missing.");
    session_check_path(config.wstring());
    auto config_lock = std::make_unique<win32_configuration_lock>(config);
    SECURITY_ATTRIBUTES security = { sizeof(security), nullptr, TRUE };
    win32_session_handle log_file(CreateFileW(output.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!log_file) session_error("Unable to create the session log");
    win32_session_handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, 0, nullptr));
    if (!input) session_error("Unable to create the child input stream");
    // Keep the configuration locked through manager failure.
    win32_session_handle child_lock;
    if (!DuplicateHandle(GetCurrentProcess(), config_lock->handle, GetCurrentProcess(),
        &child_lock.value, 0, TRUE, DUPLICATE_SAME_ACCESS))
        session_error("Unable to inherit the configuration lock");
    win32_session_handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        session_error("Unable to create the machine process job");
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log_file.value;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
        session_error("Unable to initialize child process attributes");
    struct attribute_guard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~attribute_guard() { DeleteProcThreadAttributeList(list); }
    } attribute_cleanup{startup.lpAttributeList};
    HANDLE handles[] = { input.value, log_file.value, child_lock.value };
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof(handles), nullptr, nullptr))
        session_error("Unable to isolate child process handles");
    std::wstring command = session_quote(program.wstring()) + L" --run " + session_quote(config.wstring());
    std::vector<wchar_t> environment = session_environment(host);
    struct child_guard
    {
        PROCESS_INFORMATION info = {};
        ~child_guard()
        {
            if (info.hProcess)
            {
                TerminateProcess(info.hProcess, 1);
                CloseHandle(info.hProcess);
            }
            if (info.hThread) CloseHandle(info.hThread);
        }
    } child;
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        environment.data(), directory.c_str(), &startup.StartupInfo, &child.info))
        session_error("Unable to start the virtual machine");
    if (!AssignProcessToJobObject(job.value, child.info.hProcess))
        session_error("Unable to manage the virtual machine process");
    if (ResumeThread(child.info.hThread) == (DWORD)-1)
        session_error("Unable to resume the virtual machine");
    impl->config_lock = std::move(config_lock);
    impl->job.reset(job.release());
    impl->process.reset(child.info.hProcess);
    child.info.hProcess = nullptr;
    impl->pid = child.info.dwProcessId;
    impl->host = host;
    impl->display = nullptr;
    impl->exit_code = 0;
    impl->stopping = false;
}

bool win32_session::attach_display(HWND display, DWORD announced_pid)
{
    DWORD pid = 0;
    if (!running() || announced_pid != impl->pid || GetParent(display) != impl->host ||
        !GetWindowThreadProcessId(display, &pid) || pid != impl->pid ||
        WaitForSingleObject(impl->process.value, 0) != WAIT_TIMEOUT) return false;
    impl->display = display;
    if (impl->stopping) stop();
    return true;
}

bool win32_session::send(win32_manager_action action) const
{
    DWORD pid = 0;
    if (!running() || !impl->display || WaitForSingleObject(impl->process.value, 0) != WAIT_TIMEOUT ||
        !GetWindowThreadProcessId(impl->display, &pid) || pid != impl->pid) return false;
    return PostMessageW(impl->display, win32_manager_command, action, 0) != 0;
}

void win32_session::stop()
{
    if (!running()) return;
    impl->stopping = true;
    send(win32_manager_release);
    send(win32_manager_shutdown);
}

void win32_session::force_stop()
{
    if (!running()) return;
    if (!TerminateJobObject(impl->job.value, 1)) session_error("Unable to stop the virtual machine");
    impl->stopping = true;
}

bool win32_session::poll()
{
    if (!running()) return false;
    const DWORD status = WaitForSingleObject(impl->process.value, 0);
    if (status == WAIT_FAILED) session_error("Unable to check the virtual machine");
    if (status == WAIT_TIMEOUT)
    {
        if (impl->stopping) send(win32_manager_shutdown);
        return false;
    }
    if (!GetExitCodeProcess(impl->process.value, &impl->exit_code))
        session_error("Unable to read the virtual machine exit code");
    impl->clear();
    return true;
}

struct win32_settings_session::state
{
    unique_ptr<win32_configuration_lock> config_lock;
    win32_session_handle process;
    DWORD pid = 0, exit_code = 0;
};

win32_settings_session::win32_settings_session() : impl(make_unique<state>()) {}
win32_settings_session::~win32_settings_session() = default;
bool win32_settings_session::running() const { return !!impl->process; }
DWORD win32_settings_session::exit_code() const { return impl->exit_code; }

void win32_settings_session::start(const filesystem::path& executable,
    const filesystem::path& configuration)
{
    if (running()) { activate(); return; }
    const filesystem::path program = filesystem::absolute(executable).lexically_normal();
    const filesystem::path config = filesystem::absolute(configuration).lexically_normal();
    const filesystem::path directory = config.parent_path();
    if (!filesystem::is_regular_file(program)) throw runtime_error("The native configuration editor is missing.");
    if (!filesystem::is_regular_file(config)) throw runtime_error("The configuration file is missing.");
    session_check_path(config.wstring());
    auto config_lock = make_unique<win32_configuration_lock>(config);
    win32_session_handle child_lock;
    if (!DuplicateHandle(GetCurrentProcess(), config_lock->handle, GetCurrentProcess(),
        &child_lock.value, 0, TRUE, DUPLICATE_SAME_ACCESS))
        session_error("Unable to inherit the settings configuration lock");
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    vector<unsigned char> attributes(bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
        session_error("Unable to initialize settings process attributes");
    struct attribute_guard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~attribute_guard() { DeleteProcThreadAttributeList(list); }
    } attribute_cleanup{startup.lpAttributeList};
    HANDLE handles[] = { child_lock.value };
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof(handles), nullptr, nullptr))
        session_error("Unable to isolate settings process handles");
    const wstring config_argument = session_quote(config.wstring());
    wstring command = session_quote(program.wstring()) + L" " + config_argument + L" " + config_argument;
    PROCESS_INFORMATION information = {};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(),
        &startup.StartupInfo, &information))
        session_error("Unable to open machine settings");
    win32_session_handle child(information.hProcess), thread(information.hThread);
    impl->config_lock = move(config_lock);
    impl->process.reset(child.release());
    impl->pid = information.dwProcessId;
    impl->exit_code = 0;
}

void win32_settings_session::activate()
{
    if (!running() || WaitForSingleObject(impl->process.value, 0) != WAIT_TIMEOUT) return;
    struct window_search { DWORD pid; HWND window; } found{impl->pid, nullptr};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL
    {
        auto& search = *reinterpret_cast<window_search*>(parameter);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != search.pid || !IsWindowVisible(window)) return TRUE;
        search.window = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    if (!found.window) return;
    HWND popup = GetLastActivePopup(found.window);
    DWORD popup_pid = 0;
    if (popup && IsWindowVisible(popup) && GetWindowThreadProcessId(popup, &popup_pid) && popup_pid == impl->pid)
        found.window = popup;
    ShowWindow(found.window, IsIconic(found.window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(found.window);
}

bool win32_settings_session::poll()
{
    if (!running()) return false;
    const DWORD status = WaitForSingleObject(impl->process.value, 0);
    if (status == WAIT_FAILED) session_error("Unable to check the settings editor");
    if (status == WAIT_TIMEOUT) return false;
    if (!GetExitCodeProcess(impl->process.value, &impl->exit_code))
        session_error("Unable to read the settings editor exit code");
    impl->process.reset();
    impl->config_lock.reset();
    impl->pid = 0;
    return true;
}
#endif
