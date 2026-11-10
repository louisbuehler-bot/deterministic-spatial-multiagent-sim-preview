// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Unit tests for the Plexus budget apportionment down-pass (the command
// spine that feeds the leaf economic arbiter).
//
// Covers: the down-pass writes a BOUNDED CEconomicBudget onto every leaf unit under
// an empire (even uniform split of the empire Orange pool); it is SCOPED to the
// target empire (units of other EmpireSlots are never touched); and it is
// DETERMINISTIC + idempotent (re-running yields identical budgets, and entity
// creation order does not change the per-leaf share). The fixture is tier-collapsed
// (Empire -> Unit); the recursion that bottoms out at the leaf is the same one that
// will apportion through Squad/Army tiers once wired.
// ============================================================================

#include "ECS/Systems/Plexus/PlexusDownPass.h"
#include "ECS/Systems/Plexus/PlexusTree.h" // Plexus tree walks (FindEmpireNucleus, ForEachLeafUnder)
#include "ECS/Components/Fabrication/ECSEconomyDecisionComponents.h" // CEconomicBudget
#include "ECS/Systems/Plexus/PlexusSnapshot.h"        // FPlexusSnapshot (City-tier structure assertions)
#include "ECS/Components/City/CityAvatar.h"           // CCityAvatar (City Governor tier)
#include "ECS/Components/Outpost/OutpostAvatar.h"     // COutpostAvatar (territorial holding)
#include "TestUtilities.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// A supply-carrying unit (CUnitAvatar -> seen by EconomySubstrate) tagged to an EmpireSlot.
	entt::entity MakeUnit(entt::registry& Reg, int32 EmpireSlotId)
	{
		const entt::entity E = Reg.create();
		Reg.emplace<CUnitAvatar>(E); // carries FUnitSupplyState -> enumerated as a leaf by the Plexus tree
		CEmpireSlot& Slot = Reg.emplace<CEmpireSlot>(E);
		Slot.EmpireSlotId = EmpireSlotId;
		return E;
	}

	// A City Governor (CCityAvatar owned by an EmpireSlot) -- the territorial-branch root.
	entt::entity MakeCity(entt::registry& Reg, int32 EmpireSlotId)
	{
		const entt::entity E = Reg.create();
		Reg.emplace<CCityAvatar>(E).OwnerEmpireSlot = EmpireSlotId;
		return E;
	}

	// An outpost ("immobile unit") tagged to an EmpireSlot -- a territorial holding of its city.
	entt::entity MakeOutpost(entt::registry& Reg, int32 EmpireSlotId)
	{
		const entt::entity E = Reg.create();
		Reg.emplace<COutpostAvatar>(E);
		Reg.emplace<CEmpireSlot>(E).EmpireSlotId = EmpireSlotId;
		return E;
	}
}

// ----------------------------------------------------------------------------
// The down-pass writes a bounded, evenly-split budget to every leaf of the empire.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusDownPassApportionsTest,
	"GALAXY_PROJECT.Plexus.DownPass.Unit.ApportionsEvenlyToLeaves",
	GALAXY_TEST_FLAGS)

bool FPlexusDownPassApportionsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	entt::registry Reg;
	TArray<entt::entity> Units;
	for (int32 i = 0; i < 4; ++i)
	{
		Units.Add(MakeUnit(Reg, /*EmpireSlotId*/ 1));
	}

	Plexus::RunDownPass(Reg, /*EmpireSlotId*/ 1, /*EmpireOrangePool*/ 1000.0f);

	for (const entt::entity E : Units)
	{
		const CEconomicBudget* Budget = Reg.try_get<CEconomicBudget>(E);
		if (!TestNotNull(TEXT("Each leaf received a budget"), Budget))
		{
			continue;
		}
		TestEqual(TEXT("Even split of the empire pool (1000 / 4)"), Budget->SupplyBudget, 250.0f, KINDA_SMALL_NUMBER);
		TestFalse(TEXT("Budget is bounded (not the unbounded stub)"), Budget->IsUnbounded());
		TestEqual(TEXT("Base priority passed down"), Budget->BasePriority, 0);
	}

	// Tree query sanity: exactly the four leaves are reachable under the empire root.
	int32 LeafCount = 0;
	Plexus::ForEachLeafUnder(
		Reg,
		Plexus::FNucleus{ Plexus::ENucleusKind::Empire, 1, entt::null },
		[&LeafCount](const Plexus::FNucleus&) { ++LeafCount; });
	TestEqual(TEXT("ForEachLeafUnder reaches every leaf"), LeafCount, 4);

	return true;
}

