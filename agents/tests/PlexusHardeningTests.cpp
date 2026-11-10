// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Plexus hardening tests.
//
// Covers the four guarantees added before the Plexus takes load-bearing weight:
//   1. DURABLE LAYOUT -- sibling ordering keys on CGalaxyStableId, so the tree
//      shape (incl. WHICH city is the capital) is identical regardless of entity
//      creation / entt-handle order (the save/load + churn stability property).
//   2. STRUCTURAL INVARIANTS + CONSERVATION -- a seeded fuzz sweep builds random
//      registries and asserts FPlexusSnapshot::Validate, build-twice determinism,
//      up-pass rollup == sum of leaf shortfalls, and down-pass conservation
//      (leaf budgets sum to the pool whenever the tree can absorb it).
//   3. CHILDLESS-CITY CONSERVATION -- the uniform fallback must not evaporate a
//      pool share into a City Governor with no outpost holdings (the leak the
//      City tier introduced; pinned here).
//   4. LOCALITY EDGE HYGIENE -- ResolvePatronOutpost applies the validity rules
//      (live outpost, this registry, same empire) and the economy-pass sweep
//      clears ties that no longer resolve, through the single writer.
//   5. READ-ONLY HELPERS -- the helpers the g.plexus.* dumps call take a const
//      registry, so a dump creates no component pool (diagnostics-contract).
// ============================================================================

#include "ECS/Systems/Plexus/PlexusSnapshot.h"
#include "ECS/Systems/Plexus/PlexusDownPass.h"
#include "ECS/Systems/Plexus/PlexusUpPass.h"
#include "ECS/Systems/Plexus/PlexusEconomyPass.h"
#include "ECS/Systems/Plexus/PlexusLocality.h"
#include "ECS/Systems/Plexus/PlexusIncome.h"            // ComputeEmpireSupplyStock
#include "ECS/Systems/Plexus/SquadMembership.h"         // JoinNoRebuild / AreCachesConsistent
#include "ECS/Systems/Fabrication/EconomySubstrate.h"   // ForEachSupply (both overloads)
#include "ECS/Components/City/CityAvatar.h"
#include "ECS/Components/Outpost/OutpostAvatar.h"
#include "ECS/Components/Core/ECSGalaxyTopologyComponents.h" // CGalaxyStableId
#include "TestUtilities.h"

