/* ES40 emulator.
 * Copyright (C) 2026 by gdwnldsKSC
 * SPDX-License-Identifier: BSD-1-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
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

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Shared control contains no disk pointer, so a selector remains safe during
// emulator teardown. The emulation thread observes the flag at I/O boundaries.
class CDiskFaultControl
{
public:
    CDiskFaultControl(const std::string& label, bool initial_offline)
        : device_label(label), offline(initial_offline), active(true) {}

    const std::string& label() const { return device_label; }
    bool is_offline() const noexcept { return offline.load(std::memory_order_acquire); }
    bool is_active() const noexcept { return active.load(std::memory_order_acquire); }
    bool set_offline(bool value) noexcept
    {
        if (!is_active()) return false;
        offline.store(value, std::memory_order_release);
        return true;
    }
    void deactivate() noexcept { active.store(false, std::memory_order_release); }

private:
    const std::string device_label;
    std::atomic<bool> offline;
    std::atomic<bool> active;
};

struct SDiskFaultRegistry
{
    std::mutex mutex;
    std::vector<std::weak_ptr<CDiskFaultControl>> controls;
};

inline SDiskFaultRegistry& get_disk_fault_registry()
{
    static SDiskFaultRegistry instance;
    return instance;
}

inline void register_disk_fault_control(const std::shared_ptr<CDiskFaultControl>& control)
{
    auto& registry = get_disk_fault_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.controls.push_back(control);
}

inline std::vector<std::shared_ptr<CDiskFaultControl>> disk_fault_controls()
{
    auto& registry = get_disk_fault_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::vector<std::shared_ptr<CDiskFaultControl>> result;
    for (auto it = registry.controls.begin(); it != registry.controls.end();)
    {
        auto control = it->lock();
        if (control && control->is_active())
        {
            result.push_back(std::move(control));
            ++it;
        }
        else
            it = registry.controls.erase(it);
    }
    return result;
}
