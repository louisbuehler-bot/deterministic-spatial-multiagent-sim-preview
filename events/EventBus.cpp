// Copyright GALAXY_PROJECT. All Rights Reserved.
// =============================================================================
// EventBus.cpp
//
// Log category, global stats registry, FJsonEventRecorder, and console commands
// for the FEventBusManager pub/sub system.
// =============================================================================

#include "EventBus.h"
#include "CoreTypes/SchedulerConstants.h" // TickConstants::FramesPerSuperTick -- was reached through EventBus.h
// g.eventbus.Record / .DumpStats below are FAutoConsoleCommandWithWorldArgsAndOutputDevice.
// Previously supplied by the engine shared PCH, which GalaxyCore no longer uses (see the
// note in EventBus.h).
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/CriticalSection.h"

DEFINE_LOG_CATEGORY(LogEventBus);

void FEventBusManager::RegisterForStats(FName BusName, FEventBusManager* Bus)
{
	RegisteredEventBuses.Add(BusName, Bus);
	// If a recording session is already active, attach it now so buses created
	// mid-recording (dynamically spawned planet/combat shards) are captured too
	// -- otherwise only the buses present when g.eventbus.Record 1 ran would trace.
	if (Bus && ActiveEventRecorder)
	{
		Bus->SetEventRecorder(ActiveEventRecorder.Get());
	}
}

void FEventBusManager::UnregisterForStats(FName BusName)
{
	RegisteredEventBuses.Remove(BusName);
}

// =============================================================================
// FJsonEventRecorder - JSONL trace writer (D1)
// =============================================================================

/**
 * Writes one JSON line per emit and per dispatch to a .jsonl file.
 * Thread-safe via a mutex -- shard buses run on worker threads so concurrent
 * calls to RecordEmit/RecordDispatch are expected.
 *
 * Output format (one JSON object per line):
 *   emit:     {"type":"emit","tick":N,"seq":N,"domain":"X","event":"Y","shard":N}
 *   dispatch: {"type":"dispatch","event":"Y","handler":N}
 *
 * Performance: intended for debug/bug-repro only. The mutex + file write per
 * event is not suitable for production use.
 */
class FJsonEventRecorder final : public IEventRecorder
{
public:
	explicit FJsonEventRecorder(const FString& FilePath)
	{
		FileHandle.Reset(IFileManager::Get().CreateFileWriter(*FilePath, FILEWRITE_Append));
		if (FileHandle)
		{
			UE_LOG(LogEventBus, Display,
				TEXT("[EventBus][Recorder] Trace opened: %s"), *FilePath);
		}
		else
		{
			UE_LOG(LogEventBus, Warning,
				TEXT("[EventBus][Recorder] Failed to open trace file: %s"), *FilePath);
		}
	}

	~FJsonEventRecorder() override
	{
		FScopeLock Lock(&WriteMutex);
		FileHandle.Reset();
	}

	void RecordEmit(const IEvent& Event, const FEventMetadata& Meta) override
	{
		const FString Line = FString::Printf(
			TEXT("{\"type\":\"emit\",\"tick\":%llu,\"seq\":%u,\"domain\":\"%s\","
			     "\"event\":\"%s\",\"shard\":%u}\n"),
			Meta.EmitTick,
			Meta.TickSequence,
			*Meta.SourceDomain.ToString(),
			*Event.GetEventName(),
			static_cast<uint32>(Meta.SourceShardId));
		WriteLine(Line);
	}

	void RecordDispatch(const IEvent& Event, uint64 HandlerId) override
	{
		const FString Line = FString::Printf(
			TEXT("{\"type\":\"dispatch\",\"event\":\"%s\",\"handler\":%llu}\n"),
			*Event.GetEventName(),
			HandlerId);
		WriteLine(Line);
	}

	[[nodiscard]] bool IsOpen() const { return FileHandle.IsValid(); }

private:
	void WriteLine(const FString& Line)
	{
		if (!FileHandle) return;
		FScopeLock Lock(&WriteMutex);
		const auto Ansi = StringCast<ANSICHAR>(*Line);
		FileHandle->Serialize(const_cast<ANSICHAR*>(Ansi.Get()), Ansi.Length());
	}

	TUniquePtr<FArchive> FileHandle;
	FCriticalSection WriteMutex;
};

// =============================================================================
// Console commands (D1, D2)
// =============================================================================

TMap<FName, FEventBusManager*> FEventBusManager::RegisteredEventBuses;
TSharedPtr<IEventRecorder> FEventBusManager::ActiveEventRecorder;

