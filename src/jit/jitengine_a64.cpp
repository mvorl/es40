/* ES40 emulator -- JIT engine: the ARM64 (AArch64) codegen backend.
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
#ifdef ES40_JIT

#include "jitengine.h"        // defines ES40_JIT_X64 / ES40_JIT_A64 from the host arch
#include "jitengine_internal.h"

#ifdef ES40_JIT_A64

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <initializer_list>
#include <type_traits>
#include <vector>
#define ASMJIT_STATIC
#include <asmjit/a64.h>

// register convention for both block and trace codegen:
//   x9-x15 = transient expansion/tail scratch; x19 = CAlphaCPU*, x20 = state.r[] base,
//   x21 = completed-instruction count, x22 = next guest PC / chain target,
//   x23-x28 = guest-GPR pin bank.
struct CJitEngine::RegAlloc {
  static constexpr uint32_t kGuestPinCount = 6;

  static constexpr asmjit::a64::Gp kArgCpu = asmjit::a64::x0;
  static constexpr asmjit::a64::Gp kArgRegs = asmjit::a64::x1;
  static constexpr asmjit::a64::Gp kResultCount = asmjit::a64::w0;
  static constexpr asmjit::a64::Gp kScratch0 = asmjit::a64::x9;
  static constexpr asmjit::a64::Gp kScratch1 = asmjit::a64::x10;
  static constexpr asmjit::a64::Gp kScratch2 = asmjit::a64::x11;
  static constexpr asmjit::a64::Gp kScratch3 = asmjit::a64::x12;
  static constexpr asmjit::a64::Gp kScratch4 = asmjit::a64::x13;
  static constexpr asmjit::a64::Gp kScratch5 = asmjit::a64::x14;
  static constexpr asmjit::a64::Gp kScratch6 = asmjit::a64::x15;
  static constexpr asmjit::a64::Gp kCallTarget = asmjit::a64::x16;

  static constexpr asmjit::a64::Gp kCpu = asmjit::a64::x19;
  static constexpr asmjit::a64::Gp kRegs = asmjit::a64::x20;
  static constexpr asmjit::a64::Gp kChainCount = asmjit::a64::x21;
  static constexpr asmjit::a64::Gp kNextPc = asmjit::a64::x22;
  static constexpr asmjit::a64::Gp kGuestPin0 = asmjit::a64::x23;
  static constexpr asmjit::a64::Gp kGuestPin1 = asmjit::a64::x24;
  static constexpr asmjit::a64::Gp kGuestPin2 = asmjit::a64::x25;
  static constexpr asmjit::a64::Gp kGuestPin3 = asmjit::a64::x26;
  static constexpr asmjit::a64::Gp kGuestPin4 = asmjit::a64::x27;
  static constexpr asmjit::a64::Gp kGuestPin5 = asmjit::a64::x28;

  static constexpr asmjit::RegMask kPersistentGpMask =
      asmjit::Support::bit_mask<asmjit::RegMask>(
          kCpu.id(), kRegs.id(), kChainCount.id(), kNextPc.id(),
          kGuestPin0.id(), kGuestPin1.id(), kGuestPin2.id(),
          kGuestPin3.id(), kGuestPin4.id(), kGuestPin5.id());

  // Bindings are explicit and ordered: entry i maps to host x23+i. Keeping the
  // policy out of this table lets blocks and traces share the same routing machinery.
  std::array<uint8_t, kGuestPinCount> guest_by_pin{};
  uint8_t pin_count = 0;

  static constexpr bool guest_is_shadowable(uint32_t raw_guest) noexcept {
    return raw_guest < 32 && (raw_guest & 0x0cu) == 0x04u;
  }

  // Dynamic bindings exclude variant-dependent slots. Fixed block bindings may use
  // bind_main_guest() for an explicitly main-bank value that PAL-shadow routing bypasses.
  static constexpr bool guest_pin_candidate(uint32_t raw_guest) noexcept {
    return raw_guest < 31 && !guest_is_shadowable(raw_guest);
  }

  constexpr bool bind_main_guest(uint32_t raw_guest) noexcept {
    if (raw_guest >= 31 || pin_count >= kGuestPinCount
        || pin_index_of(raw_guest) >= 0) return false;
    guest_by_pin[pin_count++] = static_cast<uint8_t>(raw_guest);
    return true;
  }

  constexpr bool bind_guest(uint32_t raw_guest) noexcept {
    return guest_pin_candidate(raw_guest) && bind_main_guest(raw_guest);
  }

  constexpr int pin_index_of(uint32_t raw_guest) const noexcept {
    if (raw_guest >= 32) return -1;
    for (uint32_t i = 0; i < pin_count; ++i)
      if (guest_by_pin[i] == raw_guest) return static_cast<int>(i);
    return -1;
  }

  constexpr int guest_of_pin(uint32_t pin_index) const noexcept {
    return pin_index < pin_count ? guest_by_pin[pin_index] : -1;
  }

  constexpr int host_of(uint32_t raw_guest, bool pal_shadow) const noexcept {
    if (raw_guest >= 31 || (pal_shadow && guest_is_shadowable(raw_guest))) return -1;
    const int pin_index = pin_index_of(raw_guest);
    return pin_index < 0 ? -1 : static_cast<int>(kGuestPin0.id()) + pin_index;
  }
};

static_assert(CJitEngine::RegAlloc::kGuestPin1.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 1
              && CJitEngine::RegAlloc::kGuestPin2.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 2
              && CJitEngine::RegAlloc::kGuestPin3.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 3
              && CJitEngine::RegAlloc::kGuestPin4.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 4
              && CJitEngine::RegAlloc::kGuestPin5.id()
                  == CJitEngine::RegAlloc::kGuestPin0.id() + 5,
              "A64 guest pin indices must map contiguously to x23-x28");

static_assert((CJitEngine::RegAlloc::kPersistentGpMask
               & asmjit::Support::bit_mask<asmjit::RegMask>(8, 16, 17,
                     asmjit::a64::Gp::kIdOs, 29, 30, 31)) == 0,
              "A64 persistent registers must exclude ABI, linker, platform, frame, link, and stack registers");
static_assert((CJitEngine::RegAlloc::kPersistentGpMask
               & asmjit::Support::bit_mask<asmjit::RegMask>(
                     CJitEngine::RegAlloc::kScratch0.id())) == 0,
              "A64 expansion scratch must not overlap persistent registers");
static_assert(CJitEngine::RegAlloc::kScratch6.id()
                  - CJitEngine::RegAlloc::kScratch0.id() == 6
              && CJitEngine::RegAlloc::kCallTarget.id()
                  == CJitEngine::RegAlloc::kScratch6.id() + 1,
              "A64 tail scratch registers must remain the contiguous x9-x16 bank");

namespace {

// Every block body shares this exact mapping so a chained entry can inherit the
// live pin bank without an adapter. R22/R23 name the main-bank slots; PAL-shadow
// accesses to those architectural registers continue to route through memory.
static constexpr std::array<uint8_t, CJitEngine::RegAlloc::kGuestPinCount>
    kA64BlockGuestPins{{1, 16, 30, 22, 23, 27}};

static constexpr CJitEngine::RegAlloc make_a64_block_regalloc() noexcept
{
  CJitEngine::RegAlloc regs{};
  for (const uint8_t guest : kA64BlockGuestPins)
    if (!regs.bind_main_guest(guest)) return {};
  return regs;
}

enum A64GprOperandMask : uint8_t {
  kA64GprRa = 1u << 0,
  kA64GprRb = 1u << 1,
  kA64GprRc = 1u << 2
};

enum class A64OpKind : uint8_t {
  kUnsupported,
  kValidationProbe
};

enum class A64TerminatorKind : uint8_t {
  kNone,
  kDirect,
  kIndirect,
  kCallPal,
  kRedispatch
};

enum class A64PlanStop : uint8_t {
  kInvalidSource,
  kEmptyBlock,
  kUnsupported,
  kBlockEnd,
  kPageBoundary,
  kInstructionLimit,
  kTerminator
};

struct A64OpClass {
  A64OpKind kind = A64OpKind::kUnsupported;
  A64TerminatorKind terminator = A64TerminatorKind::kNone;
  uint8_t gpr_reads = 0;
  uint8_t gpr_writes = 0;
};

struct A64DecodedOp {
  uint32_t ins = 0;
  A64OpKind kind = A64OpKind::kUnsupported;
  A64TerminatorKind terminator = A64TerminatorKind::kNone;
  uint8_t gpr_reads = 0;
  uint8_t gpr_writes = 0;
  uint8_t opcode = 0;
  uint8_t ra = 0;
  uint8_t rb = 0;
  uint8_t rc = 0;
  uint8_t literal = 0;
  bool is_literal = false;
};

static_assert(sizeof(A64DecodedOp) <= 16,
              "A64 block plans must keep their fixed instruction snapshot compact");

struct A64GprUsage {
  std::array<uint16_t, 32> refs{};
  uint32_t reads = 0;
  uint32_t writes = 0;
};

static constexpr void note_a64_gpr(A64GprUsage& usage, uint32_t raw_guest,
                                   bool write) noexcept
{
  if (raw_guest >= 31) return;
  ++usage.refs[raw_guest];
  if (write) usage.writes |= 1u << raw_guest;
  else       usage.reads  |= 1u << raw_guest;
}

static constexpr void note_a64_op_gprs(A64GprUsage& usage,
                                       const A64DecodedOp& op) noexcept
{
  if (op.gpr_reads & kA64GprRa) note_a64_gpr(usage, op.ra, false);
  if (op.gpr_reads & kA64GprRb) note_a64_gpr(usage, op.rb, false);
  if (op.gpr_reads & kA64GprRc) note_a64_gpr(usage, op.rc, false);
  if (op.gpr_writes & kA64GprRa) note_a64_gpr(usage, op.ra, true);
  if (op.gpr_writes & kA64GprRb) note_a64_gpr(usage, op.rb, true);
  if (op.gpr_writes & kA64GprRc) note_a64_gpr(usage, op.rc, true);
}

static constexpr uint64_t a64_advance_pc(uint64_t pc, uint32_t n_instr) noexcept
{
  // Alpha PC arithmetic wraps modulo 2^64; adding whole words preserves its mode bits.
  return pc + static_cast<uint64_t>(n_instr) * sizeof(uint32_t);
}

static constexpr int64_t a64_branch_displacement(uint32_t ins) noexcept
{
  const uint32_t raw = ins & 0x1fffffu;
  return (raw & 0x100000u) != 0
      ? static_cast<int64_t>(raw) - 0x200000
      : static_cast<int64_t>(raw);
}

static constexpr uint64_t a64_branch_target(uint64_t fallthrough_pc,
                                            uint32_t ins) noexcept
{
  const int64_t byte_disp = a64_branch_displacement(ins) * 4;
  return fallthrough_pc + static_cast<uint64_t>(byte_disp);
}

static constexpr uint64_t a64_jmp_target(uint64_t source_pc, uint64_t rb) noexcept
{
  return (rb & ~uint64_t(3)) | (source_pc & 3u);
}

static constexpr uint64_t a64_hw_ret_target(uint64_t rb) noexcept
{
  return rb & ~uint64_t(2);
}

enum class A64ExitKind : uint8_t {
  kNone,
  kFallthrough,
  kDirect,
  kIndirect,
  kCallPal,
  kRedispatch
};

enum class A64PcAuthority : uint8_t {
  kNextPc,
  kCpuState
};

struct A64BlockExit {
  // Deltas are added exactly once to x21, which carries prior chained-block progress.
  uint64_t fallthrough_pc = 0;
  uint32_t completed_delta = 0;
  A64ExitKind kind = A64ExitKind::kNone;

  constexpr bool valid() const noexcept { return kind != A64ExitKind::kNone; }
};

struct A64OpExit {
  uint64_t next_pc = 0;
  uint32_t completed_delta = 0;
  A64PcAuthority pc_authority = A64PcAuthority::kNextPc;
};

static constexpr A64BlockExit plan_a64_exit(uint64_t start_pc, uint32_t count,
                                            A64TerminatorKind terminator) noexcept
{
  if (count == 0) return {start_pc, 0, A64ExitKind::kNone};

  A64ExitKind kind = A64ExitKind::kFallthrough;
  switch (terminator) {
    case A64TerminatorKind::kDirect:     kind = A64ExitKind::kDirect; break;
    case A64TerminatorKind::kIndirect:   kind = A64ExitKind::kIndirect; break;
    case A64TerminatorKind::kCallPal:    kind = A64ExitKind::kCallPal; break;
    case A64TerminatorKind::kRedispatch: kind = A64ExitKind::kRedispatch; break;
    case A64TerminatorKind::kNone:       break;
  }
  return {a64_advance_pc(start_pc, count), count, kind};
}

static constexpr A64OpExit a64_retry_exit(uint64_t start_pc,
                                          uint32_t op_index) noexcept
{
  return {a64_advance_pc(start_pc, op_index), op_index, A64PcAuthority::kNextPc};
}

static constexpr A64OpExit a64_completed_trap_exit(uint32_t op_index) noexcept
{
  return {0, op_index + 1, A64PcAuthority::kCpuState};
}

static constexpr bool a64_publish_pc_before_chain(const A64BlockExit& exit,
                                                  bool pal_block) noexcept
{
  return pal_block || exit.kind == A64ExitKind::kCallPal
      || exit.kind == A64ExitKind::kRedispatch;
}

enum class A64ChainGate : uint8_t {
  kNone,
  kPollAll,
  kDeferInterrupt,
  kPollInterruptOnNativeTarget
};

enum class A64LinkVariantPolicy : uint8_t {
  kNone,
  kAlways,
  kIfTargetPal
};

static constexpr uint32_t kA64MaxBlockOps = 64;

// Local policy for the shared LinkSlot protocol.
struct A64DirectChainContract {
  A64ExitKind kind = A64ExitKind::kNone;
  uint32_t completed_delta = 0;
  A64ChainGate gate = A64ChainGate::kNone;
  A64PcAuthority bailout_pc = A64PcAuthority::kNextPc;
  bool source_pal_guard = false;
  bool target_pal_guard = false;
  bool request_patch_on_slot_miss = false;
  bool self_loop_candidate = false;
  bool publish_pc_before_probe = false;
  bool source_pal_shadow = false;

  constexpr bool eligible() const noexcept {
    return kind == A64ExitKind::kFallthrough
        || kind == A64ExitKind::kDirect
        || kind == A64ExitKind::kCallPal
        || kind == A64ExitKind::kRedispatch;
  }

  constexpr bool valid() const noexcept {
    if (kind == A64ExitKind::kNone || completed_delta == 0
        || completed_delta > kA64MaxBlockOps
        || bailout_pc != A64PcAuthority::kNextPc)
      return false;

    if (!eligible()) {
      if (kind != A64ExitKind::kIndirect)
        return false;
      return gate == A64ChainGate::kNone
          && !source_pal_guard && !target_pal_guard
          && !request_patch_on_slot_miss && !self_loop_candidate
          && !publish_pc_before_probe && !source_pal_shadow;
    }

    return gate == (source_pal_guard ? A64ChainGate::kDeferInterrupt
                                     : A64ChainGate::kPollAll)
        && target_pal_guard
            == (kind == A64ExitKind::kCallPal && !source_pal_guard)
        && request_patch_on_slot_miss
        && self_loop_candidate == (kind != A64ExitKind::kFallthrough)
        && publish_pc_before_probe
            == (source_pal_guard || target_pal_guard
                || kind == A64ExitKind::kRedispatch)
        && (source_pal_guard || !source_pal_shadow);
  }
};

static constexpr A64DirectChainContract plan_a64_direct_chain(
    const A64BlockExit& exit, bool pal_block, bool pal_shadow) noexcept
{
  A64DirectChainContract contract{};
  contract.kind = exit.kind;
  contract.completed_delta = exit.completed_delta;
  if (!contract.eligible()) return contract;

  contract.gate = pal_block ? A64ChainGate::kDeferInterrupt
                            : A64ChainGate::kPollAll;
  contract.source_pal_guard = pal_block;
  contract.target_pal_guard = exit.kind == A64ExitKind::kCallPal && !pal_block;
  contract.request_patch_on_slot_miss = true;
  // x64 sends every non-fallthrough direct exit through the same branch-tail
  // self-link check, including CALL_PAL and I_CTL redispatch.
  contract.self_loop_candidate = exit.kind != A64ExitKind::kFallthrough;
  contract.publish_pc_before_probe = a64_publish_pc_before_chain(exit, pal_block);
  contract.source_pal_shadow = pal_block && pal_shadow;
  return contract;
}

enum class A64IndirectKind : uint8_t {
  kNone,
  kJmp,
  kHwRet,
  kInvalid
};

struct A64IndirectChainContract {
  A64IndirectKind kind = A64IndirectKind::kNone;
  uint32_t completed_delta = 0;
  A64ChainGate gate = A64ChainGate::kNone;
  A64LinkVariantPolicy target_variant = A64LinkVariantPolicy::kNone;
  bool source_pal_guard = false;
  bool source_pal_shadow = false;
  bool probe_pic = false;
  bool call_resolver_on_miss = false;
  bool publish_pc_before_probe = false;
  bool publish_pc_on_miss = false;

  constexpr bool eligible() const noexcept {
    return kind == A64IndirectKind::kJmp || kind == A64IndirectKind::kHwRet;
  }

  constexpr bool valid() const noexcept {
    if (kind == A64IndirectKind::kNone)
      return completed_delta == 0 && gate == A64ChainGate::kNone
          && target_variant == A64LinkVariantPolicy::kNone
          && !source_pal_guard && !source_pal_shadow && !probe_pic
          && !call_resolver_on_miss && !publish_pc_before_probe
          && !publish_pc_on_miss;
    if (!eligible() || (kind == A64IndirectKind::kHwRet && !source_pal_guard)
        || completed_delta == 0
        || completed_delta > kA64MaxBlockOps)
      return false;

    const A64ChainGate expected_gate = !source_pal_guard
        ? A64ChainGate::kPollAll
        : kind == A64IndirectKind::kHwRet
            ? A64ChainGate::kPollInterruptOnNativeTarget
            : A64ChainGate::kDeferInterrupt;
    const A64LinkVariantPolicy expected_variant =
        kind == A64IndirectKind::kHwRet
            ? A64LinkVariantPolicy::kIfTargetPal
            : A64LinkVariantPolicy::kNone;
    return gate == expected_gate && target_variant == expected_variant
        && (source_pal_guard || !source_pal_shadow)
        && probe_pic && call_resolver_on_miss
        && publish_pc_before_probe == source_pal_guard
        && publish_pc_on_miss;
  }
};

static constexpr A64IndirectChainContract plan_a64_indirect_chain(
    const A64BlockExit& exit, uint32_t terminator_ins,
    bool pal_block, bool pal_shadow) noexcept
{
  A64IndirectChainContract contract{};
  if (exit.kind != A64ExitKind::kIndirect) return contract;

  contract.completed_delta = exit.completed_delta;
  switch (terminator_ins >> 26) {
    case 0x1a: contract.kind = A64IndirectKind::kJmp; break;
    case 0x1e:
      contract.kind = pal_block ? A64IndirectKind::kHwRet
                                : A64IndirectKind::kInvalid;
      if (!pal_block) return contract;
      break;
    default:   contract.kind = A64IndirectKind::kInvalid; return contract;
  }
  contract.gate = !pal_block
      ? A64ChainGate::kPollAll
      : contract.kind == A64IndirectKind::kHwRet
          ? A64ChainGate::kPollInterruptOnNativeTarget
          : A64ChainGate::kDeferInterrupt;
  contract.target_variant = contract.kind == A64IndirectKind::kHwRet
      ? A64LinkVariantPolicy::kIfTargetPal
      : A64LinkVariantPolicy::kNone;
  contract.source_pal_guard = pal_block;
  contract.source_pal_shadow = pal_block && pal_shadow;
  contract.probe_pic = true;
  contract.call_resolver_on_miss = true;
  contract.publish_pc_before_probe = pal_block;
  contract.publish_pc_on_miss = true;
  return contract;
}

static constexpr uint64_t kA64LinkPalShadowBit = uint64_t(1) << 63;
static constexpr uint64_t kA64LinkEpochMask = ~kA64LinkPalShadowBit;
static constexpr uint64_t kA64InvalidLinkTag = ~uint64_t(0);

static constexpr uint64_t a64_pack_link_vgen(uint64_t epoch,
                                             bool pal_shadow) noexcept
{
  return (epoch & kA64LinkEpochMask)
      | (pal_shadow ? kA64LinkPalShadowBit : 0);
}

static constexpr uint64_t a64_link_epoch(uint64_t packed_vgen) noexcept
{
  return packed_vgen & kA64LinkEpochMask;
}

static constexpr bool a64_link_pal_shadow(uint64_t packed_vgen) noexcept
{
  return (packed_vgen & kA64LinkPalShadowBit) != 0;
}

struct A64LinkProbe {
  uint64_t tag = kA64InvalidLinkTag;
  uint64_t packed_vgen = 0;
  bool body_present = false;

  constexpr bool matches(uint64_t target, uint64_t current_epoch,
                         A64LinkVariantPolicy variant_policy,
                         bool live_pal_shadow) const noexcept {
    if (!body_present || tag != target
        || a64_link_epoch(packed_vgen)
            != (current_epoch & kA64LinkEpochMask))
      return false;
    switch (variant_policy) {
      case A64LinkVariantPolicy::kNone:
        return true;
      case A64LinkVariantPolicy::kAlways:
        return a64_link_pal_shadow(packed_vgen) == live_pal_shadow;
      case A64LinkVariantPolicy::kIfTargetPal:
        return (target & 1u) == 0
            || a64_link_pal_shadow(packed_vgen) == live_pal_shadow;
    }
    return false;
  }
};

static constexpr int a64_find_link_probe(
    const std::array<A64LinkProbe, CJitEngine::kLinkSlots>& slots,
    uint64_t target, uint64_t current_epoch,
    A64LinkVariantPolicy variant_policy,
    bool live_pal_shadow) noexcept
{
  for (uint32_t i = 0; i < slots.size(); ++i)
    if (slots[i].matches(target, current_epoch, variant_policy,
                         live_pal_shadow))
      return static_cast<int>(i);
  return -1;
}

static_assert(sizeof(void*) == 8
              && std::is_standard_layout<CJitEngine::LinkSlot>::value
              && offsetof(CJitEngine::LinkSlot, tag) == 0
              && offsetof(CJitEngine::LinkSlot, vgen) == 8
              && offsetof(CJitEngine::LinkSlot, body) == 16
              && sizeof(CJitEngine::LinkSlot) == 24
              && alignof(CJitEngine::LinkSlot) >= 8
              && CJitEngine::kLinkSlots == 2,
              "A64 cached links must match the shared two-slot snapshot ABI");

static constexpr bool a64_direct_chain_contract_probe() noexcept
{
  const A64BlockExit none = plan_a64_exit(0x1000, 0, A64TerminatorKind::kNone);
  const A64BlockExit fall = plan_a64_exit(0x1000, 2, A64TerminatorKind::kNone);
  const A64BlockExit direct =
      plan_a64_exit(0x1000, 1, A64TerminatorKind::kDirect);
  const A64BlockExit indirect =
      plan_a64_exit(0x1000, 1, A64TerminatorKind::kIndirect);
  const A64BlockExit call_pal =
      plan_a64_exit(0x1000, 1, A64TerminatorKind::kCallPal);
  const A64BlockExit redispatch =
      plan_a64_exit(0x1000, 1, A64TerminatorKind::kRedispatch);

  const A64DirectChainContract native_fall =
      plan_a64_direct_chain(fall, false, false);
  const A64DirectChainContract native_direct =
      plan_a64_direct_chain(direct, false, false);
  const A64DirectChainContract native_call =
      plan_a64_direct_chain(call_pal, false, false);
  const A64DirectChainContract pal_fall =
      plan_a64_direct_chain(fall, true, false);
  const A64DirectChainContract pal_direct =
      plan_a64_direct_chain(direct, true, true);
  const A64DirectChainContract pal_call =
      plan_a64_direct_chain(call_pal, true, true);
  const A64DirectChainContract pal_redispatch =
      plan_a64_direct_chain(redispatch, true, true);
  const A64DirectChainContract dynamic =
      plan_a64_direct_chain(indirect, false, false);
  const A64DirectChainContract native_redispatch =
      plan_a64_direct_chain(redispatch, false, false);
  const A64DirectChainContract empty =
      plan_a64_direct_chain(none, false, false);

  A64DirectChainContract malformed = native_direct;
  malformed.request_patch_on_slot_miss = false;
  A64DirectChainContract zero_count = native_direct;
  zero_count.completed_delta = 0;
  A64DirectChainContract max_count = native_direct;
  max_count.completed_delta = kA64MaxBlockOps;
  A64DirectChainContract unknown_gate = native_direct;
  unknown_gate.gate = static_cast<A64ChainGate>(0xff);
  A64DirectChainContract native_shadow = native_direct;
  native_shadow.source_pal_shadow = true;
  A64DirectChainContract missing_target_guard = native_call;
  missing_target_guard.target_pal_guard = false;
  A64DirectChainContract inactive_patch = dynamic;
  inactive_patch.request_patch_on_slot_miss = true;
  const A64DirectChainContract oversized =
      plan_a64_direct_chain(
          {0x1104, kA64MaxBlockOps + 1, A64ExitKind::kDirect}, false, false);
  const A64DirectChainContract unknown =
      plan_a64_direct_chain({0x1104, 1, static_cast<A64ExitKind>(0xff)}, false, false);

  return native_fall.valid() && native_fall.eligible()
      && native_fall.gate == A64ChainGate::kPollAll
      && native_fall.request_patch_on_slot_miss
      && !native_fall.self_loop_candidate
      && !native_fall.publish_pc_before_probe
      && native_direct.valid() && native_direct.self_loop_candidate
      && native_direct.gate == A64ChainGate::kPollAll
      && native_call.valid() && native_call.target_pal_guard
      && native_call.self_loop_candidate && native_call.publish_pc_before_probe
      && !native_call.source_pal_guard
      && pal_fall.valid() && pal_fall.source_pal_guard
      && !pal_fall.source_pal_shadow
      && pal_fall.gate == A64ChainGate::kDeferInterrupt
      && pal_fall.publish_pc_before_probe
      && pal_direct.valid() && pal_direct.source_pal_guard
      && pal_direct.source_pal_shadow && pal_direct.self_loop_candidate
      && pal_call.valid() && pal_call.source_pal_guard
      && !pal_call.target_pal_guard && pal_call.self_loop_candidate
      && pal_call.publish_pc_before_probe
      && pal_redispatch.valid() && pal_redispatch.eligible()
      && pal_redispatch.gate == A64ChainGate::kDeferInterrupt
      && pal_redispatch.source_pal_guard
      && pal_redispatch.source_pal_shadow
      && !pal_redispatch.target_pal_guard
      && pal_redispatch.request_patch_on_slot_miss
      && pal_redispatch.self_loop_candidate
      && pal_redispatch.publish_pc_before_probe
      && dynamic.valid() && !dynamic.eligible()
      && !dynamic.request_patch_on_slot_miss
      && native_redispatch.valid() && native_redispatch.eligible()
      && native_redispatch.gate == A64ChainGate::kPollAll
      && !native_redispatch.source_pal_guard
      && !native_redispatch.target_pal_guard
      && native_redispatch.request_patch_on_slot_miss
      && native_redispatch.self_loop_candidate
      && native_redispatch.publish_pc_before_probe
      && !empty.valid() && !malformed.valid()
      && !zero_count.valid() && max_count.valid()
      && !unknown_gate.valid() && !native_shadow.valid()
      && !missing_target_guard.valid() && !inactive_patch.valid()
      && !oversized.valid()
      && !unknown.valid();
}

static constexpr bool a64_indirect_chain_contract_probe() noexcept
{
  constexpr uint32_t jmp = 0x1au << 26;
  constexpr uint32_t hw_ret = 0x1eu << 26;
  const A64BlockExit indirect =
      plan_a64_exit(0x1001, 1, A64TerminatorKind::kIndirect);
  const A64BlockExit fall =
      plan_a64_exit(0x1001, 1, A64TerminatorKind::kNone);

  const A64IndirectChainContract native_jmp =
      plan_a64_indirect_chain(indirect, jmp, false, false);
  const A64IndirectChainContract native_hw =
      plan_a64_indirect_chain(indirect, hw_ret, false, false);
  const A64IndirectChainContract pal_jmp =
      plan_a64_indirect_chain(indirect, jmp, true, false);
  const A64IndirectChainContract pal_hw =
      plan_a64_indirect_chain(indirect, hw_ret, true, true);
  const A64IndirectChainContract inactive =
      plan_a64_indirect_chain(fall, jmp, false, false);
  const A64IndirectChainContract unknown =
      plan_a64_indirect_chain(indirect, 0x10u << 26, false, false);

  A64IndirectChainContract zero_count = native_jmp;
  zero_count.completed_delta = 0;
  A64IndirectChainContract max_count = native_jmp;
  max_count.completed_delta = kA64MaxBlockOps;
  A64IndirectChainContract oversized = native_jmp;
  oversized.completed_delta = kA64MaxBlockOps + 1;
  A64IndirectChainContract wrong_gate = pal_hw;
  wrong_gate.gate = A64ChainGate::kDeferInterrupt;
  A64IndirectChainContract wrong_variant = pal_hw;
  wrong_variant.target_variant = A64LinkVariantPolicy::kAlways;
  A64IndirectChainContract no_resolver = native_jmp;
  no_resolver.call_resolver_on_miss = false;
  A64IndirectChainContract bad_kind = native_jmp;
  bad_kind.kind = static_cast<A64IndirectKind>(0xff);

  return native_jmp.valid() && native_jmp.eligible()
      && native_jmp.kind == A64IndirectKind::kJmp
      && native_jmp.gate == A64ChainGate::kPollAll
      && native_jmp.target_variant == A64LinkVariantPolicy::kNone
      && !native_jmp.publish_pc_before_probe
      && !native_hw.valid()
      && pal_jmp.valid() && pal_jmp.source_pal_guard
      && !pal_jmp.source_pal_shadow
      && pal_jmp.gate == A64ChainGate::kDeferInterrupt
      && pal_jmp.publish_pc_before_probe
      && pal_hw.valid() && pal_hw.source_pal_guard
      && pal_hw.source_pal_shadow
      && pal_hw.gate == A64ChainGate::kPollInterruptOnNativeTarget
      && pal_hw.target_variant == A64LinkVariantPolicy::kIfTargetPal
      && pal_hw.publish_pc_before_probe
      && inactive.valid() && !inactive.eligible()
      && !unknown.valid() && !zero_count.valid() && max_count.valid()
      && !oversized.valid() && !wrong_gate.valid() && !wrong_variant.valid()
      && !no_resolver.valid() && !bad_kind.valid();
}

static constexpr bool a64_link_probe_contract_probe() noexcept
{
  constexpr uint64_t native_target = 0x2000;
  constexpr uint64_t pal_target = 0x2001;
  constexpr uint64_t epoch = 0x12345678;
  const std::array<A64LinkProbe, CJitEngine::kLinkSlots> native_slots{{
      {0x3000, a64_pack_link_vgen(epoch, false), true},
      {native_target, a64_pack_link_vgen(epoch, true), true}}};
  const std::array<A64LinkProbe, CJitEngine::kLinkSlots> pal_slots{{
      {pal_target, a64_pack_link_vgen(epoch, false), true},
      {pal_target, a64_pack_link_vgen(epoch, true), true}}};
  const std::array<A64LinkProbe, CJitEngine::kLinkSlots> empty_slots{{
      {pal_target, a64_pack_link_vgen(epoch, true), false},
      {kA64InvalidLinkTag, 0, false}}};

  return a64_pack_link_vgen(epoch, false) == epoch
      && a64_pack_link_vgen(UINT64_MAX, false) == kA64LinkEpochMask
      && a64_pack_link_vgen(UINT64_MAX, true) == UINT64_MAX
      && a64_link_epoch(a64_pack_link_vgen(epoch, true)) == epoch
      && a64_link_pal_shadow(a64_pack_link_vgen(epoch, true))
      && !a64_link_pal_shadow(a64_pack_link_vgen(epoch, false))
      // A native target ignores the packed PAL variant; slot zero misses by tag.
      && a64_find_link_probe(
          native_slots, native_target, epoch,
          A64LinkVariantPolicy::kNone, false) == 1
      && a64_find_link_probe(native_slots, 0x4000, epoch,
          A64LinkVariantPolicy::kNone, false) == -1
      // A PAL target skips the wrong variant in slot zero and reaches slot one.
      && a64_find_link_probe(pal_slots, pal_target, epoch,
          A64LinkVariantPolicy::kAlways, true) == 1
      && a64_find_link_probe(pal_slots, pal_target, epoch,
          A64LinkVariantPolicy::kAlways, false) == 0
      // HW_RET ignores the variant for a native target, but enforces it in PALmode.
      && a64_find_link_probe(native_slots, native_target, epoch,
          A64LinkVariantPolicy::kIfTargetPal, false) == 1
      && a64_find_link_probe(pal_slots, pal_target, epoch,
          A64LinkVariantPolicy::kIfTargetPal, true) == 1
      && a64_find_link_probe(
          pal_slots, pal_target, epoch + 1,
          A64LinkVariantPolicy::kAlways, true) == -1
      && a64_find_link_probe(
          empty_slots, pal_target, epoch,
          A64LinkVariantPolicy::kAlways, true) == -1
      && a64_find_link_probe(pal_slots, pal_target, epoch,
          static_cast<A64LinkVariantPolicy>(0xff), true) == -1;
}

static_assert(a64_direct_chain_contract_probe(),
              "A64 direct-chain policy must gate, guard, and route every exit kind consistently");
static_assert(a64_indirect_chain_contract_probe(),
              "A64 indirect-chain policy must distinguish JMP and PAL-return gates");
static_assert(a64_link_probe_contract_probe(),
              "A64 link probes must reject stale, absent, mistagged, and wrong-variant snapshots");

struct A64BlockPlan {
  static constexpr uint32_t kMaxOps = kA64MaxBlockOps;

  std::array<A64DecodedOp, kMaxOps> ops{};
  A64GprUsage gpr_usage{};
  uint32_t count = 0;
  uint32_t breaker_word = 0;
  A64PlanStop stop = A64PlanStop::kInvalidSource;
  A64TerminatorKind terminator = A64TerminatorKind::kNone;
};

enum class A64EmitState : uint8_t {
  kUnhandled,
  kComplete
};

enum class A64Mainline : uint8_t {
  kNone,
  kContinues,
  kExitReady
};

static constexpr bool a64_known_terminator(A64TerminatorKind kind) noexcept
{
  switch (kind) {
    case A64TerminatorKind::kNone:
    case A64TerminatorKind::kDirect:
    case A64TerminatorKind::kIndirect:
    case A64TerminatorKind::kCallPal:
    case A64TerminatorKind::kRedispatch:
      return true;
  }
  return false;
}

static constexpr bool a64_supported_op_kind(A64OpKind kind) noexcept
{
  switch (kind) {
    case A64OpKind::kUnsupported:
    case A64OpKind::kValidationProbe:
      return false;
  }
  return false;
}

static constexpr bool a64_validation_op_kind(A64OpKind kind) noexcept
{
  return kind == A64OpKind::kValidationProbe;
}

// A nonempty prefix may legitimately stop before an unsupported instruction.
// Dispatch back at start_pc + count * 4.
static constexpr bool a64_plan_shape_valid(const A64BlockPlan& plan) noexcept
{
  if (plan.count == 0 || plan.count > plan.ops.size()
      || !a64_known_terminator(plan.terminator))
    return false;

  const bool has_terminator = plan.terminator != A64TerminatorKind::kNone;
  if (has_terminator != (plan.stop == A64PlanStop::kTerminator)) return false;
  if (!has_terminator
      && plan.stop != A64PlanStop::kUnsupported
      && plan.stop != A64PlanStop::kBlockEnd
      && plan.stop != A64PlanStop::kPageBoundary
      && plan.stop != A64PlanStop::kInstructionLimit)
    return false;
  if (plan.stop == A64PlanStop::kInstructionLimit
      && plan.count != A64BlockPlan::kMaxOps)
    return false;

  for (uint32_t i = 0; i < plan.count; ++i) {
    const A64DecodedOp& op = plan.ops[i];
    if (!a64_known_terminator(op.terminator))
      return false;
    const A64TerminatorKind expected = i + 1 == plan.count
        ? plan.terminator : A64TerminatorKind::kNone;
    if (op.terminator != expected) return false;
  }
  return true;
}

static constexpr bool a64_plan_ready_for_emission(
    const A64BlockPlan& plan) noexcept
{
  if (!a64_plan_shape_valid(plan)) return false;
  for (uint32_t i = 0; i < plan.count; ++i)
    if (!a64_supported_op_kind(plan.ops[i].kind)) return false;
  return true;
}

static constexpr bool a64_plan_ready_for_validation(
    const A64BlockPlan& plan) noexcept
{
  if (!a64_plan_shape_valid(plan)) return false;
  for (uint32_t i = 0; i < plan.count; ++i)
    if (!a64_validation_op_kind(plan.ops[i].kind)) return false;
  return true;
}

struct A64OpEmitReceipt {
  asmjit::Error error = asmjit::Error::kInvalidInstruction;
  A64OpKind kind = A64OpKind::kUnsupported;
  A64EmitState state = A64EmitState::kUnhandled;
  A64Mainline mainline = A64Mainline::kNone;
  A64TerminatorKind terminator = A64TerminatorKind::kNone;

  constexpr bool accepted_for(const A64DecodedOp& op,
                              bool validation_probe = false) const noexcept {
    const A64Mainline expected = op.terminator == A64TerminatorKind::kNone
        ? A64Mainline::kContinues : A64Mainline::kExitReady;
    const bool known_kind = a64_supported_op_kind(op.kind)
        || (validation_probe && a64_validation_op_kind(op.kind));
    return known_kind
        && error == asmjit::Error::kOk
        && kind == op.kind
        && state == A64EmitState::kComplete
        && mainline == expected
        && terminator == op.terminator;
  }
};

struct A64BodyEmitReceipt {
  asmjit::Error error = asmjit::Error::kInvalidState;
  uint32_t planned_count = 0;
  uint32_t attempted_count = 0;
  uint32_t emitted_count = 0;
  A64TerminatorKind observed_terminator = A64TerminatorKind::kNone;
  bool validation_probe = false;
  bool complete = false;

  constexpr bool complete_for(const A64BlockPlan& plan) const noexcept {
    return error == asmjit::Error::kOk && complete && !validation_probe
        && planned_count == plan.count
        && attempted_count == plan.count && emitted_count == plan.count
        && observed_terminator == plan.terminator
        && a64_plan_ready_for_emission(plan);
  }

  constexpr bool complete_for_validation(
      const A64BlockPlan& plan) const noexcept {
    return error == asmjit::Error::kOk && complete && validation_probe
        && planned_count == plan.count
        && attempted_count == plan.count && emitted_count == plan.count
        && observed_terminator == plan.terminator
        && a64_plan_ready_for_validation(plan);
  }
};

static constexpr A64BodyEmitReceipt a64_begin_body_emission(
    const A64BlockPlan& plan) noexcept
{
  A64BodyEmitReceipt receipt{};
  receipt.planned_count = plan.count;
  receipt.error = a64_plan_ready_for_emission(plan)
      ? asmjit::Error::kOk : asmjit::Error::kInvalidArgument;
  return receipt;
}

static constexpr A64BodyEmitReceipt a64_begin_body_validation(
    const A64BlockPlan& plan) noexcept
{
  A64BodyEmitReceipt receipt{};
  receipt.planned_count = plan.count;
  receipt.validation_probe = true;
  receipt.error = a64_plan_ready_for_validation(plan)
      ? asmjit::Error::kOk : asmjit::Error::kInvalidArgument;
  return receipt;
}

static constexpr bool a64_begin_op_emission(A64BodyEmitReceipt& body,
    const A64BlockPlan& plan, uint32_t index) noexcept
{
  if (body.error != asmjit::Error::kOk || body.complete
      || body.planned_count != plan.count
      || index != body.attempted_count || index != body.emitted_count
      || index >= plan.count) {
    if (body.error == asmjit::Error::kOk)
      body.error = asmjit::Error::kInvalidState;
    return false;
  }
  ++body.attempted_count;
  return true;
}

static constexpr bool a64_accept_op_emission(A64BodyEmitReceipt& body,
    const A64BlockPlan& plan, uint32_t index,
    const A64OpEmitReceipt& op) noexcept
{
  if (body.error != asmjit::Error::kOk || body.complete
      || body.planned_count != plan.count
      || index != body.emitted_count || body.attempted_count != index + 1
      || index >= plan.count) {
    if (body.error == asmjit::Error::kOk)
      body.error = asmjit::Error::kInvalidState;
    return false;
  }
  if (!op.accepted_for(plan.ops[index], body.validation_probe)) {
    body.error = op.error == asmjit::Error::kOk
        ? asmjit::Error::kInvalidState : op.error;
    return false;
  }
  if (op.terminator != A64TerminatorKind::kNone)
    body.observed_terminator = op.terminator;
  ++body.emitted_count;
  return true;
}

static constexpr bool a64_finish_body_emission(A64BodyEmitReceipt& body,
    const A64BlockPlan& plan) noexcept
{
  if (body.error != asmjit::Error::kOk || body.complete
      || body.planned_count != plan.count
      || body.attempted_count != plan.count
      || body.emitted_count != plan.count
      || body.observed_terminator != plan.terminator) {
    if (body.error == asmjit::Error::kOk)
      body.error = asmjit::Error::kInvalidState;
    return false;
  }
  body.complete = true;
  return true;
}

// Block image description.
struct A64PendingPublication {
  uint64_t code_size = 0;
  uint64_t body_off = 0;
  uint64_t source_hash = 0;
  uint64_t prior_code_bytes = 0;
  uint32_t plan_count = 0;
  uint32_t prefix_len = 0;
  uint32_t hash_len = 0;
  bool source_current = false;
  bool layout_complete = false;

  constexpr bool ready() const noexcept {
    return source_current && layout_complete
        && plan_count != 0 && plan_count <= A64BlockPlan::kMaxOps
        && prefix_len == plan_count
        && hash_len == prefix_len
        && code_size != 0 && (code_size & 3u) == 0
        && code_size <= UINT32_MAX
        && (body_off & 3u) == 0 && body_off < code_size
        && body_off <= UINT32_MAX
        && code_size <= UINT64_MAX - prior_code_bytes;
  }
};

struct A64ScanLimit {
  uint32_t count;
  A64PlanStop stop;
};

static constexpr bool a64_source_extent_valid(uint64_t phys, uint32_t words,
                                               uint64_t dram_size) noexcept
{
  return words != 0 && (phys & 3u) == 0 && phys <= dram_size
      && static_cast<uint64_t>(words)
          <= (dram_size - phys) / sizeof(uint32_t);
}

// Classification stays fail-closed: each translation will add its classifier and emitter together.
static constexpr A64OpClass classify_a64_op(uint32_t, bool) noexcept
{
  return {};
}

static constexpr A64DecodedOp decode_a64_op(uint32_t ins, bool pal_block) noexcept
{
  const A64OpClass classification = classify_a64_op(ins, pal_block);
  A64DecodedOp decoded{};
  decoded.ins = ins;
  decoded.kind = classification.kind;
  decoded.terminator = classification.terminator;
  decoded.gpr_reads = classification.gpr_reads;
  decoded.gpr_writes = classification.gpr_writes;
  decoded.opcode = static_cast<uint8_t>(ins >> 26);
  decoded.ra = static_cast<uint8_t>((ins >> 21) & 31u);
  decoded.rb = static_cast<uint8_t>((ins >> 16) & 31u);
  decoded.rc = static_cast<uint8_t>(ins & 31u);
  decoded.literal = static_cast<uint8_t>((ins >> 13) & 0xffu);
  decoded.is_literal = ((ins >> 12) & 1u) != 0;
  return decoded;
}

static constexpr A64ScanLimit a64_scan_limit(uint32_t n_instr, uint64_t phys) noexcept
{
  constexpr uint32_t kGuestPageBytes = 0x2000;
  A64ScanLimit limit{n_instr, A64PlanStop::kBlockEnd};
  if (limit.count > A64BlockPlan::kMaxOps) {
    limit.count = A64BlockPlan::kMaxOps;
    limit.stop = A64PlanStop::kInstructionLimit;
  }

  const uint32_t page_offset = static_cast<uint32_t>(phys & (kGuestPageBytes - 1));
  const uint32_t page_words = (kGuestPageBytes - page_offset) / sizeof(uint32_t);
  if (limit.count > page_words) {
    limit.count = page_words;
    limit.stop = A64PlanStop::kPageBoundary;
  }
  return limit;
}

static uint32_t load_a64_guest_u32(const uint8_t* source) noexcept
{
  return static_cast<uint32_t>(source[0])
      | (static_cast<uint32_t>(source[1]) << 8)
      | (static_cast<uint32_t>(source[2]) << 16)
      | (static_cast<uint32_t>(source[3]) << 24);
}

static A64BlockPlan plan_a64_block(const CJitEngine::JitBlock& block,
    const uint8_t* dram, uint64_t dram_size) noexcept
{
  A64BlockPlan plan{};
  if (block.n_instr == 0) {
    plan.stop = A64PlanStop::kEmptyBlock;
    return plan;
  }
  if (dram == nullptr
      || !a64_source_extent_valid(block.phys, block.n_instr, dram_size))
    return plan;

  const A64ScanLimit limit = a64_scan_limit(block.n_instr, block.phys);
  plan.stop = limit.stop;
  const uint8_t* const source = dram + static_cast<size_t>(block.phys);
  const bool pal_block = (block.tag & 1u) != 0;
  for (uint32_t i = 0; i < limit.count; ++i) {
    const uint32_t word = load_a64_guest_u32(source + static_cast<size_t>(i) * 4);
    const A64DecodedOp decoded = decode_a64_op(word, pal_block);
    if (decoded.kind == A64OpKind::kUnsupported) {
      plan.breaker_word = word;
      plan.stop = A64PlanStop::kUnsupported;
      return plan;
    }

    plan.ops[plan.count++] = decoded;
    note_a64_op_gprs(plan.gpr_usage, decoded);
    if (decoded.terminator != A64TerminatorKind::kNone) {
      plan.terminator = decoded.terminator;
      plan.stop = A64PlanStop::kTerminator;
      return plan;
    }
  }
  return plan;
}

static A64PendingPublication prepare_a64_block_publication(
    const CJitEngine::JitBlock& block, const A64BlockPlan& plan,
    const A64BlockExit& exit, const A64BodyEmitReceipt& emission,
    const uint8_t* dram, uint64_t dram_size, const asmjit::CodeHolder& code,
    const asmjit::Label& body, uint64_t prior_code_bytes) noexcept
{
  A64PendingPublication pending{};
  pending.code_size = static_cast<uint64_t>(code.code_size());
  pending.prior_code_bytes = prior_code_bytes;
  pending.plan_count = plan.count;
  pending.prefix_len = plan.count;
  // Placeholder until lookahead and fusion are brought in.
  pending.hash_len = plan.count;

  const bool body_bound = code.is_label_bound(body);
  pending.layout_complete = emission.complete_for(plan)
      && exit.valid()
      && exit.completed_delta == plan.count
      && !code.has_unresolved_fixups()
      && code.section_count() == 1
      && code.section_by_id(0)->has_offset()
      && code.section_by_id(0)->offset() == 0
      && body_bound
      && code.label_entry_of(body).section_id() == 0;
  if (body_bound) pending.body_off = code.label_offset(body);

  if (dram == nullptr
      || !a64_source_extent_valid(block.phys, pending.hash_len, dram_size))
    return pending;
  if (pending.prefix_len > plan.ops.size()
      || pending.hash_len != pending.prefix_len)
    return pending;

  const uint8_t* const source = dram + static_cast<size_t>(block.phys);
  std::array<uint32_t, A64BlockPlan::kMaxOps> current_words{};
  for (uint32_t i = 0; i < pending.prefix_len; ++i) {
    const uint32_t word =
        load_a64_guest_u32(source + static_cast<size_t>(i) * sizeof(uint32_t));
    if (word != plan.ops[i].ins)
      return pending;
    current_words[i] = word;
  }

  // Hash reads that prove snapshot current
  pending.source_hash = src_hash(
      reinterpret_cast<const uint8_t*>(current_words.data()), pending.hash_len);
  pending.source_current = true;
  return pending;
}

static constexpr uint32_t kA64DecodeProbe =
    (0x10u << 26) | (17u << 21) | (9u << 16) | 29u;
static constexpr uint32_t kA64LiteralProbe =
    (0x11u << 26) | (4u << 21) | (0xa5u << 13) | (1u << 12) | 7u;
static_assert(decode_a64_op(kA64DecodeProbe, false).opcode == 0x10
              && decode_a64_op(kA64DecodeProbe, false).ra == 17
              && decode_a64_op(kA64DecodeProbe, false).rb == 9
              && decode_a64_op(kA64DecodeProbe, false).rc == 29
              && !decode_a64_op(kA64DecodeProbe, false).is_literal,
              "A64 planning must decode Alpha register-form operands once");
static_assert(decode_a64_op(kA64LiteralProbe, false).is_literal
              && decode_a64_op(kA64LiteralProbe, false).literal == 0xa5,
              "A64 planning must decode Alpha literal operands once");
static_assert(a64_scan_limit(3, 0).count == 3
              && a64_scan_limit(3, 0).stop == A64PlanStop::kBlockEnd
              && a64_scan_limit(65, 0).count == 64
              && a64_scan_limit(65, 0).stop == A64PlanStop::kInstructionLimit
              && a64_scan_limit(3, 0x1ff8).count == 2
              && a64_scan_limit(3, 0x1ff8).stop == A64PlanStop::kPageBoundary
              && a64_scan_limit(1, 0x2000).count == 1,
              "A64 planning must honor block, instruction, and physical-page bounds");
static_assert(a64_source_extent_valid(0, 1, 4)
              && a64_source_extent_valid(4, 2, 12)
              && a64_source_extent_valid(~uint64_t(7), 1, UINT64_MAX)
              && !a64_source_extent_valid(0, 0, 4)
              && !a64_source_extent_valid(1, 1, 8)
              && !a64_source_extent_valid(8, 1, 8)
              && !a64_source_extent_valid(4, 2, 11)
              && !a64_source_extent_valid(12, 1, 8)
              && !a64_source_extent_valid(~uint64_t(7), 2, UINT64_MAX),
              "A64 source extents must reject empty, unaligned, truncated, and wrapping ranges");

static constexpr A64OpEmitReceipt a64_completed_op_receipt(
    const A64DecodedOp& op,
    asmjit::Error error = asmjit::Error::kOk) noexcept
{
  if (error != asmjit::Error::kOk)
    return {error, op.kind, A64EmitState::kUnhandled,
            A64Mainline::kNone, A64TerminatorKind::kNone};
  return {asmjit::Error::kOk, op.kind, A64EmitState::kComplete,
          op.terminator == A64TerminatorKind::kNone
              ? A64Mainline::kContinues : A64Mainline::kExitReady,
          op.terminator};
}

static constexpr bool a64_body_emission_contract_probe() noexcept
{
  A64BlockPlan linear{};
  linear.count = 3;
  linear.stop = A64PlanStop::kBlockEnd;
  for (uint32_t i = 0; i < linear.count; ++i) {
    linear.ops[i].ins = i + 1;
    linear.ops[i].kind = A64OpKind::kValidationProbe;
  }

  A64BlockPlan prefix = linear;
  prefix.count = 2;
  prefix.stop = A64PlanStop::kUnsupported;

  A64BlockPlan terminated = linear;
  terminated.stop = A64PlanStop::kTerminator;
  terminated.terminator = A64TerminatorKind::kDirect;
  terminated.ops[2].terminator = A64TerminatorKind::kDirect;

  A64BlockPlan unsupported = linear;
  unsupported.ops[1].kind = A64OpKind::kUnsupported;
  A64BlockPlan early_terminator = linear;
  early_terminator.ops[1].terminator = A64TerminatorKind::kDirect;
  A64BlockPlan missing_terminator = terminated;
  missing_terminator.ops[2].terminator = A64TerminatorKind::kNone;
  A64BlockPlan wrong_terminator = terminated;
  wrong_terminator.ops[2].terminator = A64TerminatorKind::kIndirect;
  A64BlockPlan wrong_stop = terminated;
  wrong_stop.stop = A64PlanStop::kBlockEnd;
  A64BlockPlan oversized = linear;
  oversized.count = A64BlockPlan::kMaxOps + 1;
  A64BlockPlan unknown_kind = linear;
  unknown_kind.ops[0].kind = static_cast<A64OpKind>(0x7f);

  if (a64_plan_ready_for_emission(linear)
      || !a64_plan_ready_for_validation(linear)
      || !a64_plan_ready_for_validation(prefix)
      || !a64_plan_ready_for_validation(terminated)
      || a64_plan_ready_for_emission(A64BlockPlan{})
      || a64_plan_ready_for_validation(unsupported)
      || a64_plan_ready_for_validation(early_terminator)
      || a64_plan_ready_for_validation(missing_terminator)
      || a64_plan_ready_for_validation(wrong_terminator)
      || a64_plan_ready_for_validation(wrong_stop)
      || a64_plan_ready_for_validation(oversized)
      || a64_plan_ready_for_emission(unknown_kind)
      || a64_plan_ready_for_validation(unknown_kind))
    return false;

  A64BodyEmitReceipt complete = a64_begin_body_validation(linear);
  for (uint32_t i = 0; i < linear.count; ++i) {
    if (!a64_begin_op_emission(complete, linear, i)
        || !a64_accept_op_emission(
            complete, linear, i, a64_completed_op_receipt(linear.ops[i])))
      return false;
  }
  if (!a64_finish_body_emission(complete, linear)
      || !complete.complete_for_validation(linear)
      || complete.complete_for(linear))
    return false;

  A64BodyEmitReceipt terminal = a64_begin_body_validation(terminated);
  for (uint32_t i = 0; i < terminated.count; ++i) {
    if (!a64_begin_op_emission(terminal, terminated, i)
        || !a64_accept_op_emission(
            terminal, terminated, i,
            a64_completed_op_receipt(terminated.ops[i])))
      return false;
  }
  if (!a64_finish_body_emission(terminal, terminated)
      || terminal.observed_terminator != A64TerminatorKind::kDirect
      || !terminal.complete_for_validation(terminated))
    return false;

  A64BodyEmitReceipt duplicate = a64_begin_body_validation(linear);
  if (!a64_begin_op_emission(duplicate, linear, 0)
      || a64_begin_op_emission(duplicate, linear, 0)
      || duplicate.error != asmjit::Error::kInvalidState)
    return false;

  A64BodyEmitReceipt partial = a64_begin_body_validation(linear);
  if (!a64_begin_op_emission(partial, linear, 0)
      || !a64_accept_op_emission(
          partial, linear, 0, a64_completed_op_receipt(linear.ops[0]))
      || a64_finish_body_emission(partial, linear)
      || partial.complete || partial.emitted_count != 1)
    return false;

  A64BodyEmitReceipt rejected = a64_begin_body_validation(linear);
  if (!a64_begin_op_emission(rejected, linear, 0)
      || a64_accept_op_emission(rejected, linear, 0, {})
      || rejected.error != asmjit::Error::kInvalidInstruction
      || rejected.emitted_count != 0)
    return false;

  A64BodyEmitReceipt mismatch = a64_begin_body_validation(terminated);
  A64OpEmitReceipt wrong_mainline =
      a64_completed_op_receipt(terminated.ops[0]);
  wrong_mainline.mainline = A64Mainline::kExitReady;
  if (!a64_begin_op_emission(mismatch, terminated, 0)
      || a64_accept_op_emission(mismatch, terminated, 0, wrong_mainline)
      || mismatch.error != asmjit::Error::kInvalidState)
    return false;

  return true;
}

static_assert(a64_body_emission_contract_probe(),
              "A64 body emission must be exact, ordered, terminator-aware, and fail-closed");
static_assert(A64PendingPublication{64, 16, 1, 0, 3, 3, 3, true, true}.ready()
              && A64PendingPublication{64, 16, 1, UINT64_MAX - 64,
                                       3, 3, 3, true, true}.ready()
              && !A64PendingPublication{}.ready()
              && !A64PendingPublication{64, 16, 1, 0, 3, 0, 3, true, true}.ready()
              && !A64PendingPublication{64, 16, 1, 0, 3, 2, 3, true, true}.ready()
              && !A64PendingPublication{64, 16, 1, 0, 65, 65, 65, true, true}.ready()
              && !A64PendingPublication{64, 16, 1, 0, 3, 3, 2, true, true}.ready()
              && !A64PendingPublication{64, 16, 1, 0, 3, 3, 4, true, true}.ready()
              && !A64PendingPublication{0, 0, 1, 0, 3, 3, 3, true, true}.ready()
              && !A64PendingPublication{62, 16, 1, 0, 3, 3, 3, true, true}.ready()
              && !A64PendingPublication{64, 64, 1, 0, 3, 3, 3, true, true}.ready()
              && !A64PendingPublication{64, 18, 1, 0, 3, 3, 3, true, true}.ready()
              && !A64PendingPublication{uint64_t(UINT32_MAX) + 1, 16, 1, 0,
                                         3, 3, 3, true, true}.ready()
              && !A64PendingPublication{64, 16, 1, UINT64_MAX - 63,
                                         3, 3, 3, true, true}.ready()
              && !A64PendingPublication{64, 16, 1, 0, 3, 3, 3, false, true}.ready()
              && !A64PendingPublication{64, 16, 1, 0, 3, 3, 3, true, false}.ready(),
              "A64 publication must be complete, aligned, bounded, and transactionally safe");

static constexpr bool a64_exit_contract_probe() noexcept
{
  const A64BlockExit none = plan_a64_exit(0x1001, 0, A64TerminatorKind::kNone);
  const A64BlockExit fall = plan_a64_exit(0x1001, 3, A64TerminatorKind::kNone);
  const A64BlockExit direct = plan_a64_exit(0x1001, 2, A64TerminatorKind::kDirect);
  const A64BlockExit indirect = plan_a64_exit(0x1001, 2, A64TerminatorKind::kIndirect);
  const A64BlockExit call_pal = plan_a64_exit(0x1000, 1, A64TerminatorKind::kCallPal);
  const A64BlockExit redispatch =
      plan_a64_exit(0x1001, 1, A64TerminatorKind::kRedispatch);
  const A64OpExit retry = a64_retry_exit(0x1001, 63);
  const A64OpExit trapped = a64_completed_trap_exit(63);

  return !none.valid() && none.completed_delta == 0 && none.fallthrough_pc == 0x1001
      && fall.kind == A64ExitKind::kFallthrough
      && fall.completed_delta == 3 && fall.fallthrough_pc == 0x100d
      && direct.kind == A64ExitKind::kDirect
      && indirect.kind == A64ExitKind::kIndirect
      && call_pal.kind == A64ExitKind::kCallPal
      && redispatch.kind == A64ExitKind::kRedispatch
      && redispatch.completed_delta == 1
      && redispatch.fallthrough_pc == 0x1005
      && retry.completed_delta == 63 && retry.next_pc == 0x10fd
      && retry.pc_authority == A64PcAuthority::kNextPc
      && trapped.completed_delta == 64
      && trapped.pc_authority == A64PcAuthority::kCpuState
      && !a64_publish_pc_before_chain(fall, false)
      && a64_publish_pc_before_chain(fall, true)
      && a64_publish_pc_before_chain(call_pal, false)
      && a64_publish_pc_before_chain(redispatch, false);
}

static_assert(a64_advance_pc(~uint64_t(0) - 2, 1) == 1
              && a64_branch_displacement(0x000fffffu) == 1048575
              && a64_branch_displacement(0x00100000u) == -1048576
              && a64_branch_displacement(0x001fffffu) == -1
              && a64_branch_target(0x1005, 0x001fffffu) == 0x1001
              && a64_jmp_target(0x1001, 0x2007) == 0x2005
              && a64_hw_ret_target(0x2007) == 0x2005,
              "A64 exit planning must preserve Alpha PC arithmetic and target masks");
static_assert(a64_exit_contract_probe(),
              "A64 exits must distinguish normal, retry, trap, and terminator bookkeeping");

static constexpr bool a64_gpr_usage_probe() noexcept
{
  A64GprUsage usage{};
  A64DecodedOp register_form{};
  register_form.ra = 1;
  register_form.rb = 1;
  register_form.rc = 2;
  register_form.gpr_reads = kA64GprRa | kA64GprRb;
  register_form.gpr_writes = kA64GprRc;
  note_a64_op_gprs(usage, register_form);

  // Operand roles, not bit 12 by itself, decide whether Rb is a register.
  A64DecodedOp literal_form{};
  literal_form.ra = 2;
  literal_form.rb = 16;
  literal_form.rc = 31;
  literal_form.is_literal = true;
  literal_form.gpr_reads = kA64GprRa;
  literal_form.gpr_writes = kA64GprRc;
  note_a64_op_gprs(usage, literal_form);

  return usage.refs[1] == 2 && usage.refs[2] == 2
      && usage.refs[16] == 0 && usage.refs[31] == 0
      && usage.reads == ((1u << 1) | (1u << 2))
      && usage.writes == (1u << 2);
}

static_assert(a64_gpr_usage_probe(),
              "A64 GPR analysis must follow semantic roles and exclude architectural R31");

static constexpr uint32_t a64_guest_gpr_slot(uint32_t raw_reg, bool pal_shadow) noexcept
{
  const uint32_t reg = raw_reg & 31u;
  return reg + ((pal_shadow && CJitEngine::RegAlloc::guest_is_shadowable(reg)) ? 32u : 0u);
}

enum class A64GprRouteKind : uint8_t {
  kInvalid,
  kMemory,
  kPinned,
  kZero,
  kDiscard
};

struct A64GprRoute {
  A64GprRouteKind kind;
  uint8_t slot;
  int8_t host;
};

// A route selects operand storage only. In particular, kDiscard suppresses the
// R31 destination write, never the instruction's faults or other side effects.

static constexpr A64GprRoute a64_guest_gpr_read_route(
    const CJitEngine::RegAlloc& regs, uint32_t raw_reg, bool pal_shadow) noexcept
{
  if (raw_reg >= 32) return {A64GprRouteKind::kInvalid, 0, -1};
  const uint32_t reg = raw_reg;
  if (reg == 31) return {A64GprRouteKind::kZero, 31, -1};
  const uint8_t slot = static_cast<uint8_t>(a64_guest_gpr_slot(reg, pal_shadow));
  const int host = regs.host_of(reg, pal_shadow);
  return host < 0 ? A64GprRoute{A64GprRouteKind::kMemory, slot, -1}
                  : A64GprRoute{A64GprRouteKind::kPinned, slot,
                                static_cast<int8_t>(host)};
}

static constexpr A64GprRoute a64_guest_gpr_write_route(
    const CJitEngine::RegAlloc& regs, uint32_t raw_reg, bool pal_shadow) noexcept
{
  if (raw_reg >= 32) return {A64GprRouteKind::kInvalid, 0, -1};
  const uint32_t reg = raw_reg;
  if (reg == 31) return {A64GprRouteKind::kDiscard, 31, -1};
  const uint8_t slot = static_cast<uint8_t>(a64_guest_gpr_slot(reg, pal_shadow));
  const int host = regs.host_of(reg, pal_shadow);
  return host < 0 ? A64GprRoute{A64GprRouteKind::kMemory, slot, -1}
                  : A64GprRoute{A64GprRouteKind::kPinned, slot,
                                static_cast<int8_t>(host)};
}

static constexpr bool a64_gpr_route_probe() noexcept
{
  CJitEngine::RegAlloc regs{};
  if (regs.host_of(1, false) != -1
      || !regs.bind_guest(1)
      || !regs.bind_guest(16)
      || regs.bind_guest(1)
      || regs.bind_guest(22)
      || regs.bind_guest(31)
      || !regs.bind_guest(0)
      || !regs.bind_guest(2)
      || !regs.bind_guest(3)
      || !regs.bind_guest(30)
      || regs.bind_guest(27)) return false;

  const A64GprRoute r1 = a64_guest_gpr_read_route(regs, 1, false);
  const A64GprRoute r16 = a64_guest_gpr_read_route(regs, 16, false);
  const A64GprRoute p22 = a64_guest_gpr_read_route(regs, 22, true);
  const A64GprRoute r24 = a64_guest_gpr_read_route(regs, 24, true);
  const A64GprRoute r31 = a64_guest_gpr_read_route(regs, 31, false);
  const A64GprRoute w31 = a64_guest_gpr_write_route(regs, 31, false);
  const A64GprRoute bad = a64_guest_gpr_read_route(regs, 32, false);
  return regs.guest_of_pin(0) == 1 && regs.guest_of_pin(1) == 16
      && r1.kind == A64GprRouteKind::kPinned
      && r1.host == CJitEngine::RegAlloc::kGuestPin0.id()
      && r16.kind == A64GprRouteKind::kPinned
      && r16.host == CJitEngine::RegAlloc::kGuestPin1.id()
      && p22.kind == A64GprRouteKind::kMemory && p22.slot == 54
      && r24.kind == A64GprRouteKind::kMemory && r24.slot == 24
      && r31.kind == A64GprRouteKind::kZero
      && w31.kind == A64GprRouteKind::kDiscard
      && bad.kind == A64GprRouteKind::kInvalid
      && CJitEngine::RegAlloc::guest_pin_candidate(3)
      && !CJitEngine::RegAlloc::guest_pin_candidate(4)
      && !CJitEngine::RegAlloc::guest_pin_candidate(20)
      && CJitEngine::RegAlloc::guest_pin_candidate(24)
      && !CJitEngine::RegAlloc::guest_pin_candidate(31);
}

static_assert(a64_gpr_route_probe(),
              "A64 GPR routing must preserve pin, PAL-shadow, and R31 semantics");

static constexpr asmjit::a64::Mem a64_guest_gpr_mem(uint32_t raw_reg,
                                                     bool pal_shadow) noexcept
{
  return asmjit::a64::ptr(CJitEngine::RegAlloc::kRegs,
      static_cast<int32_t>(a64_guest_gpr_slot(raw_reg, pal_shadow) * sizeof(uint64_t)));
}

static_assert(a64_guest_gpr_slot(3, true) == 3
              && a64_guest_gpr_slot(4, true) == 36
              && a64_guest_gpr_slot(23, true) == 55
              && a64_guest_gpr_slot(24, true) == 24
              && a64_guest_gpr_slot(23, false) == 23,
              "A64 guest operands must match the PAL-shadow register mapping");
static_assert(a64_guest_gpr_mem(23, true).base_id() == CJitEngine::RegAlloc::kRegs.id()
              && a64_guest_gpr_mem(23, true).offset() == 55 * 8,
              "A64 guest GPR operands must be fixed offsets from the register-bank base");

static constexpr bool a64_block_pin_contract_probe() noexcept
{
  const CJitEngine::RegAlloc regs = make_a64_block_regalloc();
  return regs.pin_count == kA64BlockGuestPins.size()
      && regs.guest_of_pin(0) == 1 && regs.host_of(1, false) == 23
      && regs.guest_of_pin(1) == 16 && regs.host_of(16, false) == 24
      && regs.guest_of_pin(2) == 30 && regs.host_of(30, false) == 25
      && regs.guest_of_pin(3) == 22 && regs.host_of(22, false) == 26
      && regs.guest_of_pin(4) == 23 && regs.host_of(23, false) == 27
      && regs.guest_of_pin(5) == 27 && regs.host_of(27, false) == 28
      && regs.host_of(22, true) == -1 && regs.host_of(23, true) == -1
      && a64_guest_gpr_slot(22, true) == 54
      && a64_guest_gpr_slot(23, true) == 55
      && !CJitEngine::RegAlloc::guest_pin_candidate(22)
      && !CJitEngine::RegAlloc::guest_pin_candidate(23);
}

static_assert(a64_block_pin_contract_probe(),
              "A64 block bodies must share one PAL-safe guest-pin convention");

static asmjit::Error emit_a64_load_guest_pins(asmjit::a64::Assembler& a,
                                               const CJitEngine::RegAlloc& regs)
{
  using namespace asmjit;
  for (uint32_t i = 0; i < regs.pin_count; ++i) {
    const int guest = regs.guest_of_pin(i);
    if (guest < 0) return Error::kInvalidArgument;
    const Error err = a.ldr(a64::x(CJitEngine::RegAlloc::kGuestPin0.id() + i),
                            a64_guest_gpr_mem(static_cast<uint32_t>(guest), false));
    if (err != Error::kOk) return err;
  }
  return Error::kOk;
}

static asmjit::Error emit_a64_sync_guest_pins(asmjit::a64::Assembler& a,
                                               const CJitEngine::RegAlloc& regs)
{
  using namespace asmjit;
  for (uint32_t i = 0; i < regs.pin_count; ++i) {
    const int guest = regs.guest_of_pin(i);
    if (guest < 0) return Error::kInvalidArgument;
    const Error err = a.str(a64::x(CJitEngine::RegAlloc::kGuestPin0.id() + i),
                            a64_guest_gpr_mem(static_cast<uint32_t>(guest), false));
    if (err != Error::kOk) return err;
  }
  return Error::kOk;
}

static bool a64_is_real_x(const asmjit::a64::Gp& reg) noexcept
{
  return reg.is_gp64() && reg.id() < asmjit::a64::Gp::kIdSp;
}

static bool a64_is_address_x(const asmjit::a64::Gp& reg) noexcept
{
  return reg.is_gp64() && reg.id() <= asmjit::a64::Gp::kIdSp;
}

// AsmJit's MOV pseudo-op selects a logical immediate or the shortest MOVZ/MOVN/MOVK sequence.
static asmjit::Error emit_a64_mov_u64(asmjit::a64::Assembler& a,
                                      const asmjit::a64::Gp& dst, uint64_t value)
{
  if (!a64_is_real_x(dst)) return asmjit::Error::kInvalidArgument;
  return a.mov(dst, asmjit::imm(value));
}

static asmjit::Error emit_a64_add_offset(asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& dst, const asmjit::a64::Gp& base, int64_t delta,
    const asmjit::a64::Gp& scratch = CJitEngine::RegAlloc::kScratch0)
{
  using namespace asmjit;
  if (!a64_is_address_x(dst) || !a64_is_address_x(base)) return Error::kInvalidArgument;
  if (delta == 0)
    return dst.id() == base.id() ? Error::kOk : a.mov(dst, base);

  const bool negative = delta < 0;
  const uint64_t magnitude = negative ? uint64_t(0) - uint64_t(delta) : uint64_t(delta);
  auto emit_part = [&](const a64::Gp& part_dst, const a64::Gp& part_base,
                       uint64_t part) -> Error {
    return negative ? a.sub(part_dst, part_base, imm(part))
                    : a.add(part_dst, part_base, imm(part));
  };

  if (arm::Utils::is_add_sub_imm(magnitude))
    return emit_part(dst, base, magnitude);

  // Two immediate instructions cover every 24-bit magnitude without consuming scratch.
  if (magnitude <= 0x00ffffffu) {
    Error err = emit_part(dst, base, magnitude & 0xfffu);
    return err != Error::kOk ? err : emit_part(dst, dst, magnitude & 0xfff000u);
  }

  if (!a64_is_real_x(scratch) || scratch.id() == base.id())
    return Error::kInvalidArgument;
  Error err = emit_a64_mov_u64(a, scratch, magnitude);
  if (err != Error::kOk) return err;
  return negative ? a.sub(dst, base, scratch) : a.add(dst, base, scratch);
}

static bool a64_is_tail_scratch(const asmjit::a64::Gp& reg) noexcept
{
  return reg.is_gp() && reg.id() >= CJitEngine::RegAlloc::kScratch0.id()
      && reg.id() <= CJitEngine::RegAlloc::kScratch6.id();
}

static asmjit::a64::Gp a64_tail_scratch_avoiding(
    uint32_t first, uint32_t second = UINT32_MAX) noexcept
{
  for (uint32_t id = CJitEngine::RegAlloc::kScratch0.id();
       id <= CJitEngine::RegAlloc::kScratch6.id(); ++id)
    if (id != first && id != second) return asmjit::a64::x(id);
  return asmjit::a64::xzr;
}

static asmjit::Error emit_a64_load_cpu_u64(asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& dst, uint32_t offset)
{
  using namespace asmjit;
  if (!dst.is_gp64() || !a64_is_tail_scratch(dst)) return Error::kInvalidArgument;
  const a64::Gp expansion = a64_tail_scratch_avoiding(dst.id());
  Error err = emit_a64_add_offset(a, dst, CJitEngine::RegAlloc::kCpu,
                                  static_cast<int64_t>(offset), expansion);
  return err != Error::kOk ? err : a.ldr(dst, a64::ptr(dst));
}

static asmjit::Error emit_a64_load_cpu_u8(asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& dst, uint32_t offset, bool acquire)
{
  using namespace asmjit;
  if (!dst.is_gp32() || !a64_is_tail_scratch(dst)) return Error::kInvalidArgument;
  const a64::Gp address = a64::x(dst.id());
  const a64::Gp expansion = a64_tail_scratch_avoiding(dst.id());
  Error err = emit_a64_add_offset(a, address, CJitEngine::RegAlloc::kCpu,
                                  static_cast<int64_t>(offset), expansion);
  if (err != Error::kOk) return err;
  return acquire ? a.ldarb(dst, a64::ptr(address))
                 : a.ldrb(dst, a64::ptr(address));
}

static asmjit::Error emit_a64_store_cpu_u64(asmjit::a64::Assembler& a,
    const asmjit::a64::Gp& src, uint32_t offset)
{
  using namespace asmjit;
  if (!a64_is_real_x(src) || src.id() == CJitEngine::RegAlloc::kCpu.id())
    return Error::kInvalidArgument;
  const a64::Gp address = a64_tail_scratch_avoiding(src.id());
  const a64::Gp expansion =
      a64_tail_scratch_avoiding(src.id(), address.id());
  if (!a64_is_real_x(address) || !a64_is_real_x(expansion))
    return Error::kInvalidArgument;
  Error err = emit_a64_add_offset(a, address, CJitEngine::RegAlloc::kCpu,
                                  static_cast<int64_t>(offset), expansion);
  return err != Error::kOk ? err : a.str(src, a64::ptr(address));
}

struct A64FrameLayout {
  static constexpr int32_t kCpuRegs = 16;
  static constexpr int32_t kCountPc = 32;
  static constexpr int32_t kPin01 = 48;
  static constexpr int32_t kPin23 = 64;
  static constexpr int32_t kPin45 = 80;
  static constexpr int32_t kHelperOut = 96;
  static constexpr int32_t kHelperScratch = 104;
  static constexpr int32_t kSize = 112;
};

static_assert(A64FrameLayout::kSize % 16 == 0,
              "A64 JIT frame must preserve 16-byte stack alignment");
static_assert(A64FrameLayout::kHelperOut % alignof(uint64_t) == 0
              && A64FrameLayout::kHelperOut + 8 <= A64FrameLayout::kHelperScratch,
              "A64 helper out storage must be aligned and disjoint from call scratch");
static_assert(A64FrameLayout::kHelperScratch + 8 == A64FrameLayout::kSize,
              "A64 helper locals must fit inside the fixed frame");

enum class A64CallArgKind : uint8_t {
  kCpu,
  kGuest,
  kGuestOrZero,
  kHost,
  kOut,
  kImm32,
  kImm64
};

struct A64CallArg {
  A64CallArgKind kind = A64CallArgKind::kImm64;
  uint64_t value = 0;
};

static constexpr uint32_t kA64MaxCallArgs = 8;

static constexpr bool a64_valid_helper_host_source(uint32_t host) noexcept
{
  // x16 is the call target, x17 is linker scratch, x18 is platform-reserved,
  // and x29/x30/SP/ZR are fixed framee, not helper
  return host < 16 || (host >= 19 && host <= 28);
}

static constexpr bool a64_valid_call_arg(const A64CallArg& arg) noexcept
{
  switch (arg.kind) {
    case A64CallArgKind::kCpu:
    case A64CallArgKind::kOut:
    case A64CallArgKind::kImm64:
      return true;
    case A64CallArgKind::kGuest:
      return arg.value < 31;
    case A64CallArgKind::kGuestOrZero:
      return arg.value < 32;
    case A64CallArgKind::kHost:
      return arg.value <= UINT32_MAX
          && a64_valid_helper_host_source(static_cast<uint32_t>(arg.value));
    case A64CallArgKind::kImm32:
      return arg.value <= UINT32_MAX;
  }
  return false;
}

static_assert(kA64MaxCallArgs == 8
              && a64_valid_call_arg({A64CallArgKind::kGuest, 30})
              && !a64_valid_call_arg({A64CallArgKind::kGuest, 31})
              && a64_valid_call_arg({A64CallArgKind::kGuestOrZero, 31})
              && a64_valid_helper_host_source(0)
              && a64_valid_helper_host_source(15)
              && !a64_valid_helper_host_source(16)
              && !a64_valid_helper_host_source(17)
              && !a64_valid_helper_host_source(18)
              && a64_valid_helper_host_source(19)
              && a64_valid_helper_host_source(28)
              && !a64_valid_helper_host_source(29),
              "A64 helper arguments must respect AAPCS64 register roles");

static int a64_helper_index(const CJitEngine::HelperSet& helpers,
                            const void* helper) noexcept
{
  constexpr size_t kHelperCount = 19;
  static_assert(sizeof(CJitEngine::HelperSet) == kHelperCount * sizeof(void*),
                "HelperSet must remain the CPU helper table's pure pointer layout");
  if (helper == nullptr) return -1;
  std::array<void*, kHelperCount> table{};
  std::memcpy(table.data(), &helpers, sizeof(helpers));
  for (size_t i = 0; i < table.size(); ++i)
    if (table[i] == helper) return static_cast<int>(i);
  return -1;
}

static constexpr asmjit::a64::Mem a64_helper_out_mem() noexcept
{
  return asmjit::a64::ptr(asmjit::a64::sp, A64FrameLayout::kHelperOut);
}

static constexpr asmjit::a64::Mem a64_helper_scratch_mem() noexcept
{
  return asmjit::a64::ptr(asmjit::a64::sp, A64FrameLayout::kHelperScratch);
}

static_assert(a64_helper_out_mem().base_id() == asmjit::a64::Gp::kIdSp
              && a64_helper_out_mem().offset() == A64FrameLayout::kHelperOut
              && a64_helper_scratch_mem().offset() == A64FrameLayout::kHelperScratch,
              "A64 helper locals must remain fixed SP-relative operands");

// Preserve the incoming values of x0-x7 when helper arguments change them.
static asmjit::Error emit_a64_parallel_arg_moves(asmjit::a64::Assembler& a,
    const std::array<A64CallArg, kA64MaxCallArgs>& args, uint32_t count)
{
  using namespace asmjit;
  constexpr int8_t kNoSource = -1;
  constexpr int8_t kStackSource = -2;
  std::array<int8_t, kA64MaxCallArgs> source{};
  std::array<bool, kA64MaxCallArgs> pending{};
  source.fill(kNoSource);

  uint32_t remaining = 0;
  for (uint32_t dst = 0; dst < count; ++dst) {
    const A64CallArg& arg = args[dst];
    if (arg.kind != A64CallArgKind::kHost || arg.value >= kA64MaxCallArgs
        || arg.value == dst) continue;
    source[dst] = static_cast<int8_t>(arg.value);
    pending[dst] = true;
    ++remaining;
  }

  while (remaining != 0) {
    bool progressed = false;
    for (uint32_t dst = 0; dst < count; ++dst) {
      if (!pending[dst]) continue;
      bool old_dst_needed = false;
      for (uint32_t other = 0; other < count; ++other) {
        if (pending[other] && source[other] == static_cast<int8_t>(dst)) {
          old_dst_needed = true;
          break;
        }
      }
      if (old_dst_needed) continue;

      const Error err = source[dst] == kStackSource
          ? a.ldr(a64::x(dst), a64_helper_scratch_mem())
          : a.mov(a64::x(dst), a64::x(static_cast<uint32_t>(source[dst])));
      if (err != Error::kOk) return err;
      pending[dst] = false;
      --remaining;
      progressed = true;
    }
    if (progressed) continue;

    uint32_t cycle_dst = 0;
    while (cycle_dst < count && !pending[cycle_dst]) ++cycle_dst;
    if (cycle_dst == count) return Error::kInvalidState;
    for (uint32_t i = 0; i < count; ++i)
      if (pending[i] && source[i] == kStackSource) return Error::kInvalidState;

    Error err = a.str(a64::x(cycle_dst), a64_helper_scratch_mem());
    if (err != Error::kOk) return err;
    for (uint32_t i = 0; i < count; ++i)
      if (pending[i] && source[i] == static_cast<int8_t>(cycle_dst))
        source[i] = kStackSource;
  }
  return Error::kOk;
}

static asmjit::Error emit_a64_call_arg(asmjit::a64::Assembler& a,
    const CJitEngine::RegAlloc& regs, bool pal_shadow, uint32_t index,
    const A64CallArg& arg)
{
  using namespace asmjit;
  const a64::Gp dst = a64::x(index);
  switch (arg.kind) {
    case A64CallArgKind::kCpu:
      return a.mov(dst, CJitEngine::RegAlloc::kCpu);
    case A64CallArgKind::kGuest:
    case A64CallArgKind::kGuestOrZero: {
      const A64GprRoute route = a64_guest_gpr_read_route(
          regs, static_cast<uint32_t>(arg.value), pal_shadow);
      switch (route.kind) {
        case A64GprRouteKind::kMemory:
          return a.ldr(dst, a64::ptr(CJitEngine::RegAlloc::kRegs,
              static_cast<int32_t>(route.slot * sizeof(uint64_t))));
        case A64GprRouteKind::kPinned:
          return a.mov(dst, a64::x(static_cast<uint32_t>(route.host)));
        case A64GprRouteKind::kZero:
          return a.mov(dst, a64::xzr);
        default:
          return Error::kInvalidArgument;
      }
    }
    case A64CallArgKind::kHost:
      // Sources in x0-x7 were resolved as a parallel copy above.
      return arg.value < kA64MaxCallArgs
          ? Error::kOk
          : a.mov(dst, a64::x(static_cast<uint32_t>(arg.value)));
    case A64CallArgKind::kOut:
      return emit_a64_add_offset(a, dst, a64::sp, A64FrameLayout::kHelperOut);
    case A64CallArgKind::kImm32:
      return a.mov(dst.w(), imm(static_cast<uint32_t>(arg.value)));
    case A64CallArgKind::kImm64:
      return emit_a64_mov_u64(a, dst, arg.value);
  }
  return Error::kInvalidArgument;
}

[[maybe_unused]] static asmjit::Error emit_a64_helper_call(
    asmjit::a64::Assembler& a, const CJitEngine::JitOffsets& offsets,
    const CJitEngine::HelperSet& helpers, const CJitEngine::RegAlloc& regs,
    bool pal_shadow, const void* helper, std::initializer_list<A64CallArg> call_args)
{
  using namespace asmjit;
  if (helper == nullptr || call_args.size() > kA64MaxCallArgs)
    return Error::kInvalidArgument;

  std::array<A64CallArg, kA64MaxCallArgs> args{};
  uint32_t count = 0;
  for (const A64CallArg& arg : call_args) {
    if (!a64_valid_call_arg(arg)) return Error::kInvalidArgument;
    args[count++] = arg;
  }

  Error err = emit_a64_parallel_arg_moves(a, args, count);
  if (err != Error::kOk) return err;
  for (uint32_t i = 0; i < count; ++i) {
    err = emit_a64_call_arg(a, regs, pal_shadow, i, args[i]);
    if (err != Error::kOk) return err;
  }

  const int helper_slot = a64_helper_index(helpers, helper);
  if (helper_slot >= 0 && offsets.helpers != 0) {
    const uint64_t table_offset = static_cast<uint64_t>(offsets.helpers)
        + static_cast<uint64_t>(helper_slot) * sizeof(void*);
    err = emit_a64_add_offset(a, CJitEngine::RegAlloc::kScratch0,
        CJitEngine::RegAlloc::kCpu, static_cast<int64_t>(table_offset));
    if (err != Error::kOk) return err;
    err = a.ldr(CJitEngine::RegAlloc::kCallTarget,
                a64::ptr(CJitEngine::RegAlloc::kScratch0));
  }
  else {
    err = emit_a64_mov_u64(a, CJitEngine::RegAlloc::kCallTarget,
                           reinterpret_cast<uintptr_t>(helper));
  }
  if (err != Error::kOk) return err;
  return a.blr(CJitEngine::RegAlloc::kCallTarget);
}

struct A64EmitContext {
  asmjit::a64::Assembler& assembler;
  const CJitEngine::JitOffsets& offsets;
  const CJitEngine::HelperSet& helpers;
  CJitEngine::RegAlloc& regs;
  const asmjit::Label& done;
  uint64_t start_pc;
  bool pal_block;
  bool pal_shadow;
  const A64BlockPlan* plan = nullptr;
  A64OpEmitReceipt receipt{};
  uint32_t receipt_index = UINT32_MAX;
};

static A64OpEmitReceipt emit_a64_planned_op(A64EmitContext& context,
    const A64DecodedOp& op, uint32_t index)
{
  (void)context;
  if (index >= A64BlockPlan::kMaxOps)
    return {asmjit::Error::kInvalidArgument, op.kind};

  switch (op.kind) {
    case A64OpKind::kUnsupported:
    case A64OpKind::kValidationProbe:
      return {};
  }
  return {};
}

template<typename EmitOne>
static A64BodyEmitReceipt drive_a64_block_body(const A64BlockPlan& plan,
    A64BodyEmitReceipt body, EmitOne&& emit_one)
{
  if (body.error != asmjit::Error::kOk) return body;

  for (uint32_t i = 0; i < plan.count; ++i) {
    if (!a64_begin_op_emission(body, plan, i)) return body;
    const A64OpEmitReceipt op = emit_one(plan.ops[i], i);
    if (!a64_accept_op_emission(body, plan, i, op)) return body;
  }
  (void)a64_finish_body_emission(body, plan);
  return body;
}

template<typename EmitOne>
static A64BodyEmitReceipt emit_a64_block_body(const A64BlockPlan& plan,
                                               EmitOne&& emit_one)
{
  return drive_a64_block_body(
      plan, a64_begin_body_emission(plan), emit_one);
}

template<typename EmitOne>
static A64BodyEmitReceipt validate_a64_block_body(const A64BlockPlan& plan,
                                                   EmitOne&& emit_one)
{
  return drive_a64_block_body(
      plan, a64_begin_body_validation(plan), emit_one);
}

static asmjit::Error emit_a64_chain_gate(asmjit::a64::Assembler& a,
    const CJitEngine::JitOffsets& offsets, A64ChainGate gate,
    const asmjit::Label& bailout)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;

  if (gate != A64ChainGate::kPollAll
      && gate != A64ChainGate::kDeferInterrupt
      && gate != A64ChainGate::kPollInterruptOnNativeTarget)
    return Error::kInvalidArgument;

  Error err = emit_a64_load_cpu_u64(a, RA::kScratch0, offsets.jit_budget);
  if (err != Error::kOk) return err;
  err = a.cmp(RA::kChainCount, RA::kScratch0);
  if (err != Error::kOk) return err;
  err = a.b_ge(bailout);  // m_jit_budget is signed, matching the x64 JGE gate.
  if (err != Error::kOk) return err;

  if (gate == A64ChainGate::kPollAll) {
    err = emit_a64_load_cpu_u8(a, RA::kScratch0.w(), offsets.check_int, true);
    if (err != Error::kOk) return err;
    err = a.cbnz(RA::kScratch0.w(), bailout);
    if (err != Error::kOk) return err;
  }
  else if (gate == A64ChainGate::kPollInterruptOnNativeTarget) {
    const Label no_interrupt = a.new_label();
    err = emit_a64_load_cpu_u8(a, RA::kScratch0.w(), offsets.check_int, true);
    if (err != Error::kOk) return err;
    err = a.cbz(RA::kScratch0.w(), no_interrupt);
    if (err != Error::kOk) return err;
    err = a.tst(RA::kNextPc, imm(1));
    if (err != Error::kOk) return err;
    err = a.b_eq(bailout);  // A pending interrupt is deferred only to a PAL target.
    if (err != Error::kOk) return err;
    err = a.bind(no_interrupt);
    if (err != Error::kOk) return err;
  }

  err = emit_a64_load_cpu_u8(a, RA::kScratch0.w(), offsets.check_timers, true);
  return err != Error::kOk ? err : a.cbnz(RA::kScratch0.w(), bailout);
}

static asmjit::Error emit_a64_source_pal_guard(asmjit::a64::Assembler& a,
    const CJitEngine::JitOffsets& offsets, bool enabled, bool expected_shadow,
    const asmjit::Label& bailout)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;
  if (!enabled) return Error::kOk;

  Error err = emit_a64_load_cpu_u8(a, RA::kScratch6.w(), offsets.sde, false);
  if (err != Error::kOk) return err;
  err = a.cmp(RA::kScratch6.w(), imm(expected_shadow ? 1 : 0));
  return err != Error::kOk ? err : a.b_ne(bailout);
}

// Probe snapshots owned by the source block.
static asmjit::Error emit_a64_link_slots_probe(asmjit::a64::Assembler& a,
    const CJitEngine::JitOffsets& offsets, const CJitEngine::LinkSlot* slots,
    const uint64_t* current_epoch, A64LinkVariantPolicy variant_policy,
    const asmjit::Label& miss)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;
  using namespace asmjit::a64;

  if (slots == nullptr || current_epoch == nullptr
      || (variant_policy != A64LinkVariantPolicy::kNone
          && variant_policy != A64LinkVariantPolicy::kAlways
          && variant_policy != A64LinkVariantPolicy::kIfTargetPal))
    return Error::kInvalidArgument;

  Error err = emit_a64_mov_u64(a, RA::kScratch5,
      reinterpret_cast<uintptr_t>(current_epoch));
  if (err != Error::kOk) return err;
  err = a.ldr(RA::kScratch5, ptr(RA::kScratch5));
  if (err != Error::kOk) return err;
  err = a.and_(RA::kScratch5, RA::kScratch5, imm(kA64LinkEpochMask));
  if (err != Error::kOk) return err;

  err = emit_a64_mov_u64(a, RA::kScratch1,
                         reinterpret_cast<uintptr_t>(slots));
  if (err != Error::kOk) return err;
  if (variant_policy != A64LinkVariantPolicy::kNone) {
    err = emit_a64_load_cpu_u8(a, RA::kScratch6.w(), offsets.sde, false);
    if (err != Error::kOk) return err;
  }

  for (uint32_t slot = 0; slot < CJitEngine::kLinkSlots; ++slot) {
    const Label next = slot + 1 < CJitEngine::kLinkSlots
        ? a.new_label() : miss;
    const int32_t base = static_cast<int32_t>(slot * sizeof(CJitEngine::LinkSlot));

    // A null body always misses.
    err = a.ldr(RA::kScratch4,
                ptr(RA::kScratch1, base + offsetof(CJitEngine::LinkSlot, body)));
    if (err != Error::kOk) return err;
    err = a.cbz(RA::kScratch4, next);
    if (err != Error::kOk) return err;

    err = a.ldr(RA::kScratch2,
                ptr(RA::kScratch1, base + offsetof(CJitEngine::LinkSlot, tag)));
    if (err != Error::kOk) return err;
    err = a.cmp(RA::kScratch2, RA::kNextPc);
    if (err != Error::kOk) return err;
    err = a.b_ne(next);
    if (err != Error::kOk) return err;

    err = a.ldr(RA::kScratch3,
                ptr(RA::kScratch1, base + offsetof(CJitEngine::LinkSlot, vgen)));
    if (err != Error::kOk) return err;
    err = a.and_(RA::kScratch4, RA::kScratch3, imm(kA64LinkEpochMask));
    if (err != Error::kOk) return err;
    err = a.cmp(RA::kScratch4, RA::kScratch5);
    if (err != Error::kOk) return err;
    err = a.b_ne(next);
    if (err != Error::kOk) return err;

    Label variant_ok;
    if (variant_policy == A64LinkVariantPolicy::kIfTargetPal) {
      variant_ok = a.new_label();
      err = a.tst(RA::kNextPc, imm(1));
      if (err != Error::kOk) return err;
      err = a.b_eq(variant_ok);
      if (err != Error::kOk) return err;
    }
    if (variant_policy != A64LinkVariantPolicy::kNone) {
      err = a.lsr(RA::kScratch4, RA::kScratch3, 63);
      if (err != Error::kOk) return err;
      err = a.cmp(RA::kScratch4.w(), RA::kScratch6.w());
      if (err != Error::kOk) return err;
      err = a.b_ne(next);
      if (err != Error::kOk) return err;
    }
    if (variant_policy == A64LinkVariantPolicy::kIfTargetPal) {
      err = a.bind(variant_ok);
      if (err != Error::kOk) return err;
    }

    err = a.ldr(RA::kScratch4,
                ptr(RA::kScratch1, base + offsetof(CJitEngine::LinkSlot, body)));
    if (err != Error::kOk) return err;
    err = a.cbz(RA::kScratch4, next);
    if (err != Error::kOk) return err;
    err = a.br(RA::kScratch4);
    if (err != Error::kOk) return err;
    if (slot + 1 < CJitEngine::kLinkSlots) {
      err = a.bind(next);
      if (err != Error::kOk) return err;
    }
  }
  return Error::kOk;
}

static asmjit::Error emit_a64_direct_chain_tail(asmjit::a64::Assembler& a,
    const CJitEngine::JitOffsets& offsets,
    const A64DirectChainContract& contract, CJitEngine::LinkSlot* slots,
    const uint64_t* current_epoch, uint64_t source_tag,
    const asmjit::Label& body, const asmjit::Label& done)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;
  if (!contract.valid() || !contract.eligible()
      || slots == nullptr || current_epoch == nullptr)
    return Error::kInvalidArgument;

  Error err = a.add(RA::kChainCount, RA::kChainCount,
                    imm(contract.completed_delta));
  if (err != Error::kOk) return err;
  if (contract.publish_pc_before_probe) {
    err = emit_a64_store_cpu_u64(a, RA::kNextPc, offsets.state_pc);
    if (err != Error::kOk) return err;
  }

#ifdef JIT_VERIFY
  if (!contract.publish_pc_before_probe) {
    err = emit_a64_store_cpu_u64(a, RA::kNextPc, offsets.state_pc);
    if (err != Error::kOk) return err;
  }
  return a.b(done);
#else
  const Label bailout = a.new_label();
  const Label miss = a.new_label();
  err = emit_a64_chain_gate(a, offsets, contract.gate, bailout);
  if (err != Error::kOk) return err;
  err = emit_a64_source_pal_guard(a, offsets, contract.source_pal_guard,
                                  contract.source_pal_shadow, bailout);
  if (err != Error::kOk) return err;

  if (contract.self_loop_candidate) {
    err = emit_a64_mov_u64(a, RA::kScratch0, source_tag);
    if (err != Error::kOk) return err;
    err = a.cmp(RA::kNextPc, RA::kScratch0);
    if (err != Error::kOk) return err;
    err = a.b_eq(body);
    if (err != Error::kOk) return err;
  }

  const A64LinkVariantPolicy variant = contract.target_pal_guard
      ? A64LinkVariantPolicy::kAlways : A64LinkVariantPolicy::kNone;
  err = emit_a64_link_slots_probe(a, offsets, slots, current_epoch,
                                  variant, miss);
  if (err != Error::kOk) return err;

  err = a.bind(miss);
  if (err != Error::kOk) return err;
  err = emit_a64_mov_u64(a, RA::kScratch1,
                         reinterpret_cast<uintptr_t>(slots));
  if (err != Error::kOk) return err;
  err = emit_a64_store_cpu_u64(a, RA::kScratch1, offsets.link_from);
  if (err != Error::kOk) return err;

  err = a.bind(bailout);
  if (err != Error::kOk) return err;
  if (!contract.publish_pc_before_probe) {
    err = emit_a64_store_cpu_u64(a, RA::kNextPc, offsets.state_pc);
    if (err != Error::kOk) return err;
  }
  return a.b(done);
#endif
}

static asmjit::Error emit_a64_indirect_chain_tail(asmjit::a64::Assembler& a,
    const CJitEngine::JitOffsets& offsets, const CJitEngine::HelperSet& helpers,
    const CJitEngine::RegAlloc& regs,
    const A64IndirectChainContract& contract, CJitEngine::LinkSlot* slots,
    const uint64_t* current_epoch, const asmjit::Label& done)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;
  if (!contract.valid() || !contract.eligible()
      || slots == nullptr || current_epoch == nullptr)
    return Error::kInvalidArgument;
#ifndef JIT_VERIFY
  if (helpers.indirect_helper == nullptr) return Error::kInvalidArgument;
#endif

  Error err = a.add(RA::kChainCount, RA::kChainCount,
                    imm(contract.completed_delta));
  if (err != Error::kOk) return err;
  if (contract.publish_pc_before_probe) {
    err = emit_a64_store_cpu_u64(a, RA::kNextPc, offsets.state_pc);
    if (err != Error::kOk) return err;
  }

#ifdef JIT_VERIFY
  (void)helpers;
  (void)regs;
  if (!contract.publish_pc_before_probe) {
    err = emit_a64_store_cpu_u64(a, RA::kNextPc, offsets.state_pc);
    if (err != Error::kOk) return err;
  }
  return a.b(done);
#else
  const Label bailout = a.new_label();
  const Label resolver = a.new_label();
  err = emit_a64_chain_gate(a, offsets, contract.gate, bailout);
  if (err != Error::kOk) return err;
  err = emit_a64_source_pal_guard(a, offsets, contract.source_pal_guard,
                                  contract.source_pal_shadow, bailout);
  if (err != Error::kOk) return err;
  err = emit_a64_link_slots_probe(a, offsets, slots, current_epoch,
                                  contract.target_variant, resolver);
  if (err != Error::kOk) return err;

  err = a.bind(resolver);
  if (err != Error::kOk) return err;
  if (!contract.publish_pc_before_probe) {
    err = emit_a64_store_cpu_u64(a, RA::kNextPc, offsets.state_pc);
    if (err != Error::kOk) return err;
  }
  err = emit_a64_helper_call(a, offsets, helpers, regs,
      contract.source_pal_shadow, helpers.indirect_helper,
      {{A64CallArgKind::kCpu, 0},
       {A64CallArgKind::kHost, RA::kNextPc.id()},
       {A64CallArgKind::kImm64, reinterpret_cast<uintptr_t>(slots)}});
  if (err != Error::kOk) return err;
  err = a.cbz(a64::x0, done);
  if (err != Error::kOk) return err;
  err = a.br(a64::x0);
  if (err != Error::kOk) return err;

  err = a.bind(bailout);
  if (err != Error::kOk) return err;
  if (!contract.publish_pc_before_probe) {
    err = emit_a64_store_cpu_u64(a, RA::kNextPc, offsets.state_pc);
    if (err != Error::kOk) return err;
  }
  return a.b(done);
#endif
}

static asmjit::Error emit_a64_prologue(asmjit::a64::Assembler& a)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;
  using namespace asmjit::a64;

  Error err = a.stp(x29, x30, ptr_pre(sp, -A64FrameLayout::kSize));
  if (err != Error::kOk) return err;
  err = emit_a64_add_offset(a, x29, sp, 0);
  if (err != Error::kOk) return err;
  err = a.stp(RA::kCpu, RA::kRegs, ptr(sp, A64FrameLayout::kCpuRegs));
  if (err != Error::kOk) return err;
  err = a.stp(RA::kChainCount, RA::kNextPc, ptr(sp, A64FrameLayout::kCountPc));
  if (err != Error::kOk) return err;
  err = a.stp(RA::kGuestPin0, RA::kGuestPin1, ptr(sp, A64FrameLayout::kPin01));
  if (err != Error::kOk) return err;
  err = a.stp(RA::kGuestPin2, RA::kGuestPin3, ptr(sp, A64FrameLayout::kPin23));
  if (err != Error::kOk) return err;
  err = a.stp(RA::kGuestPin4, RA::kGuestPin5, ptr(sp, A64FrameLayout::kPin45));
  if (err != Error::kOk) return err;

  err = a.mov(RA::kCpu, RA::kArgCpu);
  if (err != Error::kOk) return err;
  err = a.mov(RA::kRegs, RA::kArgRegs);
  if (err != Error::kOk) return err;
  return emit_a64_mov_u64(a, RA::kChainCount, 0);
}

static asmjit::Error emit_a64_epilogue(asmjit::a64::Assembler& a)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;
  using namespace asmjit::a64;

  Error err = a.ldp(RA::kGuestPin4, RA::kGuestPin5, ptr(sp, A64FrameLayout::kPin45));
  if (err != Error::kOk) return err;
  err = a.ldp(RA::kGuestPin2, RA::kGuestPin3, ptr(sp, A64FrameLayout::kPin23));
  if (err != Error::kOk) return err;
  err = a.ldp(RA::kGuestPin0, RA::kGuestPin1, ptr(sp, A64FrameLayout::kPin01));
  if (err != Error::kOk) return err;
  err = a.ldp(RA::kChainCount, RA::kNextPc, ptr(sp, A64FrameLayout::kCountPc));
  if (err != Error::kOk) return err;
  err = a.ldp(RA::kCpu, RA::kRegs, ptr(sp, A64FrameLayout::kCpuRegs));
  if (err != Error::kOk) return err;
  err = a.ldp(x29, x30, ptr_post(sp, A64FrameLayout::kSize));
  if (err != Error::kOk) return err;
  return a.ret(x30);
}

#ifndef NDEBUG
static bool a64_validate_body_driver(const asmjit::Environment& environment,
                                     const asmjit::CpuFeatures& features)
{
  using namespace asmjit;

  A64BlockPlan plan{};
  plan.count = 3;
  plan.stop = A64PlanStop::kBlockEnd;
  for (uint32_t i = 0; i < plan.count; ++i) {
    plan.ops[i].ins = i + 1;
    plan.ops[i].kind = A64OpKind::kValidationProbe;
  }

  CodeHolder complete_code;
  if (complete_code.init(environment, features) != Error::kOk) return false;
  a64::Assembler complete_assembler;
  if (complete_code.attach(&complete_assembler) != Error::kOk) return false;
  complete_assembler.add_diagnostic_options(
      DiagnosticOptions::kValidateAssembler);
  uint32_t complete_calls = 0;
  const A64BodyEmitReceipt complete = validate_a64_block_body(
      plan, [&](const A64DecodedOp& op, uint32_t index) {
        if (index != complete_calls++)
          return A64OpEmitReceipt{};
        return a64_completed_op_receipt(op, complete_assembler.nop());
      });
  if (!complete.complete_for_validation(plan) || complete_calls != plan.count
      || complete_assembler.finalize() != Error::kOk
      || complete_code.has_unresolved_fixups()
      || complete_code.code_size() != plan.count * sizeof(uint32_t))
    return false;

  // Host bytes emitted before a later error are deliberately not rolled back or
  // accepted as a shorter body; the caller abandons this whole CodeHolder.
  CodeHolder partial_code;
  if (partial_code.init(environment, features) != Error::kOk) return false;
  a64::Assembler partial_assembler;
  if (partial_code.attach(&partial_assembler) != Error::kOk) return false;
  partial_assembler.add_diagnostic_options(
      DiagnosticOptions::kValidateAssembler);
  uint32_t partial_calls = 0;
  const A64BodyEmitReceipt partial = validate_a64_block_body(
      plan, [&](const A64DecodedOp& op, uint32_t index) {
        ++partial_calls;
        const Error err = partial_assembler.nop();
        return a64_completed_op_receipt(
            op, index == 1 && err == Error::kOk ? Error::kInvalidState : err);
      });
  if (partial.complete || partial.error != Error::kInvalidState
      || partial.attempted_count != 2 || partial.emitted_count != 1
      || partial_calls != 2
      || partial_code.code_size() != 2 * sizeof(uint32_t))
    return false;

  CJitEngine::JitOffsets offsets{};
  CJitEngine::HelperSet helpers{};
  CJitEngine::RegAlloc regs = make_a64_block_regalloc();
  const Label done = partial_assembler.new_label();
  A64EmitContext context{partial_assembler, offsets, helpers, regs, done,
                         0x1000, false, false};
  const A64BodyEmitReceipt inert = validate_a64_block_body(
      plan, [&](const A64DecodedOp& op, uint32_t index) {
        return emit_a64_planned_op(context, op, index);
      });
  A64DecodedOp unsupported{};
  const A64OpEmitReceipt unsupported_receipt =
      emit_a64_planned_op(context, unsupported, 0);
  return !inert.complete && inert.error == Error::kInvalidInstruction
      && inert.attempted_count == 1 && inert.emitted_count == 0
      && !unsupported_receipt.accepted_for(unsupported)
      && unsupported_receipt.error == Error::kInvalidInstruction
      && partial_code.code_size() == 2 * sizeof(uint32_t);
}

static bool a64_validate_tail_emitters(const asmjit::Environment& environment,
                                       const asmjit::CpuFeatures& features)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;

  CJitEngine::JitOffsets offsets{};
  offsets.state_pc = 4095;
  offsets.link_from = 4096;
  offsets.check_timers = 32760;
  offsets.helpers = 32768;
  offsets.sde = 0x00ffffffu;
  offsets.check_int = 0x01000000u;
  offsets.jit_budget = UINT32_MAX;

  CJitEngine::JitBlock block{};
  block.tag = 0x1001;
  block.pal_shadow = true;
  uint64_t epoch = 0x12345678;
  CJitEngine::HelperSet helpers{};
  helpers.indirect_helper = reinterpret_cast<void*>(uintptr_t(1));
  const CJitEngine::RegAlloc regs = make_a64_block_regalloc();

  auto finish = [&](CodeHolder& code, a64::Assembler& a,
                    const Label& done) -> bool {
    Error err = a.bind(done);
    if (err == Error::kOk)
      err = a.mov(RA::kResultCount, RA::kChainCount.w());
    if (err == Error::kOk) err = a.ret(a64::x30);
    if (err == Error::kOk) err = a.finalize();
    return err == Error::kOk && !code.has_unresolved_fixups()
        && code.code_size() != 0;
  };

  auto validate_direct = [&](A64ExitKind kind, bool pal_block,
                             bool pal_shadow) -> bool {
    CodeHolder code;
    if (code.init(environment, features) != Error::kOk) return false;
    a64::Assembler a;
    if (code.attach(&a) != Error::kOk) return false;
    a.add_diagnostic_options(DiagnosticOptions::kValidateAssembler);
    const Label body = a.new_label();
    const Label done = a.new_label();
    if (a.bind(body) != Error::kOk
        || emit_a64_mov_u64(a, RA::kChainCount, 0) != Error::kOk
        || emit_a64_mov_u64(a, RA::kNextPc, 0x2001) != Error::kOk)
      return false;
    const A64BlockExit exit{0x1005, 1, kind};
    const A64DirectChainContract contract =
        plan_a64_direct_chain(exit, pal_block, pal_shadow);
    if (emit_a64_direct_chain_tail(a, offsets, contract, &block.link[0],
          &epoch, block.tag, body, done) != Error::kOk)
      return false;
    return finish(code, a, done);
  };

  auto validate_indirect = [&](uint32_t opcode, bool pal_block,
                               bool pal_shadow) -> bool {
    CodeHolder code;
    if (code.init(environment, features) != Error::kOk) return false;
    a64::Assembler a;
    if (code.attach(&a) != Error::kOk) return false;
    a.add_diagnostic_options(DiagnosticOptions::kValidateAssembler);
    const Label body = a.new_label();
    const Label done = a.new_label();
    if (a.bind(body) != Error::kOk
        || emit_a64_mov_u64(a, RA::kChainCount, 0) != Error::kOk
        || emit_a64_mov_u64(a, RA::kNextPc, 0x2001) != Error::kOk)
      return false;
    const A64BlockExit exit{0x1005, 1, A64ExitKind::kIndirect};
    const A64IndirectChainContract contract = plan_a64_indirect_chain(
        exit, opcode << 26, pal_block, pal_shadow);
    if (emit_a64_indirect_chain_tail(a, offsets, helpers, regs, contract,
          &block.link[0], &epoch, done) != Error::kOk)
      return false;
    return finish(code, a, done);
  };

  return validate_direct(A64ExitKind::kFallthrough, false, false)
      && validate_direct(A64ExitKind::kDirect, true, true)
      && validate_direct(A64ExitKind::kCallPal, false, false)
      && validate_direct(A64ExitKind::kRedispatch, true, true)
      && validate_indirect(0x1a, false, false)
      && validate_indirect(0x1a, true, false)
      && validate_indirect(0x1e, true, true);
}

static bool a64_abi_compatible(const asmjit::Environment& environment)
{
  using namespace asmjit;
  if (!environment.is_family_aarch64()) return false;

  CallConv cc;
  if (cc.init(CallConvId::kCDecl, environment) != Error::kOk) return false;
  const uint8_t* const gp_args = cc.passed_order(RegGroup::kGp);
  if (gp_args[0] != CJitEngine::RegAlloc::kArgCpu.id()
      || gp_args[1] != CJitEngine::RegAlloc::kArgRegs.id()) return false;
  for (uint32_t i = 2; i < 8; ++i) if (gp_args[i] != i) return false;

  const RegMask preserved = cc.preserved_regs(RegGroup::kGp);
  return (preserved & CJitEngine::RegAlloc::kPersistentGpMask)
          == CJitEngine::RegAlloc::kPersistentGpMask
      && (preserved & Support::bit_mask<RegMask>(CJitEngine::RegAlloc::kScratch0.id())) == 0
      && cc.natural_stack_alignment() == 16;
}
#endif

} // namespace

void CJitEngine::emit_op(void* a_ptr, const uint8_t* gpa, void* done_ptr,
                         const HelperSet& hs, bool pal_block, JitBlock* b,
                         uint32_t ins, uint32_t i, RegAlloc& regalloc,
                         void* cold, bool defer_pc)
{
  (void)gpa;
  auto* const context = static_cast<A64EmitContext*>(cold);
  if (context == nullptr) return;

  context->receipt = {};
  context->receipt_index = i;
  if (a_ptr != &context->assembler || done_ptr != &context->done
      || &hs != &context->helpers || &regalloc != &context->regs
      || b == nullptr || b->tag != context->start_pc
      || pal_block != context->pal_block || !defer_pc
      || context->plan == nullptr || i >= context->plan->count
      || context->plan->ops[i].ins != ins) {
    context->receipt.error = asmjit::Error::kInvalidArgument;
    return;
  }

  context->receipt = emit_a64_planned_op(
      *context, context->plan->ops[i], i);
}

void CJitEngine::compile_block(JitBlock* b, const uint8_t* dram, uint64_t dram_size,
    void* read_helper, void* write_helper, void* opcdec_helper, void* hw_mfpr_helper,
    void* hw_ld_helper, void* hw_mtpr_helper, void* hw_st_helper, void* indirect_helper,
    void* read_locked_helper, void* stc_helper, void* misc_helper, void* read_vpte_helper,
    void* read_wchk_helper, void* itof_helper, void* ftoi_helper, void* fltl_helper,
    void* fp_read_helper, void* fp_write_helper, void* fltv_helper)
{
  // Match the x64 cold-path lifecycle.
  if (m_rt && m_code_bytes >= kReclaimBytes) {
    reclaim_code();
    b->valid = true;
  }

  // Mark the attempt
  b->compiled = true;
  b->code = nullptr;
  b->jit_body = nullptr;
  b->prefix_len = 0;
  b->body_off = 0;
  b->src_sum = 0;
  b->hash_len = 0;

#ifndef NDEBUG
  if (m_rt != nullptr) {
    auto* const validation_rt = static_cast<asmjit::JitRuntime*>(m_rt);
    static const bool emitters_valid =
        a64_validate_body_driver(validation_rt->environment(),
                                 validation_rt->cpu_features())
        && a64_validate_tail_emitters(validation_rt->environment(),
                                      validation_rt->cpu_features());
    assert(emitters_valid);
  }
#endif

  const A64BlockPlan plan = plan_a64_block(*b, dram, dram_size);
#ifdef JIT_STATS
  if (plan.stop == A64PlanStop::kUnsupported) {
    const uint32_t bop = plan.breaker_word >> 26;
    m_term_op[bop]++;                 // tally what cut this block (the coverage gap to chase)
    if (bop == 0x00)                  // CALL_PAL: also tally the function code (low 8 bits)
      m_pal_func[plan.breaker_word & 0xFF]++;
    else if (bop == 0x1d)             // HW_MTPR: tally the IPR index -- which writes break blocks
      m_mtpr_func[(plan.breaker_word >> 8) & 0xFF]++;
    else if (bop == 0x1b)             // HW_LD: tally the form (phys/virt/lock/vpte/chk, ins[15:12])
      m_hwld_func[(plan.breaker_word >> 12) & 0xF]++;
    else if (bop == 0x18)             // MISC: tally the Ra==31 form (ins[15:12]: 0xc RPCC / 0xe RC / 0xf RS)
      m_misc_func[(plan.breaker_word >> 12) & 0xF]++;
    // Punch list: one-shot print of the first ACTIONABLE breaker -- keep the exclusions
    // identical to the x64 backend so both hosts point at the same next translation target.
    if (!m_first_breaker_logged && bop != 0x00 && bop != 0x1b && bop != 0x1d && bop != 0x1f && bop != 0x10 && bop != 0x18 && bop != 0x13 && bop != 0x14 && bop != 0x17) {
      m_first_breaker_logged = true;
      printf("[JIT][PUNCH][CPU%d] first unhandled breaker: %s(0x%02x) ins=%08x at pc=%016llx%s\n",
             m_cpu_id, jit_opcode_name(bop), bop, plan.breaker_word,
             (unsigned long long) ((b->tag & ~(uint64_t) 1) + (uint64_t) plan.count * 4),
             (b->tag & 1) ? "  [PALmode]" : "");
    }
  }
#endif
  if (!a64_plan_ready_for_emission(plan)) return;
  const A64BlockExit exit = plan_a64_exit(b->tag, plan.count, plan.terminator);
  const bool pal_block = (b->tag & 1u) != 0;
  const A64DirectChainContract chain =
      plan_a64_direct_chain(exit, pal_block, b->pal_shadow);
  const uint32_t terminator_ins =
      plan.count != 0 ? plan.ops[plan.count - 1].ins : 0;
  const A64IndirectChainContract indirect = plan_a64_indirect_chain(
      exit, terminator_ins, pal_block, b->pal_shadow);
  // Only one currently scaffolded tail must own a supported block.
  if (!exit.valid() || !chain.valid() || !indirect.valid()
      || chain.eligible() == indirect.eligible()) return;

  if (m_rt == nullptr) return;
  auto* const rt = static_cast<asmjit::JitRuntime*>(m_rt);
#ifndef NDEBUG
  static const bool abi_compatible =
      a64_abi_compatible(rt->environment());
  assert(abi_compatible);
#endif

  // Build a complete fixed frame.
  asmjit::CodeHolder code;
  if (code.init(rt->environment(), rt->cpu_features()) != asmjit::Error::kOk) return;

  asmjit::a64::Assembler a;
  if (code.attach(&a) != asmjit::Error::kOk) return;
#ifndef NDEBUG
  a.add_diagnostic_options(asmjit::DiagnosticOptions::kValidateAssembler);
#endif
  assert(a.is_initialized());

  RegAlloc regalloc = make_a64_block_regalloc();
  [[maybe_unused]] const HelperSet helpers = {
      read_helper, write_helper, opcdec_helper, hw_mfpr_helper, hw_ld_helper,
      hw_mtpr_helper, hw_st_helper, indirect_helper, read_locked_helper, stc_helper,
      misc_helper, read_vpte_helper, read_wchk_helper, itof_helper, ftoi_helper,
      fltl_helper, fp_read_helper, fp_write_helper, fltv_helper};
  if (emit_a64_prologue(a) != asmjit::Error::kOk) return;
  // Cold entry materializes the shared block convention. Chained entries target
  // body directly and inherit these live values plus x19-x22 from their predecessor.
  if (emit_a64_load_guest_pins(a, regalloc) != asmjit::Error::kOk) return;

  asmjit::Label done = a.new_label();
  asmjit::Label body = a.new_label();
  if (a.bind(body) != asmjit::Error::kOk) return;
  // Every body begins with its forward default.
  if (emit_a64_mov_u64(a, RegAlloc::kNextPc, exit.fallthrough_pc)
      != asmjit::Error::kOk) return;

  A64EmitContext emit_context{a, m_off, helpers, regalloc, done, b->tag,
                              pal_block, b->pal_shadow};
  emit_context.plan = &plan;
  const A64BodyEmitReceipt body_emission = emit_a64_block_body(
      plan, [&](const A64DecodedOp& op, uint32_t index) {
        emit_op(&a, nullptr, &done, helpers, pal_block, b, op.ins, index,
                regalloc, &emit_context, true);
        return emit_context.receipt_index == index
            ? emit_context.receipt : A64OpEmitReceipt{};
      });
  if (!body_emission.complete_for(plan)) return;

  const asmjit::Error tail_err = chain.eligible()
      ? emit_a64_direct_chain_tail(a, m_off, chain, &b->link[0],
                                  &m_vgen_cur, b->tag, body, done)
      : emit_a64_indirect_chain_tail(a, m_off, helpers, regalloc, indirect,
                                    &b->link[0], &m_vgen_cur, done);
  if (tail_err != asmjit::Error::kOk) return;

  if (a.bind(done) != asmjit::Error::kOk) return;
  if (a.mov(RegAlloc::kResultCount, RegAlloc::kChainCount.w())
      != asmjit::Error::kOk) return;
  // Every dispatcher or bailout exit converges here while x20 still names state.r[].
  if (emit_a64_sync_guest_pins(a, regalloc) != asmjit::Error::kOk) return;
  if (emit_a64_epilogue(a) != asmjit::Error::kOk) return;
  if (a.finalize() != asmjit::Error::kOk) return;

  const A64PendingPublication pending = prepare_a64_block_publication(
      *b, plan, exit, body_emission, dram, dram_size, code, body, m_code_bytes);
  if (!pending.ready()) return;
  assert(pending.body_off != 0);  // Chained entry must remain past the cold prologue.

  JitFn fn = nullptr;
  if (rt->add(&fn, &code) != asmjit::Error::kOk) return;

  const A64PendingPublication committed = prepare_a64_block_publication(
      *b, plan, exit, body_emission, dram, dram_size, code, body, m_code_bytes);
  if (!committed.ready()) {
    (void)rt->release(fn);
    return;
  }

  void* const jit_body = static_cast<void*>(
      reinterpret_cast<uint8_t*>(reinterpret_cast<void*>(fn)) + committed.body_off);
  b->body_off = static_cast<uint32_t>(committed.body_off);
  b->src_sum = committed.source_hash;
  b->hash_len = committed.hash_len;
  b->prefix_len = committed.prefix_len;
#ifdef JIT_REGPROF
  b->rp_mask = plan.gpr_usage.reads | plan.gpr_usage.writes;
  b->rp_csz = static_cast<uint32_t>(committed.code_size);
#endif
  m_code_bytes = committed.prior_code_bytes + committed.code_size;
#ifdef JIT_STATS
  m_stat_compiled++;
  m_stat_plen_sum += committed.prefix_len;
  m_stat_code_bytes += committed.code_size;
#endif
  b->code = fn;
  b->jit_body = jit_body;  // Runnable chain entry publishes last.
}

void CJitEngine::compile_trace(TraceFragment*, JitBlock**, uint32_t,
                               const uint8_t*, uint64_t, const HelperSet&) {}

#endif // ES40_JIT_A64

#endif // ES40_JIT

// Keeps this translation unit non-empty when ARM64 is not the build host (MSVC LNK4221).
extern const char jit_a64_backend_tu;
const char jit_a64_backend_tu = 0;
