// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// PlexusDownPass -- the budget apportionment down-pass (intent flows DOWN).
//
// Walks the materialized
// Plexus snapshot (FPlexusSnapshot) top-down and writes CEconomicBudget onto each LEAF
// unit so the gated leaf arbiter (UnitEconomicDecisionSystem) runs against a BOUNDED
// Orange budget instead of the unbounded "spend what you have" stub.
//
// SNAPSHOT WALK (replaces the prior blind recursion): the snapshot's layout invariant
// (children contiguous at higher indices) makes the down-pass ONE ascending array walk
// -- each internal node splits its pool across its child range; a childless leaf Unit
// receives the budget. Subtree demand is read off the snapshot (rolled up once at
// build), not recomputed per child per tier. Tier deepening (Army/City) happens in
// FPlexusSnapshot::Build; this walk is unchanged by it.
//
// Determinism: pure function of the (deterministic) snapshot + the weights; shares
// derive from a shared pool by a deterministic ratio. No wall-clock / RNG.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "ThirdParty/GalaxyEnTT.h"
#include "ECS/Systems/Plexus/PlexusSnapshot.h"
#include "ECS/Components/Logistics/ECSSupplyDemandComponents.h"    // CSupplyDemandProfile / CSpendingWeights (demand-weighted flow)

namespace Plexus
{
	/**
	 * RETIRED from production (kept for explicit-pool tests/tools): the Phase-1 stub
	 * per-empire Orange pool. The production driver now derives each empire's pool from
	 * its REAL aggregate leaf Supply stock (PlexusIncome::ComputeEmpireSupplyStock via
	 * the pool-deriving RunEconomyPass overload); the apportionment math is unchanged.
	 */
	inline constexpr float kPhase1StubEmpireOrangePool = 1000.0f;

	/** The downward intent handed to a Nucleus: the Orange pool it may split + priority + objective weights. */
	struct FApportionment
	{
		float OrangePool = 0.0f;
		int32 BasePriority = 0;
		CSpendingWeights Weights; // per-objective emphasis biasing the demand-weighted split (default uniform)
	};

	/**
	 * Multiplicative headroom on the capped-at-need draw cap (1 + 2^-10, exactly representable).
	 * The cap sits a hair ABOVE the advertised demand. It was load-bearing while the leaf arbiter
	 * converted budget to whole units with a bare FloorToInt(Budget / PerUnitCost), where a budget
	 * capped EXACTLY at demand lost the final unit to float rounding (5 rounds x 0.1 -> 0.5f / 0.1f
	 * floored to 4). The leaf now counts through WholeUnits::Affordable, which forgives that noise,
	 * so this is the second guard, not the only one. Over-permission is bounded and harmless -- the
	 * leaf still clamps at its real deficit and its feedstock.
	 */
	inline constexpr float kNeedCapHeadroom = 1.0009765625f;

	/**
	 * Apportion a pool down a PRE-BUILT snapshot (one ascending walk). A childless leaf Unit
	 * receives the budget (CEconomicBudget); an internal node splits its pool across children.
	 * Split is DEMAND-WEIGHTED with a CAPPED-AT-NEED REFLOW: shares flow
	 * proportional to each child subtree's objective-weighted demand (snapshot demand dotted
	 * with Incoming.Weights), but a child's draw is capped at its subtree's RAW demand (x a
	 * small float headroom) and overflow reflows to unsaturated siblings -- so under scarcity
	 * the weights ration, with ample pool everyone gets exactly their need, and the surplus is
	 * RETAINED (the empire reserve) instead of sprayed as excess permission. FALLBACK: when no
	 * demand information exists under a node the split is UNIFORM among absorbing children
	 * (the prior behavior, so demand-agnostic callers / tests are unchanged).
	 */
	GALAXY_PROJECT_API void ApportionDown(entt::registry& Registry, const FPlexusSnapshot& Snapshot, const FApportionment& Incoming);

	/**
	 * Convenience overload: build the snapshot for Node, then apportion down it. Callers running
	 * BOTH passes should build one FPlexusSnapshot and pass it to each (see PlexusEconomyPass).
	 */
	inline void ApportionDown(entt::registry& Registry, const FNucleus& Node, const FApportionment& Incoming)
	{
		ApportionDown(Registry, FPlexusSnapshot::Build(Registry, Node), Incoming);
	}

	/**
	 * Run the down-pass for ONE empire: apportion EmpireOrangePool across every leaf under the
	 * empire root, writing CEconomicBudget. Call once per live EmpireSlot per strategic pass.
	 * Pool is a parameter so callers (driver = real aggregate stock; tests = explicit) control
	 * the source.
	 */
	GALAXY_PROJECT_API void RunDownPass(entt::registry& Registry, int32 EmpireSlotId, float EmpireOrangePool);
}
