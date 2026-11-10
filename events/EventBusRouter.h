// Copyright GALAXY_PROJECT. All Rights Reserved.
// =============================================================================
// EventBusRouter.h
//
// Game-thread-only cross-shard event fan-out.
// Receives exported events from per-shard FEventBusManagers and dispatches them
// to globally-registered handlers after all shards finish Phase_BuildSnapshot.

#pragma once

#include "CoreMinimal.h"
#include "Infrastructure/EventBus.h"

DECLARE_LOG_CATEGORY_EXTERN(LogEventBusRouter, Log, All);

// =============================================================================
// FEventBusRouter
// =============================================================================

/**
 * Routes events exported from per-shard event buses to globally-registered
 * handlers. Owned by UGlobalShardScheduler; all methods are game-thread-only.
 *
 * Dispatch order is deterministic:
 *   1. Events are sorted by (SourceShardId, EmitTick, TickSequence) before dispatch.
 *   2. Within that order, handlers are sorted by Priority then RegistrationOrder.
 *
 * Usage (inside UGlobalShardScheduler::Phase_BuildSnapshots_All after the loop):
 *   for each shard with a bus (Shard.GetShardEventBus() != nullptr):
 *       RouterBus.IngestFromShard(ShardId, Shard.GetShardEventBus()->StealExportedEvents());
 *   RouterBus.FlushRouted();
 *
 * Thread safety: call only from the game thread (UGlobalShardScheduler tick).
 */
class GALAXYCORE_API FEventBusRouter
{
public:
	FEventBusRouter() = default;
	~FEventBusRouter() = default;

	// Non-copyable, non-movable
	FEventBusRouter(const FEventBusRouter&) = delete;
	FEventBusRouter& operator=(const FEventBusRouter&) = delete;

	// =========================================================================
	// Lifecycle
	// =========================================================================

	/**
	 * Initialize (or re-initialize) the router.
	 * Clears all subscriptions and pending events.
	 */
	void Initialize();

	// =========================================================================
	// Subscription
	// =========================================================================

	/**
	 * Subscribe to an event type across all shards.
	 * Handler is called once per matching exported event during FlushRouted().
	 */
	template<typename EventType>
	FEventHandlerHandle Subscribe(
		TFunction<void(const EventType&)> Handler,
		EEventPriority Priority = EEventPriority::Normal)
	{
		static_assert(TIsDerivedFrom<EventType, IEvent>::Value,
			"EventType must derive from IEvent");
		return SubscribeInternal(
			EventType::GetStaticTypeId(),
			Priority,
			[UserHandler = MoveTemp(Handler)](const IEvent& Event)
			{
				UserHandler(static_cast<const EventType&>(Event));
			},
			TFunction<bool(const IEvent&)>{},
			TWeakObjectPtr<UObject>{});
	}

	/**
	 * Subscribe to an event type with an optional filter predicate.
	 * Handler is only invoked when Filter(event) returns true.
	 */
	template<typename EventType>
	FEventHandlerHandle Subscribe(
		TFunction<void(const EventType&)> Handler,
		EEventPriority Priority,
		TFunction<bool(const EventType&)> Filter)
	{
		static_assert(TIsDerivedFrom<EventType, IEvent>::Value,
			"EventType must derive from IEvent");

		TFunction<bool(const IEvent&)> ErasedFilter;
		if (Filter)
		{
			ErasedFilter = [TypedFilter = MoveTemp(Filter)](const IEvent& Event) -> bool
			{
				return TypedFilter(static_cast<const EventType&>(Event));
			};
		}

		return SubscribeInternal(
			EventType::GetStaticTypeId(),
			Priority,
			[UserHandler = MoveTemp(Handler)](const IEvent& Event)
			{
				UserHandler(static_cast<const EventType&>(Event));
			},
			MoveTemp(ErasedFilter),
			TWeakObjectPtr<UObject>{});
	}

	/**
	 * Subscribe to an event type with a UObject lifetime guard.
	 * If Owner is garbage-collected, the entry is tombstoned during next FlushRouted().
	 */
	template<typename EventType>
	FEventHandlerHandle SubscribeWeak(
		UObject* Owner,
		TFunction<void(const EventType&)> Handler,
		EEventPriority Priority = EEventPriority::Normal)
	{
		static_assert(TIsDerivedFrom<EventType, IEvent>::Value,
			"EventType must derive from IEvent");
		return SubscribeInternal(
			EventType::GetStaticTypeId(),
			Priority,
			[UserHandler = MoveTemp(Handler)](const IEvent& Event)
			{
				UserHandler(static_cast<const EventType&>(Event));
			},
			TFunction<bool(const IEvent&)>{},
			TWeakObjectPtr<UObject>(Owner));
	}