#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	entt::entity MakeHardeningUnit(entt::registry& Reg, int32 EmpireSlotId, int64 StableId = 0)
	{
		const entt::entity E = Reg.create();
		Reg.emplace<CUnitAvatar>(E);
		Reg.emplace<CEmpireSlot>(E).EmpireSlotId = EmpireSlotId;
		if (StableId != 0)
		{
			Reg.emplace<CGalaxyStableId>(E).Id = StableId;
		}
		return E;
	}

	entt::entity MakeHardeningCity(entt::registry& Reg, int32 EmpireSlotId, int64 StableId = 0)
	{
		const entt::entity E = Reg.create();
		Reg.emplace<CCityAvatar>(E).OwnerEmpireSlot = EmpireSlotId;
		if (StableId != 0)
		{
			Reg.emplace<CGalaxyStableId>(E).Id = StableId;
		}
		return E;
	}

	entt::entity MakeHardeningOutpost(entt::registry& Reg, int32 EmpireSlotId, int64 StableId = 0)
	{
		const entt::entity E = Reg.create();
		Reg.emplace<COutpostAvatar>(E).EmpireSlotId = EmpireSlotId;
		Reg.emplace<CEmpireSlot>(E).EmpireSlotId = EmpireSlotId;
		if (StableId != 0)
		{
			Reg.emplace<CGalaxyStableId>(E).Id = StableId;
		}
		return E;
	}

	entt::entity MakeHardeningSquadWithMembers(entt::registry& Reg, int32 EmpireSlotId, int64 SquadId, int32 MemberCount, int64 FirstMemberStableId = 0)
	{
		const entt::entity SquadE = Reg.create();
		Reg.emplace<CSquad>(SquadE, SquadId, /*CoreId*/ 0u, /*CardId*/ NAME_None);
		Reg.emplace<CEmpireSlot>(SquadE).EmpireSlotId = EmpireSlotId;
		for (int32 m = 0; m < MemberCount; ++m)
		{
			const entt::entity Member = MakeHardeningUnit(Reg, EmpireSlotId, FirstMemberStableId != 0 ? FirstMemberStableId + m : 0);
			Reg.emplace<CSquadMembership>(Member, SquadE, SquadId, m);
		}
		return SquadE;
	}

	/** Layout signature independent of entt handles: one "kind:sid(parent-sid)" token per node. */
	FString LayoutSignature(entt::registry& Reg, const Plexus::FPlexusSnapshot& Snapshot)
	{
		const auto StableOf = [&Reg](entt::entity E) -> int64
		{
			const CGalaxyStableId* Id = Reg.valid(E) ? Reg.try_get<CGalaxyStableId>(E) : nullptr;
			return Id ? Id->Id : 0;
		};
		FString Signature;
		for (const Plexus::FPlexusSnapshot::FNode& Node : Snapshot.Nodes)
		{
			const int64 ParentSid = Node.Parent >= 0 ? StableOf(Snapshot.Nodes[Node.Parent].Nucleus.Entity) : -1;
			Signature += FString::Printf(TEXT("%d:%lld(%lld);"),
				static_cast<int32>(Node.Nucleus.Kind), StableOf(Node.Nucleus.Entity), ParentSid);
		}
		return Signature;
	}

	/** Sum of CEconomicBudget over the snapshot's unit leaves. */
	float SumLeafBudgets(entt::registry& Reg, const Plexus::FPlexusSnapshot& Snapshot)
	{
		float Sum = 0.0f;
		for (const Plexus::FPlexusSnapshot::FNode& Node : Snapshot.Nodes)
		{
			if (!Node.HasChildren() && Node.Nucleus.Kind == Plexus::ENucleusKind::Unit && Reg.valid(Node.Nucleus.Entity))
			{
				if (const CEconomicBudget* Budget = Reg.try_get<CEconomicBudget>(Node.Nucleus.Entity))
				{
					Sum += Budget->SupplyBudget;
				}
			}
		}
		return Sum;
	}
}

// ----------------------------------------------------------------------------
// 1. The spine layout is a function of STABLE IDS, not entt creation order: two
// registries with identical logical content but reversed creation order produce
// the same tree -- including the same capital city holding the outposts.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusStableOrderTest,
	"GALAXY_PROJECT.Plexus.Snapshot.Unit.StableOrderSurvivesCreationOrder",
	GALAXY_TEST_FLAGS)

bool FPlexusStableOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Registry A: cities created high-sid-first, settlements before forces.
	entt::registry RegA;
	MakeHardeningCity(RegA, /*slot*/ 1, /*sid*/ 200);
	MakeHardeningCity(RegA, /*slot*/ 1, /*sid*/ 100);
	MakeHardeningOutpost(RegA, 1, 400);
	MakeHardeningOutpost(RegA, 1, 300);
	MakeHardeningUnit(RegA, 1, 500);

	// Registry B: the SAME logical content, created in the opposite order.
	entt::registry RegB;
	MakeHardeningUnit(RegB, 1, 500);
	MakeHardeningOutpost(RegB, 1, 300);
	MakeHardeningOutpost(RegB, 1, 400);
	MakeHardeningCity(RegB, 1, 100);
	MakeHardeningCity(RegB, 1, 200);

	const Plexus::FPlexusSnapshot SnapA = Plexus::FPlexusSnapshot::Build(RegA, Plexus::FNucleus{ Plexus::ENucleusKind::Empire, 1 });
	const Plexus::FPlexusSnapshot SnapB = Plexus::FPlexusSnapshot::Build(RegB, Plexus::FNucleus{ Plexus::ENucleusKind::Empire, 1 });

	TestTrue(TEXT("Snapshot A passes structural validation"), SnapA.Validate());
	TestTrue(TEXT("Snapshot B passes structural validation"), SnapB.Validate());
	TestEqual(TEXT("Layout signature is creation-order independent"), LayoutSignature(RegA, SnapA), LayoutSignature(RegB, SnapB));

	// The capital (the city holding the outposts) is the LOWEST-STABLE-ID city in both.
	for (const Plexus::FPlexusSnapshot* Snap : { &SnapA, &SnapB })
	{
		entt::registry& Reg = (Snap == &SnapA) ? RegA : RegB;
		bool bFoundCapital = false;
		for (const Plexus::FPlexusSnapshot::FNode& Node : Snap->Nodes)
		{
			if (Node.Nucleus.Kind == Plexus::ENucleusKind::City && Node.HasChildren())
			{
				bFoundCapital = true;
				const CGalaxyStableId* Sid = Reg.try_get<CGalaxyStableId>(Node.Nucleus.Entity);
				TestEqual(TEXT("Capital is the lowest-stable-id city"), Sid ? Sid->Id : 0, static_cast<int64>(100));
				TestEqual(TEXT("Capital holds both outposts"), Node.ChildCount, 2);
			}
		}
		TestTrue(TEXT("A capital city with holdings exists"), bFoundCapital);
	}
	return true;
}