void FEventBusManager::SetRecordingEnabled(bool bEnable, FOutputDevice& Out)
{
	if (bEnable)
	{
		if (ActiveEventRecorder)
		{
			Out.Log(TEXT("[EventBus][Recorder] Already recording. Stop first with g.eventbus.Record 0."));
			return;
		}

		const FString Dir = FPaths::ProjectSavedDir() / TEXT("EventTraces");
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		const FString Path = Dir / FString::Printf(TEXT("%lld.jsonl"),
			FDateTime::UtcNow().GetTicks()); // wallclock-safe: names a debug trace file, never read back into sim

		TSharedPtr<FJsonEventRecorder> NewRecorder = MakeShared<FJsonEventRecorder>(Path);
		if (NewRecorder->IsOpen())
		{
			ActiveEventRecorder = StaticCastSharedPtr<IEventRecorder>(NewRecorder);
			for (auto& KV : RegisteredEventBuses) // tmap-order-safe: sets the same recorder on every bus; no bus is read
			{
				if (KV.Value)
				{
					KV.Value->SetEventRecorder(ActiveEventRecorder.Get());
				}
			}
			Out.Logf(TEXT("[EventBus][Recorder] Recording started. %d bus(es) attached. File: %s"),
				RegisteredEventBuses.Num(), *Path);
		}
		else
		{
			Out.Log(TEXT("[EventBus][Recorder] Failed to open trace file."));
		}
		return;
	}

	if (!ActiveEventRecorder)
	{
		Out.Log(TEXT("[EventBus][Recorder] Not recording."));
		return;
	}

	for (auto& KV : RegisteredEventBuses) // tmap-order-safe: clears the recorder on every bus; no bus is read
	{
		if (KV.Value)
		{
			KV.Value->SetEventRecorder(nullptr);
		}
	}
	ActiveEventRecorder.Reset();
	Out.Log(TEXT("[EventBus][Recorder] Recording stopped."));
}

void FEventBusManager::DumpRegisteredBusStats(FOutputDevice& Out)
{
	if (RegisteredEventBuses.Num() == 0)
	{
		Out.Log(TEXT("[EventBus][Stats] No buses registered. Call FEventBusManager::RegisterForStats() to enable."));
		return;
	}

	TArray<FName> BusNames;
	RegisteredEventBuses.GetKeys(BusNames);
	BusNames.Sort([](const FName& A, const FName& B)
	{
		return A.ToString() < B.ToString();
	});

	for (const FName& BusName : BusNames)
	{
		FEventBusManager* Bus = RegisteredEventBuses.FindRef(BusName);
		if (!Bus)
		{
			continue;
		}

		const FEventBusTelemetry& Tel = Bus->GetTelemetry();
		Out.Logf(TEXT("[EventBus][Stats] === Bus: %s ==="), *BusName.ToString());
		Out.Logf(TEXT("  Aggregate: emitted=%llu dispatched=%llu handler_calls=%llu queued=%d subs=%d"),
			Tel.TotalEmitted, Tel.TotalDispatched, Tel.TotalHandlerCalls,
			Tel.QueuedCount, Tel.SubscriptionCount);
		Out.Logf(TEXT("  Bytes:     total_emitted=%llu"), Tel.TotalBytesEmitted);
		Out.Logf(TEXT("  Dropped:   no_subs=%llu overflow=%llu coalesce_overflow=%llu reentrant=%llu"),
			Tel.DroppedNoSubscribers, Tel.DroppedOverflow, Tel.DroppedCoalesceOverflow,
			Tel.ReentrantEmissions);

		if (Bus->GetDomainTelemetry().Num() > 0)
		{
			Out.Log(TEXT("  Per-domain breakdown:"));
			TArray<FName> Domains;
			Bus->GetDomainTelemetry().GetKeys(Domains);
			Domains.Sort([](const FName& A, const FName& B)
			{
				return A.ToString() < B.ToString();
			});
			for (const FName& Domain : Domains)
			{
				const FDomainStats* DS = Bus->GetDomainTelemetry().Find(Domain);
				if (DS)
				{
					Out.Logf(TEXT("    [%s] emitted=%llu dispatched=%llu handler_calls=%llu bytes=%llu"),
						*Domain.ToString(), DS->TotalEmitted, DS->TotalDispatched,
						DS->TotalHandlerCalls, DS->BytesEmitted);
				}
			}
		}
		else
		{
			Out.Log(TEXT("  (no domain-tagged events emitted)"));
		}

		if (Bus->GetTypeTelemetry().Num() > 0)
		{
			Out.Log(TEXT("  Per-type breakdown:"));
			TArray<uint64> TypeIds;
			Bus->GetTypeTelemetry().GetKeys(TypeIds);
			TypeIds.Sort([&](uint64 A, uint64 B)
			{
				const FTypeStats* SA = Bus->GetTypeTelemetry().Find(A);
				const FTypeStats* SB = Bus->GetTypeTelemetry().Find(B);
				const uint64 EA = SA ? SA->TotalEmitted : 0;
				const uint64 EB = SB ? SB->TotalEmitted : 0;
				// Equal counts fall to the type id, so ties never print in the map's order.
				return EA != EB ? EA > EB : A < B;
			});
			for (const uint64 TypeId : TypeIds)
			{
				const FTypeStats* TS = Bus->GetTypeTelemetry().Find(TypeId);
				if (TS)
				{
					Out.Logf(TEXT("    [%s] emitted=%llu dispatched=%llu handler_calls=%llu bytes=%llu"),
						*TS->EventName, TS->TotalEmitted, TS->TotalDispatched,
						TS->TotalHandlerCalls, TS->BytesEmitted);
				}
			}
		}
	}
}

