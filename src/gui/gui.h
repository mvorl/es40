/* ES40 emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 *
 * WWW    : http://sourceforge.net/projects/es40
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
 * Contains the definitions for the bx_gui_c base class used for interfacing with
 * SDL and other device interfaces.
 *
 * $Id: gui.h,v 1.9 2008/03/14 15:31:29 iamcamiel Exp $
 *
 * X-1.8        Camiel Vanderhoeven                             11-MAR-2008
 *      Named, debuggable mutexes.
 *
 * X-1.7        Camiel Vanderhoeven                             05-MAR-2008
 *      Multi-threading version.
 *
 * X-1.6        David Leonard                                   20-FEB-2008
 *      Avoid 'Xlib: unexpected async reply' errors on Linux/Unix/BSD's by
 *      adding some thread interlocking.
 *
 * X-1.5        Camiel Vanderhoeven                             20-JAN-2008
 *      Avoid compiler warnings.
 *
 * X-1.4        Camiel Vanderhoeven                             02-JAN-2008
 *      Comments.
 *
 * X-1.3        Camiel Vanderhoeven                             10-DEC-2007
 *      Use Configurator.
 *
 * X-1.2        Camiel Vanderhoeven                             7-DEC-2007
 *      Code cleanup.
 *
 * X-1.1        Camiel Vanderhoeven                             6-DEC-2007
 *      Initial version for ES40 emulator.
 *
 **/
#ifndef __GUI_H__
#define __GUI_H__

#if defined(DEBUG_VGA)
#define BX_DEBUG(a)  \
  {                  \
    printf  a;       \
    printf("   \n"); \
  }
#else
#define BX_DEBUG(a)
#endif
#define BX_INFO(a)  BX_DEBUG(a)
#define BX_PANIC(a) BX_DEBUG(a)
#define BX_ERROR(a) BX_DEBUG(a)
#include "vga.h"

/// VGA mode information for GUI
typedef struct
{
  u16   start_address;
  u8    cs_start;
  u8    cs_end;
  u16   line_offset;
  u16   line_compare;
  u8    h_panning;
  u8    v_panning;
  bool  line_graphics;
  bool  split_hpanning;
} bx_vga_tminfo_t;

/// VGA tile information for GUI
typedef struct
{
  u16           bpp, pitch;
  u8            red_shift, green_shift, blue_shift;
  u8            is_indexed, is_little_endian;
  unsigned long red_mask, green_mask, blue_mask;
} bx_svga_tileinfo_t;

extern class bx_gui_c*  bx_gui;

/**
 * \brief Abstract base class for GUI implementations.
 **/
class                   bx_gui_c
{
  public:
    bx_gui_c(void);
    virtual                       ~bx_gui_c();
    virtual void                  specific_init(unsigned x_tilesize,
                                                unsigned y_tilesize) = 0;
    virtual void                  text_update(u8*  old_text, u8*  new_text,
                                              unsigned long cursor_x,
                                              unsigned long cursor_y,
                                              bx_vga_tminfo_t tm_info,
                                              unsigned rows) = 0;
    virtual void                  graphics_tile_update(u8*  snapshot, unsigned x,
                                                       unsigned y) = 0;
    virtual bx_svga_tileinfo_t*   graphics_tile_info(bx_svga_tileinfo_t* info);
    virtual u8*                   graphics_tile_get(unsigned x, unsigned y,
                                                    unsigned*  w, unsigned*  h);
    virtual void                  graphics_tile_update_in_place(unsigned x,
                                                                unsigned y,
                                                                unsigned w,
                                                                unsigned h);
    virtual void                  handle_events(void) = 0;
    virtual void                  flush(void) = 0;
    virtual void                  clear_screen(void) = 0;
    virtual bool                  palette_change(unsigned index, unsigned red,
                                                 unsigned green, unsigned blue) = 0;
    virtual void                  dimension_update(unsigned x, unsigned y,
                                                   unsigned fheight = 0,
                                                   unsigned fwidth = 0,
                                                   unsigned bpp = 8) = 0;
    virtual void                  mouse_enabled_changed_specific(bool val) = 0;
    virtual void                  exit(void) = 0;

