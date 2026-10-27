/* ES40 emulator.
 * Copyright (C) 2026 by gdwnldsKSC
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef ES40_HOST_AUDIO_BACKEND_H
#define ES40_HOST_AUDIO_BACKEND_H
#include "HostAudio.h"

namespace host_audio
{
namespace detail
{
struct Driver
{
    bool (*initialize)();
    bool (*set_sample_frames_hint)(const char*);
    std::uintptr_t (*open_input_device)();
    std::uintptr_t (*open_output_device)();
    bool (*output_device_format)(std::uintptr_t, int*, int*);
    void (*close_device)(std::uintptr_t);
    const char* (*error)();
    void (*create_stream)(Stream&, const Format*, const Format*);
    void (*destroy_stream)(Stream&);
    bool (*set_input_callback)(Stream&, Callback, void*);
    bool (*set_output_callback)(Stream&, Callback, void*);
    bool (*set_format)(Stream&, const Format*, const Format*);
    bool (*bind_stream)(std::uintptr_t, Stream&);
    void (*unbind_stream)(Stream&);
    bool (*lock_stream)(Stream&);
    bool (*unlock_stream)(Stream&);
    bool (*clear_stream)(Stream&);
    int (*available)(Stream&);
    int (*queued)(Stream&);
    int (*read)(Stream&, void*, int);
    bool (*write)(Stream&, const void*, int);
};

#ifdef HAVE_SDL
const Driver& sdl_driver();
#endif
#ifdef _WIN32
const Driver& dsound_driver();
#endif
const Driver& null_driver();
}
}
#endif
