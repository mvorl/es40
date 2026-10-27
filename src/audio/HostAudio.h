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
#ifndef ES40_HOST_AUDIO_H
#define ES40_HOST_AUDIO_H

#include <cstdint>
#include <initializer_list>
#include <vector>

// Devices retain scheduling, error policy, lifetime and lock scope.
namespace host_audio
{
enum class Backend { sdl, dsound };
struct DeviceId
{
    std::uintptr_t native;
    Backend backend;
    explicit operator bool() const { return native != 0; }
};
enum class SampleFormat { u8, s16_le };
struct Format
{
    int freq;
    int channels;
    SampleFormat format;
};

using Callback = void (*)(void* userdata);

// Backend-only storage; keep its address stable until destruction completes.
struct Stream
{
    void* native;
    Callback callback;
    void* userdata;
    Backend backend;
    explicit operator bool() const { return native != nullptr; }
};

// Release partial native setup while retaining legacy SDL failure behavior.
class SetupGuard
{
public:
    SetupGuard(Backend backend, std::initializer_list<DeviceId*> devices,
        std::initializer_list<Stream*> streams);
    ~SetupGuard();
    SetupGuard(const SetupGuard&) = delete;
    SetupGuard& operator=(const SetupGuard&) = delete;
    void complete();
private:
    Backend backend_;
    bool active_;
    std::vector<DeviceId*> devices_;
    std::vector<Stream*> streams_;
};

Backend backend_for_gui(const char* gui);
bool initialize(Backend backend = Backend::sdl);
bool set_sample_frames_hint(const char* frames, Backend backend = Backend::sdl);
DeviceId open_input_device(Backend backend = Backend::sdl);
DeviceId open_output_device(Backend backend = Backend::sdl);
bool output_device_format(DeviceId device, int* frequency, int* sample_frames);
void close_device(DeviceId device);
const char* error(Backend backend = Backend::sdl);
[[noreturn]] void fail(Backend backend, const char* context, const char* file,
    int line, const char* function);

// Null formats preserve the host side chosen when the stream is bound.
void create_stream(Stream& stream, const Format* source, const Format* target,
    Backend backend = Backend::sdl);
void destroy_stream(Stream& stream);
bool set_input_callback(Stream& stream, Callback callback, void* userdata);
bool set_output_callback(Stream& stream, Callback callback, void* userdata);
bool set_format(Stream& stream, const Format* source, const Format* target);
bool bind_stream(DeviceId device, Stream& stream);
void unbind_stream(Stream& stream);
bool lock_stream(Stream& stream);
bool unlock_stream(Stream& stream);
bool clear_stream(Stream& stream);

// Available bytes use the target format; queued bytes use the source format.
int available(Stream& stream);
int queued(Stream& stream);
int read(Stream& stream, void* data, int bytes);
bool write(Stream& stream, const void* data, int bytes);
}

#ifdef HAVE___FUNCTION__
#define HOST_AUDIO_FAILURE(backend, context) \
    host_audio::fail(backend, context, __FILE__, __LINE__, __FUNCTION__)
#else
#define HOST_AUDIO_FAILURE(backend, context) \
    host_audio::fail(backend, context, __FILE__, __LINE__, nullptr)
#endif

#endif