// QA tier: diagnostics only, so neither command ships (console-command-conventions).
#if !UE_BUILD_SHIPPING
static FAutoConsoleCommandWithWorldArgsAndOutputDevice GEventBusCmdRecord(
	TEXT("g.eventbus.Record"),
	TEXT("Usage: g.eventbus.Record [0|1]  -- start (1) or stop (0) event trace recording.\n"
	     "Writes JSONL to Saved/EventTraces/. Attaches to all buses registered via\n"
	     "FEventBusManager::RegisterForStats(). Stop recording before shard ticks to avoid\n"
	     "a race between the worker thread and this game-thread command."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld*, FOutputDevice& Out)
		{
			const bool bEnable = (Args.Num() == 0) || (Args[0] != TEXT("0"));
			FEventBusManager::SetRecordingEnabled(bEnable, Out);
		}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GEventBusCmdStats(
	TEXT("g.eventbus.Stats"),
	TEXT("Print aggregate and per-domain telemetry for all registered event buses.\n"
	     "Buses must call FEventBusManager::RegisterForStats() to appear here."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>&, UWorld*, FOutputDevice& Out)
		{
			FEventBusManager::DumpRegisteredBusStats(Out);
		}));
#endif // !UE_BUILD_SHIPPING

// =============================================================================
// FEventBusManager non-template method implementations
// Kept in .cpp to limit recompile scope: changes here touch only 1 TU
// instead of every file that transitively includes EventBus.h.
// =============================================================================

void FEventBusManager::Initialize(const FEventBusConfig& InConfig)
{
	Config = InConfig;
	Telemetry.Reset();
	// Clear any live events before reallocating the ring
	if (Ring.Num() > 0)
	{
		for (int32 i = 0; i < RingCount; ++i)
			Ring[(RingHead + i) % Ring.Num()].Event.Reset();
	}
	Ring.Reset();
	const int32 Cap = (Config.MaxQueueDepth > 0) ? Config.MaxQueueDepth : DefaultRingCapacity;
	Ring.SetNum(Cap);
	RingHead = 0;
	RingCount = 0;
	CoalesceSlots.Reset();
	FlushScratch.Reset();
	HandlersByType.Reset();
	NextHandlerId = 1;
	NextRegistrationOrder = 1;
	CurrentTick = 0;
	TickSequence = 0;
	bInFlush = false;
	SourceShardId = 0;
	ExportQueue.Reset();
}

