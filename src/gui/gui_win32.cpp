/* ES40 emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2025-2026 by gdwnldsKSC
 *
 * WWW    : https://github.com/ES40-Emu/es40
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
 *
 *  Much of this file was written by:
 *  David Ross
 *  dross@pobox.com
 */

 /**
  * \file
  * Win32 GUI implementation. Allows use of gfx without SDL on WIndows
  *
  * $Id$
  *
  * X-1.5        Camiel Vanderhoeven                             05-MAR-2008
  *      Multi-threading version.
  *
  * X-1.4        Camiel Vanderhoeven                             12-FEB-2008
  *      Moved keyboard code into it's own class (CKeyboard)
  *
  * X-1.3        Camiel Vanderhoeven                             22-JAN-2008
  *      Commented out mousewheel-code, doesn't work with older win32
  *      versions.
  *
  * X-1.2        Camiel Vanderhoeven                             19-JAN-2008
  *      Changed window title.
  *
  * X-1.1        Camiel Vanderhoeven                             19-JAN-2008
  *      Initial version for ES40 emulator.
  *
  **/
#include "../StdAfx.h"

#if defined(_WIN32)
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include "config/Configurator.h"
#include "io/Keyboard.h"
#include "system/System.h"
#include "keymap.h"
#include "gui.h"

class bx_win32_gui_c;
static LRESULT CALLBACK win32_window_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK win32_dispatch_proc(HWND, UINT, WPARAM, LPARAM);
static const char win32_window_class[] = "ES40 Display";
static const char win32_dispatch_class[] = "ES40 Display Dispatcher";
static constexpr UINT win32_invoke_message = WM_APP + 1;
static constexpr UINT win32_wake_message = WM_APP + 2;
static constexpr UINT win32_reconcile_message = WM_APP + 3;

struct win32_call
{
	std::function<void()> fn;
	std::exception_ptr error;
};

// One application owns the output bindings, window thread and guest input.
class win32_application
{
public:
	explicit win32_application(CConfigurator* cfg);
	~win32_application();
	bx_win32_gui_c& create_display();
	bx_win32_gui_c& display_for_output(const std::string&, unsigned);
	const bx_win32_gui_c* find_display_for_output(const std::string&, unsigned) const;
	void initialize();
	void pump();
	void stop();
	void invoke(std::function<void()> fn);
	void exit_displays();
	void poll_input();
	void pause_input() { input_paused = true; }
	void resume_input() { input_paused = false; }
	void detach_input() { keyboard = nullptr; system = nullptr; }
	void request_shutdown();
	void record_callback_error() noexcept;
	void send_key(unsigned scan, bool released);
	void send_mouse(int dx, int dy, int dz, unsigned buttons);
	void send_host_mouse(int dx, int dy, int dz, unsigned buttons);
	void set_mouse_capture(bx_win32_gui_c* target);
	void release_input_session();
	bx_win32_gui_c* window_owner(HWND window) const;
	bx_keymap_c* keymap() const { return shared_keymap.get(); }

	// Accessed only on the application thread, including callbacks.
	std::map<HWND, bx_win32_gui_c*> windows;
	bx_win32_gui_c* capture_owner = nullptr;
	unsigned mouse_buttons = 0;
	bool capture_hotkey_down = false;
	bool input_paused = false;
	bool shutting_down = false;
private:
	void release_guest_keys();
	void sync_input();
	CConfigurator* cfg;
	std::unique_ptr<bx_keymap_c> shared_keymap;
	std::vector<std::unique_ptr<bx_win32_gui_c>> displays;
	std::map<std::pair<std::string, unsigned>, bx_win32_gui_c*> outputs;
	const DWORD owner_thread = GetCurrentThreadId();
	HWND dispatcher = nullptr;
	std::atomic<bool> running{ false };
	CKeyboard* keyboard = nullptr;
	CSystem* system = nullptr;
	bool guest_mouse_captured = false;
	double mouse_speed = 1.0;
	bool mouse_invert_x = false, mouse_invert_y = false;
	// One guest pointer retains fractional motion across output handoffs.
	double mouse_remainder_x = 0.0, mouse_remainder_y = 0.0;
	bool input_reset_pending = false;
	bool guest_keys[512] = {};
	std::exception_ptr callback_error;
};

