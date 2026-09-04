/* ES40 Emulator.
 * Copyright (C) 2007-2025 by the ES40 Emulator Project
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might serve
 * the general public.
 *
 */

 /**
  * \file
  * Startup banner shared by the es40 and es40-cfg executables. Padding is
  * computed at runtime, so the border stays aligned no matter how long
  * VERSION (or any other line) gets.
  **/
#if !defined(__BANNER_H__)
#define __BANNER_H__

#include <stdio.h>
#include <string.h>

#define BANNER_INNER_WIDTH  70

static inline void banner_border(void)
{
	int i;
	printf("   **");
	for (i = 0; i < BANNER_INNER_WIDTH; i++)
		putchar('=');
	printf("**\n");
}

static inline void banner_line(const char* text, bool centered)
{
	int len = (int)strlen(text);
	if (len > BANNER_INNER_WIDTH - 4)
		len = BANNER_INNER_WIDTH - 4;  // truncate rather than break the border

	int lpad = centered ? (BANNER_INNER_WIDTH - len + 1) / 2 : 2;
	int rpad = BANNER_INNER_WIDTH - len - lpad;
	printf("   ||%*s%.*s%*s||\n", lpad, "", len, text, rpad, "");
}

static inline void print_es40_banner(const char* title)
{
	printf("\n\n");
	banner_border();
	banner_line(title, true);
	banner_line("Version " VERSION, true);
	banner_line("", false);
	banner_line("Copyright (C) 2007-2025 by the ES40 Emulator Project & Others", false);
	banner_line("Website: https://github.com/ES40-Emu/es40/", false);
	banner_line("", false);
	banner_line("", false);
	banner_line("This program is free software; you can redistribute it and/or", false);
	banner_line("modify it under the terms of the GNU General Public License", false);
	banner_line("as published by the Free Software Foundation; either version 2", false);
	banner_line("of the License, or (at your option) any later version.", false);
	banner_border();
	printf("\n\n");
}
#endif  //!defined(__BANNER_H__)
