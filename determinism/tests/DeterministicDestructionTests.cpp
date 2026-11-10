// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// GalaxyDeterminism::DestructionOrder / DestroySorted -- the destroy SEQUENCE.
//
// The property under test is not "the helper sorts". It is that a batch collected
// in two different orders is destroyed in ONE order, and therefore returns handles
// to EnTT's free list in one order -- which is what makes the entities created
// next identical. The recycle test at the bottom is the one that would actually
// have caught the seven defects this helper was written for; the rest are the
// contract around it.
// ============================================================================

#include "ECS/Determinism/DeterministicDestruction.h"
#include "ECS/Components/Core/ECSGalaxyTopologyComponents.h" // CGalaxyStableId
#include "TestUtilities.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A creation-ordered pool of plain entities, no stable ids. */
	TArray<entt::entity> MakeEntities(entt::registry& Registry, int32 Count)
	{
		TArray<entt::entity> Made;
		Made.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Made.Add(Registry.create());
		}
		return Made;
	}

	/** Deterministic shuffle: a fixed permutation, so the two inputs really do differ. */
	TArray<entt::entity> Rotated(const TArray<entt::entity>& In, int32 By)
	{
		TArray<entt::entity> Out;
		Out.Reserve(In.Num());
		for (int32 Index = 0; Index < In.Num(); ++Index)
		{
			Out.Add(In[(Index + By) % In.Num()]);
		}
		return Out;
	}
}

// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionOrderIsIndependentOfCollectionOrderTest,
	"GALAXY_PROJECT.Determinism.DestructionOrder.Unit.IndependentOfCollectionOrder",
	GALAXY_TEST_FLAGS
)

bool FDestructionOrderIsIndependentOfCollectionOrderTest::RunTest(const FString& Parameters)
{
	entt::registry Registry = TestUtilities::CreateTestRegistry();
	const TArray<entt::entity> Pool = MakeEntities(Registry, 8);

	const TArray<entt::entity> FromOrderA = GalaxyDeterminism::DestructionOrder(Registry, Pool);
	const TArray<entt::entity> FromOrderB = GalaxyDeterminism::DestructionOrder(Registry, Rotated(Pool, 3));

	TestEqual(TEXT("both orderings yield the same count"), FromOrderB.Num(), FromOrderA.Num());
	for (int32 Index = 0; Index < FromOrderA.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("element %d matches across collection orders"), Index),
			FromOrderA[Index] == FromOrderB[Index]);
	}
	return true;
}

// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionOrderPrefersStableIdTest,
	"GALAXY_PROJECT.Determinism.DestructionOrder.Unit.PrefersStableId",
	GALAXY_TEST_FLAGS
)

bool FDestructionOrderPrefersStableIdTest::RunTest(const FString& Parameters)
{
	// Stable ids DESCEND as the handles ascend, so the two orders disagree completely.
	// If the projection were ever dropped, this is the test that notices.
	entt::registry Registry = TestUtilities::CreateTestRegistry();
	const TArray<entt::entity> Pool = MakeEntities(Registry, 4);
	for (int32 Index = 0; Index < Pool.Num(); ++Index)
	{
		Registry.emplace<CGalaxyStableId>(Pool[Index]).Id = static_cast<int64>(100 - Index);
	}

	const TArray<entt::entity> Ordered = GalaxyDeterminism::DestructionOrder(Registry, Pool);

	TestEqual(TEXT("all four survive the projection"), Ordered.Num(), 4);
	for (int32 Index = 0; Index + 1 < Ordered.Num(); ++Index)
	{
		const int64 Current = Registry.get<CGalaxyStableId>(Ordered[Index]).Id;
		const int64 Next = Registry.get<CGalaxyStableId>(Ordered[Index + 1]).Id;
		TestTrue(TEXT("stable ids ascend, so handle order did NOT decide this"), Current < Next);
	}
	return true;
}

// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionOrderMixesStampedAndUnstampedTest,
	"GALAXY_PROJECT.Determinism.DestructionOrder.Unit.UnstampedFallback",
	GALAXY_TEST_FLAGS
)