// ----------------------------------------------------------------------------
// 2. Seeded fuzz: random registries hold the structural invariants, build
// deterministically, and conserve material through both passes.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusFuzzInvariantsTest,
	"GALAXY_PROJECT.Plexus.Snapshot.Unit.FuzzedInvariantsAndConservation",
	GALAXY_TEST_FLAGS)

bool FPlexusFuzzInvariantsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FRandomStream Rand(20260719); // rng-safe: fixed seed, test-only
	constexpr int32 kIterations = 40;
	constexpr float kPool = 1000.0f;
	constexpr int32 kTick = 7;

	for (int32 Iter = 0; Iter < kIterations; ++Iter)
	{
		entt::registry Reg;
		int64 NextSid = 1000;
		const auto NextStableId = [&NextSid, &Rand]() -> int64 { NextSid += Rand.RandRange(1, 7); return NextSid; };
		const auto MaybeSid = [&NextStableId, &Rand]() -> int64 { return Rand.RandRange(0, 3) > 0 ? NextStableId() : 0; }; // some entities stay unstamped

		float ExpectedUnfunded = 0.0f;
		TArray<entt::entity> Slot1Leaves;

		const auto SeedShortfall = [&](entt::entity Leaf)
		{
			if (Rand.RandRange(0, 9) < 3)
			{
				CEconomicShortfall& Shortfall = Reg.emplace<CEconomicShortfall>(Leaf);
				Shortfall.Reset(kTick);
				FEconomicShortfallEntry Entry;
				Entry.Objective = ESpendingObjective::Defense;
				Entry.UnfundedMaterial = static_cast<float>(Rand.RandRange(1, 50));
				Entry.UnfundedUnits = 1;
				Shortfall.Entries.Add(Entry);
				ExpectedUnfunded += Entry.UnfundedMaterial;
			}
		};
		float ExpectedDemandTotal = 0.0f;
		const auto SeedDemand = [&](entt::entity Leaf)
		{
			if (Rand.RandRange(0, 9) < 2)
			{
				const float Demand = static_cast<float>(Rand.RandRange(1, 20));
				Reg.emplace<CSupplyDemandProfile>(Leaf).ByObjective[static_cast<int32>(ESpendingObjective::Defense)] = Demand;
				ExpectedDemandTotal += Demand;
			}
		};

		// Slot-1 content: cities (some childless), outposts, squads with members, loose units.
		const int32 NumCities = Rand.RandRange(0, 3);
		for (int32 i = 0; i < NumCities; ++i)
		{
			MakeHardeningCity(Reg, 1, MaybeSid());
		}
		const int32 NumOutposts = Rand.RandRange(0, 4);
		for (int32 i = 0; i < NumOutposts; ++i)
		{
			const entt::entity Outpost = MakeHardeningOutpost(Reg, 1, MaybeSid());
			Slot1Leaves.Add(Outpost);
			SeedShortfall(Outpost);
			SeedDemand(Outpost);
		}
		const int32 NumSquads = Rand.RandRange(0, 3);
		for (int32 s = 0; s < NumSquads; ++s)
		{
			const entt::entity SquadE = Reg.create();
			Reg.emplace<CSquad>(SquadE, /*SquadId*/ static_cast<int64>(100 + s), /*CoreId*/ 0u, /*CardId*/ NAME_None);
			const int32 Members = Rand.RandRange(1, 3);
			for (int32 m = 0; m < Members; ++m)
			{
				const entt::entity Member = MakeHardeningUnit(Reg, 1, MaybeSid());
				Reg.emplace<CSquadMembership>(Member, SquadE, static_cast<int64>(100 + s), m);
				Slot1Leaves.Add(Member);
				SeedShortfall(Member);
				SeedDemand(Member);
			}
		}
		const int32 NumLoose = Rand.RandRange(0, 4);
		for (int32 i = 0; i < NumLoose; ++i)
		{
			const entt::entity Loose = MakeHardeningUnit(Reg, 1, MaybeSid());
			Slot1Leaves.Add(Loose);
			SeedShortfall(Loose);
			SeedDemand(Loose);
		}
		// Foreign-empire noise (never part of slot 1's tree).
		for (int32 i = 0; i < Rand.RandRange(0, 3); ++i)
		{
			MakeHardeningUnit(Reg, 2, MaybeSid());
		}

		// Structural invariants + build-twice determinism.
		const Plexus::FPlexusSnapshot Snapshot = Plexus::FPlexusSnapshot::Build(Reg, Plexus::FNucleus{ Plexus::ENucleusKind::Empire, 1 });
		TArray<FString> Errors;
		if (!Snapshot.Validate(&Errors))
		{
			for (const FString& Error : Errors)
			{
				AddError(FString::Printf(TEXT("iter %d: %s"), Iter, *Error));
			}
			return false;
		}
		const Plexus::FPlexusSnapshot Rebuilt = Plexus::FPlexusSnapshot::Build(Reg, Plexus::FNucleus{ Plexus::ENucleusKind::Empire, 1 });
		TestEqual(FString::Printf(TEXT("iter %d: build-twice identical layout"), Iter),
			LayoutSignature(Reg, Snapshot), LayoutSignature(Reg, Rebuilt));

		// Up-pass conservation: root rollup == sum of seeded leaf shortfalls.
		const CEconomicShortfallRollup Rollup = Plexus::AggregateUp(Reg, Snapshot, kTick);
		TestEqual(FString::Printf(TEXT("iter %d: rollup equals sum of leaf shortfalls"), Iter),
			Rollup.UnfundedMaterial, ExpectedUnfunded, 0.01f);

		// Down-pass conservation under the capped-at-need law: with demand present the
		// distributed total is min(pool, total demand x headroom) -- ample pool funds
		// exactly the need, the surplus stays retained; with NO demand info the uniform
		// fallback distributes the whole pool to absorbing leaves.
		Plexus::ApportionDown(Reg, Snapshot, Plexus::FApportionment{ kPool, /*BasePriority*/ 0 });
		const float ExpectedDistributed = ExpectedDemandTotal > KINDA_SMALL_NUMBER
			? FMath::Min(kPool, ExpectedDemandTotal * Plexus::kNeedCapHeadroom)
			: (Snapshot.Root().UnitLeafCount > 0 ? kPool : 0.0f);
		TestEqual(FString::Printf(TEXT("iter %d: distributed total matches the capped law (%d leaves)"), Iter, Snapshot.Root().UnitLeafCount),
			SumLeafBudgets(Reg, Snapshot), ExpectedDistributed, 0.05f);

		// Scarce leg: pool below total demand -> pure proportional rationing distributes it all.
		if (ExpectedDemandTotal > 1.0f)
		{
			const float ScarcePool = ExpectedDemandTotal * 0.5f;
			Plexus::ApportionDown(Reg, Snapshot, Plexus::FApportionment{ ScarcePool, /*BasePriority*/ 0 });
			TestEqual(FString::Printf(TEXT("iter %d: scarce pool fully distributed"), Iter),
				SumLeafBudgets(Reg, Snapshot), ScarcePool, 0.05f);
		}
	}
	return true;
}

