/* ES40 emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 *
 * WWW    : http://es40.org
 * E-mail : camiel@camicom.com
 *
 *  This file is based upon Bochs.
 *
 *  Copyright (C) 2002  MandrakeSoft S.A.
 *
 *    MandrakeSoft S.A.
 *    43, rue d'Aboukir
 *    75002 Paris - France
 *    http://www.linux-mandrake.com/
 *    http://www.mandrakesoft.com/
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

 /**
  * \file
  * Contains the code for the bx_sdl_gui_c class used for interfacing with
  * SDL.
  *
  * $Id$
  *
  * X-1.18       Martin Borgman                                  10-APR-2008
  *	    Handle SDL support on OS X through OS_X/SDLMain.m.
  *
  * X-1.15       Camiel Vanderhoeven                             29-FEB-2008
  *      Comments
  *
  * X-1.14       Camiel Vanderhoeven                             12-FEB-2008
  *      Moved keyboard code into it's own class (CKeyboard)
  *
  * X-1.13       Camiel Vanderhoeven                             22-JAN-2008
  *      Minor cleanups.
  *
  * X-1.12       Fang Zhe                                        05-JAN-2008
  *      Last patch was applied incompletely.
  *
  * X-1.11       Fang Zhe                                        04-JAN-2008
  *      Improved compatibility with Apple OS X; keyboard works now.
  *
  * X-1.10       Fang Zhe                                        03-JAN-2008
  *      Compatibility with Apple OS X.
  *
  * X-1.9        Camiel Vanderhoeven                             02-JAN-2008
  *      Comments.
  *
  * X-1.4        Camiel Vanderhoeven                             10-DEC-2007
  *      Use Configurator.
  *
  * X-1.3        Camiel Vanderhoeven                             7-DEC-2007
  *      Made keyboard messages conditional.
  *
  * X-1.2        Camiel Vanderhoeven                             7-DEC-2007
  *      Code cleanup.
  *
  * X-1.1        Camiel Vanderhoeven                             6-DEC-2007
  *      Initial version for ES40 emulator.
  *
  **/
#include "../StdAfx.h"

#if defined(HAVE_SDL)
#include "gui.h"
#include "keymap.h"
#include "sdl_media.h"
#include "../VGA.h"
#include "../System.h"

  //#include "../AliM1543C.h"
#include "../Keyboard.h"
#include "../Configurator.h"

#define _MULTI_THREAD

// Define BX_PLUGGABLE in files that can be compiled into plugins.  For
// platforms that require a special tag on exported symbols, BX_PLUGGABLE
// is used to know when we are exporting symbols and when we are importing.
#define BX_PLUGGABLE

#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <cctype>
#include <exception>
#include <string>
#include <SDL3/SDL.h>

#include "sdl_fonts.h"

enum
{
	SDL_HOTKEY_MOD_CTRL = 1u << 0,
	SDL_HOTKEY_MOD_ALT = 1u << 1,
	SDL_HOTKEY_MOD_SHIFT = 1u << 2,
	SDL_HOTKEY_MOD_GUI = 1u << 3,
	// AltGr/Mode and Level5 are deliberately not configurable today, but
	// they must participate in exact matching so they cannot accidentally
	// satisfy a Ctrl+Alt binding.
	SDL_HOTKEY_MOD_UNSUPPORTED = 1u << 31
};

struct sdl_hotkey_binding
{
	const char* config_name = NULL;
	SDL_Keycode key = SDLK_UNKNOWN;
	unsigned modifiers = 0;
	bool enabled = false;
	std::string display;

	bool matches(const SDL_KeyboardEvent& event) const;
};

static unsigned sdl_hotkey_modifiers(SDL_Keymod modifiers);
static sdl_hotkey_binding parse_sdl_hotkey(CConfigurator* cfg,
	const char* config_name, const char* default_value);

/**
 * \brief GUI implementation using SDL3.
 **/
class bx_sdl_gui_c : public bx_gui_c
{
public:
	bx_sdl_gui_c(CConfigurator* cfg);
	virtual void    specific_init(unsigned x_tilesize, unsigned y_tilesize) override;
	virtual void    text_update(u8* old_text, u8* new_text, unsigned long cursor_x, unsigned long cursor_y, bx_vga_tminfo_t tm_info, unsigned rows) override {}
	virtual void    graphics_tile_update(u8* snapshot, unsigned x, unsigned y) override;
	virtual void    handle_events(void) override;
	virtual void    flush(void) override;
	virtual void    clear_screen(void) override;
	virtual bool    palette_change(unsigned index, unsigned red, unsigned green, unsigned blue) override;
	virtual void    dimension_update(unsigned x, unsigned y, unsigned fheight = 0, unsigned fwidth = 0, unsigned bpp = 8) override;
	virtual void    mouse_enabled_changed_specific(bool val) override;
	virtual void    exit(void) override;
	virtual			bx_svga_tileinfo_t* graphics_tile_info(bx_svga_tileinfo_t* info) override;
	virtual			u8* graphics_tile_get(unsigned x, unsigned y, unsigned* w, unsigned* h) override;
	virtual void    graphics_tile_update_in_place(unsigned x, unsigned y, unsigned w, unsigned h) override;
	void			graphics_frame_update(const u32* pixels, unsigned w, unsigned h) override;
	virtual bool    requires_main_thread() override;
	virtual void    main_thread_init() override;
	virtual void    main_thread_pump() override;
	virtual void    main_thread_stop() override;
private:
	CConfigurator* myCfg;
	unsigned int   vid_scale = 0;
	bool           vid_linear = true;
	bool           vid_scale_change_enable = false;
	double         mouse_speed = 1.0;
	bool           mouse_invert_x = false;
	bool           mouse_invert_y = false;
	sdl_hotkey_binding hotkey_mouse_capture;
	sdl_hotkey_binding hotkey_media;
	sdl_hotkey_binding hotkey_ctrl_alt_delete;
	sdl_hotkey_binding hotkey_reset_window;
	sdl_hotkey_binding hotkey_scale_up;
	sdl_hotkey_binding hotkey_scale_down;
	std::string    window_title;
	std::string    window_title_grabbed;
	u32            guest_key_by_scancode[SDL_SCANCODE_COUNT] = {};
	bool           guest_key_pressed[SDL_SCANCODE_COUNT] = {};
	bool           swallowed_hotkey_releases[SDL_SCANCODE_COUNT] = {};
	// Bodies of the public entry points above; always executed on the thread
	// that owns the SDL window (see on_main_thread).
	void           specific_init_impl(unsigned x_tilesize, unsigned y_tilesize);
	void           handle_events_impl();
	void           clear_screen_impl();
	void           dimension_update_impl(unsigned x, unsigned y, unsigned fheight,
		unsigned fwidth, unsigned bpp);
	void           graphics_frame_update_impl(const u32* pixels, unsigned w, unsigned h);
	void           mouse_enabled_changed_specific_impl(bool val);
	void           exit_impl();
	void           reset_window_size();
	void           adjust_window_scale(int delta);
	void           load_hotkeys();
	void           build_window_titles();
	void           suppress_hotkey_releases(const SDL_KeyboardEvent& event,
		const sdl_hotkey_binding& binding, bool release_guest_modifiers);
	void           release_guest_key(SDL_Scancode scancode);
	void           release_all_guest_keys();
	void           send_guest_ctrl_alt_delete();
	void           clear_hotkey_release_state();
	void           reconcile_hotkey_release_state();
};