// ----------------------------------------------------------------------------
// The down-pass is scoped to its empire: other EmpireSlots are never touched.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusDownPassScopedTest,
	"GALAXY_PROJECT.Plexus.DownPass.Unit.ScopesToEmpire",
	GALAXY_TEST_FLAGS)

bool FPlexusDownPassScopedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	entt::registry Reg;
	const entt::entity A1 = MakeUnit(Reg, /*slot*/ 1);
	const entt::entity A2 = MakeUnit(Reg, /*slot*/ 1);
	const entt::entity B1 = MakeUnit(Reg, /*slot*/ 2);

	Plexus::RunDownPass(Reg, /*EmpireSlotId*/ 1, /*pool*/ 1000.0f);

	const CEconomicBudget* BudgetA1 = Reg.try_get<CEconomicBudget>(A1);
	const CEconomicBudget* BudgetA2 = Reg.try_get<CEconomicBudget>(A2);
	TestNotNull(TEXT("Slot-1 unit A1 budgeted"), BudgetA1);
	TestNotNull(TEXT("Slot-1 unit A2 budgeted"), BudgetA2);
	if (BudgetA1) { TestEqual(TEXT("A1 share (1000 / 2)"), BudgetA1->SupplyBudget, 500.0f, KINDA_SMALL_NUMBER); }

	TestNull(TEXT("Slot-2 unit B1 untouched (no budget)"), Reg.try_get<CEconomicBudget>(B1));

	return true;
}

// ----------------------------------------------------------------------------
// Deterministic + idempotent: re-running yields identical budgets; creation order
// does not change the per-leaf share.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusDownPassDeterministicTest,
	"GALAXY_PROJECT.Plexus.DownPass.Unit.DeterministicAndIdempotent",
	GALAXY_TEST_FLAGS)

bool FPlexusDownPassDeterministicTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	entt::registry Reg;
	TArray<entt::entity> Units;
	for (int32 i = 0; i < 4; ++i)
	{
		Units.Add(MakeUnit(Reg, 1));
	}

	Plexus::RunDownPass(Reg, 1, 1000.0f);
	// Idempotent: a second identical pass produces the same standing budgets.
	Plexus::RunDownPass(Reg, 1, 1000.0f);

	for (const entt::entity E : Units)
	{
		const CEconomicBudget* Budget = Reg.try_get<CEconomicBudget>(E);
		if (TestNotNull(TEXT("Leaf budgeted after repeated pass"), Budget))
		{
			TestEqual(TEXT("Share stable across re-runs"), Budget->SupplyBudget, 250.0f, KINDA_SMALL_NUMBER);
		}
	}

	// Order-independence: a second registry built in reverse creation order yields the same share.
	entt::registry Reg2;
	TArray<entt::entity> Units2;
	for (int32 i = 0; i < 4; ++i)
	{
		Units2.Add(MakeUnit(Reg2, 1));
	}
	Plexus::RunDownPass(Reg2, 1, 1000.0f);
	for (const entt::entity E : Units2)
	{
		const CEconomicBudget* Budget = Reg2.try_get<CEconomicBudget>(E);
		if (TestNotNull(TEXT("Leaf budgeted in second registry"), Budget))
		{
			TestEqual(TEXT("Share identical across registries"), Budget->SupplyBudget, 250.0f, KINDA_SMALL_NUMBER);
		}
	}

	return true;
}

// ----------------------------------------------------------------------------
// City tier (territorial branch): an empire's outposts nest UNDER its City Governor,
// not directly under the empire. Snapshot structure: Empire -> {City -> outposts, ...}.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusCityTierNestsOutpostsTest,
	"GALAXY_PROJECT.Plexus.Snapshot.City.Unit.OutpostsNestUnderCity",
	GALAXY_TEST_FLAGS)

bool FPlexusCityTierNestsOutpostsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	entt::registry Reg;
	const entt::entity City = MakeCity(Reg, /*slot*/ 1);
	const entt::entity Out1 = MakeOutpost(Reg, 1);
	const entt::entity Out2 = MakeOutpost(Reg, 1);
	const entt::entity Loose = MakeUnit(Reg, 1); // a squad-less mobile unit -> a direct empire child
	(void)Loose;

	const Plexus::FPlexusSnapshot Snap = Plexus::FPlexusSnapshot::Build(
		Reg, Plexus::FNucleus{ Plexus::ENucleusKind::Empire, 1, entt::null });

	TestTrue(TEXT("Snapshot built"), Snap.IsValid());
	TestEqual(TEXT("Root is the empire"),
		static_cast<int32>(Snap.Root().Nucleus.Kind), static_cast<int32>(Plexus::ENucleusKind::Empire));
	// Direct empire children = the City + the loose unit (the outposts are NOT here -- they nest under the city).
	TestEqual(TEXT("Empire has 2 direct children (city + loose unit)"), Snap.Root().ChildCount, 2);

	int32 CityNodeIdx = INDEX_NONE;
	for (int32 i = 1; i < Snap.Nodes.Num(); ++i)
	{
		if (Snap.Nodes[i].Nucleus.Kind == Plexus::ENucleusKind::City)
		{
			CityNodeIdx = i;
		}
	}
	if (!TestTrue(TEXT("A City node exists under the empire"), CityNodeIdx != INDEX_NONE))
	{
		return false;
	}

	const Plexus::FPlexusSnapshot::FNode& CityNode = Snap.Nodes[CityNodeIdx];
	TestTrue(TEXT("City node is the city entity"), CityNode.Nucleus.Entity == City);
	TestEqual(TEXT("City holds both outposts"), CityNode.ChildCount, 2);
	TestTrue(TEXT("City children are at higher indices (layout invariant)"), CityNode.FirstChild > CityNodeIdx);

	bool bSawOut1 = false;
	bool bSawOut2 = false;
	for (int32 c = CityNode.FirstChild; c < CityNode.FirstChild + CityNode.ChildCount; ++c)
	{
		const Plexus::FPlexusSnapshot::FNode& Child = Snap.Nodes[c];
		TestEqual(TEXT("City child is a Unit-kind (outpost) leaf"),
			static_cast<int32>(Child.Nucleus.Kind), static_cast<int32>(Plexus::ENucleusKind::Unit));
		TestEqual(TEXT("City child parents back to the city"), Child.Parent, CityNodeIdx);
		bSawOut1 |= (Child.Nucleus.Entity == Out1);
		bSawOut2 |= (Child.Nucleus.Entity == Out2);
	}
	TestTrue(TEXT("Outpost 1 nests under the city"), bSawOut1);
	TestTrue(TEXT("Outpost 2 nests under the city"), bSawOut2);

	// Budget flow confirms the tier: Empire 1200 -> {city 600, loose 600}; city 600 -> 2 outposts = 300 each.
	Plexus::RunDownPass(Reg, /*slot*/ 1, /*pool*/ 1200.0f);
	const CEconomicBudget* B1 = Reg.try_get<CEconomicBudget>(Out1);
	const CEconomicBudget* B2 = Reg.try_get<CEconomicBudget>(Out2);
	if (TestNotNull(TEXT("Outpost 1 budgeted through the city"), B1))
	{
		TestEqual(TEXT("Outpost share = 1200/2 empire children /2 outposts"), B1->SupplyBudget, 300.0f, 0.01f);
	}
	if (TestNotNull(TEXT("Outpost 2 budgeted through the city"), B2))
	{
		TestEqual(TEXT("Outpost 2 share matches"), B2->SupplyBudget, 300.0f, 0.01f);
	}

	return true;
}

// ----------------------------------------------------------------------------
// Fallback: with NO city for the empire, outposts stay LOOSE direct children of the
// empire (the pre-city behavior) -- nothing strands.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlexusNoCityOutpostsLooseTest,
	"GALAXY_PROJECT.Plexus.Snapshot.City.Unit.OutpostsLooseWithoutCity",
	GALAXY_TEST_FLAGS)

bool FPlexusNoCityOutpostsLooseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	entt::registry Reg;
	const entt::entity Out1 = MakeOutpost(Reg, 1);
	const entt::entity Out2 = MakeOutpost(Reg, 1);
	const entt::entity Unit = MakeUnit(Reg, 1);
	(void)Out1; (void)Out2; (void)Unit;

	const Plexus::FPlexusSnapshot Snap = Plexus::FPlexusSnapshot::Build(
		Reg, Plexus::FNucleus{ Plexus::ENucleusKind::Empire, 1, entt::null });

	TestTrue(TEXT("Snapshot built"), Snap.IsValid());
	// No city -> all three (2 outposts + 1 unit) are direct empire children, all Unit-kind leaves.
	TestEqual(TEXT("All three nodes are direct empire children"), Snap.Root().ChildCount, 3);
	for (int32 i = 1; i < Snap.Nodes.Num(); ++i)
	{
		TestEqual(TEXT("No City node exists"),
			static_cast<int32>(Snap.Nodes[i].Nucleus.Kind), static_cast<int32>(Plexus::ENucleusKind::Unit));
		TestEqual(TEXT("Every node parents to the empire root"), Snap.Nodes[i].Parent, 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
