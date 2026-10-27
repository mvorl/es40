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
#pragma once

#if defined(_WIN32)
#include "win32_manager.h"
#include <filesystem>
#include <memory>

class win32_configuration_lock
{
public:
    explicit win32_configuration_lock(const std::filesystem::path& configuration);
    ~win32_configuration_lock();
    win32_configuration_lock(const win32_configuration_lock&) = delete;
    win32_configuration_lock& operator=(const win32_configuration_lock&) = delete;
private:
    friend class win32_session;
    friend class win32_settings_session;
    HANDLE handle = nullptr;
};

// UI-thread owner; destruction terminates a live child.
class win32_session
{
public:
    win32_session();
    ~win32_session();
    win32_session(const win32_session&) = delete;
    win32_session& operator=(const win32_session&) = delete;

    void start(const std::filesystem::path& executable,
        const std::filesystem::path& configuration,
        const std::filesystem::path& log, HWND host);
    bool attach_display(HWND display, DWORD announced_pid);
    bool send(win32_manager_action action) const;
    void stop();
    void force_stop();
    bool poll(); // Returns true once per completed session.

    bool running() const;
    bool stopping() const;
    DWORD process_id() const;
    DWORD exit_code() const;
    HWND display() const;
private:
    struct state;
    std::unique_ptr<state> impl;
};

// Destruction leaves the editor process and its inherited lock alive.
class win32_settings_session
{
public:
    win32_settings_session();
    ~win32_settings_session();
    win32_settings_session(const win32_settings_session&) = delete;
    win32_settings_session& operator=(const win32_settings_session&) = delete;

    void start(const std::filesystem::path& executable,
        const std::filesystem::path& configuration);
    void activate();
    bool running() const;
    bool poll();
    DWORD exit_code() const;
private:
    struct state;
    std::unique_ptr<state> impl;
};
#endif
