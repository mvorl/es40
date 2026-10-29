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
#pragma once

#if defined(_WIN32)
// The Windows 8.1 SDK uses IUnknown before declaring it under C++20.
struct IUnknown;
#include <windows.h>

// Accept only HWNDs from managed children.
constexpr UINT win32_manager_display_ready = WM_APP + 0x240;
constexpr UINT win32_manager_command = WM_APP + 0x241;
constexpr UINT win32_manager_display_size = WM_APP + 0x242;
constexpr wchar_t win32_manager_host_class[] = L"ES40.ManagerHost";

enum win32_manager_action : WPARAM
{
    win32_manager_shutdown = 1,
    win32_manager_capture = 2,
    win32_manager_release = 3,
    win32_manager_ctrl_alt_delete = 4,
    win32_manager_media = 5,
    win32_manager_fullscreen = 6,
    win32_manager_reset_size = 7,
    win32_manager_show_displays = 8
};

int win32_manager_main(const char* initial_config = nullptr);

#endif
