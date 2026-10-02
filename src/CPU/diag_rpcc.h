/* ES40 emulator -- per-thread accumulator for diagnostic-print RPCC exclusion.
 * Copyright (C) 2026 by gdwnldsKSC */
#ifndef DIAG_RPCC_H
#define DIAG_RPCC_H

#include <cstdint>

// Console-I/O stall of a hot-path diagnostic print (e.g. the PCI decode), accumulated per
// CPU thread. jit_run drains it out of cc_last_sync so the stall isn't billed to the guest RPCC --
// billing it jumps the cycle counter and, during VGA init, spirals into a boot hang.
extern thread_local int64_t g_diag_excluded_ns;

#endif // DIAG_RPCC_H
