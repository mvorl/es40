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

/* asprintf/vasprintf/strsep for NetBSD libform on MSVC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsd_compat.h"

int vasprintf(char **strp, const char *fmt, va_list ap)
{
    va_list aq;
    int len;

    *strp = NULL;
    va_copy(aq, ap);
    len = _vscprintf(fmt, aq);
    va_end(aq);
    if (len < 0)
        return -1;

    *strp = malloc((size_t)len + 1);
    if (*strp == NULL)
        return -1;
    return vsnprintf(*strp, (size_t)len + 1, fmt, ap);
}

int asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vasprintf(strp, fmt, ap);
    va_end(ap);
    return len;
}

char *strsep(char **stringp, const char *delim)
{
    char *start = *stringp;
    char *end;

    if (start == NULL)
        return NULL;

    end = start + strcspn(start, delim);
    if (*end != '\0')
    {
        *end = '\0';
        *stringp = end + 1;
    }
    else
        *stringp = NULL;
    return start;
}
