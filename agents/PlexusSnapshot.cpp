// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Definitions for PlexusSnapshot.h.

#include "PlexusSnapshot.h"

// ============================================================================
// Plexus::FPlexusSnapshot::FNode
// ============================================================================

float Plexus::FPlexusSnapshot::FNode::WeightedDemand(const CSpendingWeights& Weights) const
{
	float Sum = 0.0f;
	for (int32 i = 0; i < NUM_SPENDING_OBJECTIVES; ++i)
	{
		Sum += DemandByObjective[i] * Weights.ByObjective[i];
	}
	return Sum;
}

float Plexus::FPlexusSnapshot::FNode::TotalRawDemand() const
{
	float Sum = 0.0f;
	for (int32 i = 0; i < NUM_SPENDING_OBJECTIVES; ++i)
	{
		Sum += DemandByObjective[i];
	}
	return Sum;
}


// ============================================================================
// Plexus::FPlexusSnapshot
// ============================================================================

Plexus::FPlexusSnapshot Plexus::FPlexusSnapshot::Build(const entt::registry& Registry, const FNucleus& Root)
{
	FPlexusSnapshot Snapshot;
	TArray<FNode>& Nodes = Snapshot.Nodes;

	auto AddNode = [&Nodes](const FNucleus& Nucleus, const int32 Parent) -> int32
	{
		FNode Node;
		Node.Nucleus = Nucleus;
		Node.Parent = Parent;
		return Nodes.Add(Node);
	};
	// Sibling ordering must survive save/load and entity churn: entt handles are
	// session-local (relinked on load, index-recycled on churn), so ordering by them
	// could silently reshape the tree -- including flipping WHICH city is the capital.
	// Order by CGalaxyStableId (the durable identity); entt integral is only the
	// fallback for unstamped entities (test fixtures), which sort after stamped ones.
	const auto BySpineOrder = [&Registry](const entt::entity A, const entt::entity B)
	{
		const CGalaxyStableId* StableA = Registry.try_get<CGalaxyStableId>(A);
		const CGalaxyStableId* StableB = Registry.try_get<CGalaxyStableId>(B);
		const int64 IdA = (StableA != nullptr) ? StableA->Id : 0;
		const int64 IdB = (StableB != nullptr) ? StableB->Id : 0;
		if ((IdA > 0) != (IdB > 0))
		{
			return IdA > 0; // stamped entities order before unstamped ones
		}
		if (IdA > 0 && IdA != IdB)
		{
			return IdA < IdB;
		}
		return entt::to_integral(A) < entt::to_integral(B); // ptr-order-safe: entt integral fallback, unstamped fixtures only
	};

	if (Root.Kind == ENucleusKind::Unit)
	{
		AddNode(Root, INDEX_NONE);
	}
	else if (Root.Kind == ENucleusKind::Squad)
	{
		AddNode(Root, INDEX_NONE);
		TArray<entt::entity> Members;
		for (const entt::entity Entity : Registry.view<CSquadMembership>()) // entt-order-safe: collected, sorted below
		{
			if (Registry.get<CSquadMembership>(Entity).SquadEntity == Root.Entity)
			{
				Members.Add(Entity);
			}
		}
		Members.Sort(BySpineOrder);
		Nodes[0].FirstChild = 1;
		Nodes[0].ChildCount = Members.Num();
		for (const entt::entity Entity : Members)
		{
			AddNode(FNucleus{ ENucleusKind::Unit, Root.EmpireSlotId, Entity }, 0);
		}
	}
	else if (Root.Kind == ENucleusKind::Empire)
	{
		// ONE sweep over the empire's COMMAND physical nodes -- units + outposts ("immobile
		// units") by IDENTITY (CUnitAvatar / COutpostAvatar), each read once for its squad
		// membership edge. A squad's empire is DERIVED from its members (squad entities do not
		// reliably carry CEmpireSlot), so squads are discovered via the edge; a squad appears
		// only if it has >=1 member -> empty squads strand nothing.
		TSet<entt::entity> SeenSquads;
		TArray<entt::entity> Squads;                          // first-seen order, sorted below
		TMap<entt::entity, TArray<entt::entity>> SquadMembers; // tmap-order-safe: keyed lookup only; output order comes from sorted Squads
		TArray<entt::entity> LooseUnits;                      // squad-less command nodes of this empire
		TArray<entt::entity> Outposts; // immobile-unit territorial holdings (routed under the capital city below)
		auto ConsiderNode = [&](const entt::entity Entity) // units only (outposts collected separately below)
		{
			const CEmpireSlot* Slot = Registry.try_get<CEmpireSlot>(Entity);
			if (Slot == nullptr || Slot->EmpireSlotId != Root.EmpireSlotId)
			{
				return;
			}
			const CSquadMembership* Membership = Registry.try_get<CSquadMembership>(Entity);
			if (Membership != nullptr && Registry.valid(Membership->SquadEntity))
			{
				bool bAlreadySeen = false;
				SeenSquads.Add(Membership->SquadEntity, &bAlreadySeen);
				if (!bAlreadySeen)
				{
					Squads.Add(Membership->SquadEntity); // entt-order-safe: collected, sorted below
				}
				SquadMembers.FindOrAdd(Membership->SquadEntity).Add(Entity);
			}
			else
			{
				LooseUnits.Add(Entity); // entt-order-safe: collected, sorted below
			}
		};
		for (const entt::entity Entity : Registry.view<CUnitAvatar>()) // entt-order-safe: collected, sorted below
		{
			ConsiderNode(Entity);
		}
		// exclude<CCityAvatar>: a settled city KEEPS its
		// COutpostAvatar, so it matches this view too -- and it is enumerated as a CITY
		// below. Without the exclude the same entity would enter the command tree TWICE:
		// once as a City node and again as a territorial outpost hanging off the capital,
		// which for a single-city empire means the city is listed as its own child. It
		// takes the CITY role, the more specific of the two.
		for (const entt::entity Entity : Registry.view<COutpostAvatar>(entt::exclude<CCityAvatar>)) // immobile units -> territorial holdings
		{
			const CEmpireSlot* Slot = Registry.try_get<CEmpireSlot>(Entity);
			if (Slot != nullptr && Slot->EmpireSlotId == Root.EmpireSlotId)
			{
				Outposts.Add(Entity); // entt-order-safe: collected, sorted below
			}
		}

		// City Governors of this empire (territorial branch). An outpost is a territorial
		// holding of its empire's CAPITAL city (lowest-STABLE-ID city on this shard -- one
		// city per empire per planet is the common case, so all outposts hang off it; an
		// explicit outpost->city edge can disambiguate multi-city empires later).
		// With NO city, outposts stay LOOSE children of the empire (expeditionary fallback).
		TArray<entt::entity> Cities;
		for (const entt::entity Entity : Registry.view<CCityAvatar>()) // entt-order-safe: collected, sorted below
		{
			if (Registry.get<CCityAvatar>(Entity).OwnerEmpireSlot == Root.EmpireSlotId)
			{
				Cities.Add(Entity);
			}
		}

		Cities.Sort(BySpineOrder);
		Squads.Sort(BySpineOrder);
		LooseUnits.Sort(BySpineOrder);
		Outposts.Sort(BySpineOrder);
		const bool bHasCity = Cities.Num() > 0;

		AddNode(Root, INDEX_NONE);
		Nodes[0].FirstChild = 1;
		Nodes[0].ChildCount = Cities.Num() + Squads.Num() + LooseUnits.Num() + (bHasCity ? 0 : Outposts.Num());

		const int32 FirstCityNode = Nodes.Num();
		for (const entt::entity CityEntity : Cities)
		{
			AddNode(FNucleus{ ENucleusKind::City, Root.EmpireSlotId, CityEntity }, 0);
		}
		const int32 FirstSquadNode = Nodes.Num();
		for (const entt::entity SquadEntity : Squads)
		{
			AddNode(FNucleus{ ENucleusKind::Squad, Root.EmpireSlotId, SquadEntity }, 0);
		}
		for (const entt::entity Entity : LooseUnits)
		{
			AddNode(FNucleus{ ENucleusKind::Unit, Root.EmpireSlotId, Entity }, 0);
		}
		if (!bHasCity)
		{
			for (const entt::entity Entity : Outposts) // no city -> outposts are loose empire children
			{
				AddNode(FNucleus{ ENucleusKind::Unit, Root.EmpireSlotId, Entity }, 0);
			}
		}

		// Territorial branch: the capital city's outpost holdings (children always at HIGHER
		// indices than their parent). Non-capital cities carry no outposts in the MVP.
		if (bHasCity)
		{
			const int32 CapitalCityNode = FirstCityNode; // Cities sorted by stable id -> [0] is the capital
			Nodes[CapitalCityNode].FirstChild = Nodes.Num();
			Nodes[CapitalCityNode].ChildCount = Outposts.Num();
			for (const entt::entity Entity : Outposts)
			{
				AddNode(FNucleus{ ENucleusKind::Unit, Root.EmpireSlotId, Entity }, CapitalCityNode);
			}
		}

		// Expeditionary branch: append each squad's members AFTER all prior nodes, sorted.
		for (int32 SquadIndex = 0; SquadIndex < Squads.Num(); ++SquadIndex)
		{
			const int32 SquadNode = FirstSquadNode + SquadIndex;
			TArray<entt::entity>& Members = SquadMembers.FindChecked(Squads[SquadIndex]);
			Members.Sort(BySpineOrder);
			Nodes[SquadNode].FirstChild = Nodes.Num();
			Nodes[SquadNode].ChildCount = Members.Num();
			for (const entt::entity Entity : Members)
			{
				AddNode(FNucleus{ ENucleusKind::Unit, Root.EmpireSlotId, Entity }, SquadNode);
			}
		}
	}
	// Army/City-as-root: not wired as ROOTS yet (City appears only as an empire-child tier above).

	// Per-leaf demand + unit-leaf count, then one descending pass rolls both up
	// (children sit at higher indices, so each internal node's children are final
	// when it is visited; the per-objective sums accumulate in ascending child
	// order -- stable).
	for (FNode& Node : Nodes)
	{
		if (!Node.HasChildren() && Node.Nucleus.Kind == ENucleusKind::Unit)
		{
			Node.UnitLeafCount = 1;
			if (Registry.valid(Node.Nucleus.Entity))
			{
				if (const CSupplyDemandProfile* Profile = Registry.try_get<CSupplyDemandProfile>(Node.Nucleus.Entity))
				{
					for (int32 i = 0; i < NUM_SPENDING_OBJECTIVES; ++i)
					{
						Node.DemandByObjective[i] = Profile->ByObjective[i];
					}
				}
			}
		}
	}
	for (int32 NodeIndex = Nodes.Num() - 1; NodeIndex >= 0; --NodeIndex)
	{
		FNode& Node = Nodes[NodeIndex];
		if (!Node.HasChildren())
		{
			continue;
		}
		for (int32 Child = Node.FirstChild; Child < Node.FirstChild + Node.ChildCount; ++Child)
		{
			Node.UnitLeafCount += Nodes[Child].UnitLeafCount;
			for (int32 i = 0; i < NUM_SPENDING_OBJECTIVES; ++i)
			{
				Node.DemandByObjective[i] += Nodes[Child].DemandByObjective[i]; // float-safe: fixed ascending child order
			}
		}
	}
	return Snapshot;
}

