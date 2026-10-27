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
#include "HostAudioBackend.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>

namespace host_audio
{
namespace detail
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::uintptr_t input_device = 1, output_device = 2;
constexpr std::uint64_t queue_limit = 8 * 1024 * 1024;
constexpr std::uint64_t second_ns = 1000000000;
thread_local const char* last_error = "";

bool fail(const char* message)
{
    last_error = message;
    return false;
}

bool valid_format(const Format& format)
{
    return format.freq > 0 && format.freq <= 768000 &&
        (format.channels == 1 || format.channels == 2) &&
        (format.format == SampleFormat::u8 || format.format == SampleFormat::s16_le);
}

struct NullStream
{
    std::recursive_mutex mutex;
    Format guest;
    bool capture;
    bool bound = false;
    std::uint64_t pending = 0;
    std::uint64_t fraction = 0;
    Clock::time_point last = Clock::now();

    NullStream(const Format& format, bool recording) : guest(format), capture(recording) {}
    unsigned frame_bytes() const
    {
        return guest.channels * (guest.format == SampleFormat::u8 ? 1 : 2);
    }
    void reset()
    {
        pending = fraction = 0;
        last = Clock::now();
    }
    void advance(Clock::time_point now = Clock::now())
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count();
        last = now;
        if (!bound || elapsed <= 0) return;
        const auto ns = static_cast<std::uint64_t>(elapsed);
        const auto scaled = fraction + (ns % second_ns) * guest.freq;
        const auto frames = (ns / second_ns) * guest.freq + scaled / second_ns;
        fraction = scaled % second_ns;
        const unsigned frame = frame_bytes();
        if (capture) pending = (std::min)(pending + frames * frame, queue_limit);
        else pending -= (std::min)(frames, pending / frame) * frame;
    }
};

NullStream* native(Stream& stream)
{
    auto* result = static_cast<NullStream*>(stream.native);
    if (!result) fail("Invalid null audio stream");
    return result;
}

bool initialize() { return true; }
bool set_sample_frames_hint(const char*) { return true; }
std::uintptr_t open_input_device() { return input_device; }
std::uintptr_t open_output_device() { return output_device; }
void close_device(std::uintptr_t) {}
const char* error() { return last_error; }

bool output_device_format(std::uintptr_t device, int* frequency, int* frames)
{
    if (device != output_device || !frequency || !frames)
        return fail("Invalid null playback device");
    *frequency = 48000;
    *frames = 512;
    return true;
}

void create_stream(Stream& stream, const Format* source, const Format* target)
{
    stream.native = nullptr;
    stream.callback = nullptr;
    stream.userdata = nullptr;
    const Format* guest = source ? source : target;
    if (!guest || !valid_format(*guest)) { fail("Invalid null audio format"); return; }
    try {
        stream.native = new NullStream(*guest, source == nullptr);
    } catch (...) {
        fail("Null audio stream allocation failed");
    }
}

void destroy_stream(Stream& stream)
{
    delete static_cast<NullStream*>(stream.native);
    stream.native = nullptr;
}

bool set_callback(Stream& stream, Callback callback, void* userdata)
{
    auto* instance = native(stream);
    if (!instance) return false;
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    stream.callback = callback;
    stream.userdata = userdata;
    return true;
}

bool set_format(Stream& stream, const Format* source, const Format* target)
{
    auto* instance = native(stream);
    if (!instance) return false;
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    const Format* guest = instance->capture ? target : source;
    if (!guest) return true;
    if (!valid_format(*guest)) return fail("Invalid null audio format");
    if (guest->freq == instance->guest.freq && guest->channels == instance->guest.channels &&
        guest->format == instance->guest.format) return true;
    instance->guest = *guest;
    instance->reset();
    return true;
}

bool bind_stream(std::uintptr_t device, Stream& stream)
{
    auto* instance = native(stream);
    if (!instance) return false;
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    if (device != (instance->capture ? input_device : output_device))
        return fail("Wrong null audio device direction");
    if (!instance->bound) {
        instance->bound = true;
        instance->fraction = 0;
        instance->last = Clock::now();
    }
    return true;
}

void unbind_stream(Stream& stream)
{
    auto* instance = native(stream);
    if (!instance) return;
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    instance->advance();
    instance->bound = false;
}

bool lock_stream(Stream& stream)
{
    auto* instance = native(stream);
    if (!instance) return false;
    instance->mutex.lock();
    return true;
}

bool unlock_stream(Stream& stream)
{
    auto* instance = native(stream);
    if (!instance) return false;
    instance->mutex.unlock();
    return true;
}

bool clear_stream(Stream& stream)
{
    auto* instance = native(stream);
    if (!instance) return false;
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    instance->reset();
    return true;
}

int available(Stream& stream)
{
    auto* instance = native(stream);
    if (!instance) return -1;
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    instance->advance();
    return instance->capture ? static_cast<int>(instance->pending) : 0;
}

int queued(Stream& stream)
{
    auto* instance = native(stream);
    if (!instance) return -1;
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    instance->advance();
    return instance->capture ? 0 : static_cast<int>(instance->pending);
}

int read(Stream& stream, void* data, int bytes)
{
    auto* instance = native(stream);
    if (!instance) return -1;
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    if (!instance->capture || bytes < 0 || (!data && bytes)) {
        fail("Invalid null capture read");
        return -1;
    }
    instance->advance();
    const auto count = (std::min)(static_cast<std::uint64_t>(bytes), instance->pending);
    if (count) std::memset(data, instance->guest.format == SampleFormat::u8 ? 0x80 : 0,
        static_cast<size_t>(count));
    instance->pending -= count;
    return static_cast<int>(count);
}

bool write(Stream& stream, const void* data, int bytes)
{
    auto* instance = native(stream);
    if (!instance) return false;
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    if (instance->capture || bytes < 0 || (!data && bytes))
        return fail("Invalid null playback write");
    instance->advance();
    if (static_cast<std::uint64_t>(bytes) > queue_limit - instance->pending)
        return fail("Null playback queue is full");
    // Keep only timing credit; sample data never reaches a host device.
    instance->pending += bytes;
    return true;
}
}

const Driver& null_driver()
{
    static const Driver driver = {initialize, set_sample_frames_hint,
        open_input_device, open_output_device, output_device_format, close_device, error,
        create_stream, destroy_stream, set_callback, set_callback, set_format, bind_stream,
        unbind_stream, lock_stream, unlock_stream, clear_stream, available, queued, read, write};
    return driver;
}
}
}
