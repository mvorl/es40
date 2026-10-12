/* ES40 Emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2026 by gdwnldsKSC
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
  * Contains code macros for the processor PALmode instructions.
  * Based on HRM.
  *
  * $Id$
  *
  * X-1.15       Camiel Vanderhoeven                             14-MAR-2008
  *   1. More meaningful exceptions replace throwing (int) 1.
  *   2. U64 macro replaces X64 macro.
  *
  * X-1.14       Camiel Vanderhoeven                             05-MAR-2008
  *      Multi-threading version.
  *
  * X-1.13       Camiel Vanderhoeven                             30-JAN-2008
  *      Always use set_pc or add_pc to change the program counter.
  *
  * X-1.12       Camiel Vanderhoeven                             30-JAN-2008
  *      Remember number of instructions left in current memory page, so
  *      that the translation-buffer doens't need to be consulted on every
  *      instruction fetch when the Icache is disabled.
  *
  * X-1.11       Camiel Vanderhoeven                             25-JAN-2008
  *      Trap on unalogned memory access. The previous implementation where
  *      unaligned accesses were silently allowed could go wrong when page
  *      boundaries are crossed.
  *
  * X-1.10       Camiel Vanderhoeven                             18-JAN-2008
  *      Replaced sext_64 inlines with sext_u64_<bits> inlines for
  *      performance reasons (thanks to David Hittner for spotting this!);
  *
  * X-1.9        Camiel Vanderhoeven                             2-DEC-2007
  *      Changed the way translation buffers work, the way interrupts work.
  *
  * X-1.8        Camiel Vanderhoeven                             12-NOV-2007
  *      Fix Software Interrupt behaviour.
  *
  * X-1.7        Camiel Vanderhoeven                             30-OCT-2007
  *      IPR 0x2d identified as M_FIX (Mbox fixed behaviour)
  *
  * X-1.6        Camiel Vanderhoeven                             11-APR-2007
  *      Moved all data that should be saved to a state file to a structure
  *      "state".
  *
  * X-1.5        Camiel Vanderhoeven                             30-MAR-2007
  *      Added old changelog comments.
  *
  * X-1.4        Camiel Vanderhoeven                             8-MAR-2007
  *      Different argument list for CTranslationBuffer methods, and for
  *      va_form function.
  *
  * X-1.3        Camiel Vanderhoeven                             3-MAR-2007
  *      Put 0x2d in HW_MTPR to get rid of error messages about this unknown
  *      IPR. WE STILL NEED TO FIND OUT WHAT THIS REGISTER DOES.
  *
  * X-1.2        Camiel Vanderhoeven                             22-FEB-2007
  *      Got rid of calls to get_asn.
  *
  * X-1.1        Camiel Vanderhoeven                             18-FEB-2007
  *      File created. Contains code previously found in AlphaCPU.h
  **/

#define DO_HW_MFPR state.r[REG_1] = read_ipr(function);

#define DO_HW_MTPR write_ipr(function, state.r[REG_2], AlphaIPRWriteContext::Interpreter);

 /*
  * HW_RET (HRM 6.4.3) is a simple jump-to-target on EV6/EV68. 
  */
 #define DO_HW_RET   do {                                                               \
    u64 target = state.r[REG_2] & ~U64(0x2);                                            \
    set_pc(target);                                                                     \
  } while(0)

#define DO_HW_LDL   switch(function)                                          \
  {                                                                           \
  case 0:       /* longword physical */                                       \
    phys_address = state.r[REG_2] + DISP_12;                                  \
    state.r[REG_1] = READ_PHYS_NT(32);                                        \
    state.r[REG_1] = sext_u64_32(state.r[REG_1]);  /* HW_LDL canonical longword (vmspal hw_ldl) */ \
    break;                                                                    \
                                                                              \
  case 2:       /* longword physical locked */                                \
    phys_address = state.r[REG_2] + DISP_12;                                  \
    state.r[REG_1] = READ_PHYS_NT(32);                                        \
    state.r[REG_1] = sext_u64_32(state.r[REG_1]);  /* HW_LDL canonical longword (vmspal hw_ldl*/ \
    cSystem->cpu_lock(state.iProcNum, phys_address, state.r[REG_1]);          \
    break;                                                                    \
                                                                              \
  case 4:       /* longword virtual VPTE (HRM 6.4.1 TYPE 0102: LD_VPTE) --    \
                 * page-table-entry fetch; access checked against KERNEL      \
                 * mode regardless of executing CM. virt2phys forces cm=0     \
                 * when the VPTE flag is set. */                              \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | VPTE);               \
    state.r[REG_1] = READ_PHYS_NT(32);                                        \
    state.r[REG_1] = sext_u64_32(state.r[REG_1]);  /* HW_LDL canonical longword (vmspal hw_ldl,) */ \
    break;                                                                    \
                                                                              \
  case 8:       /* longword virtual (HRM 6.4.1 TYPE 1002) -- access checked   \
                 * against current mode */                                    \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ);                      \
    state.r[REG_1] = READ_PHYS_NT(32);                                        \
    state.r[REG_1] = sext_u64_32(state.r[REG_1]);  /* HW_LDL canonical longword (vmspal hw_ldl) */ \
    break;                                                                    \
                                                                              \
  case 10:      /* longword virtual check (HRM 6.4.1 TYPE 1012: WrChk) */     \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | WRCHK);              \
    state.r[REG_1] = READ_PHYS_NT(32);                                        \
    state.r[REG_1] = sext_u64_32(state.r[REG_1]);  /* HW_LDL canonical longword (vmspal hw_ldl) */ \
    break;                                                                    \
                                                                           \
  case 12:      /* longword virtual alt (HRM 6.4.1 TYPE 1102) -- access      \
                 * checked using DTB_ALT_MODE, matching QEMU brokenpipe        \
                 * AlphaMMUIdx_AltMode (was incorrectly bypassed via           \
                 * NO_CHECK). */                                               \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | ALT);                \
    state.r[REG_1] = READ_PHYS_NT(32);                                        \
    state.r[REG_1] = sext_u64_32(state.r[REG_1]);  /* HW_LDL canonical longword (vmspal hw_ldl) */ \
    break;                                                                    \
                                                                           \
  case 14:      /* longword virtual alt check (HRM TYPE 1112: WrChk/Alt) */   \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | ALT | WRCHK);        \
    state.r[REG_1] = READ_PHYS_NT(32);                                        \
    state.r[REG_1] = sext_u64_32(state.r[REG_1]);  /* HW_LDL canonical longword (vmspal hw_ldl) */ \
    break;                                                                    \
                                                                           \
  default:                                                                    \
    UNKNOWN2;                                                                 \
  }

