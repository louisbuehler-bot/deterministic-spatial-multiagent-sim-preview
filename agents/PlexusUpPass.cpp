// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Definitions for PlexusUpPass.h.

#include "PlexusUpPass.h"

// ============================================================================
// Plexus
// ============================================================================

namespace Plexus
{
	CEconomicShortfallRollup AggregateUp(entt::registry& Registry, const FPlexusSnapshot& Snapshot, int32 Tick)
	{
		CEconomicShortfallRollup EmptyRollup;
		EmptyRollup.Reset(Tick);
		if (!Snapshot.IsValid())
		{
			return EmptyRollup;
		}

		TArray<CEconomicShortfallRollup> Rollups;
		Rollups.Init(EmptyRollup, Snapshot.Nodes.Num());

		for (int32 NodeIndex = Snapshot.Nodes.Num() - 1; NodeIndex >= 0; --NodeIndex)
		{
			const FPlexusSnapshot::FNode& Node = Snapshot.Nodes[NodeIndex];
			CEconomicShortfallRollup& Rollup = Rollups[NodeIndex];
			if (!Node.HasChildren())
			{
				// Leaf: fold this unit's own unfunded demands (if any) into the rollup.
				if (Node.Nucleus.Kind == ENucleusKind::Unit && Registry.valid(Node.Nucleus.Entity))
				{
					if (const CEconomicShortfall* Shortfall = Registry.try_get<CEconomicShortfall>(Node.Nucleus.Entity))
					{
						Rollup.AddLeaf(*Shortfall);
					}
				}
				continue;
			}

			for (int32 Child = Node.FirstChild; Child < Node.FirstChild + Node.ChildCount; ++Child)
			{
				Rollup.AddChild(Rollups[Child]); // float-safe: fixed ascending child order
			}

			// Persist on the node's HOST entity so the General / Governor / Emperor (and the next
			// economy pass) can read it -- squad/army/city entities AND the empire Nucleus entity.
			if (Registry.valid(Node.Nucleus.Entity))
			{
				Registry.emplace_or_replace<CEconomicShortfallRollup>(Node.Nucleus.Entity, Rollup);
			}
		}
		return Rollups[0];
	}
}
