// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// PlexusSnapshot -- the per-pass MATERIALIZED Plexus tree (the single enumeration seam).
//
// Nuclei are transient managers rebuilt deterministically
// each strategic pass; this is that rebuild made EXPLICIT: ONE O(N log N) sweep over
// the authoritative ownership edges produces a flat, pass-local tree (node array,
// contiguous child ranges, per-leaf demand rolled up post-order), and the up-pass /
// down-pass become array walks over it. This replaces the prior per-node registry
// re-scans (ForEachChild scanned every unit per call; subtree demand was recomputed
// per child per tier), which were O(N^2) per empire per pass.
//
// THE SINGLE TIER SEAM: all tier-discovery knowledge lives in Build() -- the
// traversal adapters (PlexusTree.h ForEachChild/ForEachLeafUnder) and both passes
// ride this one builder. Wiring the Army/City tiers = deepening Build() (read the
// new tier's membership edge in the same sweep); no consumer changes. A new tier's
// edge must mirror the CSquadMembership pattern -- one
// edge of record + a Plexus::Membership-style single writer -- never a bespoke
// parallel store.
//
// Demand is carried PER OBJECTIVE (not pre-weighted), so one snapshot serves any
// CSpendingWeights -- the down-pass dots demand with its weights at walk time
// (e.g. authored weights biased by the up-pass shortfall, computed between the
// two walks of the same snapshot).
//
// Layout invariant: Nodes[0] = root; a node's children are CONTIGUOUS and always
// at HIGHER indices than the node. Ascending iteration = parent-before-child
// (down-pass); descending iteration = children-before-parent (up-pass/rollup).
//
// Determinism: every enumeration collects then stable-sorts before layout (slot id
// for empires; CGalaxyStableId for entities, entt integral only as the fallback for
// unstamped test fixtures) -> EnTT view order never affects the node array. Stable-id
// ordering also makes the LAYOUT durable: entt handles are session-local (relinked on
// load, recycled on churn), so an entt-keyed sort could silently reshape the tree --
// including WHICH city is the capital -- across save/load while the authoritative
// state is unchanged. Pure function of the registry's ownership edges; no wall-clock
// / RNG.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "ThirdParty/GalaxyEnTT.h"
#include "ECS/Systems/Plexus/PlexusNode.h"
#include "ECS/Components/Core/ECSEmpireSlotComponents.h"   // CEmpireSlot
#include "ECS/Components/Core/ECSGalaxyTopologyComponents.h" // CGalaxyStableId (durable sibling ordering)
#include "ECS/Components/Squad/ECSSquadComponents.h"        // CSquadMembership (the squad-tier edge of record)
#include "ECS/Components/Unit/UnitAvatar.h"                 // CUnitAvatar (command physical node identity)
#include "ECS/Components/Outpost/OutpostAvatar.h"           // COutpostAvatar ("immobile unit" command node)
#include "ECS/Components/City/CityAvatar.h"                 // CCityAvatar (City Governor -- territorial tier)
#include "ECS/Components/Logistics/ECSSupplyDemandComponents.h" // CSupplyDemandProfile / CSpendingWeights

namespace Plexus
{
	/**
	 * The pass-local materialized Plexus tree for one root. Build once per strategic pass,
	 * then run the up-pass and down-pass as array walks over the SAME snapshot.
	 */
	struct FPlexusSnapshot
	{
		struct FNode
		{
			FNucleus Nucleus;
			int32 Parent = INDEX_NONE;     // index into Nodes (INDEX_NONE for the root)
			int32 FirstChild = INDEX_NONE; // children are contiguous: [FirstChild, FirstChild + ChildCount)
			int32 ChildCount = 0;

			/**
			 * Subtree Orange demand by ESpendingObjective ordinal. Leaves carry their own
			 * CSupplyDemandProfile values; internal nodes carry the post-order sum of their
			 * children. Stored UN-weighted so one snapshot serves any CSpendingWeights.
			 */
			float DemandByObjective[NUM_SPENDING_OBJECTIVES] = { 0.0f };

			/**
			 * Unit leaves in this subtree (a childless Unit counts itself; internal nodes sum
			 * their children). Lets the down-pass route budget only toward subtrees that can
			 * ABSORB it -- a childless City Governor must never evaporate a pool share.
			 */
			int32 UnitLeafCount = 0;

			[[nodiscard]] bool HasChildren() const { return ChildCount > 0; }

			/** Objective-weighted subtree demand under the given weights (the apportionment signal). */
			[[nodiscard]] GALAXY_PROJECT_API float WeightedDemand(const CSpendingWeights& Weights) const;

			/**
			 * Total UN-weighted subtree demand -- the physical Orange this subtree can actually
			 * absorb. The down-pass's capped-at-need reflow uses this as each child's draw CAP
			 * (weights bias WHO gets contested supply first; raw demand bounds HOW MUCH anyone
			 * can take).
			 */
			[[nodiscard]] GALAXY_PROJECT_API float TotalRawDemand() const;
		};

		TArray<FNode> Nodes;

		[[nodiscard]] bool IsValid() const { return Nodes.Num() > 0; }
		[[nodiscard]] const FNode& Root() const { return Nodes[0]; }

		/**
		 * Materialize the tree under Root in one deterministic sweep. Supported roots: Empire
		 * (the normal per-empire pass root), Squad, and Unit (single-leaf base case). Army/City
		 * deepen HERE when their membership edges land (the single tier seam). Reads only: the
		 * registry is const, so a dump that calls it creates no component pools.
		 */
		[[nodiscard]] GALAXY_PROJECT_API static FPlexusSnapshot Build(const entt::registry& Registry, const FNucleus& Root);

		/**
		 * Structural invariant checker -- the layout contract every consumer (both passes, the
		 * traversal adapters, the strategic-graph projection) rides. Pure over the snapshot (no
		 * registry). Checks: root at 0 with no parent; every parent at a LOWER index than its
		 * children; child ranges in-bounds, disjoint, and back-linked (each child's Parent is
		 * the range owner); every non-root node claimed by exactly one range; Unit-kind nodes
		 * childless; demand + unit-leaf rollups equal to the sum of children. Returns true when
		 * clean; appends one line per violation to OutErrors when provided. Intended for tests
		 * (fuzz/property), g.plexus.Validate, and future Build() deepenings (Army tier).
		 */
		[[nodiscard]] GALAXY_PROJECT_API bool Validate(TArray<FString>* OutErrors = nullptr) const;
	};
}
