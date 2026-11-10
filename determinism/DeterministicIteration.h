// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// DETERMINISTIC ITERATION over associative containers.
//
// THE RULE THIS EXISTS TO MAKE CHEAP: authoritative simulation must never let
// TMap/TSet iteration order reach an outcome. Hash-container order depends on
// the insertion/removal HISTORY, not on the contents, so two peers (or a loaded
// session vs a continued one) holding identical state can still walk it in
// different orders. Anything derived from that walk -- an id handed out, a
// message ordering, an accumulated float, a checksum -- then diverges.
//
// WHY A SHARED HELPER RATHER THAN SORTING BY HAND: hand-rolled "collect keys,
// sort, re-Find" blocks were re-derived independently across the codebase, and
// the per-site variation is exactly where subtle mistakes live (see the FName
// trap below). One primitive also gives the `tmap-iteration` audit a greppable
// token, so the enforcement rule can be the cheap "member-container iteration
// goes through this helper or carries an explicit suppression" instead of
// type-inferring every range-for.
//
// TWO TRAPS THIS CLOSES BY CONSTRUCTION -- both are compile errors or are
// silently routed to the correct comparator, so a caller cannot get them wrong:
//
//   (1) FName KEYS. FName has no operator< (TLess<FName> does not compile); its
//       cheap comparator, FNameFastLess, compares name-table INDICES, which are
//       assigned in registration order and differ between runs -- so it is
//       nondeterministic and looks perfectly correct at the call site. UE's own
//       comment on FNameLexicalLess says IT is the "stable / deterministic over
//       process runs" one. SortedKeys routes FName to FNameLexicalLess automatically.
//
//   (2) POINTER KEYS. Pointer values are allocation-dependent, so ordering by
//       them is nondeterministic by definition (the `pointer-ordering` audit
//       exists for this). A pointer-keyed container is rejected at compile time;
//       key by a stable id instead.
//
// NOT A SUBSTITUTE FOR TDeterministicCatalog: catalog-shaped state (registered
// entries with stable ids) should live in a TDeterministicCatalog and iterate
// via ForEachByStableId. This helper is for the raw-map residue that will always
// exist -- scheduler shard maps, subsystem lookup tables, per-frame scratch.
//
// THE TIE TRAP (closed by SortByScoreDescThenId / SortByScoreAscThenId below).
// Sorting is the standard fix for hash-order dependence, but it only works if the
// comparator is a TOTAL order. Sorting a map's KEYS is total for free -- keys are
// unique. Sorting by a derived SCORE is not: two entries can tie, and then their
// relative order is decided by the order they were COLLECTED in, which is the hash
// order the sort was added to escape. The nondeterminism survives the fix and now
// looks handled.
//
// Precisely (checked against UE 5.5 Algo::IntroSortInternal, which picks a
// middle-element pivot and uses no randomness): TArray::Sort is a PURE function of
// (input sequence, comparator), so an unstable sort is not itself a nondeterminism
// source. The hazard is strictly the COMPOUND -- nondeterministic input order AND a
// comparator that admits ties. Either one alone is harmless. StableSort does NOT
// rescue it: stability preserves the input order for ties, and here the input order
// is the nondeterministic part.
//
// (A separate cross-COMPILER version of this exists -- different std::sort
// implementations break ties differently for identical input, which cost Factorio a
// desync -- but the project is scoped to per-binary determinism, so it cannot reach us
// unless cross-platform lockstep is ever un-deferred.)
//
// EXPORTS: GalaxyDeterminism::{SortedKeys, SortedElements, ForEachSorted,
//          SortByScoreDescThenId, SortByScoreAscThenId}
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "UObject/NameTypes.h"   // FNameLexicalLess

#include <type_traits>

namespace GalaxyDeterminism
{
	namespace Detail
	{
		// Strip cv/ref so the public templates deduce cleanly from both
		// `TMap<K,V>&` and `const TMap<K,V>&` through a single overload.
		// std:: traits rather than UE's -- TRemoveCV is not exposed via
		// CoreMinimal in UE 5.5 (it compiles as an undeclared identifier).
		template <typename T>
		using TBareType = std::remove_cv_t<std::remove_reference_t<T>>;

