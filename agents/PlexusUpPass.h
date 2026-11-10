// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// PlexusUpPass -- the shortfall aggregation up-pass (feedback flows UP).
//
// The mirror of the
// down-pass: walks the materialized Plexus snapshot bottom-up and rolls each leaf
// unit's CEconomicShortfall (what the leaf arbiter could NOT fund) up into a
// CEconomicShortfallRollup on each entity-backed internal node (squad now; army/city
// later) and the empire Nucleus. The rollup is PER-OBJECTIVE (ESpendingObjective), so
// it is consumable in the same currency the down-pass spends: the economy pass biases
// the spending weights toward objectives with persistent shortfall (the v1 reactive
// down-pass contract -- see PlexusEconomyPass::ComputeShortfallBiasedWeights).
//
// SNAPSHOT WALK (replaces the prior blind recursion): the layout invariant (children
// contiguous at higher indices) makes the up-pass ONE descending array walk -- when a
// node is visited, its children's rollups are final; an internal node folds its child
// range in ascending order and persists the result on its host entity. Tier deepening
// (Army/City) happens in FPlexusSnapshot::Build; this walk is unchanged by it.
//
// Determinism: pure function of the (deterministic) snapshot + the leaf
// CEconomicShortfall values; float sums accumulate in a fixed child order. No
// wall-clock / RNG.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "ThirdParty/GalaxyEnTT.h"
#include "ECS/Systems/Plexus/PlexusTree.h"
#include "ECS/Systems/Plexus/PlexusSnapshot.h"
#include "ECS/Components/Fabrication/ECSEconomyDecisionComponents.h" // CEconomicShortfall, CEconomicShortfallRollup

namespace Plexus
{
	/**
	 * Aggregate shortfall up a PRE-BUILT snapshot (one descending walk). Returns the root's
	 * summed rollup; every entity-backed internal node (Squad, the empire Nucleus; later
	 * Army/City) also PERSISTS its rollup so readers (StrategicGoalEmission, the next
	 * economy pass's weight bias) can consume it. A leaf Unit contributes its own
	 * CEconomicShortfall and is never written (it already carries the source component).
	 */
	GALAXY_PROJECT_API CEconomicShortfallRollup AggregateUp(entt::registry& Registry, const FPlexusSnapshot& Snapshot, int32 Tick);

	/**
	 * Convenience overload: build the snapshot for Node, then aggregate up it. Callers running
	 * BOTH passes should build one FPlexusSnapshot and pass it to each (see PlexusEconomyPass).
	 */
	inline CEconomicShortfallRollup AggregateUp(entt::registry& Registry, const FNucleus& Node, int32 Tick)
	{
		return AggregateUp(Registry, FPlexusSnapshot::Build(Registry, Node), Tick);
	}

	/**
	 * Run the up-pass for ONE empire: aggregate every leaf's shortfall up through the squad (and
	 * later army/city) tiers, writing per-node rollups. Returns the empire's TOTAL rollup (also
	 * persisted on the empire Nucleus entity when one exists). Call once per live EmpireSlot.
	 */
	inline CEconomicShortfallRollup RunUpPass(entt::registry& Registry, int32 EmpireSlotId, int32 Tick)
	{
		// Root on the empire's Nucleus entity if one exists (so the total lands there); null-safe otherwise.
		return AggregateUp(Registry, FNucleus{ ENucleusKind::Empire, EmpireSlotId, FindEmpireNucleus(Registry, EmpireSlotId) }, Tick);
	}
}