class bx_win32_gui_c : public bx_gui_c
{
public:
	bx_win32_gui_c(win32_application& app, std::size_t ordinal)
		: application(app), window_title("ES40 - Output " + std::to_string(ordinal)) {}
	void specific_init(unsigned, unsigned) override;
	// GPU renderers submit complete ARGB frames, including VGA text, as with SDL.
	void text_update(u8*, u8*, unsigned long, unsigned long,
		bx_vga_tminfo_t, unsigned) override {}
	void graphics_tile_update(u8*, unsigned, unsigned) override {}
	bx_svga_tileinfo_t* graphics_tile_info(bx_svga_tileinfo_t*) override { return nullptr; }
	u8* graphics_tile_get(unsigned, unsigned, unsigned*, unsigned*) override { return nullptr; }
	void graphics_tile_update_in_place(unsigned, unsigned, unsigned, unsigned) override {}
	void graphics_frame_update(const u32*, unsigned, unsigned) override;
	void handle_events() override;
	void flush() override {}
	void clear_screen() override;
	bool palette_change(unsigned, unsigned, unsigned, unsigned) override { return true; }
	void dimension_update(unsigned, unsigned, unsigned = 0,
		unsigned = 0, unsigned = 8) override;
	void mouse_enabled_changed_specific(bool) override;
	void get_capabilities(u16*, u16*, u16*) override;
	void exit() override;
	void exit_application() override;
	bx_gui_c& display_for_output(const std::string& path, unsigned id) override
		{ return application.display_for_output(path, id); }
	const bx_gui_c* find_display_for_output(const std::string& path, unsigned id) const override
		{ return application.find_display_for_output(path, id); }
	bool requires_main_thread() override { return true; }
	void main_thread_init() override { application.initialize(); }
	void main_thread_pump() override { application.pump(); }
	void main_thread_stop() override { application.stop(); }
	void pause_guest_input() override
		{ application.invoke([this] { application.pause_input(); }); }
	void resume_guest_input() override
		{ application.invoke([this] { application.resume_input(); }); }
	void detach_guest_input() override
		{ application.invoke([this] { application.detach_input(); }); }
private:
	friend class win32_application;
	friend LRESULT CALLBACK win32_window_proc(HWND, UINT, WPARAM, LPARAM);
	void initialize_window();
	void dimension_update_impl(unsigned, unsigned);
	void exit_impl();
	void update_title();
	void paint(HDC);
	void prepare_paint_buffer(HDC, int, int);
	void release_paint_buffer();
	void mouse_event(int, int, int, unsigned);
	void recenter_mouse();
	bool confine_mouse();
	void toggle_fullscreen();
	LRESULT window_event(HWND, UINT, WPARAM, LPARAM);
	win32_application& application;
	HWND window = nullptr;
	const std::string window_title;
	std::vector<u32> pixels;
	unsigned width = 0, height = 0;
	HDC paint_dc = nullptr;
	HBITMAP paint_bitmap = nullptr;
	HGDIOBJ paint_original_bitmap = nullptr;
	int paint_width = 0, paint_height = 0;
	int mouse_last_x = 0, mouse_last_y = 0;
	int wheel_remainder = 0;
	unsigned ignored_mouse_buttons = 0;
	bool retired = false;
	bool fullscreen = false;
	RECT windowed_rect = {};
};

static std::unique_ptr<win32_application> win32_app;