// ----------------------------------------------------------------------------
// 3. A City Governor with no outpost holdings must not evaporate budget: the
// uniform fallback splits only among children that can absorb (pinned leak).
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusChildlessCityConservationTest,
	"GALAXY_PROJECT.Plexus.DownPass.Unit.ChildlessCityConservesPool",
	GALAXY_TEST_FLAGS)

bool FPlexusChildlessCityConservationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Case 1: one childless city + two loose units, no demand -> each unit gets HALF the
	// pool (pre-fix: a third each, with the city's third silently lost).
	{
		entt::registry Reg;
		MakeHardeningCity(Reg, 1);
		const entt::entity U1 = MakeHardeningUnit(Reg, 1);
		const entt::entity U2 = MakeHardeningUnit(Reg, 1);

		Plexus::RunDownPass(Reg, /*EmpireSlotId*/ 1, /*pool*/ 1000.0f);

		const CEconomicBudget* B1 = Reg.try_get<CEconomicBudget>(U1);
		const CEconomicBudget* B2 = Reg.try_get<CEconomicBudget>(U2);
		TestNotNull(TEXT("U1 budgeted"), B1);
		TestNotNull(TEXT("U2 budgeted"), B2);
		if (B1) { TestEqual(TEXT("U1 gets half the pool (childless city absorbs nothing)"), B1->SupplyBudget, 500.0f, KINDA_SMALL_NUMBER); }
		if (B2) { TestEqual(TEXT("U2 gets half the pool"), B2->SupplyBudget, 500.0f, KINDA_SMALL_NUMBER); }
	}

	// Case 2: capital (with one outpost) + a childless second city + one loose unit ->
	// pool splits between the capital subtree and the loose unit; the childless
	// city gets nothing and the total is conserved.
	{
		entt::registry Reg;
		MakeHardeningCity(Reg, 1, /*sid*/ 100); // capital (lowest stable id)
		MakeHardeningCity(Reg, 1, /*sid*/ 200); // childless second city
		const entt::entity Outpost = MakeHardeningOutpost(Reg, 1, /*sid*/ 300);
		const entt::entity Loose = MakeHardeningUnit(Reg, 1, /*sid*/ 400);

		Plexus::RunDownPass(Reg, /*EmpireSlotId*/ 1, /*pool*/ 1000.0f);

		const CEconomicBudget* OutpostBudget = Reg.try_get<CEconomicBudget>(Outpost);
		const CEconomicBudget* LooseBudget = Reg.try_get<CEconomicBudget>(Loose);
		TestNotNull(TEXT("Outpost budgeted"), OutpostBudget);
		TestNotNull(TEXT("Loose unit budgeted"), LooseBudget);
		const float Total = (OutpostBudget ? OutpostBudget->SupplyBudget : 0.0f) + (LooseBudget ? LooseBudget->SupplyBudget : 0.0f);
		TestEqual(TEXT("Full pool reaches the two absorbing leaves"), Total, 1000.0f, 0.01f);
		if (OutpostBudget) { TestEqual(TEXT("Capital subtree share"), OutpostBudget->SupplyBudget, 500.0f, KINDA_SMALL_NUMBER); }
	}
	return true;
}

