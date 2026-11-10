// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Definitions for PlexusDownPass.h.

#include "PlexusDownPass.h"
#include "ECS/Systems/Plexus/PlexusTree.h" // Plexus tree walks (FindEmpireNucleus, ForEachLeafUnder)
#include "ECS/Components/Fabrication/ECSEconomyDecisionComponents.h" // CEconomicBudget

// ============================================================================
// Plexus
// ============================================================================

void Plexus::ApportionDown(entt::registry& Registry, const FPlexusSnapshot& Snapshot, const FApportionment& Incoming)
{
	if (!Snapshot.IsValid())
	{
		return;
	}
	TArray<float> Pools;
	Pools.Init(0.0f, Snapshot.Nodes.Num());
	Pools[0] = Incoming.OrangePool;

	for (int32 NodeIndex = 0; NodeIndex < Snapshot.Nodes.Num(); ++NodeIndex)
	{
		const FPlexusSnapshot::FNode& Node = Snapshot.Nodes[NodeIndex];
		if (!Node.HasChildren())
		{
			// Leaf: write the budget the leaf arbiter consumes. Only unit leaves carry supply/economy.
			if (Node.Nucleus.Kind == ENucleusKind::Unit && Registry.valid(Node.Nucleus.Entity))
			{
				CEconomicBudget& Budget = Registry.get_or_emplace<CEconomicBudget>(Node.Nucleus.Entity);
				Budget.SupplyBudget = Pools[NodeIndex];
				Budget.BasePriority = Incoming.BasePriority;
			}
			continue;
		}

		// The internal node's rolled-up demand IS the sum of its children's (snapshot invariant),
		// so the contested total is one dot product; per-child shares are one more each.
		const float TotalDemand = Node.WeightedDemand(Incoming.Weights);
		const bool bDemandWeighted = TotalDemand > KINDA_SMALL_NUMBER;

		if (bDemandWeighted)
		{
			// CAPPED-AT-NEED REFLOW (the apportionment rule's second half): each
			// round distributes the remaining pool proportionally to the unsaturated
			// children's WEIGHTED demand (doctrine decides who is served first), but a
			// child's total draw is CAPPED at its subtree's RAW demand x headroom (physical
			// need bounds how much anyone can take); overflow past a cap REFLOWS to the
			// still-unsaturated siblings next round. Each overflowing round saturates >=1
			// child, so the loop runs at most ChildCount rounds. Whatever no child can
			// absorb stays UNDISTRIBUTED at this node -- at the empire root that is the
			// empire's retained reserve (permission never handed out), replacing the old
			// behavior of spraying the entire pool as meaningless excess permission.
			// Under scarcity with weights proportional to caps no cap is ever hit and this
			// reduces to the prior single-round proportional split. Feedstock-aware caps
			// (local stock + inbound logistics) remain the deferred refinement (plexus).
			// Zero-weighted demand (a doctrine-zeroed objective) is deliberately never
			// funded -- its pool share stays in the reserve.
			TArray<float> Allotted;
			Allotted.Init(0.0f, Node.ChildCount);
			float Remaining = Pools[NodeIndex];
			for (int32 Round = 0; Round < Node.ChildCount && Remaining > KINDA_SMALL_NUMBER; ++Round)
			{
				float UnsaturatedWeight = 0.0f;
				for (int32 c = 0; c < Node.ChildCount; ++c)
				{
					const FPlexusSnapshot::FNode& ChildNode = Snapshot.Nodes[Node.FirstChild + c];
					const float Weight = ChildNode.WeightedDemand(Incoming.Weights);
					if (Weight > KINDA_SMALL_NUMBER
						&& Allotted[c] < ChildNode.TotalRawDemand() * kNeedCapHeadroom - KINDA_SMALL_NUMBER)
					{
						UnsaturatedWeight += Weight; // float-safe: fixed ascending child order
					}
				}
				if (UnsaturatedWeight <= KINDA_SMALL_NUMBER)
				{
					break; // everyone fundable is saturated -- the remainder is the reserve
				}
				float Overflow = 0.0f;
				for (int32 c = 0; c < Node.ChildCount; ++c)
				{
					const FPlexusSnapshot::FNode& ChildNode = Snapshot.Nodes[Node.FirstChild + c];
					const float Weight = ChildNode.WeightedDemand(Incoming.Weights);
					const float Cap = ChildNode.TotalRawDemand() * kNeedCapHeadroom;
					if (Weight <= KINDA_SMALL_NUMBER || Allotted[c] >= Cap - KINDA_SMALL_NUMBER)
					{
						continue;
					}
					// float-safe: shares derive from shared sums by deterministic ratios in a
					// fixed order; identical inputs -> identical allotments.
					const float NewAllotted = Allotted[c] + Remaining * (Weight / UnsaturatedWeight);
					if (NewAllotted >= Cap)
					{
						Overflow += NewAllotted - Cap;
						Allotted[c] = Cap;
					}
					else
					{
						Allotted[c] = NewAllotted;
					}
				}
				Remaining = Overflow;
			}
			for (int32 c = 0; c < Node.ChildCount; ++c)
			{
				Pools[Node.FirstChild + c] = Allotted[c];
			}
			continue;
		}

		// CONSERVATION: the uniform fallback (no demand INFORMATION anywhere under this
		// node -- distinct from "zero demand advertised") splits only among children that
		// can ABSORB budget (subtrees containing >=1 unit leaf, per the snapshot's
		// UnitLeafCount rollup). A childless internal node -- a City Governor with no
		// outpost holdings -- must not receive a share it can never write to a leaf.
		// Deliberately UNCAPPED: with no demand data there is no need signal to cap on,
		// and the pre-demand tests/callers rely on the even split.
		int32 AbsorbingChildren = 0;
		for (int32 Child = Node.FirstChild; Child < Node.FirstChild + Node.ChildCount; ++Child)
		{
			AbsorbingChildren += (Snapshot.Nodes[Child].UnitLeafCount > 0) ? 1 : 0;
		}
		const float UniformShare = AbsorbingChildren > 0
			? Pools[NodeIndex] / static_cast<float>(AbsorbingChildren)
			: 0.0f;
		for (int32 Child = Node.FirstChild; Child < Node.FirstChild + Node.ChildCount; ++Child)
		{
			// float-safe: even split of a shared pool among a deterministic absorber count.
			Pools[Child] = Snapshot.Nodes[Child].UnitLeafCount > 0 ? UniformShare : 0.0f;
		}
	}
}

void Plexus::RunDownPass(entt::registry& Registry, int32 EmpireSlotId, float EmpireOrangePool)
{
	ApportionDown(
		Registry,
		FNucleus{ ENucleusKind::Empire, EmpireSlotId, FindEmpireNucleus(Registry, EmpireSlotId) },
		FApportionment{ EmpireOrangePool, /*BasePriority*/ 0 });
}
