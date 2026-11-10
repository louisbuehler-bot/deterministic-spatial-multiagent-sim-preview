// Copyright GALAXY_PROJECT. All Rights Reserved.
// =============================================================================
// EventBusRouter.cpp
// =============================================================================
#include "EventBusRouter.h"

DEFINE_LOG_CATEGORY(LogEventBusRouter);

// =============================================================================
// FEventBusRouter -- Lifecycle
// =============================================================================

void FEventBusRouter::Initialize()
{
	HandlersByType.Reset();
	PendingEvents.Reset();
	NextHandlerId         = 1;
	NextRegistrationOrder = 1;
	SubscriptionCount     = 0;
}

// =============================================================================
// FEventBusRouter -- Subscription
// =============================================================================

FEventHandlerHandle FEventBusRouter::SubscribeInternal(
	uint64                        TypeId,
	EEventPriority                Priority,
	TFunction<void(const IEvent&)> Handler,
	TFunction<bool(const IEvent&)> Filter,
	TWeakObjectPtr<UObject>        WeakOwner)
{
	const uint64 HandlerId = NextHandlerId++;

	FRouterHandlerEntry Entry;
	Entry.HandlerId         = HandlerId;
	Entry.Priority          = Priority;
	Entry.RegistrationOrder = NextRegistrationOrder++;
	Entry.Handler           = MoveTemp(Handler);
	Entry.Filter            = MoveTemp(Filter);
	Entry.WeakOwner         = WeakOwner;
	Entry.bHasWeakOwner     = WeakOwner.IsValid();

	FRouterHandlerList& List = HandlersByType.FindOrAdd(TypeId);
	if (List.DispatchDepth > 0)
	{
		// Defer: appending to Handlers could reallocate the array while one of its
		// TFunctions is still executing (re-entrant same-type Subscribe from a handler).
		List.PendingAdds.Add(MoveTemp(Entry));
	}
	else
	{
		List.Handlers.Add(MoveTemp(Entry));
		List.bNeedsSorting = true;
	}

	++SubscriptionCount;

	FEventHandlerHandle Handle;
	Handle.TypeId    = TypeId;
	Handle.HandlerId = HandlerId;
	return Handle;
}

bool FEventBusRouter::Unsubscribe(const FEventHandlerHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return false;
	}

	FRouterHandlerList* List = HandlersByType.Find(Handle.TypeId);
	if (!List)
	{
		return false;
	}

	// Tombstone if dispatch is in progress, otherwise remove immediately
	if (List->DispatchDepth > 0)
	{
		for (FRouterHandlerEntry& Entry : List->Handlers)
		{
			if (Entry.bAlive && Entry.HandlerId == Handle.HandlerId)
			{
				Entry.bAlive = false;
				++List->DeadCount;
				--SubscriptionCount;
				return true;
			}
		}
		// Not in Handlers -- may be a not-yet-drained pending add (subscribed
		// earlier in this same dispatch). PendingAdds is never iterated during
		// dispatch, so removal is safe here.
		const int32 RemovedPending = List->PendingAdds.RemoveAll(
			[&Handle](const FRouterHandlerEntry& E)
			{
				return E.HandlerId == Handle.HandlerId;
			});
		if (RemovedPending > 0)
		{
			SubscriptionCount -= RemovedPending;
			return true;
		}
		return false;
	}

	const int32 Removed = List->Handlers.RemoveAll(
		[&Handle](const FRouterHandlerEntry& E)
		{
			return E.HandlerId == Handle.HandlerId;
		});

	if (Removed > 0)
	{
		SubscriptionCount -= Removed;
		return true;
	}
	return false;
}

// =============================================================================
// FEventBusRouter -- Ingestion and dispatch
// =============================================================================

void FEventBusRouter::IngestFromShard(uint16 SourceShard, TArray<TUniquePtr<IEvent>>&& ExportedEvents)
{
	PendingEvents.Reserve(PendingEvents.Num() + ExportedEvents.Num());
	for (TUniquePtr<IEvent>& EventPtr : ExportedEvents)
	{
		if (!EventPtr)
		{
			continue;
		}
		FPendingEvent Pending;
		Pending.SourceShardId = SourceShard;
		Pending.EmitTick      = EventPtr->GetMetadata().EmitTick;
		Pending.TickSequence  = EventPtr->GetMetadata().TickSequence;
		Pending.Event         = MoveTemp(EventPtr);
		PendingEvents.Add(MoveTemp(Pending));
	}
}

