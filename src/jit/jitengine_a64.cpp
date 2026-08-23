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

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <initializer_list>
#include <vector>
#define ASMJIT_STATIC
#include <asmjit/a64.h>

// register convention for both block and trace codegen:
//   x19 = CAlphaCPU*, x20 = state.r[] base, x21 = completed-instruction count,
//   x22 = next guest PC / chain target, x23-x28 = future guest-GPR pin bank.
struct CJitEngine::RegAlloc {
  static constexpr asmjit::a64::Gp kArgCpu = asmjit::a64::x0;
  static constexpr asmjit::a64::Gp kArgRegs = asmjit::a64::x1;
  static constexpr asmjit::a64::Gp kResultCount = asmjit::a64::w0;

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
};

static_assert((CJitEngine::RegAlloc::kPersistentGpMask
               & asmjit::Support::bit_mask<asmjit::RegMask>(8, 16, 17,
                     asmjit::a64::Gp::kIdOs, 29, 30, 31)) == 0,
              "A64 persistent registers must exclude ABI, linker, platform, frame, link, and stack registers");

namespace {

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
static_assert(A64FrameLayout::kHelperScratch + 8 == A64FrameLayout::kSize,
              "A64 helper locals must fit inside the fixed frame");

static asmjit::Error emit_a64_prologue(asmjit::a64::Assembler& a)
{
  using RA = CJitEngine::RegAlloc;
  using namespace asmjit;
  using namespace asmjit::a64;

  Error err = a.stp(x29, x30, ptr_pre(sp, -A64FrameLayout::kSize));
  if (err != Error::kOk) return err;
  err = a.mov(x29, sp);
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
  return a.mov(RA::kChainCount, 0);
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
      && cc.natural_stack_alignment() == 16;
}
#endif

} // namespace

void CJitEngine::emit_op(void*, const uint8_t*, void*, const HelperSet&,
                         bool, JitBlock*, uint32_t, uint32_t, RegAlloc&, void*, bool) {}

void CJitEngine::compile_block(JitBlock* b, const uint8_t*, uint64_t, void*, void*, void*,
    void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
    void*, void*, void*, void*)
{
  // Mark the attempt first so every setup failure leaves a permanent interpreter block.
  b->compiled = true;
  b->code = nullptr;
  b->jit_body = nullptr;
  b->prefix_len = 0;

  if (m_rt == nullptr) return;
  auto* const rt = static_cast<asmjit::JitRuntime*>(m_rt);
#ifndef NDEBUG
  static const bool abi_compatible =
      a64_abi_compatible(rt->environment());
  assert(abi_compatible);
#endif

  // Build a complete fixed frame, but deliberately discard the generated image.
  asmjit::CodeHolder code;
  if (code.init(rt->environment(), rt->cpu_features()) != asmjit::Error::kOk) return;

  asmjit::a64::Assembler a;
  if (code.attach(&a) != asmjit::Error::kOk) return;
#ifndef NDEBUG
  a.add_diagnostic_options(asmjit::DiagnosticOptions::kValidateAssembler);
#endif
  assert(a.is_initialized());

  if (emit_a64_prologue(a) != asmjit::Error::kOk) return;

  asmjit::Label done = a.new_label();
  asmjit::Label body = a.new_label();
  if (a.bind(body) != asmjit::Error::kOk) return;
  [[maybe_unused]] const size_t body_off = code.code_size();

  // Empty translated body: a cold call would return zero completed instructions.
  if (a.mov(RegAlloc::kResultCount, RegAlloc::kChainCount.w()) != asmjit::Error::kOk) return;
  if (a.bind(done) != asmjit::Error::kOk) return;
  if (emit_a64_epilogue(a) != asmjit::Error::kOk) return;
  if (a.finalize() != asmjit::Error::kOk) return;

  assert(body_off == 40);
  assert(code.code_size() == 72);
}

void CJitEngine::compile_trace(TraceFragment*, JitBlock**, uint32_t,
                               const uint8_t*, uint64_t, const HelperSet&) {}

#endif // ES40_JIT_A64

#endif // ES40_JIT

// Keeps this translation unit non-empty when ARM64 is not the build host (MSVC LNK4221).
extern const char jit_a64_backend_tu;
const char jit_a64_backend_tu = 0;