		// The comparator a key type must use to sort DETERMINISTICALLY.
		// Primary template: ordinary value types order by operator< (TLess).
		template <typename KeyType>
		struct TStableLess
		{
			using Type = TLess<KeyType>;
		};

		// FName: MUST compare lexically. FName has no operator<, and its index
		// order (FNameFastLess, registration order) is not stable across runs.
		// See trap (1) in the banner.
		template <>
		struct TStableLess<FName>
		{
			using Type = FNameLexicalLess;
		};

		template <typename KeyType>
		struct TIsDeterministicallyOrderable
		{
			static constexpr bool Value = !std::is_pointer_v<KeyType>;
		};
	}

	/**
	 * Keys of an associative container, in a deterministic ascending order.
	 *
	 * Use when the caller needs the key list itself (e.g. to drive a second
	 * pass, or to build a payload). When you just want to walk the container,
	 * prefer ForEachSorted -- it avoids the second Find per element.
	 */
	template <typename MapType>
	[[nodiscard]] TArray<typename Detail::TBareType<MapType>::KeyType> SortedKeys(const MapType& Map)
	{
		using FMap = Detail::TBareType<MapType>;
		using FKey = typename FMap::KeyType;

		static_assert(Detail::TIsDeterministicallyOrderable<FKey>::Value,
			"SortedKeys: pointer-keyed containers cannot be ordered deterministically "
			"(pointer values are allocation-dependent). Key by a stable id instead.");

		TArray<FKey> Keys;
		Keys.Reserve(Map.Num());
		for (const auto& Pair : Map) // tmap-order-safe -- collected then sorted below; order of collection is irrelevant
		{
			Keys.Add(Pair.Key);
		}
		Keys.Sort(typename Detail::TStableLess<FKey>::Type());
		return Keys;
	}

	/**
	 * Keys in a caller-supplied order. Use ONLY when the default ascending key
	 * order is wrong for the algorithm (e.g. a documented priority order); the
	 * predicate must still be a total order over stable data, never over
	 * pointers or FName indices.
	 */
	template <typename MapType, typename PredicateType>
	[[nodiscard]] TArray<typename Detail::TBareType<MapType>::KeyType> SortedKeys(
		const MapType& Map,
		PredicateType Predicate)
	{
		using FMap = Detail::TBareType<MapType>;
		using FKey = typename FMap::KeyType;

		TArray<FKey> Keys;
		Keys.Reserve(Map.Num());
		for (const auto& Pair : Map) // tmap-order-safe -- collected then sorted below
		{
			Keys.Add(Pair.Key);
		}
		Keys.Sort(MoveTemp(Predicate));
		return Keys;
	}

	/**
	 * Elements of a TSet, in a deterministic ascending order.
	 */
	template <typename SetType>
	[[nodiscard]] TArray<typename Detail::TBareType<SetType>::ElementType> SortedElements(const SetType& Set)
	{
		using FSet = Detail::TBareType<SetType>;
		using FElement = typename FSet::ElementType;

		static_assert(Detail::TIsDeterministicallyOrderable<FElement>::Value,
			"SortedElements: pointer-element sets cannot be ordered deterministically "
			"(pointer values are allocation-dependent). Store a stable id instead.");

		TArray<FElement> Elements;
		Elements.Reserve(Set.Num());
		for (const FElement& Element : Set) // tmap-order-safe -- collected then sorted below
		{
			Elements.Add(Element);
		}
		Elements.Sort(typename Detail::TStableLess<FElement>::Type());
		return Elements;
	}