static const u32 win32_to_bx_key[2][0x100] =
{
  {     /* normal-keys */
	/* 0x00 - 0x0f */
	0,
	BX_KEY_ESC,
	BX_KEY_1,
	BX_KEY_2,
	BX_KEY_3,
	BX_KEY_4,
	BX_KEY_5,
	BX_KEY_6,
	  BX_KEY_7,
	  BX_KEY_8,
	  BX_KEY_9,
	  BX_KEY_0,
	  BX_KEY_MINUS,
	  BX_KEY_EQUALS,
		BX_KEY_BACKSPACE,
		BX_KEY_TAB,

		/* 0x10 - 0x1f */
		BX_KEY_Q,
		BX_KEY_W,
		BX_KEY_E,
		BX_KEY_R,
		BX_KEY_T,
		BX_KEY_Y,
		BX_KEY_U,
		  BX_KEY_I,
		  BX_KEY_O,
		  BX_KEY_P,
		  BX_KEY_LEFT_BRACKET,
		  BX_KEY_RIGHT_BRACKET,
			BX_KEY_ENTER,
			BX_KEY_CTRL_L,
			BX_KEY_A,
			BX_KEY_S,

			/* 0x20 - 0x2f */
			BX_KEY_D,
			BX_KEY_F,
			BX_KEY_G,
			BX_KEY_H,
			BX_KEY_J,
			BX_KEY_K,
			BX_KEY_L,
			  BX_KEY_SEMICOLON,
			  BX_KEY_SINGLE_QUOTE,
			  BX_KEY_GRAVE,
			  BX_KEY_SHIFT_L,
				BX_KEY_BACKSLASH,
				BX_KEY_Z,
				BX_KEY_X,
				BX_KEY_C,
				BX_KEY_V,

				/* 0x30 - 0x3f */
				BX_KEY_B,
				BX_KEY_N,
				BX_KEY_M,
				BX_KEY_COMMA,
				BX_KEY_PERIOD,
				BX_KEY_SLASH,
				  BX_KEY_SHIFT_R,
				  BX_KEY_KP_MULTIPLY,
				  BX_KEY_ALT_L,
				  BX_KEY_SPACE,
					BX_KEY_CAPS_LOCK,
					BX_KEY_F1,
					BX_KEY_F2,
					BX_KEY_F3,
					BX_KEY_F4,
					BX_KEY_F5,

					/* 0x40 - 0x4f */
					BX_KEY_F6,
					BX_KEY_F7,
					BX_KEY_F8,
					BX_KEY_F9,
					BX_KEY_F10,
					BX_KEY_PAUSE,
					  BX_KEY_SCRL_LOCK,
					  BX_KEY_KP_HOME,
					  BX_KEY_KP_UP,
					  BX_KEY_KP_PAGE_UP,
						BX_KEY_KP_SUBTRACT,
						BX_KEY_KP_LEFT,
						BX_KEY_KP_5,
						BX_KEY_KP_RIGHT,
						  BX_KEY_KP_ADD,
						  BX_KEY_KP_END,

						  /* 0x50 - 0x5f */
						  BX_KEY_KP_DOWN,
						  BX_KEY_KP_PAGE_DOWN,
						  BX_KEY_KP_INSERT,
							BX_KEY_KP_DELETE,
							0,
							0,
							BX_KEY_LEFT_BACKSLASH,
							BX_KEY_F11,
							  BX_KEY_F12,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,

							  /* 0x60 - 0x6f */
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,

							  /* 0x70 - 0x7f */
							  0,  /* Todo: "Katakana" key ( ibm 133 ) for Japanese 106 keyboard */
							  0,
							  0,
							  0,  /* Todo: "Ro" key ( ibm 56 ) for Japanese 106 keyboard */
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,  /* Todo: "convert" key ( ibm 132 ) for Japanese 106 keyboard */
							  0,
							  0,  /* Todo: "non-convert" key ( ibm 131 ) for Japanese 106 keyboard */
							  0,
							  0,  /* Todo: "Yen" key ( ibm 14 ) for Japanese 106 keyboard */
							  0,
							  0,
							},
							{     /* extended-keys */
							  /* 0x00 - 0x0f */
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,

							  /* 0x10 - 0x1f */
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  BX_KEY_KP_ENTER,
							  BX_KEY_CTRL_R,
							  0,
							  0,

							  /* 0x20 - 0x2f */
							  0,
							  BX_KEY_POWER_CALC,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,
							  0,

							  /* 0x30 - 0x3f */
							  0,
							  0,
							  BX_KEY_INT_HOME,
							  0,
							  0,
							  BX_KEY_KP_DIVIDE,
							  0,
							  BX_KEY_PRINT,
								BX_KEY_ALT_R,
								0,
								0,
								0,
								0,
								0,
								0,
								0,

								/* 0x40 - 0x4f */
								0,
								0,
								0,
								0,
								0,
								BX_KEY_NUM_LOCK,
								0,
								BX_KEY_HOME,
								BX_KEY_UP,
								  BX_KEY_PAGE_UP,
								  0,
								  BX_KEY_LEFT,
								  0,
								  BX_KEY_RIGHT,
								  0,
								  BX_KEY_END,

								  /* 0x50 - 0x5f */
								  BX_KEY_DOWN,
								  BX_KEY_PAGE_DOWN,
								  BX_KEY_INSERT,
									BX_KEY_DELETE,
									0,
									0,
									0,
									0,
									0,
									0,
									0,
									BX_KEY_WIN_L,
									BX_KEY_WIN_R,
									  BX_KEY_MENU,
									  BX_KEY_POWER_POWER,
									  BX_KEY_POWER_SLEEP,

									  /* 0x60 - 0x6f */
									  0,
									  0,
									  0,
									  BX_KEY_POWER_WAKE,
									  0,
									  BX_KEY_INT_SEARCH,
									  BX_KEY_INT_FAV,
									  0,
										BX_KEY_INT_STOP,
										BX_KEY_INT_FORWARD,
										BX_KEY_INT_BACK,
										BX_KEY_POWER_MYCOMP,
										  BX_KEY_INT_MAIL,
										  0,
										  0,
										  0,
										}
};


win32_application::win32_application(CConfigurator* cfg)
	: cfg(cfg), shared_keymap(new bx_keymap_c(cfg)) {}

win32_application::~win32_application()
{
	// Normal teardown is explicit after the workers join. Cover startup errors
	// too; before main_thread_init(), no display can have native resources.
	try { exit_displays(); } catch (...) {}
	for (const auto& display : displays)
		if (bx_gui == display.get()) bx_gui = nullptr;
	if (bx_keymap == shared_keymap.get()) bx_keymap = nullptr;
}

bx_win32_gui_c& win32_application::create_display()
{
	if (shutting_down)
		FAILURE(Win32, "Cannot create a display after application shutdown");
	auto display = std::make_unique<bx_win32_gui_c>(*this, displays.size());
	displays.push_back(std::move(display));
	return *displays.back();
}