int32 FEventBusManager::Flush()
{
	// Re-entrant Flush() (a handler calling Flush on its own bus) is not
	// supported: the drain scratch is shared and the single-phase policy
	// already defers re-entrant emits to the next flush.
	if (!ensureMsgf(!bInFlush, TEXT("[EventBus] Re-entrant Flush() call ignored")))
	{
		return 0;
	}

	// D3: pre-flush hook fires before ring drain or dispatch.
	// The waiver below rests on WHO SUBSCRIBES, and today that is only
	// EventBusTests -- there is no production subscriber on either hook. A
	// subscriber that writes simulation state would make broadcast order an
	// outcome, and the suppression's per-site hash covers this line, not the
	// subscriber list, so it will NOT re-fire for you. Re-argue it here first.
	OnPreFlush.Broadcast(*this); // delegate-order-safe -- diagnostic-only subscribers, order-insensitive

	const uint64 CurrentSuperTick = static_cast<uint64>(CurrentTick / TickConstants::FramesPerSuperTick);
	int32 DispatchedCount = 0;
	const int32 Cap = Ring.Num();

	// ---------- Phase 1: drain ring into a local snapshot ----------
	// Re-entrant emits during dispatch land in the (now-empty) ring and are
	// processed on the next Flush() call (strict 1-tick-latency policy).
	TArray<EventBusInternal::FQueuedEvent>& LocalQueue = FlushScratch;
	LocalQueue.Reset();
	LocalQueue.Reserve(RingCount);
	for (int32 i = 0; i < RingCount; ++i)
		LocalQueue.Add(MoveTemp(Ring[(RingHead + i) % Cap]));
	RingHead = 0;
	RingCount = 0;
	Telemetry.QueuedCount = 0;

	// ---------- Phase 2: capture coalesce slots ----------
	// Swap out before dispatching so re-entrant EmitCoalesce calls go to the
	// NEXT Flush (same 1-tick latency contract as regular Emit).
	TMap<TPair<uint64, uint64>, TUniquePtr<IEvent>> LocalCoalesce;
	Swap(LocalCoalesce, CoalesceSlots);

	bInFlush = true;

	// ---------- Phase 3: dispatch coalesced events (C3) ----------
	// Sorted by (TypeId asc, CoalesceKey asc) for deterministic dispatch order.
	if (LocalCoalesce.Num() > 0)
	{
		TArray<TPair<uint64, uint64>> CoalesceKeys;
		CoalesceKeys.Reserve(LocalCoalesce.Num());
		// Collect-then-sort: this loop only GATHERS keys, and the very next statement sorts
		// them under a total order on (TypeId, CoalesceKey). Nothing between the two reads
		// the collection, so map order cannot survive into dispatch.
		for (auto& KV : LocalCoalesce) // tmap-order-safe -- sorted on the next statement
			CoalesceKeys.Add(KV.Key);
		CoalesceKeys.Sort([](const TPair<uint64, uint64>& A, const TPair<uint64, uint64>& B)
		{
			return (A.Key != B.Key) ? (A.Key < B.Key) : (A.Value < B.Value);
		});
		for (const TPair<uint64, uint64>& Key : CoalesceKeys)
		{
			if (TUniquePtr<IEvent>* Slot = LocalCoalesce.Find(Key))
			{
				if (*Slot)
				{
					DispatchEvent(**Slot);
					++DispatchedCount;
				}
			}
		}
	}

	// ---------- Phase 4: dispatch regular ring queue ----------
	const int32 TotalToProcess = (Config.MaxEventsPerFlush > 0)
		? FMath::Min(Config.MaxEventsPerFlush, LocalQueue.Num())
		: LocalQueue.Num();

	for (int32 i = 0; i < TotalToProcess; ++i)
	{
		if (LocalQueue[i].Event)
		{
			DispatchEvent(*LocalQueue[i].Event);
			++DispatchedCount;
		}
	}
	bInFlush = false;

	// ---------- Phase 5: spill-back if MaxEventsPerFlush was hit ----------
	// Spilled events precede any re-entrant events that landed in the ring
	// during phases 3-4. Drain re-entrant events, prepend spilled, then re-entrant.
	if (TotalToProcess < LocalQueue.Num())
	{
		const int32 ReentrantCount = RingCount; // events emitted during dispatch

		// Drain re-entrant events out of the ring
		TArray<EventBusInternal::FQueuedEvent> Reentrant;
		Reentrant.Reserve(ReentrantCount);
		for (int32 i = 0; i < ReentrantCount; ++i)
			Reentrant.Add(MoveTemp(Ring[(RingHead + i) % Cap]));
		RingHead = 0;
		RingCount = 0;
		Telemetry.QueuedCount = 0;

		// Refill through QueueEvent so the overflow policy applies when
		// spilled + re-entrant exceeds ring capacity: spilled first, then re-entrant
		for (int32 i = TotalToProcess; i < LocalQueue.Num(); ++i)
		{
			QueueEvent(MoveTemp(LocalQueue[i].Event));
		}
		for (EventBusInternal::FQueuedEvent& Item : Reentrant)
		{
			QueueEvent(MoveTemp(Item.Event));
		}
	}
	LocalQueue.Reset();

	if (Telemetry.bHasLastFlushSuperTick)
	{
		Telemetry.SuperTicksBetweenFlushes = CurrentSuperTick - Telemetry.LastFlushSuperTick;
	}
	else
	{
		Telemetry.SuperTicksBetweenFlushes = 0;
		Telemetry.bHasLastFlushSuperTick = true;
	}
	Telemetry.LastFlushSuperTick = CurrentSuperTick;

	// D3: post-flush hook fires after all dispatch and telemetry updates.
	// Same standing condition as OnPreFlush above: diagnostic-only subscribers.
	OnPostFlush.Broadcast(*this); // delegate-order-safe -- diagnostic-only subscribers, order-insensitive

	return DispatchedCount;
}

int32 FEventBusManager::ClearQueue()
{
	const int32 Count = RingCount + CoalesceSlots.Num();
	const int32 Cap = Ring.Num();
	for (int32 i = 0; i < RingCount; ++i)
		Ring[(RingHead + i) % Cap].Event.Reset();
	RingHead = 0;
	RingCount = 0;
	Telemetry.QueuedCount = 0;
	CoalesceSlots.Reset();
	return Count;
}