// ----------------------------------------------------------------------------
// 4. Locality edge hygiene: ResolvePatronOutpost applies the validity rules and
// the economy pass sweeps ties that no longer resolve (via the single writer).
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusLocalityResolveSweepTest,
	"GALAXY_PROJECT.Plexus.Locality.Unit.ResolveAndSweep",
	GALAXY_TEST_FLAGS)

bool FPlexusLocalityResolveSweepTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	entt::registry Reg;
	const entt::entity OwnOutpost = MakeHardeningOutpost(Reg, /*slot*/ 1, /*sid*/ 500);
	const entt::entity ForeignOutpost = MakeHardeningOutpost(Reg, /*slot*/ 2, /*sid*/ 600);
	const entt::entity ValidSquad = MakeHardeningSquadWithMembers(Reg, /*slot*/ 1, /*SquadId*/ 10, /*members*/ 1, /*firstSid*/ 700);
	const entt::entity DanglingSquad = MakeHardeningSquadWithMembers(Reg, /*slot*/ 1, /*SquadId*/ 11, /*members*/ 1, /*firstSid*/ 710);
	const entt::entity ForeignTieSquad = MakeHardeningSquadWithMembers(Reg, /*slot*/ 1, /*SquadId*/ 12, /*members*/ 1, /*firstSid*/ 720);

	SquadOutpostAssignment::Assign(Reg, ValidSquad, 500);       // own-empire outpost: valid
	SquadOutpostAssignment::Assign(Reg, DanglingSquad, 999);    // no such outpost: dangling
	SquadOutpostAssignment::Assign(Reg, ForeignTieSquad, 600);  // other empire's outpost: foreign

	// Resolution applies the rules.
	TestTrue(TEXT("Valid tie resolves to the outpost entity"),
		Plexus::Locality::ResolvePatronOutpost(Reg, ValidSquad) == OwnOutpost);
	TestTrue(TEXT("Dangling tie resolves to null"),
		Plexus::Locality::ResolvePatronOutpost(Reg, DanglingSquad) == entt::null);
	TestTrue(TEXT("Foreign (cross-empire) tie resolves to null"),
		Plexus::Locality::ResolvePatronOutpost(Reg, ForeignTieSquad) == entt::null);

	// A squad with UNKNOWABLE empire (no CEmpireSlot / avatar) skips the empire rule.
	const entt::entity BareSquad = Reg.create();
	Reg.emplace<CSquad>(BareSquad, /*SquadId*/ static_cast<int64>(13), /*CoreId*/ 0u, /*CardId*/ NAME_None);
	SquadOutpostAssignment::Assign(Reg, BareSquad, 600);
	TestTrue(TEXT("Unknowable-empire squad may tie to any live outpost"),
		Plexus::Locality::ResolvePatronOutpost(Reg, BareSquad) == ForeignOutpost);

	// The sweep clears exactly the unresolvable ties, through the single writer.
	const int32 Cleared = Plexus::Locality::SweepInvalidPatronAssignments(Reg);
	TestEqual(TEXT("Sweep clears the dangling + foreign ties"), Cleared, 2);
	TestEqual(TEXT("Valid tie survives the sweep"), SquadOutpostAssignment::GetPatronOutpost(Reg, ValidSquad), static_cast<int64>(500));
	TestEqual(TEXT("Bare-squad tie survives (empire unknowable)"), SquadOutpostAssignment::GetPatronOutpost(Reg, BareSquad), static_cast<int64>(600));
	TestEqual(TEXT("Dangling tie cleared"), SquadOutpostAssignment::GetPatronOutpost(Reg, DanglingSquad), static_cast<int64>(0));
	TestEqual(TEXT("Foreign tie cleared"), SquadOutpostAssignment::GetPatronOutpost(Reg, ForeignTieSquad), static_cast<int64>(0));

	// Integration: the economy pass runs the sweep (exists-implies-resolves after the pass).
	SquadOutpostAssignment::Assign(Reg, DanglingSquad, 999);
	Plexus::RunEconomyPass(Reg, /*Tick*/ 1, /*EmpireSupplyPool*/ 100.0f);
	TestEqual(TEXT("RunEconomyPass sweeps unresolvable ties"), SquadOutpostAssignment::GetPatronOutpost(Reg, DanglingSquad), static_cast<int64>(0));

	return true;
}

