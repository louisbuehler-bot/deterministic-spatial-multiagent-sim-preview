// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Out-of-line definitions for PlexusTree.h. ForEachEmpireRoot lived in the header as a
// template over the visitor; its registry parameter was spelled concretely, so the
// `Registry.view<CEmpireNucleus>()` in its body was instantiated by every includer at parse time
// (compile-time-standards, ForcedInstantiation). One body, here, compiled once.

#include "PlexusTree.h"

void Plexus::ForEachEmpireRoot(entt::registry& Registry, TFunctionRef<void(const FNucleus&)> Visit)
{
	TArray<TPair<int32, entt::entity>> Roots;
	for (const entt::entity Entity : Registry.view<CEmpireNucleus>()) // entt-order-safe: collected, sorted below
	{
		Roots.Emplace(Registry.get<CEmpireNucleus>(Entity).EmpireSlotId, Entity);
	}
	Roots.Sort([](const TPair<int32, entt::entity>& A, const TPair<int32, entt::entity>& B)
	{
		return A.Key < B.Key;
	});
	for (const TPair<int32, entt::entity>& Root : Roots)
	{
		Visit(FNucleus{ ENucleusKind::Empire, Root.Key, Root.Value });
	}
}
