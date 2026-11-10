// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// TDeterministicMap / TDeterministicSet -- a hash container you CANNOT walk in
// hash order, because the walk does not compile.
//
// WHY THIS EXISTS, GIVEN THAT DeterministicIteration.h ALREADY SOLVES THE MATH.
// `GalaxyDeterminism::SortedKeys(Map)` produces the right answer, and six audits
// look for the sites that forgot to call it. That is DETECTION: the mistake is
// made, shipped, and then found -- if the matcher happens to know the spelling.
// One catalog pass found six iteration spellings every ordering guard was
// silent on at once, and a review the same day found six live defects
// behind markers asserting the opposite. Every one of those was a raw walk that
// COMPILED.
//
// This type moves the same rule from detection to PREVENTION. It owns a TMap and
// exposes no `begin()`/`end()`, so:
//
//     TDeterministicMap<int32, FThing> Map;
//     for (const auto& Pair : Map) { ... }        // DOES NOT COMPILE
//     for (const auto& Pair : Map.SortedPairs()) { ... }   // fine, stated order
//     Map.ForEachSorted([](int32 K, const FThing& V) { ... });   // fine
//
// A matcher can be wrong about a spelling it has never seen. A missing `begin()`
// cannot: there is no phrasing of a range-for that reaches the underlying TMap
// without naming the escape hatch below, and the escape hatch is named so that
// naming it is a decision.
//
// WHAT IT IS NOT. Not a replacement for TDeterministicCatalog -- catalog-shaped
// state (registered entries with stable ids, versions, deltas) still belongs
// there. Not a replacement for a raw TMap in a UPROPERTY: UHT cannot reflect this
// template, so every reflected map stays a TMap and stays the audits' problem.
// This is for the authoritative residue in between: shard state, per-tick lookup
// tables, subsystem indices -- maps that are walked for OUTCOMES and reflected by
// nothing.
//
// THE ORDER IS ASCENDING KEY ORDER, via the same TStableLess that
// DeterministicIteration.h uses -- so FName keys route to FNameLexicalLess (never
// the per-run ComparisonIndex) and pointer keys are a compile error, both
// inherited rather than re-derived.
//
// COST MODEL, AND WHY IT IS NOT TRuntimeStableIdCatalog's. The catalog keeps its
// SortedIds array sorted on EVERY register/unregister -- an O(n) insert, paid
// whether or not anyone iterates. That is right for a catalog (low churn, read
// constantly) and wrong for a shard map rebuilt every tick. This type instead
// caches the sorted key array behind a dirty flag: mutations only mark it stale,
// and the sort is paid once on the next iteration. The common shard shape --
// mutate through one phase, iterate in another -- pays O(n log n) once per phase
// rather than per element or per walk.
//
// THE CACHE IS THE RISKY PART, so it is deliberately dumb: EVERY mutating method
// sets the flag unconditionally, including ones that may not have changed
// anything (a Remove that removed nothing). An over-eager invalidation costs one
// sort; a missed one is a stale iteration order, which is the exact bug class
// this type exists to remove. The cache is also `mutable` so that iterating a
// `const` map can still rebuild it -- iteration is logically const.
//
// EXPORTS: TDeterministicMap<K,V>, TDeterministicSet<T>
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Containers/DeterministicIteration.h"

/**
 * A TMap whose iteration order is always ascending key order, enforced by the
 * absence of begin()/end() rather than by an audit.
 *
 * The lookup and mutation surface is deliberately a subset of TMap's: everything
 * here is order-free by construction. Anything that would hand out or depend on
 * hash order is either absent or renamed to state what it is.
 */
template <typename KeyType, typename ValueType>
class TDeterministicMap
{
public:
	using FKeyType = KeyType;
	using FValueType = ValueType;
	using FUnderlyingMap = TMap<KeyType, ValueType>;

	static_assert(GalaxyDeterminism::Detail::TIsDeterministicallyOrderable<KeyType>::Value,
		"TDeterministicMap: pointer-keyed containers cannot be ordered deterministically "
		"(pointer values are allocation-dependent). Key by a stable id instead.");

	// ------------------------------------------------------------------------
	// Lookup -- all order-free, so all are plain forwards
	// ------------------------------------------------------------------------