	/**
	 * Walk a map in deterministic ascending key order, invoking Fn(Key, Value).
	 *
	 * A single template deduces MapType as const or non-const from the argument,
	 * so `Value` is handed to Fn as `const V&` for a const map and `V&` for a
	 * mutable one -- mutation during the walk is allowed, INSERTION/REMOVAL is
	 * not (the key snapshot is taken up front; erasing through Fn would leave a
	 * dangling Find). Collect and act afterwards if the body needs to remove.
	 */
	template <typename MapType, typename FuncType>
	void ForEachSorted(MapType& Map, FuncType&& Fn)
	{
		using FMap = Detail::TBareType<MapType>;
		using FKey = typename FMap::KeyType;

		static_assert(Detail::TIsDeterministicallyOrderable<FKey>::Value,
			"ForEachSorted: pointer-keyed containers cannot be ordered deterministically "
			"(pointer values are allocation-dependent). Key by a stable id instead.");

		TArray<FKey> Keys;
		Keys.Reserve(Map.Num());
		for (const auto& Pair : Map) // tmap-order-safe -- collected then sorted below
		{
			Keys.Add(Pair.Key);
		}
		Keys.Sort(typename Detail::TStableLess<FKey>::Type());

		for (const FKey& Key : Keys)
		{
			if (auto* Value = Map.Find(Key))
			{
				Fn(Key, *Value);
			}
		}
	}

	namespace Detail
	{
		/** Shared body: order by Score (direction chosen by the caller's compare),
		 *  then ALWAYS ascending by Id so no pair can compare equal. */
		template <typename ArrayType, typename ScoreFn, typename IdFn, typename ScoreLess>
		void SortByScoreThenIdImpl(ArrayType& Array, ScoreFn Score, IdFn Id, ScoreLess ScoreComesFirst)
		{
			using FId = TBareType<decltype(Id(Array[0]))>;
			static_assert(TIsDeterministicallyOrderable<FId>::Value,
				"SortByScore*ThenId: the tiebreak must be a stable id, not a pointer "
				"(pointer values are allocation-dependent, so the tiebreak would itself "
				"be nondeterministic). Project a stable id instead.");

			Array.Sort([&Score, &Id, &ScoreComesFirst](const auto& A, const auto& B)
			{
				const auto ScoreA = Score(A);
				const auto ScoreB = Score(B);
				if (ScoreComesFirst(ScoreA, ScoreB)) { return true;  }
				if (ScoreComesFirst(ScoreB, ScoreA)) { return false; }
				// Scores tie -- the tiebreak is what makes this a TOTAL order, and it
				// is the entire reason this helper exists. Without it the tied run
				// keeps whatever order collection happened to produce.
				return typename TStableLess<FId>::Type()(Id(A), Id(B));
			});
		}
	}

	/**
	 * Sort BEST-FIRST by a score, breaking ties by a stable id.
	 *
	 * The shape almost every "pick the top N" / "act on the strongest first" site
	 * wants. Both projections take an element and return a comparable value:
	 *
	 *   GalaxyDeterminism::SortByScoreDescThenId(Candidates,
	 *       [](const FCandidate& C) { return C.Threat; },      // score, high first
	 *       [](const FCandidate& C) { return C.StableId; });   // total-order tiebreak
	 *
	 * Use this rather than a hand-written single-key comparator whenever the array
	 * was collected from a TMap/TSet or in any other order you do not control.
	 * Float scores are fine (per-binary determinism); exact float ties are
	 * precisely what the tiebreak absorbs.
	 */
	template <typename ArrayType, typename ScoreFn, typename IdFn>
	void SortByScoreDescThenId(ArrayType& Array, ScoreFn Score, IdFn Id)
	{
		Detail::SortByScoreThenIdImpl(Array, MoveTemp(Score), MoveTemp(Id),
			[](const auto& L, const auto& R) { return R < L; }); // descending
	}

	/** Ascending-score counterpart of SortByScoreDescThenId; same tiebreak contract. */
	template <typename ArrayType, typename ScoreFn, typename IdFn>
	void SortByScoreAscThenId(ArrayType& Array, ScoreFn Score, IdFn Id)
	{
		Detail::SortByScoreThenIdImpl(Array, MoveTemp(Score), MoveTemp(Id),
			[](const auto& L, const auto& R) { return L < R; }); // ascending
	}
}