void FEventBusManager::Serialize(FArchive& Ar)
{
	if (Ar.IsSaving())
	{
		if (!ensureMsgf(RingCount == 0 && CoalesceSlots.Num() == 0,
			TEXT("[EventBus] Serialize() called with %d queued and %d coalesced events still pending -- Flush() first"),
			RingCount, CoalesceSlots.Num()))
		{
			return;
		}
	}

	const uint16 SerializeVersion = 1;
	uint16 Version = SerializeVersion;
	Ar << Version;
	Ar << CurrentTick;
	Ar << TickSequence;
}

void FEventBusManager::AddHandlerEntry(uint64 TypeId, EventBusInternal::FHandlerEntry&& Entry)
{
	EventBusInternal::FHandlerList& List = HandlersByType.FindOrAdd(TypeId);
	if (List.DispatchDepth > 0)
	{
		// Defer: appending to Handlers could reallocate the array while one of its
		// TFunctions is still executing (re-entrant same-type Subscribe from a handler).
		// Drained by DrainPendingIfSafe() when the dispatch completes.
		List.PendingAdds.Add(MoveTemp(Entry));
	}
	else
	{
		List.Handlers.Add(MoveTemp(Entry));
		List.bNeedsSorting = true;
	}
	++Telemetry.SubscriptionCount;
}

bool FEventBusManager::Unsubscribe(const FEventHandlerHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return false;
	}

	EventBusInternal::FHandlerList* List = HandlersByType.Find(Handle.TypeId);
	if (!List)
	{
		return false;
	}

	// If a dispatch is in progress for this list, tombstone the entry so iteration
	// stays stable. Live entries are compacted out when DispatchDepth returns to 0.
	if (List->DispatchDepth > 0)
	{
		for (EventBusInternal::FHandlerEntry& Entry : List->Handlers)
		{
			if (Entry.bAlive && Entry.HandlerId == Handle.HandlerId)
			{
				Entry.bAlive = false;
				++List->DeadCount;
				--Telemetry.SubscriptionCount;
				return true;
			}
		}
		// Not in Handlers -- may be a not-yet-drained pending add (subscribed
		// earlier in this same dispatch). PendingAdds is never iterated during
		// dispatch, so removal is safe here.
		const int32 RemovedPending = List->PendingAdds.RemoveAll(
			[&Handle](const EventBusInternal::FHandlerEntry& Entry)
			{
				return Entry.HandlerId == Handle.HandlerId;
			});
		if (RemovedPending > 0)
		{
			Telemetry.SubscriptionCount -= RemovedPending;
			return true;
		}
		return false;
	}

	const int32 Removed = List->Handlers.RemoveAll(
		[&Handle](const EventBusInternal::FHandlerEntry& Entry)
		{
			return Entry.HandlerId == Handle.HandlerId;
		});

	if (Removed > 0)
	{
		Telemetry.SubscriptionCount -= Removed;
		return true;
	}

	return false;
}

int32 FEventBusManager::UnsubscribeByOwner(const UObject* Owner)
{
	if (!Owner)
	{
		return 0;
	}

	int32 RemovedTotal = 0;
	for (auto& ListPair : HandlersByType) // tmap-order-safe: each list is pruned on its own and the count is a sum
	{
		EventBusInternal::FHandlerList& List = ListPair.Value;
		if (List.DispatchDepth > 0)
		{
			for (EventBusInternal::FHandlerEntry& Entry : List.Handlers)
			{
				if (Entry.bAlive && Entry.bHasWeakOwner && Entry.WeakOwner.Get() == Owner)
				{
					Entry.bAlive = false;
					++List.DeadCount;
					--Telemetry.SubscriptionCount;
					++RemovedTotal;
				}
			}
		}
		else
		{
			const int32 Removed = List.Handlers.RemoveAll(
				[Owner](const EventBusInternal::FHandlerEntry& Entry)
				{
					return Entry.bHasWeakOwner && Entry.WeakOwner.Get() == Owner;
				});
			if (Removed > 0)
			{
				Telemetry.SubscriptionCount -= Removed;
				RemovedTotal += Removed;
			}
		}

		// Not-yet-drained pending adds (PendingAdds is never iterated during
		// dispatch, so removal is safe in both branches).
		const int32 RemovedPending = List.PendingAdds.RemoveAll(
			[Owner](const EventBusInternal::FHandlerEntry& Entry)
			{
				return Entry.bHasWeakOwner && Entry.WeakOwner.Get() == Owner;
			});
		if (RemovedPending > 0)
		{
			Telemetry.SubscriptionCount -= RemovedPending;
			RemovedTotal += RemovedPending;
		}
	}
	return RemovedTotal;
}

