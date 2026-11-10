// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// PlexusTree -- the command-node traversal surface (roots + ad-hoc child/leaf queries).
//
// The Plexus is a recursive tree of TRANSIENT Nucleus
// managers over authoritative ECS entities. Since the snapshot refactor, ALL
// enumeration logic lives in ONE place -- FPlexusSnapshot::Build (PlexusSnapshot.h)
// -- and this header provides:
//   - ForEachEmpireRoot: the empire Nucleus-keyed empire-set SSOT enumeration (a plain
//     function over TFunctionRef, body in PlexusTree.cpp -- as a template with a
//     concretely-typed registry parameter its `Registry.view<CEmpireNucleus>()` was
//     instantiated in every includer at PARSE time, called or not), and
//   - ForEachLeafUnder: an AD-HOC query adapter over a per-call snapshot build
//     (O(registry sweep) per call -- fine for one-off queries and tests; the
//     strategic passes build ONE snapshot and walk it instead).
//
// WIRED TIERS: Empire -> {Squad, loose Unit} -> Unit. Apportionment is
// per-COMMAND-STRUCTURE (a squad gets one share, then divides it among members),
// not flat per-capita. Army/City tiers deepen FPlexusSnapshot::Build (the single
// tier seam); these adapters and the recursive consumers pick the depth up free.
//
// Layering: ECS-only (never includes UIClasses/). The UI/fogged StrategicGraph
// projection (FStrategicGraphData / BuildEmpireGraph) is a SEPARATE surface.
//
// Determinism: inherited from FPlexusSnapshot::Build (collect + stable-sort) and
// the sorted-root enumeration below. No wall-clock / RNG.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "ThirdParty/GalaxyEnTT.h"
#include "ECS/Systems/Plexus/PlexusNode.h"                  // ENucleusKind / FNucleus
#include "ECS/Systems/Plexus/PlexusSnapshot.h"              // FPlexusSnapshot (the single enumeration seam)
#include "ECS/Systems/Plexus/PlexusEmpireNucleus.h"                // CEmpireNucleus (persistent empire-root node)

namespace Plexus
{
	/**
	 * Visit every empire-root Nucleus, deterministically (ascending EmpireSlotId). The root is the
	 * persistent empire Nucleus ENTITY (the SSOT empire set -- one CEmpireNucleus per live empire IN THIS REGISTRY,
	 * kept by ReconcileEmpireNuclei). Visit signature: void(const FNucleus&); the FNucleus carries the
	 * EmpireNucleus entity.
	 */
	GALAXY_PROJECT_API void ForEachEmpireRoot(entt::registry& Registry, TFunctionRef<void(const FNucleus&)> Visit);

	/**
	 * Visit every leaf Unit reachable under a Nucleus, deterministically (ad-hoc query
	 * adapter over a per-call snapshot; deepens for free when tiers are wired). Visit
	 * signature: void(const FNucleus&).
	 */
	template<typename FnVisit>
	inline void ForEachLeafUnder(entt::registry& Registry, const FNucleus& Node, FnVisit&& Visit)
	{
		const FPlexusSnapshot Snapshot = FPlexusSnapshot::Build(Registry, Node);
		for (const FPlexusSnapshot::FNode& SnapshotNode : Snapshot.Nodes)
		{
			if (!SnapshotNode.HasChildren() && SnapshotNode.Nucleus.Kind == ENucleusKind::Unit)
			{
				Visit(SnapshotNode.Nucleus);
			}
		}
	}
}