int32 FEventBusRouter::FlushRouted()
{
	if (PendingEvents.Num() == 0)
	{
		return 0;
	}

	// Sort for deterministic cross-shard ordering before dispatch
	PendingEvents.Sort();

	int32 Dispatched = 0;
	for (FPendingEvent& Pending : PendingEvents)
	{
		if (Pending.Event)
		{
			DispatchRoutedEvent(*Pending.Event);
			++Dispatched;
		}
	}

	PendingEvents.Reset();
	return Dispatched;
}

void FEventBusRouter::DispatchRoutedEvent(const IEvent& Event)
{
	const uint64 TypeId = Event.GetTypeId();

	FRouterHandlerList* List = HandlersByType.Find(TypeId);
	if (!List || List->Handlers.Num() == 0)
	{
		return;
	}

	List->EnsureSorted();

	// Handlers may Subscribe() re-entrantly, which can rehash the handler map --
	// re-find List each iteration rather than holding the entry pointer across
	// a handler call. Same-type subscribes are deferred into PendingAdds.
	const int32 CountAtStart = List->Handlers.Num();
	++List->DispatchDepth;
	for (int32 i = 0; i < CountAtStart; ++i)
	{
		List = HandlersByType.Find(TypeId);
		if (!List || i >= List->Handlers.Num())
		{
			break;
		}
		FRouterHandlerEntry& Entry = List->Handlers[i];
		if (!Entry.bAlive)
		{
			continue;
		}
		// Tombstone stale weak owners
		if (Entry.bHasWeakOwner && !Entry.WeakOwner.IsValid())
		{
			Entry.bAlive = false;
			++List->DeadCount;
			--SubscriptionCount;
			continue;
		}
		// Optional filter predicate
		if (Entry.Filter && !Entry.Filter(Event))
		{
			continue;
		}
		Entry.Handler(Event);
	}
	List = HandlersByType.Find(TypeId);
	if (List)
	{
		--List->DispatchDepth;
		List->CompactDeadIfSafe();
		List->DrainPendingIfSafe();
	}
}

// =============================================================================
// FEventBusRouter -- Accessors
// =============================================================================

int32 FEventBusRouter::GetSubscriptionCount() const
{
	return SubscriptionCount;
}

int32 FEventBusRouter::GetPendingEventCount() const
{
	return PendingEvents.Num();
}

// ============================================================================
// FEventBusRouter::FRouterHandlerEntry
// ============================================================================

bool FEventBusRouter::FRouterHandlerEntry::operator<(const FRouterHandlerEntry& Other) const
{
	if (static_cast<int32>(Priority) != static_cast<int32>(Other.Priority))
	{
		return static_cast<int32>(Priority) < static_cast<int32>(Other.Priority);
	}
	return RegistrationOrder < Other.RegistrationOrder;
}


// ============================================================================
// FEventBusRouter::FRouterHandlerList
// ============================================================================

void FEventBusRouter::FRouterHandlerList::EnsureSorted()
{
	if (bNeedsSorting)
	{
		Handlers.Sort();
		bNeedsSorting = false;
	}
}

void FEventBusRouter::FRouterHandlerList::CompactDeadIfSafe()
{
	if (DispatchDepth == 0 && DeadCount > 0)
	{
		Handlers.RemoveAll([](const FRouterHandlerEntry& E) { return !E.bAlive; });
		DeadCount = 0;
	}
}

void FEventBusRouter::FRouterHandlerList::DrainPendingIfSafe()
{
	if (DispatchDepth == 0 && PendingAdds.Num() > 0)
	{
		for (FRouterHandlerEntry& Pending : PendingAdds)
		{
			Handlers.Add(MoveTemp(Pending));
		}
		PendingAdds.Reset();
		bNeedsSorting = true;
	}
}


// ============================================================================
// FEventBusRouter::FPendingEvent
// ============================================================================

bool FEventBusRouter::FPendingEvent::operator<(const FPendingEvent& Other) const
{
	if (SourceShardId != Other.SourceShardId) return SourceShardId < Other.SourceShardId;
	if (EmitTick      != Other.EmitTick)      return EmitTick      < Other.EmitTick;
	return TickSequence < Other.TickSequence;
}
