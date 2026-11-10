// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// PlexusNode -- the Plexus node value types (ENucleusKind / FNucleus).
//
// Split out of PlexusTree.h so the snapshot builder
// (PlexusSnapshot.h) and the traversal adapters (PlexusTree.h) can share the
// node types without a circular include. A Nucleus is a TRANSIENT value passed
// through traversals -- never stored, never persistent state.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "ThirdParty/GalaxyEnTT.h"
#include "ECS/Components/Core/ECSEmpireSlotComponents.h" // EMPIRE_SLOT_ID_NONE

namespace Plexus
{
	/** The kind of command node. Wired: Empire (root, the empire Nucleus), Squad, Unit (leaf). Army/City reserved. */
	enum class ENucleusKind : uint8
	{
		Empire,   // the empire Nucleus, the root; keyed by EmpireSlotId, backed by a persistent CEmpireNucleus entity
		Squad,    // an entity-backed squad Nucleus (the "General") -- WIRED
		Army,     // RESERVED (later): an entity-backed army Nucleus
		City,     // RESERVED (later): an entity-backed city/starbase Nucleus (the "Governor")
		Unit      // a leaf Nucleus -- a single unit/outpost that executes directly
	};

	/**
	 * A node in the Plexus. Every node carries its EmpireSlotId; entity-backed nuclei (the empire
	 * EmpireNucleus, Squad, Unit -- and later Army/City) also carry their entt::entity. This is a transient
	 * value passed through the traversal, never stored.
	 */
	struct FNucleus
	{
		ENucleusKind Kind = ENucleusKind::Unit;
		int32 EmpireSlotId = EMPIRE_SLOT_ID_NONE;
		entt::entity Entity = entt::null;

		[[nodiscard]] bool IsLeaf() const { return Kind == ENucleusKind::Unit; }
	};
}