// declare one instance of the gui object and call macro to insert the
// plugin code
static bx_sdl_gui_c* theGui = NULL;
IMPLEMENT_GUI_PLUGIN_CODE(sdl)
static unsigned     prev_cursor_x = 0;
static unsigned     prev_cursor_y = 0;
static u32          convertStringToSDLKey(const char* string);

static SDL_Window*   sdl_window = NULL;
static SDL_Renderer* sdl_renderer = NULL;
static SDL_Texture*  sdl_texture = NULL;

/// Set once main() has taken ownership of SDL and is driving main_thread_pump()
/// on our behalf, so on_main_thread() must bounce SDL calls across to it.
static std::atomic<bool> sdl_main_thread_owns_sdl(false);

/// How long main_thread_pump() blocks waiting for work before looping. Only
/// caps how quickly it notices main_thread_stop(); callbacks wake it at once.
static const int sdl_pump_idle_ms = 10;

/**
 * Run fn on the thread that called main().
 *
 * SDL wants the video subsystem, the window and the event pump all touched
 * from that one thread, and macOS enforces it. The emulator drives the GUI
 * from the VGA card's thread instead, so every SDL call made from there is
 * bounced across with SDL_RunOnMainThread(). Exceptions (FAILURE_*) must not
 * escape into SDL's C event loop, so they are caught and rethrown on the
 * calling thread. On platforms that keep the old threading model this is a
 * plain inline call.
 **/
template <typename F>
static void on_main_thread(F fn)
{
	if (!sdl_main_thread_owns_sdl.load(std::memory_order_acquire) || SDL_IsMainThread())
	{
		fn();
		return;
	}

	struct SCall
	{
		F* fn;
		std::exception_ptr error;
	} call = { &fn, nullptr };

	if (!SDL_RunOnMainThread([](void* p)
		{
			SCall* c = static_cast<SCall*>(p);
			try { (*c->fn)(); }
			catch (...) { c->error = std::current_exception(); }
		}, &call, true))
	{
		FAILURE_1(SDL, "Unable to reach the SDL main thread: %s", SDL_GetError());
	}

	if (call.error)
		std::rethrow_exception(call.error);
}

bool bx_sdl_gui_c::requires_main_thread()
{
#if defined(__APPLE__)
	return true;
#else
	return false;
#endif
}

void bx_sdl_gui_c::main_thread_init()
{
	if (!SDL_Init(SDL_INIT_VIDEO))
		FAILURE_1(SDL, "Unable to initialize SDL3 video subsystem: %s", SDL_GetError());

	sdl_main_thread_owns_sdl.store(true, std::memory_order_release);
}

void bx_sdl_gui_c::main_thread_pump()
{
	// Collect OS events into SDL's queue and dispatch whatever the GUI thread
	// posted with on_main_thread(). handle_events() drains the queue itself,
	// so the wait is passed NULL and leaves the events where they are.
	//
	// SDL_PumpEvents() is what runs the queued on_main_thread() callbacks, so
	// it has to happen every time round: SDL_WaitEventTimeout() returns early
	// without pumping while the queue is non-empty, and waiting on that alone
	// would hang any thread blocked in on_main_thread(). With an empty queue
	// it sleeps until a posted callback or an OS event wakes it, which keeps
	// the loop off the CPU without adding latency to the GUI thread.
	while (sdl_main_thread_owns_sdl.load(std::memory_order_acquire))
	{
		SDL_PumpEvents();
		if (SDL_WaitEventTimeout(NULL, sdl_pump_idle_ms))
			SDL_Delay(1);  // queue not drained yet; let the GUI thread catch up
	}
}

void bx_sdl_gui_c::main_thread_stop()
{
	sdl_main_thread_owns_sdl.store(false, std::memory_order_release);
}

SDL_Event           sdl_event;
int                 sdl_grab = 0;
unsigned            res_x = 0, res_y = 0;
unsigned            half_res_x, half_res_y;
static int          last_driven_w = 0, last_driven_h = 0;
static int          runtime_scale_override = 0;  // 0 = inactive; >0 = use this integer scale
static const int    runtime_scale_min = 1;
static const int    runtime_scale_max = 8;
u8                  old_mousebuttons = 0, new_mousebuttons = 0;
int                 old_mousex = 0, new_mousex = 0;
int                 old_mousey = 0, new_mousey = 0;
static int          sdl_mouse_button_state = 0;
// Fractional motion left over after scaling by mouse.speed; carried across
// events so multipliers < 1.0 don't drop slow movement.
static double       sdl_mouse_accum_x = 0.0;
static double       sdl_mouse_accum_y = 0.0;

static std::string trim_hotkey_text(const std::string& value)
{
	size_t first = 0;
	while (first < value.size() &&
		std::isspace((unsigned char)value[first]))
		first++;

	size_t last = value.size();
	while (last > first && std::isspace((unsigned char)value[last - 1]))
		last--;

	return value.substr(first, last - first);
}

static bool hotkey_text_equals(const std::string& lhs, const char* rhs)
{
	if (!rhs || lhs.size() != strlen(rhs))
		return false;

	for (size_t i = 0; i < lhs.size(); i++)
	{
		if (std::tolower((unsigned char)lhs[i]) !=
			std::tolower((unsigned char)rhs[i]))
			return false;
	}
	return true;
}

static bool parse_hotkey_modifier(const std::string& token,
	unsigned* modifier)
{
	if (hotkey_text_equals(token, "ctrl") ||
		hotkey_text_equals(token, "control"))
		*modifier = SDL_HOTKEY_MOD_CTRL;
	else if (hotkey_text_equals(token, "alt") ||
		hotkey_text_equals(token, "option"))
		*modifier = SDL_HOTKEY_MOD_ALT;
	else if (hotkey_text_equals(token, "shift"))
		*modifier = SDL_HOTKEY_MOD_SHIFT;
	else if (hotkey_text_equals(token, "gui") ||
		hotkey_text_equals(token, "super") ||
		hotkey_text_equals(token, "win") ||
		hotkey_text_equals(token, "windows") ||
		hotkey_text_equals(token, "cmd") ||
		hotkey_text_equals(token, "command") ||
		hotkey_text_equals(token, "meta"))
		*modifier = SDL_HOTKEY_MOD_GUI;
	else
		return false;

	return true;
}

