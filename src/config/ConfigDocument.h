/* ES40 emulator -- Source-preserving configuration documents.
 * Copyright (C) 2026 by the ES40 Emulator Project
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

#include <filesystem>
#include <memory>
#include <string>

class CConfigurator;

// Keeps source spans independently of CConfigurator's normalized device model.
class ConfigDocument
{
public:
    explicit ConfigDocument(std::string source);
    ~ConfigDocument();
    CConfigurator *configuration();
    void bind(CConfigurator *configuration);
    std::string render(CConfigurator *configuration) const;
    const std::string& source() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// A snapshot also serves as an optimistic concurrency check at save time.
struct ConfigFileSnapshot
{
    std::filesystem::path path;
    bool exists = false;
    std::string text;
};
ConfigFileSnapshot read_config_file(const std::filesystem::path&, bool allow_missing = false);
void save_config_file(const ConfigFileSnapshot&, const std::string& text);