void FEventBusManager::Emit(TUniquePtr<IEvent> Event, EEventDispatchMode Mode, FName SourceDomain)
{
	if (!Event)
	{
		return;
	}

	// C1: early-out when no subscribers exist and no cross-shard export is needed.
	// Avoids metadata setup, telemetry increments, and ring write for unreachable events.
	const uint64 TypeId = Event->GetTypeId();
	const bool bHasHandlers = HandlersByType.Contains(TypeId);
	const bool bMustExport = (Mode == EEventDispatchMode::QueuedAndExport);
	if (!bHasHandlers && !bMustExport)
	{
		++Telemetry.DroppedNoSubscribers;
		return;
	}

	// Set metadata (stamp source shard if one has been configured)
	FEventMetadata Meta(CurrentTick, TickSequence++, SourceDomain, SourceShardId);
	Event->SetMetadata(Meta);

	++Telemetry.TotalEmitted;

	// Byte-level + per-type telemetry (items 5, 7)
	const size_t EventBytes = Event->GetEventSizeBytes();
	Telemetry.TotalBytesEmitted += static_cast<uint64>(EventBytes);
	{
		FTypeStats& TS = Telemetry.TypeTelemetry.FindOrAdd(TypeId);
		if (TS.EventName.IsEmpty())
		{
			TS.EventName = Event->GetEventName();
		}
		++TS.TotalEmitted;
		TS.BytesEmitted += static_cast<uint64>(EventBytes);
	}

	// D2: per-domain emission tracking (NAME_None entries are excluded)
	if (!SourceDomain.IsNone())
	{
		FDomainStats& DS = Telemetry.DomainTelemetry.FindOrAdd(SourceDomain);
		++DS.TotalEmitted;
		DS.BytesEmitted += static_cast<uint64>(EventBytes);
	}

	// D1: recorder hook -- metadata is already stamped on the event above
	if (Recorder)
	{
		Recorder->RecordEmit(*Event, Event->GetMetadata());
	}

	// Track re-entrant emission during Flush(). These land in the NEXT flush
	// (1-tick latency) per the strict single-phase policy.
	if (bInFlush && Mode != EEventDispatchMode::Immediate)
	{
		++Telemetry.ReentrantEmissions;
		if (!Config.bAllowReentrantEmission)
		{
			UE_LOG(LogEventBus, Warning,
				TEXT("[EventBus] Re-entrant emission of %s during Flush() (1-tick latency)"),
				*Event->GetEventName());
		}
	}

	if (Mode == EEventDispatchMode::Immediate)
	{
		DispatchEvent(*Event);
	}
	else if (Mode == EEventDispatchMode::QueuedAndExport)
	{
		ExportQueue.Add(Event->Clone());
		QueueEvent(MoveTemp(Event));
	}
	else
	{
		QueueEvent(MoveTemp(Event));
	}
}

void FEventBusManager::EmitCoalesceInternal(uint64 TypeId, TUniquePtr<IEvent> NewEvent, uint64 CoalesceKey, FName SourceDomain)
{
	FEventMetadata Meta(CurrentTick, TickSequence++, SourceDomain, SourceShardId);
	NewEvent->SetMetadata(Meta);

	TPair<uint64, uint64> Key{TypeId, CoalesceKey};

	// Coalesce-slot cap (item 3). Re-emits of existing keys always succeed; only new
	// keys can overflow. Cap of 0 disables the check (legacy behavior).
	if (Config.MaxCoalesceSlots > 0
		&& CoalesceSlots.Num() >= Config.MaxCoalesceSlots
		&& !CoalesceSlots.Contains(Key))
	{
		++Telemetry.DroppedCoalesceOverflow;
		UE_LOG(LogEventBus, Warning,
			TEXT("[EventBus] Coalesce map full (%d slots); dropping new key for %s"),
			Config.MaxCoalesceSlots, *NewEvent->GetEventName());
		return;
	}

	++Telemetry.TotalEmitted;

	// Byte-level + per-type telemetry (items 5, 7)
	const size_t EventBytes = NewEvent->GetEventSizeBytes();
	Telemetry.TotalBytesEmitted += static_cast<uint64>(EventBytes);
	{
		FTypeStats& TS = Telemetry.TypeTelemetry.FindOrAdd(TypeId);
		if (TS.EventName.IsEmpty())
		{
			TS.EventName = NewEvent->GetEventName();
		}
		++TS.TotalEmitted;
		TS.BytesEmitted += static_cast<uint64>(EventBytes);
	}

	if (!SourceDomain.IsNone())
	{
		FDomainStats& DS = Telemetry.DomainTelemetry.FindOrAdd(SourceDomain);
		++DS.TotalEmitted;
		DS.BytesEmitted += static_cast<uint64>(EventBytes);
	}

	// D1: recorder hook -- fires before the upsert so the emit record always
	// appears in the trace even when the same key gets overwritten later.
	// The emit is counted here; a dispatch record will follow in Flush().
	if (Recorder)
	{
		Recorder->RecordEmit(*NewEvent, NewEvent->GetMetadata());
	}

	// Re-entrant coalesce emissions land in the NEXT flush (the slots were
	// swapped out at flush start) -- same 1-tick-latency contract as Emit().
	if (bInFlush)
	{
		++Telemetry.ReentrantEmissions;
		if (!Config.bAllowReentrantEmission)
		{
			UE_LOG(LogEventBus, Warning,
				TEXT("[EventBus] Re-entrant coalesce emission of %s during Flush() (1-tick latency)"),
				*NewEvent->GetEventName());
		}
	}

	// Upsert: if a coalesce entry already exists for this key, overwrite it.
	// Latest payload wins; metadata is from this (most recent) emit call.
	CoalesceSlots.Add(Key, MoveTemp(NewEvent));
}

