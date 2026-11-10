// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// NUMERICALLY-STABLE SUMMATION -- compensated (Kahan-Babuska-Neumaier) accumulation.
//
// Naive left-to-right float summation loses low-order bits when the running total
// dwarfs the next term: at a running sum of 1e7, a `float` ulp is ~1.0, so adding
// 1.0 a million times can vanish entirely. This is the AESTHETIC/accuracy failure
// that compounds over long reductions (polymass pressure totals, force sums,
// fitness scoring). It is SEPARATE from the ordering hazard -- stable iteration
// order (guarded by the `tmap-iteration` / `entt-iteration` / `pointer-ordering`
// audits) makes a sum reproducible; compensation makes it ACCURATE. Use both.
//
// TKahanAccumulator carries a running compensation term that captures the bits the
// running sum dropped, then folds it back in at Get(). KBN (Neumaier's improved
// Kahan) also handles the case where the incoming term is LARGER than the running
// sum, which plain Kahan mishandles.
//
// Determinism: this is plain float arithmetic with a magnitude COMPARISON branch
// (not a float-equality branch) -- under same-binary lockstep it is
// fully deterministic. Adopting it at a site CHANGES the float result (a one-time
// uniform outcome change); review tuned/tie-sensitive consumers first (cf. the
// route-slerp lesson in math-helpers).
//
// EXPORTS: GalaxyMath::TKahanAccumulator<T>, GalaxyMath::KahanSum,
//   GalaxyMath::FFloatKahan, GalaxyMath::FDoubleKahan
// ============================================================================
#pragma once

#include "CoreMinimal.h"

namespace GalaxyMath
{
	/**
	 * Compensated (Kahan-Babuska-Neumaier) summation accumulator.
	 * Accumulates in T while tracking the low-order bits the running sum drops,
	 * folded back in at Get(). Far more accurate than naive `+=` for long
	 * reductions where the running total dwarfs individual terms.
	 *
	 * Usage:
	 *   TKahanAccumulator<float> Acc;
	 *   for (float V : Values) { Acc.Add(V); }
	 *   const float Total = Acc.Get();
	 */
	template<typename T = float>
	class TKahanAccumulator
	{
	public:
		TKahanAccumulator() = default;

		/** Add one term, compensating for the bits the running sum cannot hold. */
		FORCEINLINE void Add(T Value)
		{
			// `volatile` forces NewSum to be MATERIALIZED at storage (float/double)
			// precision before the recovery subtraction. Without it an optimizing
			// compiler keeps NewSum in an extended-precision register (or algebraically
			// folds RunningSum + Value), so `RunningSum - NewSum` cancels to exactly
			// -Value and the compensation silently collapses to zero -- defeating Kahan.
			volatile T NewSum = RunningSum + Value;
			// KBN: route the lost low bits into the compensation term. Branch on
			// MAGNITUDE (a comparison, not a float-equality test).
			if (FMath::Abs(RunningSum) >= FMath::Abs(Value))
			{
				Compensation += (RunningSum - NewSum) + Value;
			}
			else
			{
				Compensation += (Value - NewSum) + RunningSum;
			}
			RunningSum = NewSum;
			++Count;
		}

		/** The compensated total (running sum + accumulated compensation). */
		[[nodiscard]] FORCEINLINE T Get() const { return RunningSum + Compensation; }

		/** Number of terms added so far. */
		[[nodiscard]] FORCEINLINE int32 Num() const { return Count; }

		/** Reset to the empty sum. */
		FORCEINLINE void Reset()
		{
			RunningSum = T(0);
			Compensation = T(0);
			Count = 0;
		}

	private:
		T RunningSum = T(0);
		T Compensation = T(0);
		int32 Count = 0;
	};

	/** Compensated sum over a view of terms. */
	template<typename T>
	[[nodiscard]] FORCEINLINE T KahanSum(TArrayView<const T> Values)
	{
		TKahanAccumulator<T> Acc;
		for (const T& V : Values)
		{
			Acc.Add(V);
		}
		return Acc.Get();
	}

	/** Compensated sum over a TArray (convenience overload). */
	template<typename T>
	[[nodiscard]] FORCEINLINE T KahanSum(const TArray<T>& Values)
	{
		return KahanSum<T>(TArrayView<const T>(Values));
	}

	using FFloatKahan = TKahanAccumulator<float>;
	using FDoubleKahan = TKahanAccumulator<double>;

} // namespace GalaxyMath