bx_win32_gui_c& win32_application::display_for_output(
	const std::string& device_path, unsigned output_id)
{
	if (shutting_down)
		FAILURE(Win32, "Cannot bind a display after application shutdown");
	if (device_path.empty())
		FAILURE(Configuration, "Display output requires a device identity");
	const auto key = std::make_pair(device_path, output_id);
	const auto existing = outputs.find(key);
	if (existing != outputs.end()) return *existing->second;
	const auto entry = outputs.emplace(key, nullptr).first;
	try
	{
		entry->second = outputs.size() == 1 && !displays.empty()
			? displays.front().get() : &create_display();
	}
	catch (...) { outputs.erase(entry); throw; }
	return *entry->second;
}

const bx_win32_gui_c* win32_application::find_display_for_output(
	const std::string& device_path, unsigned output_id) const
{
	if (shutting_down || device_path.empty()) return nullptr;
	const auto entry = outputs.find(std::make_pair(device_path, output_id));
	return entry == outputs.end() ? nullptr : entry->second;
}

int libwin32_LTX_plugin_init(CConfigurator* cfg)
{
	if (bx_gui || win32_app)
		FAILURE(Configuration, "A GUI application is already installed");
	auto app = std::make_unique<win32_application>(cfg);
	auto& initial_display = app->create_display();
	win32_app = std::move(app);
	bx_gui = &initial_display;
	bx_keymap = win32_app->keymap();
	printf("%%GUI-I-INS: Installing win32 module as the ES40 GUI\n");
	return 0;
}

void libwin32_LTX_plugin_fini() {}

static void register_win32_class(const char* name, WNDPROC proc)
{
	WNDCLASS wc = {};
	wc.lpfnWndProc = proc;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = name;
	if (!RegisterClass(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		FAILURE(Win32, "Unable to register display window class");
}

void win32_application::initialize()
{
	if (GetCurrentThreadId() != owner_thread || shutting_down)
		FAILURE(Win32, "Invalid display application initialization");
	if (dispatcher) return;
	const char* speed = cfg->get_text_value("mouse.speed", "1.0");
	mouse_speed = std::atof(speed);
	if (!std::isfinite(mouse_speed) || mouse_speed <= 0.0 ||
		mouse_speed > 10.0)
	{
		printf("%%WIN32-W-MOUSESPEED: invalid mouse.speed \"%s\" "
			"(valid: 0.0 < speed <= 10.0); using 1.0.\n", speed);
		mouse_speed = 1.0;
	}
	mouse_invert_x = cfg->get_bool_value("mouse.invert_x", false);
	mouse_invert_y = cfg->get_bool_value("mouse.invert_y", false);
	register_win32_class(win32_window_class, win32_window_proc);
	register_win32_class(win32_dispatch_class, win32_dispatch_proc);
	dispatcher = CreateWindowEx(0, win32_dispatch_class, "", 0, 0, 0,
		0, 0, HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), nullptr);
	if (!dispatcher)
		FAILURE(Win32, "Unable to create display dispatcher");
	if (cfg->get_bool_value("keyboard.use_mapping", false))
		shared_keymap->loadKeymap(nullptr);
	keyboard = theKeyboard;
	system = theSystem;
	if (keyboard) keyboard->set_mouse_capture(false);
	running.store(true, std::memory_order_release);
}

void win32_application::invoke(std::function<void()> fn)
{
	if (GetCurrentThreadId() == owner_thread) { fn(); return; }
	if (!running.load(std::memory_order_acquire) || !dispatcher)
		FAILURE(Win32, "Display application thread is not running");
	win32_call call = { std::move(fn), nullptr };
	if (SendMessage(dispatcher, win32_invoke_message, 0, (LPARAM)&call) != 1)
		FAILURE(Win32, "Unable to reach the display application thread");
	if (call.error) std::rethrow_exception(call.error);
}

static LRESULT CALLBACK win32_dispatch_proc(HWND hwnd, UINT msg,
	WPARAM wparam, LPARAM lparam)
{
	if (msg == win32_invoke_message)
	{
		auto& call = *reinterpret_cast<win32_call*>(lparam);
		try { call.fn(); } catch (...) { call.error = std::current_exception(); }
		return 1;
	}
	if (msg == win32_wake_message) return 0;
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

void win32_application::pump()
{
	while (running.load(std::memory_order_acquire))
	{
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT) request_shutdown();
			else { TranslateMessage(&msg); DispatchMessage(&msg); }
		}
		MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
	}
}

void win32_application::stop()
{
	running.store(false, std::memory_order_release);
	if (dispatcher) PostMessage(dispatcher, win32_wake_message, 0, 0);
}

bx_win32_gui_c* win32_application::window_owner(HWND window) const
{
	const auto entry = windows.find(window);
	return entry == windows.end() ? nullptr : entry->second;
}

void win32_application::request_shutdown()
{
	if (system) system->RequestShutdown();
}

void win32_application::record_callback_error() noexcept
{
	if (!callback_error)
	{
		callback_error = std::current_exception();
		request_shutdown();
	}
}