static std::string normalize_hotkey_key_name(const std::string& name)
{
	if (hotkey_text_equals(name, "pgup")) return "PageUp";
	if (hotkey_text_equals(name, "pgdn") ||
		hotkey_text_equals(name, "pagedn")) return "PageDown";
	if (hotkey_text_equals(name, "esc")) return "Escape";
	if (hotkey_text_equals(name, "del")) return "Delete";
	if (hotkey_text_equals(name, "ins")) return "Insert";
	if (hotkey_text_equals(name, "keypadplus") ||
		hotkey_text_equals(name, "kpplus")) return "Keypad +";
	if (hotkey_text_equals(name, "keypadminus") ||
		hotkey_text_equals(name, "kpminus")) return "Keypad -";
	return name;
}

static bool is_hotkey_modifier_key(SDL_Keycode key)
{
	return key == SDLK_LCTRL || key == SDLK_RCTRL ||
		key == SDLK_LALT || key == SDLK_RALT ||
		key == SDLK_LSHIFT || key == SDLK_RSHIFT ||
		key == SDLK_LGUI || key == SDLK_RGUI;
}

static unsigned sdl_hotkey_modifiers(SDL_Keymod modifiers)
{
	unsigned result = 0;
	if (modifiers & SDL_KMOD_CTRL) result |= SDL_HOTKEY_MOD_CTRL;
	if (modifiers & SDL_KMOD_ALT) result |= SDL_HOTKEY_MOD_ALT;
	if (modifiers & SDL_KMOD_SHIFT) result |= SDL_HOTKEY_MOD_SHIFT;
	if (modifiers & SDL_KMOD_GUI) result |= SDL_HOTKEY_MOD_GUI;
	if (modifiers & (SDL_KMOD_MODE | SDL_KMOD_LEVEL5))
		result |= SDL_HOTKEY_MOD_UNSUPPORTED;
	return result;
}

bool sdl_hotkey_binding::matches(const SDL_KeyboardEvent& event) const
{
	return enabled && event.key == key &&
		sdl_hotkey_modifiers(event.mod) == modifiers;
}

static sdl_hotkey_binding parse_sdl_hotkey(CConfigurator* cfg,
	const char* config_name, const char* default_value)
{
	sdl_hotkey_binding binding;
	binding.config_name = config_name;

	const char* configured = cfg->get_text_value(config_name, default_value);
	std::string value = trim_hotkey_text(configured ? configured : "");
	if (hotkey_text_equals(value, "none"))
	{
		binding.display = "none";
		return binding;
	}
	if (value.empty())
	{
		printf("%%SDL-W-HOTKEY: %s has an empty binding; action disabled.\n",
			config_name);
		binding.display = "none";
		return binding;
	}

	std::string key_name = value;
	unsigned modifiers = 0;
	for (;;)
	{
		size_t separator = key_name.find('+');
		if (separator == std::string::npos)
			break;

		std::string token = trim_hotkey_text(key_name.substr(0, separator));
		unsigned modifier = 0;
		if (!parse_hotkey_modifier(token, &modifier))
			break;
		if (modifiers & modifier)
		{
			printf("%%SDL-W-HOTKEY: %s has invalid binding \"%s\"; "
				"action disabled.\n", config_name, value.c_str());
			binding.display = "none";
			return binding;
		}
		modifiers |= modifier;
		key_name = trim_hotkey_text(key_name.substr(separator + 1));
	}

	key_name = normalize_hotkey_key_name(trim_hotkey_text(key_name));
	if (key_name.empty())
	{
		printf("%%SDL-W-HOTKEY: %s has invalid binding \"%s\"; "
			"action disabled.\n", config_name, value.c_str());
		binding.display = "none";
		return binding;
	}

	SDL_ClearError();
	SDL_Keycode key = SDL_GetKeyFromName(key_name.c_str());
	if (key == SDLK_UNKNOWN || is_hotkey_modifier_key(key))
	{
		printf("%%SDL-W-HOTKEY: %s has invalid binding \"%s\"; "
			"action disabled.\n", config_name, value.c_str());
		binding.display = "none";
		return binding;
	}

	binding.key = key;
	binding.modifiers = modifiers;
	binding.enabled = true;
	if (modifiers & SDL_HOTKEY_MOD_CTRL) binding.display += "Ctrl+";
	if (modifiers & SDL_HOTKEY_MOD_ALT) binding.display += "Alt+";
	if (modifiers & SDL_HOTKEY_MOD_SHIFT) binding.display += "Shift+";
	if (modifiers & SDL_HOTKEY_MOD_GUI) binding.display += "GUI+";
	const char* display_key = SDL_GetKeyName(key);
	binding.display += (display_key && *display_key) ? display_key : key_name;
	return binding;
}

bx_sdl_gui_c::bx_sdl_gui_c(CConfigurator* cfg)
{
	myCfg = cfg;
	bx_keymap = new bx_keymap_c(cfg);
}

void bx_sdl_gui_c::load_hotkeys()
{
	hotkey_mouse_capture = parse_sdl_hotkey(myCfg,
		"hotkey.mouse_capture", "Ctrl+F10");
	hotkey_media = parse_sdl_hotkey(myCfg,
		"hotkey.media", "Ctrl+F11");
	hotkey_ctrl_alt_delete = parse_sdl_hotkey(myCfg,
		"hotkey.ctrl_alt_delete", "Ctrl+Alt+End");
	hotkey_reset_window = parse_sdl_hotkey(myCfg,
		"hotkey.reset_window", "Ctrl+Alt+Home");
	hotkey_scale_up = parse_sdl_hotkey(myCfg,
		"hotkey.scale_up", "Ctrl+PageUp");
	hotkey_scale_down = parse_sdl_hotkey(myCfg,
		"hotkey.scale_down", "Ctrl+PageDown");

	sdl_hotkey_binding* bindings[] = {
		&hotkey_mouse_capture,
		&hotkey_media,
		&hotkey_ctrl_alt_delete,
		&hotkey_reset_window,
		&hotkey_scale_up,
		&hotkey_scale_down
	};
	const size_t binding_count = sizeof(bindings) / sizeof(bindings[0]);
	bool active[] = {
		hotkey_mouse_capture.enabled,
		hotkey_media.enabled,
		hotkey_ctrl_alt_delete.enabled,
		hotkey_reset_window.enabled,
		hotkey_scale_up.enabled && vid_scale_change_enable,
		hotkey_scale_down.enabled && vid_scale_change_enable
	};
	bool duplicate[sizeof(bindings) / sizeof(bindings[0])] = {};
	for (size_t i = 0; i < binding_count; i++)
	{
		if (!active[i])
			continue;
		for (size_t j = i + 1; j < binding_count; j++)
		{
			if (active[j] &&
				bindings[i]->key == bindings[j]->key &&
				bindings[i]->modifiers == bindings[j]->modifiers)
			{
				printf("%%SDL-W-HOTKEY: %s and %s both use \"%s\"; "
					"both actions disabled.\n",
					bindings[i]->config_name, bindings[j]->config_name,
					bindings[i]->display.c_str());
				duplicate[i] = true;
				duplicate[j] = true;
			}
		}
	}
	for (size_t i = 0; i < binding_count; i++)
	{
		if (duplicate[i])
		{
			bindings[i]->enabled = false;
			bindings[i]->display = "none";
		}
	}
}