bool FDestructionOrderMixesStampedAndUnstampedTest::RunTest(const FString& Parameters)
{
	// A batch of projectiles (no stable id) mixed with saved entities (stamped). The
	// unstamped run keys to 0 and must stay TOTAL among itself via the handle tiebreak.
	entt::registry Registry = TestUtilities::CreateTestRegistry();
	const TArray<entt::entity> Pool = MakeEntities(Registry, 6);
	Registry.emplace<CGalaxyStableId>(Pool[1]).Id = 7;
	Registry.emplace<CGalaxyStableId>(Pool[4]).Id = 9;

	const TArray<entt::entity> FromOrderA = GalaxyDeterminism::DestructionOrder(Registry, Pool);
	const TArray<entt::entity> FromOrderB = GalaxyDeterminism::DestructionOrder(Registry, Rotated(Pool, 5));

	TestEqual(TEXT("mixed batch keeps every element"), FromOrderA.Num(), 6);
	for (int32 Index = 0; Index < FromOrderA.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("mixed element %d is order-stable"), Index),
			FromOrderA[Index] == FromOrderB[Index]);
	}

	// The two stamped entities sort last, after the whole key-0 run.
	TestTrue(TEXT("stamped entities sort after unstamped ones"),
		Registry.all_of<CGalaxyStableId>(FromOrderA[4]) && Registry.all_of<CGalaxyStableId>(FromOrderA[5]));
	return true;
}

// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestroySortedSkipsInvalidTest,
	"GALAXY_PROJECT.Determinism.DestructionOrder.Unit.SkipsInvalid",
	GALAXY_TEST_FLAGS
)

bool FDestroySortedSkipsInvalidTest::RunTest(const FString& Parameters)
{
	entt::registry Registry = TestUtilities::CreateTestRegistry();
	TArray<entt::entity> Pool = MakeEntities(Registry, 5);

	// Kill two out from under the batch, the way a cascading teardown would.
	Registry.destroy(Pool[0]);
	Registry.destroy(Pool[3]);

	const int32 Destroyed = GalaxyDeterminism::DestroySorted(Registry, Pool);

	TestEqual(TEXT("only the live entities were destroyed"), Destroyed, 3);
	for (const entt::entity Entity : Pool)
	{
		TestFalse(TEXT("nothing in the batch survives"), Registry.valid(Entity));
	}
	return true;
}

// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestroySortedProjectionTest,
	"GALAXY_PROJECT.Determinism.DestructionOrder.Unit.Projection",
	GALAXY_TEST_FLAGS
)

bool FDestroySortedProjectionTest::RunTest(const FString& Parameters)
{
	// The shape UnitDerivedStateProjector and EntitySetTransfer use: the entity is a
	// FIELD of the collected element, not the element itself.
	struct FCarcass { int32 Payload; entt::entity Entity; };

	entt::registry Registry = TestUtilities::CreateTestRegistry();
	const TArray<entt::entity> Pool = MakeEntities(Registry, 4);

	TArray<FCarcass> Batch;
	for (int32 Index = 0; Index < Pool.Num(); ++Index)
	{
		Batch.Add(FCarcass{ Index, Pool[Index] });
	}

	const int32 Destroyed = GalaxyDeterminism::DestroySorted(Registry, Batch,
		[](const FCarcass& Item) { return Item.Entity; });

	TestEqual(TEXT("the projection reached every entity"), Destroyed, 4);
	for (const entt::entity Entity : Pool)
	{
		TestFalse(TEXT("projected entity destroyed"), Registry.valid(Entity));
	}
	return true;
}

// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionOrderRecycleParityTest,
	"GALAXY_PROJECT.Determinism.DestructionOrder.Unit.RecycleParity",
	GALAXY_TEST_FLAGS
)

bool FDestructionOrderRecycleParityTest::RunTest(const FString& Parameters)
{
	// THE POINT OF THE WHOLE HELPER. Two registries built identically, their doomed
	// batches collected in DIFFERENT orders, then destroyed and refilled. If the
	// destroy sequence differed, the entities created afterwards would receive
	// different handles -- and this codebase tie-breaks on entt::to_integral, so that
	// divergence reaches real decisions.
	auto RunOne = [](int32 RotateBy)
	{
		entt::registry Registry = TestUtilities::CreateTestRegistry();
		const TArray<entt::entity> Pool = MakeEntities(Registry, 6);

		GalaxyDeterminism::DestroySorted(Registry, Rotated(Pool, RotateBy));

		TArray<uint32> Reissued;
		for (int32 Index = 0; Index < 6; ++Index)
		{
			Reissued.Add(entt::to_integral(Registry.create()));
		}
		return Reissued;
	};

	const TArray<uint32> HandlesA = RunOne(0);
	const TArray<uint32> HandlesB = RunOne(4);

	TestEqual(TEXT("both runs reissued the same count"), HandlesB.Num(), HandlesA.Num());
	for (int32 Index = 0; Index < HandlesA.Num(); ++Index)
	{
		TestEqual(
			FString::Printf(TEXT("recycled handle %d matches across destroy orders"), Index),
			HandlesB[Index], HandlesA[Index]);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