void win32_application::poll_input()
{
	if (callback_error) std::rethrow_exception(callback_error);
	if (input_paused) return;
	sync_input();
	auto* focus = window_owner(GetFocus());
	if (!focus) release_input_session();
	else if (capture_owner && capture_owner != focus) set_mouse_capture(focus);
	else if (capture_owner && GetCapture() != capture_owner->window)
		set_mouse_capture(nullptr);
}

void win32_application::sync_input()
{
	if (input_paused || !keyboard || !input_reset_pending) return;
	input_reset_pending = false;
	release_guest_keys();
	if (guest_mouse_captured) keyboard->mouse_motion(0, 0, 0, 0);
	guest_mouse_captured = capture_owner != nullptr;
	keyboard->set_mouse_capture(guest_mouse_captured);
	mouse_buttons = 0;
}

static u32 translate_win32_key(unsigned scan)
{
	if (scan >= 512) return BX_KEY_UNHANDLED;
	const u32 key = win32_to_bx_key[(scan >> 8) & 1][scan & 0xff];
	// Zero denotes an unmapped scan code except for the left Control key.
	return key || scan == 0x1d ? key : BX_KEY_UNHANDLED;
}

void win32_application::send_key(unsigned scan, bool released)
{
	if (!keyboard || input_paused || scan >= 512) return;
	sync_input();
	const u32 key = translate_win32_key(scan);
	if (key == BX_KEY_UNHANDLED || (released && !guest_keys[scan])) return;
	if (!released && guest_keys[scan] && (key == BX_KEY_CTRL_L ||
		key == BX_KEY_CTRL_R || key == BX_KEY_ALT_L || key == BX_KEY_ALT_R ||
		key == BX_KEY_SHIFT_L || key == BX_KEY_SHIFT_R)) return;
	keyboard->gen_scancode(key | (released ? BX_KEY_RELEASED : 0));
	guest_keys[scan] = !released;
}

void win32_application::release_guest_keys()
{
	if (input_paused) { input_reset_pending = true; return; }
	for (unsigned i = 0; i < 512; ++i)
		if (guest_keys[i]) send_key(i, true);
}

void win32_application::send_mouse(int dx, int dy, int dz, unsigned buttons)
{
	if (keyboard && !input_paused)
		keyboard->mouse_motion(dx, dy, dz, buttons);
}

void win32_application::send_host_mouse(int dx, int dy, int dz,
	unsigned buttons)
{
	if (!keyboard || input_paused) return;
	// The window has already converted host Y to the PS/2 positive-up axis.
	mouse_remainder_x += dx * (mouse_invert_x ? -mouse_speed : mouse_speed);
	mouse_remainder_y += dy * (mouse_invert_y ? -mouse_speed : mouse_speed);
	const int scaled_x = (int)mouse_remainder_x;
	const int scaled_y = (int)mouse_remainder_y;
	mouse_remainder_x -= scaled_x;
	mouse_remainder_y -= scaled_y;
	if (scaled_x || scaled_y || dz || buttons != mouse_buttons)
		send_mouse(scaled_x, scaled_y, dz, buttons);
	mouse_buttons = buttons;
}

void win32_application::release_input_session()
{
	release_guest_keys();
	mouse_remainder_x = mouse_remainder_y = 0.0;
	set_mouse_capture(nullptr);
	capture_hotkey_down = false;
}

void win32_application::set_mouse_capture(bx_win32_gui_c* target)
{
	if (target && (!target->window || target->retired ||
		GetActiveWindow() != target->window)) return;
	if (capture_owner == target) return;
	auto* previous = capture_owner;
	// Release buttons before disabling guest capture, including handoffs.
	if (guest_mouse_captured) send_mouse(0, 0, 0, 0);
	if (input_paused) input_reset_pending = true;
	mouse_buttons = 0;
	capture_owner = nullptr;
	if (previous)
	{
		ClipCursor(nullptr);
		if (GetCapture() == previous->window) ReleaseCapture();
		previous->update_title();
	}
	if (target)
	{
		target->wheel_remainder = 0;
		target->ignored_mouse_buttons =
			((GetKeyState(VK_LBUTTON) & 0x8000) ? MK_LBUTTON : 0) |
			((GetKeyState(VK_RBUTTON) & 0x8000) ? MK_RBUTTON : 0) |
			((GetKeyState(VK_MBUTTON) & 0x8000) ? MK_MBUTTON : 0);
		// Publish the new owner before SetCapture notifies the previous window.
		capture_owner = target;
		SetCapture(target->window);
		if (GetCapture() != target->window || !target->confine_mouse())
		{
			capture_owner = nullptr;
			mouse_remainder_x = mouse_remainder_y = 0.0;
			ClipCursor(nullptr);
			if (GetCapture() == target->window) ReleaseCapture();
		}
		else target->recenter_mouse();
		target->update_title();
	}
	SetCursor(capture_owner ? nullptr : LoadCursor(nullptr, IDC_ARROW));
	if (keyboard && !input_paused)
	{
		guest_mouse_captured = capture_owner != nullptr;
		keyboard->set_mouse_capture(guest_mouse_captured);
	}
}