void bx_sdl_gui_c::build_window_titles()
{
	auto append_hint = [](std::string& title,
		const sdl_hotkey_binding& binding, const char* description)
	{
		if (binding.enabled)
		{
			title += " - ";
			title += binding.display;
			title += " ";
			title += description;
		}
	};

	window_title = "ES40 Emulator";
	append_hint(window_title, hotkey_media, "media");
	append_hint(window_title, hotkey_ctrl_alt_delete, "sends C+A+Del");
	append_hint(window_title, hotkey_reset_window, "resets window");

	window_title_grabbed = "ES40 Emulator";
	append_hint(window_title_grabbed, hotkey_mouse_capture, "releases mouse");
	append_hint(window_title_grabbed, hotkey_media, "media");
	append_hint(window_title_grabbed, hotkey_ctrl_alt_delete, "sends C+A+Del");
	append_hint(window_title_grabbed, hotkey_reset_window, "resets window");
}

void bx_sdl_gui_c::specific_init(unsigned x_tilesize, unsigned y_tilesize)
{
	on_main_thread([&] { specific_init_impl(x_tilesize, y_tilesize); });
}

void bx_sdl_gui_c::specific_init_impl(unsigned x_tilesize, unsigned y_tilesize)
{
	// Already done by main_thread_init() where the GUI owns main().
	if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO))
	{
		FAILURE_1(SDL, "Unable to initialize SDL3 video subsystem: %s", SDL_GetError());
	}

	// SDL3: key repeat is handled by the OS; no SDL_EnableKeyRepeat().

	// load keymap for sdl
	if (myCfg->get_bool_value("keyboard.use_mapping", false))
	{
		bx_keymap->loadKeymap(convertStringToSDLKey);
	}

	this->vid_linear = myCfg->get_bool_value("video.linear", true);
	this->vid_scale = (int)myCfg->get_num_value("video.scale_ratio", true, 0);
	this->vid_scale_change_enable = myCfg->get_bool_value("video.scale_change_enable", false);

	const char* ms = myCfg->get_text_value("mouse.speed", "1.0");
	this->mouse_speed = atof(ms);
	if (this->mouse_speed <= 0.0 || this->mouse_speed > 10.0)
	{
		printf("%%SDL-W-MOUSESPEED: invalid mouse.speed \"%s\" (valid: 0.0 < speed <= 10.0); using 1.0.\n", ms);
		this->mouse_speed = 1.0;
	}

	this->mouse_invert_x = myCfg->get_bool_value("mouse.invert_x", false);
	this->mouse_invert_y = myCfg->get_bool_value("mouse.invert_y", false);
	load_hotkeys();
	build_window_titles();
	clear_hotkey_release_state();

	// Create the initial window + renderer + texture at 640x480.
	// dimension_update() will recreate the texture if the resolution changes.
	dimension_update(640, 480);

	new_gfx_api = 1;
}

void bx_sdl_gui_c::graphics_frame_update(const u32* pixels, unsigned width, unsigned height)
{
	on_main_thread([&] { graphics_frame_update_impl(pixels, width, height); });
}

void bx_sdl_gui_c::graphics_frame_update_impl(const u32* pixels, unsigned width, unsigned height)
{
	if (!sdl_texture || !sdl_renderer)
		return;

	// Upload the ARGB32 pixels directly to the streaming texture.
	// pitch = width * 4 bytes per pixel
	SDL_UpdateTexture(sdl_texture, NULL, pixels, (int)(width * sizeof(u32)));

	// Present: clear -> draw texture -> flip
	SDL_RenderClear(sdl_renderer);
	SDL_RenderTexture(sdl_renderer, sdl_texture, NULL, NULL);
	SDL_RenderPresent(sdl_renderer);
}

void bx_sdl_gui_c::graphics_tile_update(u8* snapshot, unsigned x, unsigned y)
{
//
}

bx_svga_tileinfo_t* bx_sdl_gui_c::graphics_tile_info(bx_svga_tileinfo_t* info)
{
	return NULL;
}

u8* bx_sdl_gui_c::graphics_tile_get(unsigned x0, unsigned y0, unsigned* w,
	unsigned* h)
{
	return NULL;
}

void bx_sdl_gui_c::graphics_tile_update_in_place(unsigned x0, unsigned y0,
	unsigned w, unsigned h)
{
	//
}