    virtual u32                   get_sighandler_mask() { return 0; }
    virtual void                  sighandler(int sig)   { }
    virtual void                  beep_on(float frequency);
    virtual void                  beep_off();
    virtual void                  get_capabilities(u16*  xres, u16*  yres,
                                                   u16*  bpp);

    static void                   key_event(u32 key);
    static void                   set_text_charmap(u8* fbuffer);
    static void                   set_text_charbyte(u16 address, u8 data);

    void                          init(unsigned x_tilesize, unsigned y_tilesize);
    void                          cleanup(void);
    static void                   mouse_enabled_changed(bool val);
    static void                   init_signal_handlers();

    void                          lock();
    void                          unlock();
  protected:
    CMutex*       guiMutex;
    static s32    make_text_snapshot(char ** snapshot, u32* length);

    //  static void toggle_mouse_enable(void);
    unsigned char vga_charmap[0x2000];
    bool          charmap_updated;
    bool          char_changed[256];
    bool          new_gfx_api;
    u16           host_xres;
    u16           host_yres;
    u16           host_pitch;
    u8            host_bpp;
    u8*           framebuffer;
};

#define BX_KEY_PRESSED        0x00000000
#define BX_KEY_RELEASED       0x80000000

#define BX_KEY_UNHANDLED      0x10000000

#define BX_KEY_CTRL_L         0
#define BX_KEY_SHIFT_L        1

#define BX_KEY_F1             2
#define BX_KEY_F2             3
#define BX_KEY_F3             4
#define BX_KEY_F4             5
#define BX_KEY_F5             6
#define BX_KEY_F6             7
#define BX_KEY_F7             8
#define BX_KEY_F8             9
#define BX_KEY_F9             10
#define BX_KEY_F10            11
#define BX_KEY_F11            12
#define BX_KEY_F12            13

#define BX_KEY_CTRL_R         14
#define BX_KEY_SHIFT_R        15
#define BX_KEY_CAPS_LOCK      16
#define BX_KEY_NUM_LOCK       17
#define BX_KEY_ALT_L          18
#define BX_KEY_ALT_R          19

#define BX_KEY_A              20
#define BX_KEY_B              21
#define BX_KEY_C              22
#define BX_KEY_D              23
#define BX_KEY_E              24
#define BX_KEY_F              25
#define BX_KEY_G              26
#define BX_KEY_H              27
#define BX_KEY_I              28
#define BX_KEY_J              29
#define BX_KEY_K              30
#define BX_KEY_L              31
#define BX_KEY_M              32
#define BX_KEY_N              33
#define BX_KEY_O              34
#define BX_KEY_P              35
#define BX_KEY_Q              36
#define BX_KEY_R              37
#define BX_KEY_S              38
#define BX_KEY_T              39
#define BX_KEY_U              40
#define BX_KEY_V              41
#define BX_KEY_W              42
#define BX_KEY_X              43
#define BX_KEY_Y              44
#define BX_KEY_Z              45

#define BX_KEY_0              46
#define BX_KEY_1              47
#define BX_KEY_2              48
#define BX_KEY_3              49
#define BX_KEY_4              50
#define BX_KEY_5              51
#define BX_KEY_6              52
#define BX_KEY_7              53
#define BX_KEY_8              54
#define BX_KEY_9              55

#define BX_KEY_ESC            56

#define BX_KEY_SPACE          57
#define BX_KEY_SINGLE_QUOTE   58
#define BX_KEY_COMMA          59
#define BX_KEY_PERIOD         60
#define BX_KEY_SLASH          61

#define BX_KEY_SEMICOLON      62
#define BX_KEY_EQUALS         63