void bx_win32_gui_c::specific_init(unsigned, unsigned)
{
	application.invoke([this] { initialize_window(); });
	new_gfx_api = true;
}

void bx_win32_gui_c::initialize_window()
{
	if (retired || application.shutting_down)
		FAILURE(Win32, "Cannot initialize a retired display");
	if (window) return;
	if (width == 0 || height == 0) dimension_update_impl(640, 480);
	const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	RECT rect = { 0, 0, (LONG)width, (LONG)height };
	AdjustWindowRectEx(&rect, style, FALSE, 0);
	HWND created = CreateWindowEx(0, win32_window_class, window_title.c_str(),
		style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
		rect.bottom - rect.top, nullptr, nullptr, GetModuleHandle(nullptr), this);
	if (!created)
		FAILURE(Win32, "Unable to create display window");
	SetTimer(window, 1, 330, nullptr);
	ShowWindow(window, SW_SHOW);
}

void bx_win32_gui_c::update_title()
{
	// Same base and hint formatting as SDL's build_window_titles(). 
	if (window)
		SetWindowText(window, (window_title + (application.capture_owner == this
			? " - Ctrl+F10 releases mouse" : "")).c_str());
}

void bx_win32_gui_c::dimension_update(unsigned x, unsigned y,
	unsigned, unsigned, unsigned)
{
	application.invoke([=] { dimension_update_impl(x, y); });
}