static u32 sdl_scan_to_bx_key(SDL_Scancode sym)
{
	switch (sym)
	{
	case SDL_SCANCODE_BACKSPACE:    return BX_KEY_BACKSPACE;
	case SDL_SCANCODE_TAB:          return BX_KEY_TAB;
	case SDL_SCANCODE_RETURN:       return BX_KEY_ENTER;
	case SDL_SCANCODE_PAUSE:        return BX_KEY_PAUSE;
	case SDL_SCANCODE_ESCAPE:       return BX_KEY_ESC;
	case SDL_SCANCODE_SPACE:        return BX_KEY_SPACE;
	case SDL_SCANCODE_APOSTROPHE:   return BX_KEY_SINGLE_QUOTE;
	case SDL_SCANCODE_COMMA:        return BX_KEY_COMMA;
	case SDL_SCANCODE_MINUS:        return BX_KEY_MINUS;
	case SDL_SCANCODE_PERIOD:       return BX_KEY_PERIOD;
	case SDL_SCANCODE_SLASH:        return BX_KEY_SLASH;

	case SDL_SCANCODE_0:            return BX_KEY_0;
	case SDL_SCANCODE_1:            return BX_KEY_1;
	case SDL_SCANCODE_2:            return BX_KEY_2;
	case SDL_SCANCODE_3:            return BX_KEY_3;
	case SDL_SCANCODE_4:            return BX_KEY_4;
	case SDL_SCANCODE_5:            return BX_KEY_5;
	case SDL_SCANCODE_6:            return BX_KEY_6;
	case SDL_SCANCODE_7:            return BX_KEY_7;
	case SDL_SCANCODE_8:            return BX_KEY_8;
	case SDL_SCANCODE_9:            return BX_KEY_9;

	case SDL_SCANCODE_SEMICOLON:    return BX_KEY_SEMICOLON;
	case SDL_SCANCODE_EQUALS:       return BX_KEY_EQUALS;

	case SDL_SCANCODE_LEFTBRACKET:  return BX_KEY_LEFT_BRACKET;
	case SDL_SCANCODE_BACKSLASH:    return BX_KEY_BACKSLASH;
	case SDL_SCANCODE_NONUSBACKSLASH: return BX_KEY_BACKSLASH;
	case SDL_SCANCODE_RIGHTBRACKET: return BX_KEY_RIGHT_BRACKET;
	case SDL_SCANCODE_GRAVE:        return BX_KEY_GRAVE;

	case SDL_SCANCODE_A:            return BX_KEY_A;
	case SDL_SCANCODE_B:            return BX_KEY_B;
	case SDL_SCANCODE_C:            return BX_KEY_C;
	case SDL_SCANCODE_D:            return BX_KEY_D;
	case SDL_SCANCODE_E:            return BX_KEY_E;
	case SDL_SCANCODE_F:            return BX_KEY_F;
	case SDL_SCANCODE_G:            return BX_KEY_G;
	case SDL_SCANCODE_H:            return BX_KEY_H;
	case SDL_SCANCODE_I:            return BX_KEY_I;
	case SDL_SCANCODE_J:            return BX_KEY_J;
	case SDL_SCANCODE_K:            return BX_KEY_K;
	case SDL_SCANCODE_L:            return BX_KEY_L;
	case SDL_SCANCODE_M:            return BX_KEY_M;
	case SDL_SCANCODE_N:            return BX_KEY_N;
	case SDL_SCANCODE_O:            return BX_KEY_O;
	case SDL_SCANCODE_P:            return BX_KEY_P;
	case SDL_SCANCODE_Q:            return BX_KEY_Q;
	case SDL_SCANCODE_R:            return BX_KEY_R;
	case SDL_SCANCODE_S:            return BX_KEY_S;
	case SDL_SCANCODE_T:            return BX_KEY_T;
	case SDL_SCANCODE_U:            return BX_KEY_U;
	case SDL_SCANCODE_V:            return BX_KEY_V;
	case SDL_SCANCODE_W:            return BX_KEY_W;
	case SDL_SCANCODE_X:            return BX_KEY_X;
	case SDL_SCANCODE_Y:            return BX_KEY_Y;
	case SDL_SCANCODE_Z:            return BX_KEY_Z;

	case SDL_SCANCODE_DELETE:       return BX_KEY_DELETE;

		// Keypad
	case SDL_SCANCODE_KP_0:         return BX_KEY_KP_INSERT;
	case SDL_SCANCODE_KP_1:         return BX_KEY_KP_END;
	case SDL_SCANCODE_KP_2:         return BX_KEY_KP_DOWN;
	case SDL_SCANCODE_KP_3:         return BX_KEY_KP_PAGE_DOWN;
	case SDL_SCANCODE_KP_4:         return BX_KEY_KP_LEFT;
	case SDL_SCANCODE_KP_5:         return BX_KEY_KP_5;
	case SDL_SCANCODE_KP_6:         return BX_KEY_KP_RIGHT;
	case SDL_SCANCODE_KP_7:         return BX_KEY_KP_HOME;
	case SDL_SCANCODE_KP_8:         return BX_KEY_KP_UP;
	case SDL_SCANCODE_KP_9:         return BX_KEY_KP_PAGE_UP;
	case SDL_SCANCODE_KP_PERIOD:    return BX_KEY_KP_DELETE;
	case SDL_SCANCODE_KP_DIVIDE:    return BX_KEY_KP_DIVIDE;
	case SDL_SCANCODE_KP_MULTIPLY:  return BX_KEY_KP_MULTIPLY;
	case SDL_SCANCODE_KP_MINUS:     return BX_KEY_KP_SUBTRACT;
	case SDL_SCANCODE_KP_PLUS:      return BX_KEY_KP_ADD;
	case SDL_SCANCODE_KP_ENTER:     return BX_KEY_KP_ENTER;

		// Arrows + Home/End pad
	case SDL_SCANCODE_UP:           return BX_KEY_UP;
	case SDL_SCANCODE_DOWN:         return BX_KEY_DOWN;
	case SDL_SCANCODE_RIGHT:        return BX_KEY_RIGHT;
	case SDL_SCANCODE_LEFT:         return BX_KEY_LEFT;
	case SDL_SCANCODE_INSERT:       return BX_KEY_INSERT;
	case SDL_SCANCODE_HOME:         return BX_KEY_HOME;
	case SDL_SCANCODE_END:          return BX_KEY_END;
	case SDL_SCANCODE_PAGEUP:       return BX_KEY_PAGE_UP;
	case SDL_SCANCODE_PAGEDOWN:     return BX_KEY_PAGE_DOWN;

		// Function keys
	case SDL_SCANCODE_F1:           return BX_KEY_F1;
	case SDL_SCANCODE_F2:           return BX_KEY_F2;
	case SDL_SCANCODE_F3:           return BX_KEY_F3;
	case SDL_SCANCODE_F4:           return BX_KEY_F4;
	case SDL_SCANCODE_F5:           return BX_KEY_F5;
	case SDL_SCANCODE_F6:           return BX_KEY_F6;
	case SDL_SCANCODE_F7:           return BX_KEY_F7;
	case SDL_SCANCODE_F8:           return BX_KEY_F8;
	case SDL_SCANCODE_F9:           return BX_KEY_F9;
	case SDL_SCANCODE_F10:          return BX_KEY_F10;
	case SDL_SCANCODE_F11:          return BX_KEY_F11;
	case SDL_SCANCODE_F12:          return BX_KEY_F12;

		// Modifier keys
	case SDL_SCANCODE_NUMLOCKCLEAR: return BX_KEY_NUM_LOCK;
	case SDL_SCANCODE_CAPSLOCK:     return BX_KEY_CAPS_LOCK;
	case SDL_SCANCODE_SCROLLLOCK:   return BX_KEY_SCRL_LOCK;
	case SDL_SCANCODE_RSHIFT:       return BX_KEY_SHIFT_R;
	case SDL_SCANCODE_LSHIFT:       return BX_KEY_SHIFT_L;
	case SDL_SCANCODE_RCTRL:        return BX_KEY_CTRL_R;
	case SDL_SCANCODE_LCTRL:        return BX_KEY_CTRL_L;
	case SDL_SCANCODE_RALT:         return BX_KEY_ALT_R;
	case SDL_SCANCODE_LALT:         return BX_KEY_ALT_L;
	case SDL_SCANCODE_LGUI:         return BX_KEY_WIN_L;
	case SDL_SCANCODE_RGUI:         return BX_KEY_WIN_R;

		// Misc function keys
	case SDL_SCANCODE_PRINTSCREEN:  return BX_KEY_PRINT;
	case SDL_SCANCODE_MENU:         return BX_KEY_MENU;

	default:
		BX_ERROR(("sdl3 scancode 0x%x not mapped", (unsigned)sym));
		return BX_KEY_UNHANDLED;
	}
}

