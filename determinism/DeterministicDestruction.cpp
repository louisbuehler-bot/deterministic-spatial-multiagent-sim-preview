// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Definitions for DeterministicDestruction.h.

#include "DeterministicDestruction.h"

// ============================================================================
// GalaxyDeterminism::Detail
// ============================================================================

int64 GalaxyDeterminism::Detail::DestructionSortKey(const entt::registry& Registry, const entt::entity Entity)
{
	const CGalaxyStableId* Stable = Registry.try_get<CGalaxyStableId>(Entity);
	return Stable != nullptr ? Stable->Id : 0;
}
