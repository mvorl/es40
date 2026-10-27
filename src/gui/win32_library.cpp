/* ES40 native machine library.
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
#include <windows.h>
#include "win32_library.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

using namespace std;

struct win32_library_handle
{
    HANDLE value;
    explicit win32_library_handle(HANDLE handle) : value(handle) {}
    win32_library_handle(const win32_library_handle&) = delete;
    win32_library_handle& operator=(const win32_library_handle&) = delete;
    ~win32_library_handle()
    { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

static void library_error(const char* message, DWORD error = GetLastError())
{ throw system_error(static_cast<int>(error), system_category(), message); }

static filesystem::path library_absolute(const filesystem::path& path)
{
    if (path.empty()) throw runtime_error("The machine configuration path is empty.");
    const wstring text = path.wstring();
    if (any_of(text.begin(), text.end(), [](wchar_t c) { return c < 32; }))
        throw runtime_error("The machine configuration path contains a control character.");
    return filesystem::absolute(path).lexically_normal();
}

static wstring library_identity(const filesystem::path& path)
{
    const filesystem::path absolute = library_absolute(path);
    error_code error;
    const filesystem::path canonical = filesystem::weakly_canonical(absolute, error);
    return (error ? absolute : canonical).wstring();
}

bool win32_library_same_configuration(const filesystem::path& first, const filesystem::path& second)
{
    const wstring a = library_identity(first), b = library_identity(second);
    if (CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
    error_code error;
    return filesystem::equivalent(first, second, error) && !error;
}

static wstring library_mutex_name(const filesystem::path& store)
{
    wstring canonical = filesystem::weakly_canonical(store).wstring();
    CharLowerBuffW(canonical.data(), (DWORD)canonical.size());
    uint64_t identity = 14695981039346656037ULL;
    for (wchar_t c : canonical) { identity ^= (uint16_t)c; identity *= 1099511628211ULL; }
    wostringstream name;
    name << L"Local\\ES40.Workstation.Library." << hex << identity;
    return name.str();
}

vector<filesystem::path> win32_library_load(const filesystem::path& store)
{
    const filesystem::path path = library_absolute(store) / L"machines.txt";
    win32_library_handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        library_error("Unable to read the machine library", error);
    }
    string bytes;
    char buffer[8192];
    for (;;)
    {
        DWORD count = 0;
        if (!ReadFile(file.value, buffer, sizeof(buffer), &count, nullptr))
            library_error("Unable to read the machine library");
        if (!count) break;
        if (bytes.size() > static_cast<size_t>(INT_MAX) - count)
            throw runtime_error("The machine library is too large.");
        bytes.append(buffer, count);
    }
    if (bytes.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), (int)bytes.size(), nullptr, 0);
    if (!count) throw runtime_error("The machine library is not valid UTF-8.");
    wstring text(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), (int)bytes.size(), text.data(), count))
        library_error("Unable to decode the machine library");
    vector<filesystem::path> entries;
    size_t begin = text.front() == 0xfeff ? 1 : 0;
    while (begin < text.size())
    {
        size_t end = text.find(L'\n', begin);
        if (end == wstring::npos) end = text.size();
        size_t length = end - begin;
        if (length && text[begin + length - 1] == L'\r') --length;
        if (length)
        {
            const filesystem::path entry(text.substr(begin, length));
            if (!entry.is_absolute()) throw runtime_error("The machine library contains a relative configuration path.");
            const filesystem::path absolute = library_absolute(entry);
            if (none_of(entries.begin(), entries.end(), [&](const auto& existing)
                { return win32_library_same_configuration(existing, absolute); }))
                entries.push_back(absolute);
        }
        begin = end == text.size() ? end : end + 1;
    }
    return entries;
}

void win32_library_update(const filesystem::path& store, const filesystem::path& configuration, bool add)
{
    const filesystem::path directory = library_absolute(store);
    const filesystem::path target = library_absolute(configuration);
    filesystem::create_directories(directory);
    const wstring name = library_mutex_name(directory);
    win32_library_handle mutex(CreateMutexW(nullptr, FALSE, name.c_str()));
    if (!mutex.value) library_error("Unable to lock the machine library");
    const DWORD waited = WaitForSingleObject(mutex.value, 5000);
    if (waited == WAIT_TIMEOUT) throw runtime_error("Another manager is updating the machine library. Try again shortly.");
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED)
        library_error("Unable to lock the machine library");
    struct mutex_guard
    {
        HANDLE handle;
        ~mutex_guard() { ReleaseMutex(handle); }
    } lock{mutex.value};

    vector<filesystem::path> entries = win32_library_load(directory);
    const auto matches = [&](const auto& entry) { return win32_library_same_configuration(entry, target); };
    if (!add) entries.erase(remove_if(entries.begin(), entries.end(), matches), entries.end());
    else if (none_of(entries.begin(), entries.end(), matches)) entries.push_back(target);
    wstring text;
    for (const auto& entry : entries) { text += entry.wstring(); text += L'\n'; }
    if (text.size() > INT_MAX) throw runtime_error("The machine library is too large.");
    string bytes;
    if (!text.empty())
    {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        if (!count) throw runtime_error("The machine library contains an invalid Unicode path.");
        bytes.resize(count);
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text.data(), (int)text.size(), bytes.data(), count, nullptr, nullptr))
            library_error("Unable to encode the machine library");
    }
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
    const filesystem::path library = directory / L"machines.txt";
    const wstring stem = L"machines-" + to_wstring(GetCurrentProcessId()) + L"-" + to_wstring(GetTickCount64());
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        temporary.path = directory / (stem + L"-" + to_wstring(attempt) + L".tmp");
        temporary.handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary.handle != INVALID_HANDLE_VALUE) { temporary.created = true; break; }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            library_error("Unable to create a temporary machine library", error);
    }
    if (!temporary.created) throw runtime_error("Unable to choose a temporary machine library filename.");
    size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD written = 0;
        if (!WriteFile(temporary.handle, bytes.data() + offset, (DWORD)(bytes.size() - offset), &written, nullptr))
            library_error("Unable to write the machine library");
        if (!written) library_error("Unable to write the machine library", ERROR_WRITE_FAULT);
        offset += written;
    }
    if (!FlushFileBuffers(temporary.handle)) library_error("Unable to flush the machine library");
    if (!CloseHandle(temporary.handle)) library_error("Unable to close the machine library");
    temporary.handle = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary.path.c_str(), library.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        library_error("Unable to replace the machine library");
    temporary.created = false;
}
#endif
