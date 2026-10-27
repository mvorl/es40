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
#ifdef HAVE_SDL
#include "HostAudio.h"
#include <SDL3/SDL.h>

namespace host_audio
{
namespace
{
SDL_AudioStream* native(Stream& stream)
{
    return static_cast<SDL_AudioStream*>(stream.native);
}

SDL_AudioSpec native_format(const Format& format)
{
    SDL_AudioSpec spec;
    spec.freq = format.freq;
    spec.channels = format.channels;
    spec.format = format.format == SampleFormat::u8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
    return spec;
}

void SDLCALL stream_callback(void* userdata, SDL_AudioStream*, int, int)
{
    auto& stream = *static_cast<Stream*>(userdata);
    // Forward on SDL's thread under its existing stream lock.
    stream.callback(stream.userdata);
}
}

bool initialize()
{
    return SDL_Init(SDL_INIT_AUDIO);
}

bool set_sample_frames_hint(const char* frames)
{
    return SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
}

DeviceId open_input_device()
{
    return SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, nullptr);
}

DeviceId open_output_device()
{
    return SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
}

bool output_device_format(DeviceId device, int* frequency, int* sample_frames)
{
    SDL_AudioSpec spec;
    const bool result = SDL_GetAudioDeviceFormat(device, &spec, sample_frames);
    if (result) *frequency = spec.freq;
    return result;
}

void close_device(DeviceId device)
{
    SDL_CloseAudioDevice(device);
}

const char* error()
{
    return SDL_GetError();
}

void create_stream(Stream& stream, const Format* source, const Format* target)
{
    SDL_AudioSpec src, dst;
    if (source) src = native_format(*source);
    if (target) dst = native_format(*target);
    stream.native = SDL_CreateAudioStream(source ? &src : nullptr, target ? &dst : nullptr);
}

void destroy_stream(Stream& stream)
{
    SDL_DestroyAudioStream(native(stream));
}

bool set_input_callback(Stream& stream, Callback callback, void* userdata)
{
    stream.callback = callback;
    stream.userdata = userdata;
    return SDL_SetAudioStreamPutCallback(native(stream), stream_callback, &stream);
}

bool set_output_callback(Stream& stream, Callback callback, void* userdata)
{
    stream.callback = callback;
    stream.userdata = userdata;
    return SDL_SetAudioStreamGetCallback(native(stream), stream_callback, &stream);
}

bool set_format(Stream& stream, const Format* source, const Format* target)
{
    SDL_AudioSpec src, dst;
    if (source) src = native_format(*source);
    if (target) dst = native_format(*target);
    return SDL_SetAudioStreamFormat(native(stream), source ? &src : nullptr, target ? &dst : nullptr);
}

bool bind_stream(DeviceId device, Stream& stream)
{
    return SDL_BindAudioStream(device, native(stream));
}

void unbind_stream(Stream& stream)
{
    SDL_UnbindAudioStream(native(stream));
}

bool lock_stream(Stream& stream)
{
    return SDL_LockAudioStream(native(stream));
}

bool unlock_stream(Stream& stream)
{
    return SDL_UnlockAudioStream(native(stream));
}

bool clear_stream(Stream& stream)
{
    return SDL_ClearAudioStream(native(stream));
}

int available(Stream& stream)
{
    return SDL_GetAudioStreamAvailable(native(stream));
}

int queued(Stream& stream)
{
    return SDL_GetAudioStreamQueued(native(stream));
}

int read(Stream& stream, void* data, int bytes)
{
    return SDL_GetAudioStreamData(native(stream), data, bytes);
}

bool write(Stream& stream, const void* data, int bytes)
{
    return SDL_PutAudioStreamData(native(stream), data, bytes);
}
}
#endif // HAVE_SDL