	[[nodiscard]] ValueType* Find(const KeyType& Key) { return Map.Find(Key); }
	[[nodiscard]] const ValueType* Find(const KeyType& Key) const { return Map.Find(Key); }

	[[nodiscard]] ValueType FindRef(const KeyType& Key) const { return Map.FindRef(Key); }
	[[nodiscard]] bool Contains(const KeyType& Key) const { return Map.Contains(Key); }
	[[nodiscard]] int32 Num() const { return Map.Num(); }
	[[nodiscard]] bool IsEmpty() const { return Map.IsEmpty(); }

	// ------------------------------------------------------------------------
	// Mutation -- every one invalidates the cache, unconditionally
	// ------------------------------------------------------------------------

	ValueType& Add(const KeyType& Key, const ValueType& Value)
	{
		bSortedKeysDirty = true;
		return Map.Add(Key, Value);
	}

	ValueType& Add(const KeyType& Key, ValueType&& Value)
	{
		bSortedKeysDirty = true;
		return Map.Add(Key, MoveTemp(Value));
	}

	ValueType& FindOrAdd(const KeyType& Key)
	{
		// Invalidated even on the hit path. FindOrAdd cannot report which branch
		// it took without a second lookup, and paying one sort is strictly
		// cheaper than the class of bug a missed invalidation produces.
		bSortedKeysDirty = true;
		return Map.FindOrAdd(Key);
	}

	template <typename... ArgTypes>
	ValueType& Emplace(const KeyType& Key, ArgTypes&&... Args)
	{
		bSortedKeysDirty = true;
		return Map.Emplace(Key, Forward<ArgTypes>(Args)...);
	}

	int32 Remove(const KeyType& Key)
	{
		bSortedKeysDirty = true;
		return Map.Remove(Key);
	}

	void Reset()
	{
		Map.Reset();
		CachedSortedKeys.Reset();
		bSortedKeysDirty = false;  // empty and known-empty, not stale
	}

	void Empty(int32 ExpectedElements = 0)
	{
		Map.Empty(ExpectedElements);
		CachedSortedKeys.Reset();
		bSortedKeysDirty = false;
	}

	void Reserve(int32 Count)
	{
		Map.Reserve(Count);
		CachedSortedKeys.Reserve(Count);
	}

	// ------------------------------------------------------------------------
	// Iteration -- the whole point. There is no begin()/end().
	// ------------------------------------------------------------------------

	/**
	 * Keys in ascending deterministic order. The returned array is a VIEW onto
	 * the internal cache and is invalidated by the next mutation -- copy it if it
	 * has to outlive one.
	 */
	[[nodiscard]] const TArray<KeyType>& SortedKeys() const
	{
		RebuildSortedKeysIfDirty();
		return CachedSortedKeys;
	}

	/** Walk every pair in ascending key order. */
	template <typename FuncType>
	void ForEachSorted(FuncType&& Func) const
	{
		RebuildSortedKeysIfDirty();
		for (const KeyType& Key : CachedSortedKeys)
		{
			const ValueType* Value = Map.Find(Key);
			checkf(Value != nullptr, // checkf-ok: a PROGRAMMING invariant -- RebuildSortedKeysIfDirty() ran two lines up, so a miss here means THIS class's own cache rebuild is broken, never bad data. The ensureMsgf-and-skip remedy is WRONG for this container specifically: skipping a key would silently change what the walk YIELDS, which is a determinism divergence between peers -- strictly worse than a crash, and invisible until a desync hours later.
				TEXT("TDeterministicMap: sorted key cache out of sync with the map."));
			Func(Key, *Value);
		}
	}

	/** Walk every pair in ascending key order, with mutable values. */
	template <typename FuncType>
	void ForEachSortedMutable(FuncType&& Func)
	{
		RebuildSortedKeysIfDirty();
		// Iterate a COPY of the key array: Func may legitimately mutate values,
		// and a value-mutating callback that also touches the map (a FindOrAdd on
		// a sibling key, say) would otherwise invalidate the array being walked.
		// The keys themselves cannot change -- only which of them exist -- and a
		// removed key is caught by the null check rather than by a crash.
		const TArray<KeyType> Keys = SortedKeysCopyForMutableWalk();
		for (const KeyType& Key : Keys)
		{
			if (ValueType* Value = Map.Find(Key))
			{
				Func(Key, *Value);
			}
		}
	}