void bx_sdl_gui_c::release_guest_key(SDL_Scancode scancode)
{
	if (scancode > SDL_SCANCODE_UNKNOWN &&
		scancode < SDL_SCANCODE_COUNT && guest_key_pressed[scancode])
	{
		theKeyboard->gen_scancode(
			guest_key_by_scancode[scancode] | BX_KEY_RELEASED);
		guest_key_pressed[scancode] = false;
	}
}

void bx_sdl_gui_c::release_all_guest_keys()
{
	for (int i = 0; i < SDL_SCANCODE_COUNT; i++)
		release_guest_key((SDL_Scancode)i);
}

void bx_sdl_gui_c::clear_hotkey_release_state()
{
	memset(swallowed_hotkey_releases, 0,
		sizeof(swallowed_hotkey_releases));
}

void bx_sdl_gui_c::reconcile_hotkey_release_state()
{
	const bool* keys = SDL_GetKeyboardState(NULL);
	if (!keys)
		return;

	for (int i = 0; i < SDL_SCANCODE_COUNT; i++)
	{
		if (swallowed_hotkey_releases[i] && !keys[i])
			swallowed_hotkey_releases[i] = false;
	}
}

void bx_sdl_gui_c::suppress_hotkey_releases(
	const SDL_KeyboardEvent& event, const sdl_hotkey_binding& binding,
	bool release_guest_modifiers)
{
	if (event.scancode > SDL_SCANCODE_UNKNOWN &&
		event.scancode < SDL_SCANCODE_COUNT)
		swallowed_hotkey_releases[event.scancode] = true;

	if (!release_guest_modifiers)
		return;

	const bool* keys = SDL_GetKeyboardState(NULL);
	auto suppress_modifier = [this, keys](SDL_Scancode left,
		SDL_Scancode right)
	{
		SDL_Scancode scancodes[2] = { left, right };
		for (int i = 0; i < 2; i++)
		{
			SDL_Scancode scancode = scancodes[i];
			if ((keys && keys[scancode]) || guest_key_pressed[scancode])
			{
				swallowed_hotkey_releases[scancode] = true;
				release_guest_key(scancode);
			}
		}
	};

	if (binding.modifiers & SDL_HOTKEY_MOD_CTRL)
		suppress_modifier(SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL);
	if (binding.modifiers & SDL_HOTKEY_MOD_ALT)
		suppress_modifier(SDL_SCANCODE_LALT, SDL_SCANCODE_RALT);
	if (binding.modifiers & SDL_HOTKEY_MOD_SHIFT)
		suppress_modifier(SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT);
	if (binding.modifiers & SDL_HOTKEY_MOD_GUI)
		suppress_modifier(SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI);
}

void bx_sdl_gui_c::send_guest_ctrl_alt_delete()
{
	theKeyboard->gen_scancode(BX_KEY_CTRL_L);
	theKeyboard->gen_scancode(BX_KEY_ALT_L);
	theKeyboard->gen_scancode(BX_KEY_DELETE);
	theKeyboard->gen_scancode(BX_KEY_DELETE | BX_KEY_RELEASED);
	theKeyboard->gen_scancode(BX_KEY_ALT_L | BX_KEY_RELEASED);
	theKeyboard->gen_scancode(BX_KEY_CTRL_L | BX_KEY_RELEASED);
}

void bx_sdl_gui_c::handle_events(void)
{
	on_main_thread([&] { handle_events_impl(); });
}