void FEventBusManager::QueueEvent(TUniquePtr<IEvent>&& Event)
{
	const int32 Cap = Ring.Num();
	if (RingCount >= Cap)
	{
		// Ring is full -- apply overflow policy (C2)
		switch (Config.OverflowPolicy)
		{
		case EQueueOverflowPolicy::AssertFail:
			ensureMsgf(false, TEXT("[EventBus] Ring buffer overflow: %d/%d events"), RingCount, Cap);
			return;

		case EQueueOverflowPolicy::DropNewest:
			++Telemetry.DroppedOverflow;
			UE_LOG(LogEventBus, Warning,
				TEXT("[EventBus] Ring buffer full (%d), dropping newest event"), Cap);
			return;

		default: // DropOldest
			Ring[RingHead].Event.Reset();
			RingHead = (RingHead + 1) % Cap;
			--RingCount;
			--Telemetry.QueuedCount;
			++Telemetry.DroppedOverflow;
			UE_LOG(LogEventBus, Warning,
				TEXT("[EventBus] Ring buffer full (%d), dropping oldest event"), Cap);
			break;
		}
	}

	const int32 Tail = (RingHead + RingCount) % Cap;
	Ring[Tail].Event = MoveTemp(Event);
	Ring[Tail].Mode  = EEventDispatchMode::Queued;
	++RingCount;
	++Telemetry.QueuedCount;
	Telemetry.PeakQueueDepth = FMath::Max(Telemetry.PeakQueueDepth, Telemetry.QueuedCount);
}

void FEventBusManager::DispatchEvent(const IEvent& Event)
{
	const uint64 TypeId = Event.GetTypeId();

	EventBusInternal::FHandlerList* List = HandlersByType.Find(TypeId);
	if (!List || List->Handlers.Num() == 0)
	{
		return;
	}

	List->EnsureSorted();

	++Telemetry.TotalDispatched;

	// Per-type dispatch tracking (item 5)
	if (FTypeStats* TypeStats = Telemetry.TypeTelemetry.Find(TypeId))
	{
		++TypeStats->TotalDispatched;
	}

	// D2: per-domain dispatch tracking
	const FName EventDomain = Event.GetMetadata().SourceDomain;
	if (!EventDomain.IsNone())
	{
		++Telemetry.DomainTelemetry.FindOrAdd(EventDomain).TotalDispatched;
	}

	if (Config.bLogDispatch)
	{
		UE_LOG(LogEventBus, Verbose, TEXT("[EventBus] Dispatching %s to %d handlers"),
			*Event.GetEventName(), List->Handlers.Num());
	}

	// Tombstone-based iteration: handlers may Unsubscribe themselves or each other
	// during dispatch. Unsubscribes set bAlive=false rather than mutating the array.
	// Compaction runs when DispatchDepth returns to 0 (supports nested dispatch).
	// Handlers subscribed during this dispatch (Count rises) are NOT invoked this pass.
	// Handlers may also Emit() or Subscribe() re-entrantly, which can rehash the
	// telemetry/handler TMaps -- so no map entry pointer is held across a handler
	// call: List is re-found each iteration and handler-call counters are
	// accumulated locally, then written back after the loop.
	const int32 CountAtStart = List->Handlers.Num();
	++List->DispatchDepth;
	uint64 HandlerCallCount = 0;
	for (int32 i = 0; i < CountAtStart; ++i)
	{
		List = HandlersByType.Find(TypeId);
		if (!List || i >= List->Handlers.Num())
		{
			break;
		}
		EventBusInternal::FHandlerEntry& Entry = List->Handlers[i];
		if (!Entry.bAlive)
		{
			continue;
		}
		// Stale weak owner -- tombstone now, skip dispatch
		if (Entry.bHasWeakOwner && !Entry.WeakOwner.IsValid())
		{
			Entry.bAlive = false;
			++List->DeadCount;
			--Telemetry.SubscriptionCount;
			continue;
		}
		// Optional filter predicate
		if (Entry.Filter && !Entry.Filter(Event))
		{
			continue;
		}
		// Copy the ID before invoking: a re-entrant same-type Subscribe may
		// reallocate the handler array and invalidate Entry.
		const uint64 HandlerId = Entry.HandlerId;
		Entry.Handler(Event);
		++HandlerCallCount;
		// D1: recorder
		if (Recorder)
		{
			Recorder->RecordDispatch(Event, HandlerId);
		}
	}

	Telemetry.TotalHandlerCalls += HandlerCallCount;
	if (HandlerCallCount > 0)
	{
		if (FTypeStats* TypeStats = Telemetry.TypeTelemetry.Find(TypeId))
		{
			TypeStats->TotalHandlerCalls += HandlerCallCount;
		}
		if (!EventDomain.IsNone())
		{
			Telemetry.DomainTelemetry.FindOrAdd(EventDomain).TotalHandlerCalls += HandlerCallCount;
		}
	}

	List = HandlersByType.Find(TypeId);
	if (List)
	{
		--List->DispatchDepth;
		List->CompactDeadIfSafe();
		List->DrainPendingIfSafe();
	}
}

