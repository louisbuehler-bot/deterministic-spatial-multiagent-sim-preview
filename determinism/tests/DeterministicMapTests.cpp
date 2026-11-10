// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// TDeterministicMap / TDeterministicSet -- the container that cannot be walked
// in hash order.
//
// WHAT IS ACTUALLY UNDER TEST. The type's headline property -- "a raw range-for
// does not compile" -- is NOT testable from inside a test that must itself
// compile. It is enforced by the absence of begin()/end(), which is a fact about
// the header, and the audit `deterministic-map-surface` asserts that absence in
// the SOURCE TEXT (a C++ test cannot assert that something fails to build).
//
// What IS testable, and where every real bug in this type will live, is the
// SORTED KEY CACHE. A stale cache produces the exact defect the type exists to
// remove -- an iteration order that is a function of history rather than of
// contents -- and it produces it while every call site looks correct. So the
// cases below are overwhelmingly invalidation cases: mutate through each mutating
// method in turn and assert the NEXT walk sees the change.
//
// THE ORDER ASSERTIONS USE NON-MONOTONIC INSERTION ON PURPOSE. Keys are inserted
// in an order that is neither ascending nor descending, so "sorted" and
// "insertion order" are distinguishable results. Inserting 1,2,3 and asserting
// 1,2,3 would pass against a completely broken sort.
// ============================================================================

#include "Containers/DeterministicMap.h"
#include "TestUtilities.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Keys chosen so ascending order differs from every insertion order used below. */
	const TArray<int32> DetMapKeys = {50, 10, 90, 30, 70};

	TArray<int32> WalkKeys(const TDeterministicMap<int32, int32>& Map)
	{
		TArray<int32> Seen;
		Map.ForEachSorted([&Seen](int32 Key, int32 /*Value*/) { Seen.Add(Key); });
		return Seen;
	}

	/**
	 * Build a map whose sparse-array layout is NOT its sorted order, using the same
	 * free-list lever PerturbationTests.cpp documents: fill with decoys, free them
	 * ascending, then insert forward so entries land in reversed index order.
	 *
	 * Without this, a map built by straight ascending inserts would iterate
	 * correctly even with the sort removed entirely.
	 */
	void PopulateWithChurnedLayout(TDeterministicMap<int32, int32>& Out)
	{
		Out.Reset();
		for (int32 Index = 0; Index < DetMapKeys.Num(); ++Index)
		{
			Out.Add(-1000 - Index, 0);
		}
		for (int32 Index = 0; Index < DetMapKeys.Num(); ++Index)
		{
			Out.Remove(-1000 - Index);
		}
		for (const int32 Key : DetMapKeys)
		{
			Out.Add(Key, Key * 2);
		}
	}
}

// ----------------------------------------------------------------------------
// The order contract
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeterministicMapOrderTest,
	"GALAXY_PROJECT.Determinism.DeterministicMap.Unit.Order",
	GALAXY_TEST_FLAGS)

bool FDeterministicMapOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	TDeterministicMap<int32, int32> Map;
	PopulateWithChurnedLayout(Map);

	// THE CONTROL that makes the assertion below mean something: prove the
	// underlying map really does walk in a different order, so "sorted" is a
	// result the type produced rather than one the layout handed it. If this
	// ever fails, the engine's sparse-array behaviour changed and every order
	// assertion in this file has quietly become vacuous.
	TArray<int32> RawOrder;
	for (const auto& Pair : Map.UnorderedForCommutativeFoldOnly()) // tmap-order-safe: observing the raw order IS the control
	{
		RawOrder.Add(Pair.Key);
	}
	TestNotEqual(TEXT("raw layout differs from sorted order (control)"),
		RawOrder, DetMapKeys);

	TArray<int32> Expected = DetMapKeys;
	Expected.Sort();
	TestEqual(TEXT("ForEachSorted walks ascending key order"), WalkKeys(Map), Expected);
	TestEqual(TEXT("SortedKeys agrees with ForEachSorted"), Map.SortedKeys(), Expected);

	// Values arrive with their own keys, not shifted by one.
	bool bPairsMatch = true;
	Map.ForEachSorted([&bPairsMatch](int32 Key, int32 Value)
	{
		if (Value != Key * 2) { bPairsMatch = false; }
	});
	TestTrue(TEXT("each key is delivered with its own value"), bPairsMatch);

	return true;
}