	/**
	 * THE ESCAPE HATCH. Named at length on purpose: reaching the raw TMap is a
	 * decision, and the name is the place that decision is recorded.
	 *
	 * Legitimate uses are folds whose result cannot depend on visit order --
	 * a count, a sum of integers, a min/max, a per-key write touching nothing
	 * else. A FLOAT sum is NOT one of those (float addition is not associative);
	 * neither is anything with a break, an early return, or a first-wins.
	 *
	 * If you are reaching for this to feed an array the caller will read, you
	 * want SortedKeys() instead.
	 */
	[[nodiscard]] const FUnderlyingMap& UnorderedForCommutativeFoldOnly() const
	{
		return Map;
	}

private:
	void RebuildSortedKeysIfDirty() const
	{
		if (!bSortedKeysDirty)
		{
			return;
		}
		CachedSortedKeys.Reset(Map.Num());
		for (const auto& Pair : Map) // tmap-order-safe: collected then sorted immediately below
		{
			CachedSortedKeys.Add(Pair.Key);
		}
		CachedSortedKeys.Sort(typename GalaxyDeterminism::Detail::TStableLess<KeyType>::Type());
		bSortedKeysDirty = false;
	}

	TArray<KeyType> SortedKeysCopyForMutableWalk()
	{
		RebuildSortedKeysIfDirty();
		return CachedSortedKeys;
	}

	FUnderlyingMap Map;

	// Mutable: iteration is logically const, so a const walk must still be able
	// to pay for the rebuild it needs.
	mutable TArray<KeyType> CachedSortedKeys;
	mutable bool bSortedKeysDirty = false;
};

/**
 * The TSet counterpart. Same contract: no begin()/end(), iteration is ascending
 * element order, the raw set is reachable only through a name that says what
 * reaching for it means.
 */
template <typename ElementType>
class TDeterministicSet
{
public:
	using FElementType = ElementType;
	using FUnderlyingSet = TSet<ElementType>;

	static_assert(GalaxyDeterminism::Detail::TIsDeterministicallyOrderable<ElementType>::Value,
		"TDeterministicSet: pointer elements cannot be ordered deterministically "
		"(pointer values are allocation-dependent). Store a stable id instead.");

	[[nodiscard]] bool Contains(const ElementType& Element) const { return Set.Contains(Element); }
	[[nodiscard]] int32 Num() const { return Set.Num(); }
	[[nodiscard]] bool IsEmpty() const { return Set.IsEmpty(); }

	void Add(const ElementType& Element)
	{
		bSortedDirty = true;
		Set.Add(Element);
	}

	int32 Remove(const ElementType& Element)
	{
		bSortedDirty = true;
		return Set.Remove(Element);
	}

	void Reset()
	{
		Set.Reset();
		CachedSorted.Reset();
		bSortedDirty = false;
	}

	void Reserve(int32 Count)
	{
		Set.Reserve(Count);
		CachedSorted.Reserve(Count);
	}

	/** Elements in ascending deterministic order; a view, invalidated by mutation. */
	[[nodiscard]] const TArray<ElementType>& SortedElements() const
	{
		RebuildIfDirty();
		return CachedSorted;
	}

	template <typename FuncType>
	void ForEachSorted(FuncType&& Func) const
	{
		RebuildIfDirty();
		for (const ElementType& Element : CachedSorted)
		{
			Func(Element);
		}
	}

	/** See TDeterministicMap::UnorderedForCommutativeFoldOnly. */
	[[nodiscard]] const FUnderlyingSet& UnorderedForCommutativeFoldOnly() const
	{
		return Set;
	}

private:
	void RebuildIfDirty() const
	{
		if (!bSortedDirty)
		{
			return;
		}
		CachedSorted.Reset(Set.Num());
		for (const ElementType& Element : Set) // tmap-order-safe: collected then sorted immediately below
		{
			CachedSorted.Add(Element);
		}
		CachedSorted.Sort(typename GalaxyDeterminism::Detail::TStableLess<ElementType>::Type());
		bSortedDirty = false;
	}

	FUnderlyingSet Set;
	mutable TArray<ElementType> CachedSorted;
	mutable bool bSortedDirty = false;
};
