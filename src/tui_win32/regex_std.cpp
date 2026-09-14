/* ES40 emulator.
 * Copyright (C) 2026 by the ES40 Emulator Project
 * All rights reserved.
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
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

// regcomp/regexec/regfree on std::regex for NetBSD libform on MSVC.
// REG_NEWLINE is ignored: std::regex::multiline is ECMAScript-only, and form fields hold one line.
#include <new>
#include <regex>

#include "regex.h"

int regcomp(regex_t *preg, const char *pattern, int cflags)
{
    std::regex::flag_type flags = (cflags & REG_EXTENDED) ? std::regex::extended : std::regex::basic;
    if (cflags & REG_ICASE)
        flags |= std::regex::icase;
    if (cflags & REG_NOSUB)
        flags |= std::regex::nosubs;

    preg->re_impl = nullptr;
    preg->re_nsub = 0;
    try
    {
        std::regex *re = new std::regex(pattern, flags);
        preg->re_impl = re;
        preg->re_nsub = re->mark_count();
        return 0;
    }
    catch (const std::regex_error &)
    {
        return REG_BADPAT;
    }
    catch (const std::bad_alloc &)
    {
        return REG_ESPACE;
    }
}

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags)
{
    const std::regex *re = static_cast<const std::regex *>(preg->re_impl);
    if (re == nullptr)
        return REG_BADPAT;

    std::regex_constants::match_flag_type mflags = std::regex_constants::match_default;
    if (eflags & REG_NOTBOL)
        mflags |= std::regex_constants::match_not_bol;
    if (eflags & REG_NOTEOL)
        mflags |= std::regex_constants::match_not_eol;

    try
    {
        std::cmatch m;
        if (!std::regex_search(string, m, *re, mflags))
            return REG_NOMATCH;

        for (size_t i = 0; i < nmatch; ++i)
        {
            if (i < m.size() && m[i].matched)
            {
                pmatch[i].rm_so = m.position(i);
                pmatch[i].rm_eo = m.position(i) + m.length(i);
            }
            else
                pmatch[i].rm_so = pmatch[i].rm_eo = -1;
        }
        return 0;
    }
    catch (const std::regex_error &)
    {
        return REG_ESPACE;
    }
}

void regfree(regex_t *preg)
{
    delete static_cast<std::regex *>(preg->re_impl);
    preg->re_impl = nullptr;
}