// ---------------------------------------------------------------------------
// 5. READ-ONLY HELPERS -- what the g.plexus.* dumps call takes a const registry
// ---------------------------------------------------------------------------

namespace PlexusHardeningTestHelpers
{
	int32 CountRegistryPools(const entt::registry& Reg)
	{
		int32 Count = 0;
		for (auto&& Entry : Reg.storage())
		{
			(void)Entry;
			++Count;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusReadOnlyHelpersCreateNoPoolsTest,
	"GALAXY_PROJECT.Plexus.Diagnostics.Unit.ReadOnlyHelpersCreateNoPools",
	GALAXY_TEST_FLAGS)

bool FPlexusReadOnlyHelpersCreateNoPoolsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace PlexusHardeningTestHelpers;

	// A squad of two units and a tie to an outpost this registry never held. No outpost, city,
	// stable-id or demand-profile pool exists, and each helper below but AreCachesConsistent views
	// at least one of them. That one views only the membership pool, so it is checked further down
	// on a registry without one.
	entt::registry Reg;
	const entt::entity Squad = Reg.create();
	Reg.emplace<CSquad>(Squad, /*SquadId*/ static_cast<int64>(10), /*CoreId*/ 0u, /*CardId*/ NAME_None);
	Reg.emplace<CEmpireSlot>(Squad).EmpireSlotId = 1;
	for (int32 m = 0; m < 2; ++m)
	{
		Plexus::Membership::JoinNoRebuild(Reg, MakeHardeningUnit(Reg, /*slot*/ 1), Squad, /*SquadId*/ 10, m);
	}
	SquadOutpostAssignment::Assign(Reg, Squad, 500);
	const int32 PoolsBefore = CountRegistryPools(Reg);

	// Called with the MUTABLE registry, as the strategic passes call them: their const signature is
	// what keeps them from creating a pool, whatever the caller holds.
	const Plexus::FPlexusSnapshot EmpireSnap = Plexus::FPlexusSnapshot::Build(Reg, Plexus::FNucleus{ Plexus::ENucleusKind::Empire, 1 });
	const Plexus::FPlexusSnapshot SquadSnap = Plexus::FPlexusSnapshot::Build(Reg, Plexus::FNucleus{ Plexus::ENucleusKind::Squad, 1, Squad });
	TestEqual(TEXT("Empire snapshot holds the root, the squad and its two members"), EmpireSnap.Nodes.Num(), 4);
	TestEqual(TEXT("Squad snapshot holds the root and its two members"), SquadSnap.Nodes.Num(), 3);
	TestTrue(TEXT("Membership caches read as consistent"), Plexus::Membership::AreCachesConsistent(Reg));
	TestTrue(TEXT("A tie to an absent outpost resolves to null"), Plexus::Locality::ResolvePatronOutpost(Reg, Squad) == entt::null);
	TestEqual(TEXT("An empire holding no stored supply has an empty pool"), Plexus::ComputeEmpireSupplyStock(Reg, 1), 0.0f);

	// ForEachSupply keeps its mutable overload for the economy systems, so a reader binds const.
	int32 Visited = 0;
	EconomySubstrate::ForEachSupply(AsConst(Reg), [&Visited](const entt::entity, const FUnitSupplyState&) { ++Visited; });
	TestEqual(TEXT("The const roster visits both units"), Visited, 2);

	TestEqual(TEXT("No read-only helper created a component pool"), CountRegistryPools(Reg), PoolsBefore);

	entt::registry NoEdges;
	MakeHardeningUnit(NoEdges, /*slot*/ 1);
	const int32 NoEdgesPoolsBefore = CountRegistryPools(NoEdges);
	TestTrue(TEXT("A registry with no membership edges reads as consistent"), Plexus::Membership::AreCachesConsistent(NoEdges));
	TestEqual(TEXT("AreCachesConsistent created no membership pool"), CountRegistryPools(NoEdges), NoEdgesPoolsBefore);

	// Positive control: the mutable overload creates the outpost and city pools it views, so the
	// count above can see a created pool.
	EconomySubstrate::ForEachSupply(Reg, [](const entt::entity, FUnitSupplyState&) {});
	TestTrue(TEXT("Control: the mutable roster creates the pools it views"), CountRegistryPools(Reg) > PoolsBefore);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