// ----------------------------------------------------------------------------
// Cache invalidation -- one case per mutating method. This is where the bugs are.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeterministicMapCacheInvalidationTest,
	"GALAXY_PROJECT.Determinism.DeterministicMap.Unit.CacheInvalidation",
	GALAXY_TEST_FLAGS)

bool FDeterministicMapCacheInvalidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Every case WARMS the cache first (by walking), then mutates, then walks
	// again. Without the warm-up the cache would be rebuilt anyway and a missing
	// invalidation would pass.

	{
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		Map.Add(10, 1);
		TestEqual(TEXT("Add: warm"), WalkKeys(Map), TArray<int32>({10, 20}));
		Map.Add(5, 1);
		TestEqual(TEXT("Add invalidates"), WalkKeys(Map), TArray<int32>({5, 10, 20}));
	}

	{
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		TestEqual(TEXT("FindOrAdd: warm"), WalkKeys(Map), TArray<int32>({20}));
		Map.FindOrAdd(7) = 1;
		TestEqual(TEXT("FindOrAdd invalidates on the ADD path"),
			WalkKeys(Map), TArray<int32>({7, 20}));
	}

	{
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		Map.Add(10, 1);
		TestEqual(TEXT("Emplace: warm"), WalkKeys(Map), TArray<int32>({10, 20}));
		Map.Emplace(15, 1);
		TestEqual(TEXT("Emplace invalidates"), WalkKeys(Map), TArray<int32>({10, 15, 20}));
	}

	{
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		Map.Add(10, 1);
		Map.Add(30, 1);
		TestEqual(TEXT("Remove: warm"), WalkKeys(Map), TArray<int32>({10, 20, 30}));
		Map.Remove(20);
		TestEqual(TEXT("Remove invalidates"), WalkKeys(Map), TArray<int32>({10, 30}));
	}

	{
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		TestEqual(TEXT("Reset: warm"), WalkKeys(Map), TArray<int32>({20}));
		Map.Reset();
		TestEqual(TEXT("Reset empties the walk"), WalkKeys(Map), TArray<int32>());
		// And the map is USABLE after Reset -- a Reset that left the cache marked
		// clean-and-empty while the map refilled would walk nothing.
		Map.Add(3, 1);
		TestEqual(TEXT("Add after Reset is seen"), WalkKeys(Map), TArray<int32>({3}));
	}

	{
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		TestEqual(TEXT("Empty: warm"), WalkKeys(Map), TArray<int32>({20}));
		Map.Empty();
		TestEqual(TEXT("Empty empties the walk"), WalkKeys(Map), TArray<int32>());
		Map.Add(4, 1);
		TestEqual(TEXT("Add after Empty is seen"), WalkKeys(Map), TArray<int32>({4}));
	}

	{
		// Reserve must NOT invalidate incorrectly in either direction: it changes
		// no contents, so the walk is unchanged -- but it must also not corrupt
		// the cache it shares a capacity with.
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		Map.Add(10, 1);
		TestEqual(TEXT("Reserve: warm"), WalkKeys(Map), TArray<int32>({10, 20}));
		Map.Reserve(128);
		TestEqual(TEXT("Reserve leaves the walk unchanged"),
			WalkKeys(Map), TArray<int32>({10, 20}));
		Map.Add(1, 1);
		TestEqual(TEXT("Add after Reserve is seen"),
			WalkKeys(Map), TArray<int32>({1, 10, 20}));
	}

	{
		// A CONST walk must be able to rebuild the cache -- the cache is mutable
		// precisely so this works. If it could not, the first walk of a const
		// reference would return an empty or stale array.
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		Map.Add(10, 1);
		const TDeterministicMap<int32, int32>& ConstRef = Map;
		TArray<int32> Seen;
		ConstRef.ForEachSorted([&Seen](int32 Key, int32) { Seen.Add(Key); });
		TestEqual(TEXT("a const walk rebuilds a cold cache"), Seen, TArray<int32>({10, 20}));
	}

	return true;
}

// ----------------------------------------------------------------------------
// Mutable walk: values are writable, and mutating the MAP during the walk does
// not corrupt the iteration.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeterministicMapMutableWalkTest,
	"GALAXY_PROJECT.Determinism.DeterministicMap.Unit.MutableWalk",
	GALAXY_TEST_FLAGS)

bool FDeterministicMapMutableWalkTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	{
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		Map.Add(10, 1);
		Map.ForEachSortedMutable([](int32 Key, int32& Value) { Value = Key; });
		TestEqual(TEXT("mutable walk writes through"), *Map.Find(10), 10);
		TestEqual(TEXT("mutable walk writes through (2)"), *Map.Find(20), 20);
	}

	{
		// The reason ForEachSortedMutable walks a COPY of the key array: a callback
		// that adds to the map would otherwise reallocate the array being iterated.
		// The newly added key is NOT visited by this walk (it was not in the
		// snapshot), which is the defined behaviour rather than an accident.
		TDeterministicMap<int32, int32> Map;
		Map.Add(20, 1);
		Map.Add(10, 1);
		int32 Visits = 0;
		Map.ForEachSortedMutable([&Map, &Visits](int32 Key, int32&)
		{
			++Visits;
			if (Key == 10) { Map.Add(99, 7); }
		});
		TestEqual(TEXT("a walk that inserts visits only the pre-walk snapshot"), Visits, 2);
		TestEqual(TEXT("the insert did land"), Map.Num(), 3);
		TestEqual(TEXT("the NEXT walk sees it"), WalkKeys(Map), TArray<int32>({10, 20, 99}));
	}

	{
		// A callback that REMOVES a key it has not reached yet: the snapshot still
		// names it, so the walk must skip it rather than dereference a null Find.
		TDeterministicMap<int32, int32> Map;
		Map.Add(10, 1);
		Map.Add(20, 1);
		Map.Add(30, 1);
		TArray<int32> Seen;
		Map.ForEachSortedMutable([&Map, &Seen](int32 Key, int32&)
		{
			Seen.Add(Key);
			if (Key == 10) { Map.Remove(20); }
		});
		TestEqual(TEXT("a removed-during-walk key is skipped, not crashed on"),
			Seen, TArray<int32>({10, 30}));
	}

	return true;
}

// ----------------------------------------------------------------------------
// FName keys route to FNameLexicalLess, inherited from DeterministicIteration.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeterministicMapFNameOrderTest,
	"GALAXY_PROJECT.Determinism.DeterministicMap.Unit.FNameOrder",
	GALAXY_TEST_FLAGS)

bool FDeterministicMapFNameOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// FName's DEFAULT ordering is by name-table index (registration order), which
	// differs between runs. These names are registered here in an order that is
	// not their lexical order, so a type using TLess<FName> would very likely
	// produce registration order instead -- and this asserts lexical.
	TDeterministicMap<FName, int32> Map;
	Map.Add(FName(TEXT("zulu")), 1);
	Map.Add(FName(TEXT("alpha")), 2);
	Map.Add(FName(TEXT("mike")), 3);

	TArray<FName> Seen;
	Map.ForEachSorted([&Seen](FName Key, int32) { Seen.Add(Key); });

	TestEqual(TEXT("FName keys walk in LEXICAL order"), Seen,
		TArray<FName>({FName(TEXT("alpha")), FName(TEXT("mike")), FName(TEXT("zulu"))}));

	return true;
}

// ----------------------------------------------------------------------------
// TDeterministicSet -- same contract, element-keyed.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeterministicSetTest,
	"GALAXY_PROJECT.Determinism.DeterministicMap.Unit.Set",
	GALAXY_TEST_FLAGS)

bool FDeterministicSetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	TDeterministicSet<int32> Set;
	for (int32 Index = 0; Index < 5; ++Index) { Set.Add(-1000 - Index); }
	for (int32 Index = 0; Index < 5; ++Index) { Set.Remove(-1000 - Index); }
	for (const int32 Key : DetMapKeys) { Set.Add(Key); }

	TArray<int32> Expected = DetMapKeys;
	Expected.Sort();
	TestEqual(TEXT("set walks ascending"), Set.SortedElements(), Expected);

	TArray<int32> Seen;
	Set.ForEachSorted([&Seen](int32 Element) { Seen.Add(Element); });
	TestEqual(TEXT("ForEachSorted agrees with SortedElements"), Seen, Expected);

	// Invalidation, same shape as the map's.
	Set.Add(1);
	TestEqual(TEXT("set Add invalidates"), Set.SortedElements().Num(), Expected.Num() + 1);
	Set.Remove(1);
	TestEqual(TEXT("set Remove invalidates"), Set.SortedElements(), Expected);
	Set.Reset();
	TestEqual(TEXT("set Reset empties"), Set.SortedElements().Num(), 0);
	Set.Add(42);
	TestEqual(TEXT("set is usable after Reset"), Set.SortedElements(), TArray<int32>({42}));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
