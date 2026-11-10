// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// DETERMINISTIC FLOATING-POINT ENVIRONMENT -- scoped SSE control-word pin.
//
// Deterministic lockstep with same-binary scope makes the authoritative
// sim reproducible across peers running the IDENTICAL binary. Same binary means
// the same libm and the same SSE instruction selection, so FMath transcendentals
// are permitted. What is NOT automatically identical is the per-thread FPU
// control word (MXCSR): denormal handling (FTZ/DAZ) and the rounding mode are
// thread-local mutable state that the engine, render thread, audio, or a stray
// middleware call can leave in any configuration. If two peers tick the same
// step with different MXCSR they can produce bit-divergent floats -> desync.
//
// FGalaxyDeterministicFPScope pins the control word for the authoritative-tick
// region to a known configuration and RESTORES the caller's prior word on scope
// exit (so the sim never leaks FTZ/DAZ to engine/render threads):
//   - FTZ (flush-to-zero):       denormal RESULTS  -> 0
//   - DAZ (denormals-are-zero):  denormal OPERANDS -> 0
//   - round-to-nearest-even:     the IEEE default rounding mode, pinned
//
// Caveat: enabling FTZ/DAZ itself changes float outcomes
// (denormals collapse to zero). Under same-binary lockstep that is a one-time,
// uniform change applied to every peer's identical binary -- not a desync, only
// a recorded-replay-compatibility break (acceptable prerelease). Land it before
// other determinism-sensitive changes so the baseline is stable.
//
// Non-x86 targets: no-op (the guard saves/restores nothing). Windows x64 is the
// only shipping target today; an ARM path (FPCR FZ bit) can be added when needed.
//
// EXPORTS: FGalaxyDeterministicFPScope
// ============================================================================
#pragma once

#include "CoreMinimal.h"

#if PLATFORM_CPU_X86_FAMILY
#include <xmmintrin.h> // _mm_getcsr / _mm_setcsr (MXCSR access)
#endif

/**
 * Scoped RAII guard that pins the SSE MXCSR control word to a deterministic
 * configuration (FTZ + DAZ + round-to-nearest-even) for the lifetime of the
 * object, restoring the previous control word on destruction.
 *
 * Wrap the authoritative fixed-step region (the UGlobalShardScheduler tick loop)
 * in one of these. Construct on the same thread that runs the steps; do not move
 * it across threads. Non-copyable, non-movable.
 */
class FGalaxyDeterministicFPScope
{
public:
#if PLATFORM_CPU_X86_FAMILY
	// MXCSR bit layout (Intel SDM Vol.1 10.2.3):
	//   bit  6  (0x0040) DAZ  - denormals-are-zero (operands)
	//   bit 15  (0x8000) FTZ  - flush-to-zero (results)
	//   bits 13-14 (0x6000) RC - rounding control (00 = nearest-even)
	static constexpr uint32 MxcsrFTZ = 0x8000u;
	static constexpr uint32 MxcsrDAZ = 0x0040u;
	static constexpr uint32 MxcsrRoundMask = 0x6000u; // cleared => round-to-nearest-even

	/** Exactly the bits this scope PINS. The check below compares only these:
	 *  MXCSR bits 0-5 are sticky exception STATUS flags that ordinary float work
	 *  sets as a matter of course, and bits 7-12 are exception masks this scope
	 *  deliberately does not own -- asserting on either would fire on legitimate
	 *  variation and train the reader to ignore the check. */
	static constexpr uint32 MxcsrPinnedMask = MxcsrFTZ | MxcsrDAZ | MxcsrRoundMask;
#endif

	FGalaxyDeterministicFPScope()
	{
#if PLATFORM_CPU_X86_FAMILY
		SavedCsr = _mm_getcsr();
		uint32 NewCsr = SavedCsr;
		NewCsr |= (MxcsrFTZ | MxcsrDAZ);
		NewCsr &= ~MxcsrRoundMask;
		_mm_setcsr(NewCsr);
		PinnedCsr = NewCsr;
#endif
	}

	~FGalaxyDeterministicFPScope()
	{
#if PLATFORM_CPU_X86_FAMILY
		// PAIR ASSERTION -- setting the control word is not the same as it HOLDING
		// for the whole step, and only the second one is what determinism needs.
		//
		// The primary source on FP determinism warns that external DLL calls reset
		// the FPU control word and must be re-checked afterwards; Supreme Commander,
		// which shipped this exact technique to 1M+ customers, asserted its settings
		// EVERY TICK rather than trusting the initial set. We run the authoritative
		// step inside Unreal with engine and middleware code on the same thread, so
		// the same hazard applies to us. A silent reset mid-step is a desync whose
		// symptom surfaces arbitrarily far from its cause -- exactly the failure the
		// research calls out as the expensive kind to trace.
		//
		// ensureMsgf (not ensureAlwaysMsgf) is deliberate: ensure fires once per call
		// site, so a persistent offender logs one callstack instead of one per tick.
		// Compiled out of Shipping with the rest of the ensure machinery.
		const uint32 ExitCsr = _mm_getcsr();
		ensureMsgf(
			(ExitCsr & MxcsrPinnedMask) == (PinnedCsr & MxcsrPinnedMask),
			TEXT("[Determinism][FPScope] MXCSR was modified during the authoritative step: ")
			TEXT("pinned 0x%04X, observed 0x%04X (pinned bits 0x%04X). Something inside the step ")
			TEXT("changed FTZ/DAZ/rounding -- peers whose control word differs can produce ")
			TEXT("bit-divergent floats. Find the caller that touches MXCSR and scope it."),
			PinnedCsr & MxcsrPinnedMask, ExitCsr & MxcsrPinnedMask, MxcsrPinnedMask);

		_mm_setcsr(SavedCsr);
#endif
	}

	FGalaxyDeterministicFPScope(const FGalaxyDeterministicFPScope&) = delete;
	FGalaxyDeterministicFPScope& operator=(const FGalaxyDeterministicFPScope&) = delete;
	FGalaxyDeterministicFPScope(FGalaxyDeterministicFPScope&&) = delete;
	FGalaxyDeterministicFPScope& operator=(FGalaxyDeterministicFPScope&&) = delete;

private:
#if PLATFORM_CPU_X86_FAMILY
	uint32 SavedCsr = 0;
	uint32 PinnedCsr = 0;
#endif
};