void bx_sdl_gui_c::handle_events_impl(void)
{
	u32 key_event;

	sdl_media_pump();
	while (SDL_PollEvent(&sdl_event))
	{
		// GUI hotkeys consume their trigger key and, for actions that release
		// guest modifiers, the corresponding physical modifier releases. Keep
		// this ahead of the media popup so a remapped media hotkey also closes it.
		if (sdl_event.type == SDL_EVENT_KEY_UP)
		{
			// The media popup has its own release tracker for dialog keys. Let it
			// clear that state before the global tracker consumes a release shared
			// with the media-toggle chord.
			bool media_release_handled = sdl_media_handle_event(&sdl_event);
			if (sdl_event.key.scancode > SDL_SCANCODE_UNKNOWN &&
				sdl_event.key.scancode < SDL_SCANCODE_COUNT &&
				swallowed_hotkey_releases[sdl_event.key.scancode])
			{
				swallowed_hotkey_releases[sdl_event.key.scancode] = false;
				continue;
			}
			if (media_release_handled)
				continue;
		}

		if (sdl_event.type == SDL_EVENT_KEY_DOWN &&
			hotkey_media.matches(sdl_event.key))
		{
			if (!sdl_event.key.repeat)
			{
				suppress_hotkey_releases(sdl_event.key, hotkey_media, true);
				if (sdl_grab)
					bx_gui->mouse_enabled_changed(false);
				sdl_select_media(sdl_window);
			}
			continue;
		}

		if (sdl_event.type != SDL_EVENT_KEY_UP &&
			sdl_media_handle_event(&sdl_event))
			continue;

		switch (sdl_event.type)
		{
		case SDL_EVENT_WINDOW_EXPOSED:
			// Window needs redraw — re-present the current texture
			if (sdl_renderer && sdl_texture)
			{
				SDL_RenderClear(sdl_renderer);
				SDL_RenderTexture(sdl_renderer, sdl_texture, NULL, NULL);
				SDL_RenderPresent(sdl_renderer);
			}
			break;

		case SDL_EVENT_WINDOW_RESTORED:
		case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
			// System DPI changed — re-scale SDL GUI window
			if (res_x > 0 && res_y > 0)
			{
				dimension_update(res_x, res_y);
			}
			break;

		case SDL_EVENT_MOUSE_MOTION:
			if (sdl_grab)
			{
				// PS/2 mouse Y is positive-up, SDL is positive-down; hence the
				// baseline Y negation. invert_x/y flip on top of that.
				double mx = (double)sdl_event.motion.xrel * mouse_speed;
				double my = -(double)sdl_event.motion.yrel * mouse_speed;
				sdl_mouse_accum_x += mouse_invert_x ? -mx : mx;
				sdl_mouse_accum_y += mouse_invert_y ? -my : my;

				int dx = (int)sdl_mouse_accum_x;
				int dy = (int)sdl_mouse_accum_y;

				if (dx != 0 || dy != 0)
				{
					sdl_mouse_accum_x -= dx;
					sdl_mouse_accum_y -= dy;
					theKeyboard->mouse_motion(dx, dy, 0, sdl_mouse_button_state);
				}
			}
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			if (!sdl_grab)
			{
				if (sdl_event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
					&& sdl_event.button.button == SDL_BUTTON_LEFT)
				{
					bx_gui->mouse_enabled_changed(true);
				}
				break;
			}

			int bitmask = 0;
			switch (sdl_event.button.button)
			{
			case SDL_BUTTON_LEFT:   bitmask = 0x01; break;
			case SDL_BUTTON_RIGHT:  bitmask = 0x02; break;
			case SDL_BUTTON_MIDDLE: bitmask = 0x04; break;
			default: break;
			}

			if (sdl_event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
				sdl_mouse_button_state |= bitmask;
			else
				sdl_mouse_button_state &= ~bitmask;

			theKeyboard->mouse_motion(0, 0, 0, sdl_mouse_button_state);
			break;
		}

		case SDL_EVENT_MOUSE_WHEEL:
			if (sdl_grab)
			{
				float wy = sdl_event.wheel.y;  // SDL3: float; +y = away from user (scroll up)
				if (sdl_event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
					wy = -wy;
				int dz = (int)wy;
				if (dz != 0)
					theKeyboard->mouse_motion(0, 0, dz, sdl_mouse_button_state);
			}
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
		{
			release_all_guest_keys();
			clear_hotkey_release_state();
			if (sdl_grab)
				bx_gui->mouse_enabled_changed(false);
			break;
		}
		case SDL_EVENT_KEY_DOWN:
			if (hotkey_ctrl_alt_delete.matches(sdl_event.key))
			{
				if (!sdl_event.key.repeat)
				{
					suppress_hotkey_releases(sdl_event.key,
						hotkey_ctrl_alt_delete, true);
					send_guest_ctrl_alt_delete();
				}
				break;
			}

			if (hotkey_reset_window.matches(sdl_event.key))
			{
				if (!sdl_event.key.repeat)
				{
					suppress_hotkey_releases(sdl_event.key,
						hotkey_reset_window, false);
					reset_window_size();
				}
				break;
			}

			// Runtime scale adjustment remains gated by video.scale_change_enable.
			if (vid_scale_change_enable && hotkey_scale_up.matches(sdl_event.key))
			{
				if (!sdl_event.key.repeat)
					suppress_hotkey_releases(sdl_event.key,
						hotkey_scale_up, false);
				adjust_window_scale(+1);
				break;
			}
			if (vid_scale_change_enable && hotkey_scale_down.matches(sdl_event.key))
			{
				if (!sdl_event.key.repeat)
					suppress_hotkey_releases(sdl_event.key,
						hotkey_scale_down, false);
				adjust_window_scale(-1);
				break;
			}

			if (hotkey_mouse_capture.matches(sdl_event.key))
			{
				if (!sdl_event.key.repeat)
				{
					suppress_hotkey_releases(sdl_event.key,
						hotkey_mouse_capture, true);
					bx_gui->mouse_enabled_changed(!sdl_grab);
				}
				break;
			}

			// convert sym -> bochs code
			if (!myCfg->get_bool_value("keyboard.use_mapping", false))
			{
				key_event = sdl_scan_to_bx_key(sdl_event.key.scancode);
			}
			else
			{
				/* use mapping */
				BXKeyEntry* entry = bx_keymap->findHostKey(sdl_event.key.key);
				if (!entry)
				{
					BX_ERROR(("host key 0x%x not mapped!",
						(unsigned)sdl_event.key.key));
					break;
				}
				key_event = entry->baseKey;
			}

			if (key_event == BX_KEY_UNHANDLED)
				break;

			if (sdl_event.key.scancode > SDL_SCANCODE_UNKNOWN &&
				sdl_event.key.scancode < SDL_SCANCODE_COUNT &&
				!guest_key_pressed[sdl_event.key.scancode])
			{
				guest_key_by_scancode[sdl_event.key.scancode] = key_event;
				guest_key_pressed[sdl_event.key.scancode] = true;
			}
			theKeyboard->gen_scancode(key_event);

			// Locks: generate immediate press+release pair
			if ((key_event == BX_KEY_NUM_LOCK) || (key_event == BX_KEY_CAPS_LOCK))
			{
				theKeyboard->gen_scancode(key_event | BX_KEY_RELEASED);
				if (sdl_event.key.scancode > SDL_SCANCODE_UNKNOWN &&
					sdl_event.key.scancode < SDL_SCANCODE_COUNT)
					guest_key_pressed[sdl_event.key.scancode] = false;
			}
			break;

		case SDL_EVENT_KEY_UP:
			if (sdl_event.key.scancode > SDL_SCANCODE_UNKNOWN &&
				sdl_event.key.scancode < SDL_SCANCODE_COUNT &&
				guest_key_pressed[sdl_event.key.scancode])
			{
				key_event = guest_key_by_scancode[sdl_event.key.scancode];
			}
			else if (!myCfg->get_bool_value("keyboard.use_mapping", false))
			{
				key_event = sdl_scan_to_bx_key(sdl_event.key.scancode);
			}
			else
			{
				BXKeyEntry* entry = bx_keymap->findHostKey(sdl_event.key.key);
				if (!entry)
				{
					BX_ERROR(("host key 0x%x not mapped!",
						(unsigned)sdl_event.key.key));
					break;
				}
				key_event = entry->baseKey;
			}

			if (key_event == BX_KEY_UNHANDLED)
				break;

			if ((key_event == BX_KEY_NUM_LOCK) || (key_event == BX_KEY_CAPS_LOCK))
			{
				theKeyboard->gen_scancode(key_event);
			}

			theKeyboard->gen_scancode(key_event | BX_KEY_RELEASED);
			if (sdl_event.key.scancode > SDL_SCANCODE_UNKNOWN &&
				sdl_event.key.scancode < SDL_SCANCODE_COUNT)
				guest_key_pressed[sdl_event.key.scancode] = false;
			break;

		case SDL_EVENT_QUIT:
			if (!sdl_grab)
				FAILURE(Graceful, "User requested shutdown");
		}
	}

	// Native popups can intercept releases. Reconcile against SDL's current
	// physical keyboard state so a later normal press is never swallowed.
	reconcile_hotkey_release_state();
}

/**
 * Flush any changes to sdl_screen to the actual window.
 **/
void bx_sdl_gui_c::flush(void)
{
	//
}

/**
 * Clear sdl_screen display, and flush it.
 **/
void bx_sdl_gui_c::clear_screen(void)
{
	on_main_thread([&] { clear_screen_impl(); });
}

void bx_sdl_gui_c::clear_screen_impl(void)
{
	if (!sdl_renderer)
		return;

	SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
	SDL_RenderClear(sdl_renderer);
	SDL_RenderPresent(sdl_renderer);
}

/**
 * Set palette-entry index to the desired value.
 *
 * The palette is used in text-mode and in 8bpp VGA mode.
 **/
bool bx_sdl_gui_c::palette_change(unsigned index, unsigned red, unsigned green,
	unsigned blue)
{
	return 1;
}

void bx_sdl_gui_c::dimension_update(unsigned x, unsigned y, unsigned fheight,
	unsigned fwidth, unsigned bpp)
{
	on_main_thread([&] { dimension_update_impl(x, y, fheight, fwidth, bpp); });
}

void bx_sdl_gui_c::dimension_update_impl(unsigned x, unsigned y, unsigned fheight,
	unsigned fwidth, unsigned bpp)
{
	SDL_DisplayID display;
	float scaled_x, scaled_y;
	float content_scale = 1.0f;

	if (sdl_texture)
	{
		SDL_DestroyTexture(sdl_texture);
		sdl_texture = NULL;
	}

	if (!sdl_window)
		display = SDL_GetPrimaryDisplay();
	else
		display = SDL_GetDisplayForWindow(sdl_window);

	if (runtime_scale_override > 0)
		content_scale = (float)runtime_scale_override;
	else if (vid_scale)
		content_scale = (float)vid_scale;
	else
		content_scale = SDL_GetDisplayContentScale(display);

	scaled_x = x * content_scale;
	scaled_y = y * content_scale;

	if (!sdl_window)
	{
		sdl_window = SDL_CreateWindow(window_title.c_str(),
			(int)scaled_x, (int)scaled_y, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
		if (!sdl_window)
		{
			FAILURE_3(SDL, "Unable to create SDL3 window: %ix%i: %s\n",
				x, y, SDL_GetError());
		}

		sdl_renderer = SDL_CreateRenderer(sdl_window, NULL);
		if (!sdl_renderer)
		{
			FAILURE_3(SDL, "Unable to create SDL3 renderer: %ix%i: %s\n",
				x, y, SDL_GetError());
		}

		SDL_RaiseWindow(sdl_window);
		SDL_SetRenderLogicalPresentation(sdl_renderer,
			(int)x, (int)y,
			SDL_LOGICAL_PRESENTATION_LETTERBOX);
	}
	else
	{
		SDL_SetWindowSize(sdl_window, (int)scaled_x, (int)scaled_y);
		SDL_SetRenderLogicalPresentation(sdl_renderer,
			(int)x, (int)y,
			SDL_LOGICAL_PRESENTATION_LETTERBOX);
	}

	sdl_texture = SDL_CreateTexture(sdl_renderer,
		SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING,
		(int)x, (int)y);
	if (!sdl_texture)
	{
		FAILURE_3(SDL, "Unable to create SDL3 texture: %ix%i: %s\n",
			x, y, SDL_GetError());
	}

	SDL_SetTextureScaleMode(sdl_texture, vid_linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);

	res_x = x;
	res_y = y;
	half_res_x = x / 2;
	half_res_y = y / 2;

	// Remember the actual OS-pixel size we just drove the window to,
	// so the reset-window hotkey can snap back here after a manual resize.
	last_driven_w = (int)scaled_x;
	last_driven_h = (int)scaled_y;
}

void bx_sdl_gui_c::reset_window_size()
{
	if (!sdl_window || last_driven_w == 0 || last_driven_h == 0)
		return;

	SDL_WindowFlags flags = SDL_GetWindowFlags(sdl_window);
	if (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))
		SDL_RestoreWindow(sdl_window);

	SDL_SetWindowSize(sdl_window, last_driven_w, last_driven_h);
}

void bx_sdl_gui_c::adjust_window_scale(int delta)
{
	// Snapshot the currently-effective integer scale.
	int current;
	if (runtime_scale_override > 0)
		current = runtime_scale_override;
	else if (vid_scale)
		current = (int)vid_scale;
	else if (sdl_window)
		current = (int)(SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(sdl_window)) + 0.5f);
	else
		current = 1;

	int next = current + delta;
	if (next < runtime_scale_min) next = runtime_scale_min;
	if (next > runtime_scale_max) next = runtime_scale_max;
	if (next == runtime_scale_override) return;  // no change

	runtime_scale_override = next;

	if (sdl_window && res_x > 0 && res_y > 0)
	{
		SDL_WindowFlags flags = SDL_GetWindowFlags(sdl_window);
		if (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))
			SDL_RestoreWindow(sdl_window);
		dimension_update(res_x, res_y);
	}
}