#define BX_KEY_LEFT_BRACKET   64
#define BX_KEY_BACKSLASH      65
#define BX_KEY_RIGHT_BRACKET  66
#define BX_KEY_MINUS          67
#define BX_KEY_GRAVE          68

#define BX_KEY_BACKSPACE      69
#define BX_KEY_ENTER          70
#define BX_KEY_TAB            71

#define BX_KEY_LEFT_BACKSLASH 72
#define BX_KEY_PRINT          73
#define BX_KEY_SCRL_LOCK      74
#define BX_KEY_PAUSE          75

#define BX_KEY_INSERT         76
#define BX_KEY_DELETE         77
#define BX_KEY_HOME           78
#define BX_KEY_END            79
#define BX_KEY_PAGE_UP        80
#define BX_KEY_PAGE_DOWN      81

#define BX_KEY_KP_ADD         82
#define BX_KEY_KP_SUBTRACT    83
#define BX_KEY_KP_END         84
#define BX_KEY_KP_DOWN        85
#define BX_KEY_KP_PAGE_DOWN   86
#define BX_KEY_KP_LEFT        87
#define BX_KEY_KP_RIGHT       88
#define BX_KEY_KP_HOME        89
#define BX_KEY_KP_UP          90
#define BX_KEY_KP_PAGE_UP     91
#define BX_KEY_KP_INSERT      92
#define BX_KEY_KP_DELETE      93
#define BX_KEY_KP_5           94

#define BX_KEY_UP             95
#define BX_KEY_DOWN           96
#define BX_KEY_LEFT           97
#define BX_KEY_RIGHT          98

#define BX_KEY_KP_ENTER       99
#define BX_KEY_KP_MULTIPLY    100
#define BX_KEY_KP_DIVIDE      101

#define BX_KEY_WIN_L          102
#define BX_KEY_WIN_R          103
#define BX_KEY_MENU           104

#define BX_KEY_ALT_SYSREQ     105
#define BX_KEY_CTRL_BREAK     106

#define BX_KEY_INT_BACK       107
#define BX_KEY_INT_FORWARD    108
#define BX_KEY_INT_STOP       109
#define BX_KEY_INT_MAIL       110
#define BX_KEY_INT_SEARCH     111
#define BX_KEY_INT_FAV        112
#define BX_KEY_INT_HOME       113

#define BX_KEY_POWER_MYCOMP   114
#define BX_KEY_POWER_CALC     115
#define BX_KEY_POWER_SLEEP    116
#define BX_KEY_POWER_POWER    117
#define BX_KEY_POWER_WAKE     118

#define BX_KEY_NBKEYS         119

// If you add BX_KEYs Please update
// - BX_KEY_NBKEYS
// - the scancodes table in the file iodev/scancodes.cc
// - the bx_key_symbol table in the file gui/keymap.cc


/////////////// GUI plugin support
// Define macro to supply gui plugin code.  This macro is called once in GUI to
// supply the plugin initialization methods.  Since it is nearly identical for
// each gui module, the macro is easier to maintain than pasting the same code
// in each one.
//
// Each gui should declare a class pointer called "theGui" which is derived
// from bx_gui_c, before calling this macro.  For example, the SDL port
// says:

//   static bx_sdl_gui_c *theGui;
#define IMPLEMENT_GUI_PLUGIN_CODE(gui_name)                                   \
  int lib##gui_name##_LTX_plugin_init(CConfigurator*  cfg)                    \
  {                                                                           \
    printf("%%GUI-I-INS: Installing %s module as the ES40 GUI\n", #gui_name); \
    theGui = new bx_##gui_name##_gui_c(cfg);                                  \
    bx_gui = theGui;                                                          \
    return(0);  /* Success */                                                 \
  }                                                                           \
                                                                             \
  void lib##gui_name##_LTX_plugin_fini(void)                                  \
  { }
#endif