#define DO_HW_LDQ   switch(function)                                          \
  {                                                                           \
  case 1:       /* quadword physical */                                       \
    phys_address = state.r[REG_2] + DISP_12;                                  \
    state.r[REG_1] = READ_PHYS_NT(64);                                        \
    break;                                                                    \
                                                                           \
  case 3:       /* quadword physical locked */                                \
    phys_address = state.r[REG_2] + DISP_12;                                  \
    state.r[REG_1] = READ_PHYS_NT(64);                                        \
    cSystem->cpu_lock(state.iProcNum, phys_address, state.r[REG_1]);          \
    break;                                                                    \
                                                                           \
  case 5:       /* quadword virtual VPTE (HRM 6.4.1 TYPE 0102: LD_VPTE) --   \
                 * see HW_LDL case 4 for full notes. */                        \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | VPTE);               \
    state.r[REG_1] = READ_PHYS_NT(64);                                        \
    break;                                                                    \
                                                                           \
  case 9:       /* quadword virtual (HRM 6.4.1 TYPE 1002) -- see case 8 */    \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ);                      \
    state.r[REG_1] = READ_PHYS_NT(64);                                        \
    break;                                                                    \
                                                                           \
  case 11:      /* quadword virtual check (HRM 6.4.1 TYPE 1012: WrChk) */     \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | WRCHK);              \
    state.r[REG_1] = READ_PHYS_NT(64);                                        \
    break;                                                                    \
                                                                           \
  case 13:      /* quadword virtual alt (HRM 6.4.1 TYPE 1102) -- see case 12 */\
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | ALT);                \
    state.r[REG_1] = READ_PHYS_NT(64);                                        \
    break;                                                                    \
                                                                           \
  case 15:      /* quadword virtual alt check (HRM TYPE 1112: WrChk/Alt) */   \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_READ | ALT | WRCHK);        \
    state.r[REG_1] = READ_PHYS_NT(64);                                        \
    break;                                                                    \
                                                                           \
  default:                                                                    \
    UNKNOWN2;                                                                 \
  }

#define DO_HW_STL   switch(function)                                          \
  {                                                                           \
  case 0:       /* longword physical */                                       \
    phys_address = state.r[REG_2] + DISP_12;                                  \
    WRITE_PHYS_NT(state.r[REG_1], 32);                                        \
    break;                                                                    \
                                                                           \
  case 2:       /* longword physical conditional */                           \
    phys_address = state.r[REG_2] + DISP_12;                                  \
    state.r[REG_1] = cSystem->cpu_stx_c(state.iProcNum, phys_address, 32,     \
                                        state.r[REG_1], dram_ptr, dram_size, this); \
    break;                                                                    \
                                                                           \
  case 4:       /* longword virtual (HRM 6.4.1 Table 6-4 TYPE 0102) -- write \
                 * checked against current mode, matching QEMU brokenpipe      \
                 * AlphaMMUIdx_Privileged. */                                  \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_WRITE);                     \
    WRITE_PHYS_NT(state.r[REG_1], 32);                                        \
    break;                                                                    \
                                                                           \
  case 12:      /* longword virtual alt (HRM TYPE 1102) -- write checked     \
                 * using DTB_ALT_MODE, matching brokenpipe AlphaMMUIdx_AltMode.*/\
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_WRITE | ALT);               \
    WRITE_PHYS_NT(state.r[REG_1], 32);                                        \
    break;                                                                    \
                                                                           \
  default:                                                                    \
    UNKNOWN2;                                                                 \
  }

#define DO_HW_STQ   switch(function)                                           \
  {                                                                            \
  case 1:       /* quadword physical */                                        \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    WRITE_PHYS_NT(state.r[REG_1], 64);                                         \
    break;                                                                     \
                                                                            \
  case 3:       /* quadword physical conditional */                            \
    phys_address = state.r[REG_2] + DISP_12;                                   \
    state.r[REG_1] = cSystem->cpu_stx_c(state.iProcNum, phys_address, 64,      \
                                        state.r[REG_1], dram_ptr, dram_size, this); \
    break;                                                                     \
                                                                            \
  case 5:       /* quadword virtual (HRM 6.4.1 Table 6-4 TYPE 0102) --       \
                 * see HW_STL case 4. */                                       \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_WRITE);                     \
    WRITE_PHYS_NT(state.r[REG_1], 64);                                        \
    break;                                                                    \
                                                                           \
  case 13:      /* quadword virtual alt (HRM TYPE 1102) -- see HW_STL 12. */ \
    DATA_PHYS_NT(state.r[REG_2] + DISP_12, ACCESS_WRITE | ALT);               \
    WRITE_PHYS_NT(state.r[REG_1], 64);                                        \
    break;                                                                    \
                                                                            \
  default:                                                                     \
    UNKNOWN2;                                                                  \
  }