void bx_sdl_gui_c::mouse_enabled_changed_specific(bool val)
{
	on_main_thread([&] { mouse_enabled_changed_specific_impl(val); });
}

void bx_sdl_gui_c::mouse_enabled_changed_specific_impl(bool val)
{
	if (val)
	{
		SDL_HideCursor();
		if (sdl_window)
		{
			SDL_SetWindowKeyboardGrab(sdl_window, true);
			SDL_SetWindowRelativeMouseMode(sdl_window, true);
			SDL_SetWindowTitle(sdl_window, window_title_grabbed.c_str());
		}
	}
	else
	{
		SDL_ShowCursor();
		if (sdl_window)
		{
			SDL_SetWindowKeyboardGrab(sdl_window, false);
			SDL_SetWindowRelativeMouseMode(sdl_window, false);
			SDL_SetWindowTitle(sdl_window, window_title.c_str());
		}
	}

	sdl_grab = val;
}

void bx_sdl_gui_c::exit(void)
{
	on_main_thread([&] { exit_impl(); });
}

void bx_sdl_gui_c::exit_impl(void)
{
	sdl_media_shutdown();
	if (sdl_texture) {
		SDL_DestroyTexture(sdl_texture);
		sdl_texture = NULL;
	}
	if (sdl_renderer) {
		SDL_DestroyRenderer(sdl_renderer);
		sdl_renderer = NULL;
	}
	if (sdl_window) {
		SDL_DestroyWindow(sdl_window);
		sdl_window = NULL;
	}
}

/// key mapping for SDL
typedef struct
{
	const char* name;
	u32           value;
} keyTableEntry;

#define DEF_SDL_KEY(key) \
  {                      \
    #key, key            \
  },

keyTableEntry keytable[] = {
	// this include provides all the entries.
  #include "sdlkeys.h"
	// one final entry to mark the end
	{ NULL, 0}
};

// function to convert key names into SDLKey values.
// This first try will be horribly inefficient, but it only has
// to be done while loading a keymap.  Once the simulation starts,

// this function won't be called.
static u32 convertStringToSDLKey(const char* string)
{
	keyTableEntry* ptr;
	for (ptr = &keytable[0]; ptr->name != NULL; ptr++)
	{
		if (!strcmp(string, ptr->name))
			return ptr->value;
	}
	return 0;
}
#endif //defined(HAVE_SDL)