	/**
	 * Unsubscribe a previously registered handler.
	 * Safe to call at any time (including inside a handler -- entry is tombstoned).
	 * @return true if the handle was found and removed
	 */
	bool Unsubscribe(const FEventHandlerHandle& Handle);

	// =========================================================================
	// Ingestion and dispatch
	// =========================================================================

	/**
	 * Ingest exported events from one shard. Moves ownership into the router's
	 * pending buffer. Thread-safe IF called only from the game thread (per contract).
	 * @param SourceShard   The shard the events originated from
	 * @param ExportedEvents Events drained from FEventBusManager::StealExportedEvents()
	 */
	void IngestFromShard(uint16 SourceShard, TArray<TUniquePtr<IEvent>>&& ExportedEvents);

	/**
	 * Dispatch all pending ingested events to registered handlers.
	 * Call once per tick on the game thread after all shards have been snapshotted.
	 * Events are sorted by (SourceShardId, EmitTick, TickSequence) for determinism.
	 * @return Number of events dispatched
	 */
	int32 FlushRouted();

	// =========================================================================
	// Helpers
	// =========================================================================

	/**
	 * Returns an event filter predicate that passes only events from SourceShard.
	 * Convenience helper for Subscribe(..., Priority, MakeShardFilter(ShardId)).
	 */
	template<typename EventType>
	static TFunction<bool(const EventType&)> MakeShardFilter(uint16 ShardId)
	{
		return [ShardId](const EventType& Event) -> bool
		{
			return Event.GetMetadata().SourceShardId == ShardId;
		};
	}

	// =========================================================================
	// Accessors
	// =========================================================================

	/** Number of active subscriptions. */
	[[nodiscard]] int32 GetSubscriptionCount() const;

	/** Number of events waiting in the pending buffer (ingested, not yet flushed). */
	[[nodiscard]] int32 GetPendingEventCount() const;

private:
	// Internal handler entry (mirrors EventBusInternal::FHandlerEntry)
	struct FRouterHandlerEntry
	{
		uint64 HandlerId          = 0;
		EEventPriority Priority   = EEventPriority::Normal;
		uint64 RegistrationOrder  = 0;
		TFunction<void(const IEvent&)> Handler;
		TFunction<bool(const IEvent&)> Filter;   // null = always invoke
		TWeakObjectPtr<UObject> WeakOwner;        // null = no lifetime guard
		bool bHasWeakOwner  = false;
		bool bAlive               = true;

		[[nodiscard]] GALAXYCORE_API bool operator<(const FRouterHandlerEntry& Other) const;
	};

	struct FRouterHandlerList
	{
		TArray<FRouterHandlerEntry> Handlers;
		bool bNeedsSorting  = false;
		int32 DispatchDepth = 0;
		int32 DeadCount     = 0;

		/** Entries subscribed while a dispatch was active on this list. Appending to
		 *  Handlers mid-dispatch could reallocate the array while one of its TFunctions
		 *  is still executing; deferred entries join Handlers when depth returns to 0. */
		TArray<FRouterHandlerEntry> PendingAdds;

		GALAXYCORE_API void EnsureSorted();

		GALAXYCORE_API void CompactDeadIfSafe();

		GALAXYCORE_API void DrainPendingIfSafe();
	};

	// Pending ingested event (pre-sort key cached alongside the event pointer)
	struct FPendingEvent
	{
		TUniquePtr<IEvent> Event;
		uint16 SourceShardId = 0;
		uint64 EmitTick      = 0;
		uint32 TickSequence  = 0;

		// Sort key: deterministic ordering across shards
		[[nodiscard]] GALAXYCORE_API bool operator<(const FPendingEvent& Other) const;
	};

	FEventHandlerHandle SubscribeInternal(
		uint64 TypeId,
		EEventPriority Priority,
		TFunction<void(const IEvent&)> Handler,
		TFunction<bool(const IEvent&)> Filter,
		TWeakObjectPtr<UObject> WeakOwner);

	void DispatchRoutedEvent(const IEvent& Event);

	TMap<uint64, FRouterHandlerList> HandlersByType;
	TArray<FPendingEvent>            PendingEvents;

	uint64 NextHandlerId         = 1;
	uint64 NextRegistrationOrder = 1;
	int32  SubscriptionCount     = 0;
};