// ============================================================================
// FEventMetadata
// ============================================================================

FEventMetadata::FEventMetadata(uint64 InTick, uint32 InSequence, FName InSource)
	: EmitTick(InTick)
	, TickSequence(InSequence)
	, SourceDomain(InSource)
{
}

FEventMetadata::FEventMetadata(uint64 InTick, uint32 InSequence, FName InSource, uint16 InSourceShardId)
	: EmitTick(InTick)
	, TickSequence(InSequence)
	, SourceDomain(InSource)
	, SourceShardId(InSourceShardId)
{
}


// ============================================================================
// FEventBusTelemetry
// ============================================================================

void FEventBusTelemetry::Reset()
{
	TotalEmitted = 0;
	TotalDispatched = 0;
	TotalHandlerCalls = 0;
	QueuedCount = 0;
	SubscriptionCount = 0;
	SuperTicksBetweenFlushes = 0;
	LastFlushSuperTick = 0;
	bHasLastFlushSuperTick = false;
	PeakQueueDepth = 0;
	ReentrantEmissions = 0;
	DroppedNoSubscribers = 0;
	DroppedOverflow = 0;
	DroppedCoalesceOverflow = 0;
	TotalBytesEmitted = 0;
	DomainTelemetry.Reset();
	TypeTelemetry.Reset();
}


// ============================================================================
// FEventTypeRegistry
// ============================================================================

FEventTypeRegistry& FEventTypeRegistry::Get()
{
	// Contents are registration-ORDER-independent. Keys are GetStaticTypeId()
	// (constexpr FNV-1a of the compiler type signature) and StaticTypeTag()
	// (authored FName) -- neither is a first-use counter -- and both maps are
	// only ever point-queried (FindFactory/FindTag/IsRegistered), never
	// iterated for an outcome. Registration order moves TMap layout only.
	static FEventTypeRegistry Instance; // static-local-safe -- the argument is in the comment above: its contents are registration-ORDER-independent because both keys are content-derived (never a first-use counter) and both maps are only ever point-queried, never iterated for an outcome
	return Instance;
}


// ============================================================================
// EventBusInternal::FHandlerEntry
// ============================================================================

bool EventBusInternal::FHandlerEntry::operator<(const FHandlerEntry& Other) const
{
	// Sort by priority first, then registration order
	if (static_cast<int32>(Priority) != static_cast<int32>(Other.Priority))
	{
		return static_cast<int32>(Priority) < static_cast<int32>(Other.Priority);
	}
	return RegistrationOrder < Other.RegistrationOrder;
}


// ============================================================================
// EventBusInternal::FHandlerList
// ============================================================================

void EventBusInternal::FHandlerList::EnsureSorted()
{
	if (bNeedsSorting)
	{
		Handlers.Sort();
		bNeedsSorting = false;
	}
}

void EventBusInternal::FHandlerList::CompactDeadIfSafe()
{
	if (DispatchDepth == 0 && DeadCount > 0)
	{
		Handlers.RemoveAll([](const FHandlerEntry& E) { return !E.bAlive; });
		DeadCount = 0;
	}
}

void EventBusInternal::FHandlerList::DrainPendingIfSafe()
{
	if (DispatchDepth == 0 && PendingAdds.Num() > 0)
	{
		for (FHandlerEntry& Pending : PendingAdds)
		{
			Handlers.Add(MoveTemp(Pending));
		}
		PendingAdds.Reset();
		bNeedsSorting = true;
	}
}


// ============================================================================
// FEventBusManager
// ============================================================================

void FEventBusManager::SetCurrentTick(uint64 Tick)
{
	if (Tick != CurrentTick)
	{
		CurrentTick = Tick;
		TickSequence = 0;
	}
}

TArray<TUniquePtr<IEvent>> FEventBusManager::StealExportedEvents()
{
	TArray<TUniquePtr<IEvent>> Stolen = MoveTemp(ExportQueue);
	ExportQueue.Reset();
	return Stolen;
}
