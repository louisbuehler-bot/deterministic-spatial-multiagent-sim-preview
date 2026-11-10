// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// DETERMINISTIC DESTRUCTION of a collected batch of entities.
//
// THE RULE: the ORDER a batch is destroyed in is an authoritative decision, not
// bookkeeping. EnTT pushes destroyed entities onto a free list and reissues their
// ids in that order, so the destroy sequence decides which handles the NEXT
// entities created will receive. Anything that later tie-breaks on
// entt::to_integral is therefore reading a value the destroy order chose.
//
// WHY A HELPER RATHER THAN A CONVENTION: this shape looks finished and is not.
//
//     TArray<entt::entity> ToDestroy;
//     for (auto E : View) { ... ToDestroy.Add(E); }   // "collected, then destroyed"
//     for (auto E : ToDestroy) { Registry.destroy(E); }
//
// Buffering answers why the code does not destroy during view iteration. It says
// nothing about whether the ORDER is safe. This header decorates once (O(n)
// lookups) and then sorts, so the safe form is also the cheap one.
//
// THE ORDER: CGalaxyStableId ascending, then entt::to_integral ascending.
//   - The stable id is the SAVE-persistent identity, so a destroy sequence
//     keyed on it survives a save/load cycle -- loading reassigns raw handles,
//     and an order keyed on those would not reproduce.
//   - Entities with no CGalaxyStableId (projectiles, city blocks, corpses) all
//     project key 0 and fall through to the entity tie-break, which keeps the
//     order TOTAL. A batch mixing stamped and unstamped entities is fine: the
//     unstamped ones sort first, among themselves by handle.
//
// WHAT THIS IS NOT FOR: a site whose destruction order is already a genuine DOMAIN
// order, such as pruning per-empire nodes by empire slot, keeps its own order.
//
// EXPORTS: GalaxyDeterminism::{DestructionOrder, DestroySorted}
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "ThirdParty/GalaxyEnTT.h"                            // entt::registry / entt::entity
#include "ECS/Components/Core/ECSGalaxyTopologyComponents.h"  // CGalaxyStableId (the primary sort key)

namespace GalaxyDeterminism
{
	namespace Detail
	{
		/**
		 * The save-persistent sort key for one entity: its CGalaxyStableId, or 0
		 * when it carries none. Callers must have validated the entity -- try_get
		 * on a stale handle is exactly the EnTT misuse GalaxyEnTT.h routes to an
		 * ensure.
		 */
		[[nodiscard]] GALAXY_PROJECT_API int64 DestructionSortKey(const entt::registry& Registry, const entt::entity Entity);

		/** Shared body. Decorate once, sort the decorated pairs, hand back the entities. */
		template <typename ContainerType, typename ProjectionType> // registry-member-ok: a TU includes this header TO destroy entities, so the registry is complete there already (the one direct non-caller, OutpostSaveSystem.h, carries it for its own .cpp; its includers are save tests)
		[[nodiscard]] TArray<entt::entity> BuildDestructionOrder(
			const entt::registry& Registry,
			const ContainerType& Items,
			ProjectionType ToEntity)
		{
			TArray<TPair<int64, entt::entity>> Decorated;
			Decorated.Reserve(Items.Num());
			for (const auto& Item : Items) // tmap-order-safe -- collected then sorted below; collection order is irrelevant
			{
				const entt::entity Entity = ToEntity(Item);
				if (!Registry.valid(Entity))
				{
					continue;
				}
				Decorated.Emplace(DestructionSortKey(Registry, Entity), Entity);
			}

			Decorated.Sort([](const TPair<int64, entt::entity>& A, const TPair<int64, entt::entity>& B)
			{
				// TOTAL by construction: stable ids are unique when present, and the
				// handle tie-break resolves the whole unstamped (key 0) run. Without
				// the tie-break the unstamped entities would keep collection order,
				// which is the hash/view order this helper exists to escape.
				return A.Key != B.Key ? A.Key < B.Key
				                      : entt::to_integral(A.Value) < entt::to_integral(B.Value);
			});

			TArray<entt::entity> Ordered;
			Ordered.Reserve(Decorated.Num());
			for (const TPair<int64, entt::entity>& Pair : Decorated)
			{
				Ordered.Add(Pair.Value);
			}
			return Ordered;
		}
	}

	/**
	 * The destruction ORDER only -- for sites that must do work between ordering
	 * and destroying (clear a back-reference, emit a yield, log).
	 *
	 * Invalid entities are dropped here, so the returned array is live AT THE
	 * MOMENT OF THE CALL. A body that destroys OTHER entities (a death yield that
	 * reaps a carcass, a teardown that cascades) can still invalidate a later
	 * element, so keep the `Registry.valid()` guard inside such a loop.
	 *
	 * Accepts TArray and TSet alike -- a TSet's hash order is precisely the input
	 * this is meant to neutralize, so it must not force the caller to convert.
	 */
	template <typename ContainerType>
	[[nodiscard]] TArray<entt::entity> DestructionOrder(
		const entt::registry& Registry,
		const ContainerType& Entities)
	{
		return Detail::BuildDestructionOrder(Registry, Entities,
			[](const entt::entity Entity) { return Entity; });
	}

	/**
	 * Projection overload, for a container of structs that carry an entity
	 * (FWildlifeDeath::Creature, FDecomposed::Entity, ...). The projection must
	 * return the entt::entity to destroy.
	 */
	template <typename ContainerType, typename ProjectionType>
	[[nodiscard]] TArray<entt::entity> DestructionOrder(
		const entt::registry& Registry,
		const ContainerType& Items,
		ProjectionType ToEntity)
	{
		return Detail::BuildDestructionOrder(Registry, Items, MoveTemp(ToEntity));
	}

	/**
	 * Order and destroy in one step -- the whole operation for a pure destroy
	 * loop. Returns the number of entities actually destroyed.
	 *
	 * The per-element revalidation is not redundant with the one in
	 * DestructionOrder: destroying entity N can cascade (an on_destroy observer,
	 * a linked child) and invalidate entity N+1 of this same batch.
	 */
	template <typename ContainerType> // registry-member-ok: see BuildDestructionOrder
	int32 DestroySorted(entt::registry& Registry, const ContainerType& Entities)
	{
		const TArray<entt::entity> Ordered = DestructionOrder(Registry, Entities);

		int32 Destroyed = 0;
		for (const entt::entity Entity : Ordered)
		{
			if (Registry.valid(Entity))
			{
				Registry.destroy(Entity);
				++Destroyed;
			}
		}
		return Destroyed;
	}

	/** Projection counterpart of DestroySorted; same contract. */
	template <typename ContainerType, typename ProjectionType> // registry-member-ok: see BuildDestructionOrder
	int32 DestroySorted(entt::registry& Registry, const ContainerType& Items, ProjectionType ToEntity)
	{
		const TArray<entt::entity> Ordered = DestructionOrder(Registry, Items, MoveTemp(ToEntity));

		int32 Destroyed = 0;
		for (const entt::entity Entity : Ordered)
		{
			if (Registry.valid(Entity))
			{
				Registry.destroy(Entity);
				++Destroyed;
			}
		}
		return Destroyed;
	}
}