bool Plexus::FPlexusSnapshot::Validate(TArray<FString>* OutErrors) const
{
	int32 ErrorCount = 0;
	const auto Fail = [&ErrorCount, OutErrors](const FString& Message)
	{
		++ErrorCount;
		if (OutErrors != nullptr)
		{
			OutErrors->Add(Message);
		}
	};

	if (Nodes.Num() == 0)
	{
		Fail(TEXT("empty snapshot (no root)"));
		return false;
	}
	if (Nodes[0].Parent != INDEX_NONE)
	{
		Fail(FString::Printf(TEXT("root parent must be INDEX_NONE, got %d"), Nodes[0].Parent));
	}

	TArray<int32> ClaimedBy; // which node's child range claims each index (INDEX_NONE = unclaimed)
	ClaimedBy.Init(INDEX_NONE, Nodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		const FNode& Node = Nodes[NodeIndex];
		if (NodeIndex > 0 && (Node.Parent < 0 || Node.Parent >= NodeIndex))
		{
			Fail(FString::Printf(TEXT("node %d: parent %d is not at a lower index"), NodeIndex, Node.Parent));
		}
		if (Node.ChildCount < 0)
		{
			Fail(FString::Printf(TEXT("node %d: negative ChildCount %d"), NodeIndex, Node.ChildCount));
			continue;
		}
		if (Node.Nucleus.Kind == ENucleusKind::Unit && Node.ChildCount > 0)
		{
			Fail(FString::Printf(TEXT("node %d: Unit-kind node has %d children"), NodeIndex, Node.ChildCount));
		}
		if (!Node.HasChildren())
		{
			continue;
		}
		if (Node.FirstChild <= NodeIndex || Node.FirstChild + Node.ChildCount > Nodes.Num())
		{
			Fail(FString::Printf(TEXT("node %d: child range [%d, %d) out of bounds / not above parent"),
				NodeIndex, Node.FirstChild, Node.FirstChild + Node.ChildCount));
			continue;
		}
		float ChildDemand[NUM_SPENDING_OBJECTIVES] = { 0.0f };
		int32 ChildUnitLeaves = 0;
		for (int32 Child = Node.FirstChild; Child < Node.FirstChild + Node.ChildCount; ++Child)
		{
			if (ClaimedBy[Child] != INDEX_NONE)
			{
				Fail(FString::Printf(TEXT("node %d: child %d already claimed by node %d (overlapping ranges)"),
					NodeIndex, Child, ClaimedBy[Child]));
			}
			ClaimedBy[Child] = NodeIndex;
			if (Nodes[Child].Parent != NodeIndex)
			{
				Fail(FString::Printf(TEXT("node %d: child %d back-links to parent %d"),
					NodeIndex, Child, Nodes[Child].Parent));
			}
			ChildUnitLeaves += Nodes[Child].UnitLeafCount;
			for (int32 i = 0; i < NUM_SPENDING_OBJECTIVES; ++i)
			{
				ChildDemand[i] += Nodes[Child].DemandByObjective[i]; // float-safe: same ascending order as Build's rollup
			}
		}
		if (Node.UnitLeafCount != ChildUnitLeaves)
		{
			Fail(FString::Printf(TEXT("node %d: UnitLeafCount %d != sum of children %d"),
				NodeIndex, Node.UnitLeafCount, ChildUnitLeaves));
		}
		for (int32 i = 0; i < NUM_SPENDING_OBJECTIVES; ++i)
		{
			if (!FMath::IsNearlyEqual(Node.DemandByObjective[i], ChildDemand[i], 0.01f))
			{
				Fail(FString::Printf(TEXT("node %d: demand[%d] %.4f != children sum %.4f"),
					NodeIndex, i, Node.DemandByObjective[i], ChildDemand[i]));
			}
		}
	}
	for (int32 NodeIndex = 1; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (ClaimedBy[NodeIndex] == INDEX_NONE)
		{
			Fail(FString::Printf(TEXT("node %d: not claimed by any parent's child range (orphan)"), NodeIndex));
		}
	}
	return ErrorCount == 0;
}