void bx_win32_gui_c::dimension_update_impl(unsigned x, unsigned y)
{
	if (retired) return;
	if (!x || !y || x > (unsigned)(std::numeric_limits<int>::max)() ||
		y > (unsigned)(std::numeric_limits<int>::max)() ||
		(size_t)x > (std::numeric_limits<size_t>::max)() / sizeof(u32) / y)
		FAILURE(Win32, "Invalid display dimensions");
	if (x == width && y == height) return;
	pixels.assign((size_t)x * y, 0);
	width = x;
	height = y;
	host_xres = (u16)x;
	host_yres = (u16)y;
	host_bpp = 32;
	if (window && !fullscreen)
	{
		RECT rect = { 0, 0, (LONG)x, (LONG)y };
		AdjustWindowRectEx(&rect, (DWORD)GetWindowLongPtr(window, GWL_STYLE),
			FALSE, (DWORD)GetWindowLongPtr(window, GWL_EXSTYLE));
		SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left,
			rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
	if (application.capture_owner == this)
	{
		if (confine_mouse()) recenter_mouse();
		else application.set_mouse_capture(nullptr);
	}
}

void bx_win32_gui_c::graphics_frame_update(const u32* frame,
	unsigned x, unsigned y)
{
	application.invoke([=]
	{
		if (!window || retired || !frame) return;
		dimension_update_impl(x, y);
		std::copy_n(frame, (size_t)x * y, pixels.data());
		InvalidateRect(window, nullptr, FALSE);
		UpdateWindow(window);
	});
}

void bx_win32_gui_c::paint(HDC dc)
{
	RECT rect;
	if (!GetClientRect(window, &rect) || IsRectEmpty(&rect)) return;
	prepare_paint_buffer(dc, rect.right, rect.bottom);
	// Compose the background and image offscreen.
	FillRect(paint_dc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
	if (!pixels.empty() && width && height)
	{
		// Match SDL's letterboxing if the client area differs from the mode.
		const double scale = (std::min)((double)rect.right / width,
			(double)rect.bottom / height);
		const int w = (int)(width * scale), h = (int)(height * scale);
		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = (LONG)width;
		info.bmiHeader.biHeight = -(LONG)height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		SetStretchBltMode(paint_dc, COLORONCOLOR);
		StretchDIBits(paint_dc, (rect.right - w) / 2, (rect.bottom - h) / 2,
			w, h, 0, 0, width, height, pixels.data(), &info,
			DIB_RGB_COLORS, SRCCOPY);
	}
	BitBlt(dc, 0, 0, rect.right, rect.bottom, paint_dc, 0, 0, SRCCOPY);
}

void bx_win32_gui_c::prepare_paint_buffer(HDC reference, int x, int y)
{
	if (paint_dc && paint_width == x && paint_height == y) return;
	HDC buffer_dc = CreateCompatibleDC(reference);
	HBITMAP bitmap = buffer_dc ? CreateCompatibleBitmap(reference, x, y)
		: nullptr;
	HGDIOBJ original = bitmap ? SelectObject(buffer_dc, bitmap) : nullptr;
	if (!original || original == HGDI_ERROR)
	{
		if (bitmap) DeleteObject(bitmap);
		if (buffer_dc) DeleteDC(buffer_dc);
		FAILURE(Win32, "Unable to create display paint buffer");
	}
	// Keep the previous buffer intact until its replacement is complete.
	release_paint_buffer();
	paint_dc = buffer_dc;
	paint_bitmap = bitmap;
	paint_original_bitmap = original;
	paint_width = x;
	paint_height = y;
}

void bx_win32_gui_c::release_paint_buffer()
{
	if (paint_dc && paint_original_bitmap)
		SelectObject(paint_dc, paint_original_bitmap);
	if (paint_bitmap) DeleteObject(paint_bitmap);
	if (paint_dc) DeleteDC(paint_dc);
	paint_dc = nullptr;
	paint_bitmap = nullptr;
	paint_original_bitmap = nullptr;
	paint_width = paint_height = 0;
}

void bx_win32_gui_c::clear_screen()
{
	application.invoke([this]
	{
		if (!window || retired) return;
		std::fill(pixels.begin(), pixels.end(), 0);
		InvalidateRect(window, nullptr, FALSE);
		UpdateWindow(window);
	});
}

void bx_win32_gui_c::handle_events()
{
	application.invoke([this] { application.poll_input(); });
}

void bx_win32_gui_c::mouse_enabled_changed_specific(bool val)
{
	application.invoke([=] { application.set_mouse_capture(val ? this : nullptr); });
}

void bx_win32_gui_c::get_capabilities(u16* xres, u16* yres, u16* bpp)
{
	*xres = (u16)GetSystemMetrics(SM_CXSCREEN);
	*yres = (u16)GetSystemMetrics(SM_CYSCREEN);
	*bpp = 32;
}

bool bx_win32_gui_c::confine_mouse()
{
	RECT rect;
	if (!GetClientRect(window, &rect) || IsRectEmpty(&rect)) return false;
	MapWindowPoints(window, nullptr, (POINT*)&rect, 2);
	return ClipCursor(&rect) != 0;
}

void bx_win32_gui_c::recenter_mouse()
{
	RECT rect;
	GetClientRect(window, &rect);
	POINT pt = { (rect.right - rect.left) / 2,
		(rect.bottom - rect.top) / 2 };
	mouse_last_x = pt.x;
	mouse_last_y = pt.y;
	ClientToScreen(window, &pt);
	SetCursorPos(pt.x, pt.y);
}

void bx_win32_gui_c::mouse_event(int x, int y, int z, unsigned flags)
{
	if (application.capture_owner != this || application.input_paused) return;
	ignored_mouse_buttons &= flags;
	flags &= ~ignored_mouse_buttons;
	unsigned buttons = ((flags & MK_LBUTTON) ? 1 : 0) |
		((flags & MK_RBUTTON) ? 2 : 0) | ((flags & MK_MBUTTON) ? 4 : 0);
	int dx = x - mouse_last_x, dy = mouse_last_y - y;
	mouse_last_x = x;
	mouse_last_y = y;
	application.send_host_mouse(dx, dy, z, buttons);
}

void bx_win32_gui_c::toggle_fullscreen()
{
	const LONG_PTR normal_style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	if (!fullscreen)
	{
		MONITORINFO monitor = { sizeof(MONITORINFO) };
		if (!GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
		GetWindowRect(window, &windowed_rect);
		fullscreen = true;
		SetWindowLongPtr(window, GWL_STYLE, WS_POPUP);
		SetWindowPos(window, nullptr, monitor.rcMonitor.left, monitor.rcMonitor.top,
			monitor.rcMonitor.right - monitor.rcMonitor.left,
			monitor.rcMonitor.bottom - monitor.rcMonitor.top,
			SWP_FRAMECHANGED | SWP_NOZORDER);
	}
	else
	{
		fullscreen = false;
		SetWindowLongPtr(window, GWL_STYLE, normal_style);
		SetWindowPos(window, nullptr, windowed_rect.left, windowed_rect.top,
			windowed_rect.right - windowed_rect.left,
			windowed_rect.bottom - windowed_rect.top,
			SWP_FRAMECHANGED | SWP_NOZORDER);
	}
}

LRESULT bx_win32_gui_c::window_event(HWND hwnd, UINT msg,
	WPARAM wparam, LPARAM lparam)
{
	const bool input_message = msg == WM_KEYDOWN || msg == WM_KEYUP ||
		msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
		(msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
	if (input_message)
	{
		if (application.input_paused) return 0;
		application.poll_input();
		if (!application.window_owner(GetFocus())) return 0;
	}
	switch (msg)
	{
	case WM_CLOSE:
		application.request_shutdown();
		return 0;
	case WM_SETFOCUS:
		if (application.capture_owner && application.capture_owner != this)
			application.set_mouse_capture(this);
		return 0;
	case WM_KILLFOCUS:
		if (!application.window_owner((HWND)wparam)) application.release_input_session();
		return 0;
	case WM_ACTIVATEAPP:
		if (!wparam) application.release_input_session();
		return 0;
	case WM_CANCELMODE:
		application.release_input_session();
		return 0;
	case WM_CAPTURECHANGED:
		if (application.capture_owner == this && (HWND)lparam != hwnd)
		{
			// Focus can still be changing between our windows. Reconcile after
			// that transition; never reacquire capture inside this notification.
			ClipCursor(nullptr);
			SetCursor(LoadCursor(nullptr, IDC_ARROW));
			PostMessage(hwnd, win32_reconcile_message, 0, 0);
		}
		return 0;
	case win32_reconcile_message:
		application.poll_input();
		return 0;
	case WM_TIMER:
		if (application.capture_owner == this)
		{
			if (confine_mouse()) recenter_mouse();
			else application.set_mouse_capture(nullptr);
		}
		return 0;
	case WM_SETCURSOR:
		if (application.capture_owner == this && LOWORD(lparam) == HTCLIENT)
		{
			SetCursor(nullptr);
			return TRUE;
		}
		break;
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		try { paint(dc); }
		catch (...) { EndPaint(hwnd, &ps); throw; }
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_MOUSEMOVE:
	case WM_LBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
		mouse_event(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), 0,
			GET_KEYSTATE_WPARAM(wparam));
		return 0;
	case WM_LBUTTONDOWN:
		if (application.capture_owner != this) application.set_mouse_capture(this);
		else mouse_event(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), 0,
			GET_KEYSTATE_WPARAM(wparam));
		return 0;
	case WM_MOUSEWHEEL:
		if (application.capture_owner == this)
		{
			wheel_remainder += GET_WHEEL_DELTA_WPARAM(wparam);
			int steps = wheel_remainder / WHEEL_DELTA;
			wheel_remainder %= WHEEL_DELTA;
			POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
			ScreenToClient(hwnd, &pt);
			mouse_event(pt.x, pt.y, steps, GET_KEYSTATE_WPARAM(wparam));
		}
		return 0;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (wparam == VK_F10 && !(lparam & (1L << 30)))
			application.capture_hotkey_down = false;
		if (wparam == VK_F10 && (application.capture_hotkey_down ||
			(!(lparam & (1L << 30)) && (GetKeyState(VK_CONTROL) & 0x8000))))
		{
			if (!application.capture_hotkey_down)
				application.set_mouse_capture(application.capture_owner ? nullptr : this);
			application.capture_hotkey_down = true;
			return 0;
		}
		if (wparam == VK_RETURN && (HIWORD(lparam) & KF_ALTDOWN))
		{
			if (!(lparam & (1L << 30))) toggle_fullscreen();
			return 0;
		}
		application.send_key((HIWORD(lparam) & 0x1ff), false);
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (wparam == VK_F10 && application.capture_hotkey_down)
		{
			application.capture_hotkey_down = false;
			return 0;
		}
		application.send_key((HIWORD(lparam) & 0x1ff), true);
		return 0;
	case WM_CHAR:
	case WM_SYSCHAR:
	case WM_DEADCHAR:
	case WM_SYSDEADCHAR:
		return 0;
	}
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK win32_window_proc(HWND hwnd, UINT msg,
	WPARAM wparam, LPARAM lparam)
{
	auto* owner = reinterpret_cast<bx_win32_gui_c*>(
		GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE)
	{
		owner = static_cast<bx_win32_gui_c*>(
			reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)owner);
	}
	if (!owner) return DefWindowProc(hwnd, msg, wparam, lparam);
	try
	{
		if (msg == WM_NCCREATE)
		{
			owner->window = hwnd;
			owner->application.windows.emplace(hwnd, owner);
			return DefWindowProc(hwnd, msg, wparam, lparam);
		}
		if (msg == WM_NCDESTROY)
		{
			owner->application.windows.erase(hwnd);
			SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
			owner->window = nullptr;
			return DefWindowProc(hwnd, msg, wparam, lparam);
		}
		return owner->window_event(hwnd, msg, wparam, lparam);
	}
	catch (...)
	{
		// Never unwind through a native callback. A display worker rethrows the
		// first error through handle_events(), and main keeps serving shutdown.
		owner->application.record_callback_error();
		return msg == WM_NCCREATE ? FALSE : 0;
	}
}

void bx_win32_gui_c::exit_impl()
{
	if (retired) return;
	retired = true;
	if (application.capture_owner == this) application.set_mouse_capture(nullptr);
	if (window)
	{
		if (GetFocus() == window) application.release_input_session();
		KillTimer(window, 1);
		DestroyWindow(window);
	}
	release_paint_buffer();
	pixels.clear();
}

void bx_win32_gui_c::exit()
{
	application.invoke([this] { exit_impl(); });
}

void win32_application::exit_displays()
{
	shutting_down = true;
	for (const auto& display : displays) display->exit_impl();
	if (dispatcher) { DestroyWindow(dispatcher); dispatcher = nullptr; }
}

void bx_win32_gui_c::exit_application()
{
	application.invoke([this] { application.exit_displays(); });
}

#endif // _WIN32
