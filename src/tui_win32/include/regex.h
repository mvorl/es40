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

/* POSIX regex subset for NetBSD libform's TYPE_REGEXP, implemented on std::regex
 * (regex_std.cpp) because MSVC has no <regex.h>. */
#ifndef ES40_TUI_REGEX_H
#define ES40_TUI_REGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    void *re_impl;
    size_t re_nsub;
} regex_t;

typedef ptrdiff_t regoff_t;

typedef struct
{
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

/* regcomp() cflags */
#define REG_EXTENDED 0x0001
#define REG_ICASE 0x0002
#define REG_NOSUB 0x0004
#define REG_NEWLINE 0x0008

/* regexec() eflags */
#define REG_NOTBOL 0x0001
#define REG_NOTEOL 0x0002

/* error codes */
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ESPACE 12

int regcomp(regex_t *preg, const char *pattern, int cflags);
int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags);
void regfree(regex_t *preg);

#ifdef __cplusplus
}
#endif

#endif
