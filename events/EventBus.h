// Copyright GALAXY_PROJECT. All Rights Reserved.
// =============================================================================
// EventBus.h
//
// Tick-synchronized event bus for deterministic, decoupled event dispatch.
// Events are queued during tick and flushed at deterministic boundaries.

#pragma once
//
// Architecture:
// +-------------------------------------------------------------------------+
// |                         EVENT FLOW                                      |
// +-------------------------------------------------------------------------+
// |  Emit -> Queue -> (Tick Boundary) -> Sort by Priority -> Dispatch          |
// |                                                                         |
// |  - Events queued during tick (order preserved within tick)              |
// |  - Flush at tick boundary for deterministic timing                      |
// |  - Handlers sorted by priority, then registration order                 |
// |  - Supports immediate dispatch for non-simulation events                |
// +-------------------------------------------------------------------------+
//
// Design Decisions:
// - Tick-synchronized: Default mode queues events, flushes at tick boundary
// - Immediate mode: Available for UI/non-simulation events
// - Priority-based: Handlers have priorities for ordering
// - Type-safe: Compile-time event type checking via templates
// - Deterministic: Same inputs produce same handler invocation order
//
#include "CoreMinimal.h"
// CoreMinimal pulls in UObject/WeakObjectPtrTemplates.h, which only FORWARD-declares
// FWeakObjectPtr -- the definition lives in CoreUObject. Instantiating TWeakObjectPtr
// therefore needs this header explicitly. It used to arrive free via the engine shared
// PCH; GalaxyCore lost that PCH when it took FPSemantics = Precise (UBT disables PCHs
// for a module overriding FP semantics without a private one), which is how a TU that
// was never self-sufficient started failing. Do not remove: the module's stated IWYU
// contract means this include is the TU's own responsibility, not the PCH's.
#include "UObject/WeakObjectPtr.h"
#include "Infrastructure/EventOriginTypes.h" // EEventOrigin, IsReplayableEventOrigin (extracted)
#include "Serialization/Archive.h"

DECLARE_LOG_CATEGORY_EXTERN(LogEventBus, Log, All);

// =============================================================================
// Forward Declarations
// =============================================================================

class FEventBusManager;

// =============================================================================
// Internal: Compile-time type identity
// =============================================================================

namespace EventBusInternal
{
	/**
	 * FNV-1a 64-bit hash evaluated at compile time. Used to derive stable type IDs
	 * from compiler-provided function signatures (__FUNCSIG__ / __PRETTY_FUNCTION__).
	 * Stable across process launches, Live Coding, and platforms that share a compiler.
	 */
	constexpr uint64 FnvHash64(const char* Str, uint64 Hash = 0xCBF29CE484222325ULL)
	{
		return (*Str == '\0')
			? Hash
			: FnvHash64(Str + 1, (Hash ^ static_cast<uint64>(static_cast<unsigned char>(*Str))) * 0x100000001B3ULL);
	}
}

#if defined(_MSC_VER)
	#define GALAXY_EVENTBUS_TYPENAME_STRING __FUNCSIG__
#else
	#define GALAXY_EVENTBUS_TYPENAME_STRING __PRETTY_FUNCTION__
#endif

// =============================================================================
// EEventPriority - Handler execution priority
// =============================================================================

/**
 * Priority levels for event handlers.
 * Lower numeric values execute first.
 */
enum class EEventPriority : int32
{
	/** System-critical handlers (validation, logging) */
	System = -1000,

	/** High priority (before most game logic) */
	High = -100,

	/** Normal priority (default) */
	Normal = 0,

	/** Low priority (reactions, cleanup) */
	Low = 100,

	/** Monitor-only handlers (telemetry, debugging) */
	Monitor = 1000
};

// EEventOrigin + IsReplayableEventOrigin moved to Infrastructure/EventOriginTypes.h
// (included above) so tape/deterministic-replay consumers can classify origins without
// pulling the full event-bus translation unit.

// =============================================================================
// EEventDispatchMode - When events are dispatched
// =============================================================================

/**
 * Controls when an event is dispatched to handlers.
 */
enum class EEventDispatchMode : uint8
{
	/** Queue event, dispatch at next flush (deterministic) */
	Queued,

	/** Dispatch immediately (non-deterministic, use for UI) */
	Immediate,

	/** Queue locally AND append a clone to the export buffer (for cross-shard routing) */
	QueuedAndExport
};

// =============================================================================
// FEventMetadata - Timing and source information
// =============================================================================

/**
 * Metadata attached to every event for debugging and replay.
 */
struct FEventMetadata
{
	/** Simulation tick when event was emitted */
	uint64 EmitTick = 0;

	/** Sequence number within the tick */
	uint32 TickSequence = 0;

	/** Source system/domain that emitted this event */
	FName SourceDomain;

	/** Optional source entity ID */
	uint64 SourceEntityId = 0;

	/** Originating shard (uint16 = FShardId). Zero means unset / non-shard origin. */
	uint16 SourceShardId = 0;

	/**
	 * Replay classification. Default is Internal (event is not replayed verbatim;
	 * it is expected to reproduce from the deterministic sim). Set to PlayerIntent,
	 * AIIntent, NetworkInbound, or External for events that must be injected during
	 * replay to reproduce correct sim outcomes.
	 *
	 * See EEventOrigin and IsReplayableEventOrigin() above.
	 */
	EEventOrigin Origin = EEventOrigin::Internal;

	FEventMetadata() = default;

	GALAXYCORE_API FEventMetadata(uint64 InTick, uint32 InSequence, FName InSource = NAME_None);

	GALAXYCORE_API FEventMetadata(uint64 InTick, uint32 InSequence, FName InSource, uint16 InSourceShardId);
};

// =============================================================================
// IEvent - Base event interface
// =============================================================================

// Forward declaration so IEvent::AsSerializable() can reference the type
// before ISerializableEvent is fully defined below.
class ISerializableEvent;

/**
 * Base interface for all events.
 * Events are immutable data objects describing something that happened.
 */
class IEvent
{
public:
	virtual ~IEvent() = default;

	/** Get unique type identifier for this event type. Stable across runs (FNV-1a of compile-time signature). */
	[[nodiscard]] virtual uint64 GetTypeId() const = 0;

	/** Get human-readable event name for logging */
	[[nodiscard]] virtual FString GetEventName() const = 0;

	/**
	 * Payload size in bytes. Used for byte-level telemetry (item 7).
	 * TEvent<Derived> supplies `sizeof(Derived)` by default; override for events that
	 * own heap-allocated payloads (e.g. large TArray members) when an accurate
	 * accounting is needed.
	 */
	[[nodiscard]] virtual size_t GetEventSizeBytes() const = 0;

	/** Get attached metadata */
	[[nodiscard]] const FEventMetadata& GetMetadata() const { return Metadata; }

	/** Set metadata (for event bus internal use) */
	void SetMetadata(const FEventMetadata& InMetadata) { Metadata = InMetadata; }

	/**
	 * Produce a heap-allocated copy of this event.
	 * Required by QueuedAndExport mode to place a copy in the export buffer.
	 */
	[[nodiscard]] virtual TUniquePtr<IEvent> Clone() const = 0;

	/**
	 * Return this event as ISerializableEvent* if it implements that interface,
	 * or nullptr if it does not.
	 * RTTI-free alternative to dynamic_cast (UE builds use /GR-).
	 * Override in ISerializableEvent to return 'this'.
	 */
	[[nodiscard]] virtual ISerializableEvent* AsSerializable() { return nullptr; }
	[[nodiscard]] virtual const ISerializableEvent* AsSerializable() const { return nullptr; }

protected:
	FEventMetadata Metadata;
};

// =============================================================================
// TEvent - Typed event base with CRTP
// =============================================================================

/**
 * Typed event base using CRTP for type ID generation.
 *
 * Example:
 *   struct FFleetMovedEvent : public TEvent<FFleetMovedEvent>
 *   {
 *       FFleetId FleetId;
 *       FVector OldPosition;
 *       FVector NewPosition;
 *
 *       FString GetEventName() const override { return TEXT("FleetMoved"); }
 *   };
 */
template<typename Derived>
class TEvent : public IEvent
{
public:
	[[nodiscard]] uint64 GetTypeId() const override final
	{
		return GetStaticTypeId();
	}

	/**
	 * Compile-time stable type identifier.
	 * Derived from FNV-1a hash of the compiler's full function signature, which
	 * uniquely encodes `Derived`. Deterministic across runs, hot reloads, and builds
	 * on the same compiler. Safe to persist in save data.
	 */
	[[nodiscard]] static constexpr uint64 GetStaticTypeId()
	{
		return EventBusInternal::FnvHash64(GALAXY_EVENTBUS_TYPENAME_STRING);
	}

	[[nodiscard]] TUniquePtr<IEvent> Clone() const override
	{
		return MakeUnique<Derived>(static_cast<const Derived&>(*this));
	}

	[[nodiscard]] size_t GetEventSizeBytes() const override
	{
		return sizeof(Derived);
	}

	/**
	 * RTTI-free downcast to ISerializableEvent. Returns 'this' when Derived
	 * derives ISerializableEvent; returns nullptr otherwise. Avoids dynamic_cast
	 * which is banned in UE builds (/GR-).
	 */
	[[nodiscard]] ISerializableEvent* AsSerializable() override final
	{
		if constexpr (std::is_base_of_v<ISerializableEvent, Derived>)
		{
			return static_cast<ISerializableEvent*>(static_cast<Derived*>(this));
		}
		return nullptr;
	}
	[[nodiscard]] const ISerializableEvent* AsSerializable() const override final
	{
		if constexpr (std::is_base_of_v<ISerializableEvent, Derived>)
		{
			return static_cast<const ISerializableEvent*>(static_cast<const Derived*>(this));
		}
		return nullptr;
	}
};

// =============================================================================
// FEventHandlerHandle - Subscription handle for unsubscribing
// =============================================================================

/**
 * Handle returned when subscribing to events.
 * Use to unsubscribe when no longer interested.
 */
struct FEventHandlerHandle
{
	uint64 TypeId = 0;
	uint64 HandlerId = 0;

	[[nodiscard]] bool IsValid() const { return HandlerId != 0; }

	bool operator==(const FEventHandlerHandle& Other) const
	{
		return TypeId == Other.TypeId && HandlerId == Other.HandlerId;
	}

	friend uint32 GetTypeHash(const FEventHandlerHandle& Handle)
	{
		return HashCombine(GetTypeHash(Handle.TypeId), GetTypeHash(Handle.HandlerId));
	}
};

// =============================================================================
// FDomainStats - Per-source-domain telemetry (D2)
// =============================================================================

/** Per-source-domain emission and dispatch statistics. Keyed by FEventMetadata::SourceDomain
 *  inside FEventBusTelemetry::DomainTelemetry. NAME_None-tagged emissions are excluded. */
struct FDomainStats
{
	/** Events emitted from this domain since Initialize(). */
	uint64 TotalEmitted = 0;

	/** Events dispatched (reached at least one handler) from this domain. */
	uint64 TotalDispatched = 0;

	/** Total handler invocations for events from this domain. */
	uint64 TotalHandlerCalls = 0;

	/** Total payload bytes emitted (sum of IEvent::GetEventSizeBytes()). */
	uint64 BytesEmitted = 0;
};

// =============================================================================
// FTypeStats - Per-event-type telemetry (item 5 + 7)
// =============================================================================

/** Per-event-type emission and dispatch statistics. Keyed by GetTypeId() inside
 *  FEventBusTelemetry::TypeTelemetry. Includes byte-level counters. */
struct FTypeStats
{
	/** Human-readable event name captured from the first emit of this type. */
	FString EventName;

	/** Events emitted of this type since Initialize(). */
	uint64 TotalEmitted = 0;

	/** Events of this type dispatched to at least one handler. */
	uint64 TotalDispatched = 0;

	/** Total handler invocations for events of this type. */
	uint64 TotalHandlerCalls = 0;

	/** Total payload bytes emitted for events of this type. */
	uint64 BytesEmitted = 0;
};

// =============================================================================
// FEventBusTelemetry - Statistics for monitoring
// =============================================================================

/**
 * Telemetry data for event bus monitoring.
 */
struct FEventBusTelemetry
{
	/** Total events emitted since start */
	uint64 TotalEmitted = 0;

	/** Total events dispatched to handlers */
	uint64 TotalDispatched = 0;

	/** Total handler invocations */
	uint64 TotalHandlerCalls = 0;

	/** Events currently queued */
	int32 QueuedCount = 0;

	/** Active subscriptions */
	int32 SubscriptionCount = 0;

	/** Supertick delta between the two most recent flushes (deterministic; NOT wall-clock time) */
	uint64 SuperTicksBetweenFlushes = 0;

	/** Last flush supertick counter (deterministic) */
	uint64 LastFlushSuperTick = 0;

	/** Whether LastFlushSuperTick has been initialized */
	bool bHasLastFlushSuperTick = false;

	/** Peak queue depth */
	int32 PeakQueueDepth = 0;

	/** Number of emissions that occurred while the bus was inside Flush().
	 *  These land in the next flush (1-tick latency) per the strict single-phase policy. */
	uint64 ReentrantEmissions = 0;

	/** Events skipped because no handlers were registered for their type (C1 early-out). */
	uint64 DroppedNoSubscribers = 0;

	/** Events dropped due to ring buffer overflow (C2). */
	uint64 DroppedOverflow = 0;

	/** Events dropped because the coalesce map was full and the key was new. */
	uint64 DroppedCoalesceOverflow = 0;

	/** Total payload bytes emitted across all events (item 7). */
	uint64 TotalBytesEmitted = 0;

	/** Per-source-domain breakdown (D2). Keyed by FEventMetadata::SourceDomain.
	 *  Aggregate counters above remain valid as the sum across all domains.
	 *  NAME_None-tagged emissions are not tracked here. */
	TMap<FName, FDomainStats> DomainTelemetry;

	/** Per-event-type breakdown (item 5). Keyed by IEvent::GetTypeId().
	 *  Aggregate counters above remain valid as the sum across all types. */
	TMap<uint64, FTypeStats> TypeTelemetry;

	GALAXYCORE_API void Reset();
};

// =============================================================================
// EQueueOverflowPolicy - Ring buffer overflow behavior
// =============================================================================

/**
 * Policy applied when the event ring buffer is full and a new event arrives.
 */
enum class EQueueOverflowPolicy : uint8
{
	/** Discard the oldest event in the queue to make room (default). */
	DropOldest,

	/** Discard the incoming event; existing queue is unchanged. */
	DropNewest,

	/** Crash in development builds. Use to detect unexpected saturation. */
	AssertFail
};

// =============================================================================
// FEventBusConfig - Configuration options
// =============================================================================

/**
 * Configuration for event bus behavior.
 */
struct FEventBusConfig
{
	/** Maximum events per flush (0 = unlimited) */
	int32 MaxEventsPerFlush = 0;

	/** Ring buffer capacity. When > 0 the ring is pre-allocated to this size.
	 *  When 0 the default capacity (65536) is used. Replaces the old TArray queue. */
	int32 MaxQueueDepth = 50000;

	/** Maximum number of distinct coalesce slots (unique {TypeId,CoalesceKey} pairs).
	 *  When the map is at capacity and a new key arrives, the emit is dropped and
	 *  DroppedCoalesceOverflow increments. Re-emits of existing keys always succeed
	 *  (last-write-wins). 0 disables the cap (legacy behavior). */
	int32 MaxCoalesceSlots = 50000;

	/** What to do when the ring buffer is full and a new event arrives (C2). */
	EQueueOverflowPolicy OverflowPolicy = EQueueOverflowPolicy::DropOldest;

	/** Whether to log event dispatch (verbose) */
	bool bLogDispatch = false;

	/** Enable detailed telemetry */
	bool bEnableTelemetry = true;

	/**
	 * Emissions during Flush() carry a 1-tick latency. Set false to log a warning
	 * whenever that happens (useful for catching accidental cascades during bring-up).
	 * When true (default) re-entrant emissions are silently counted into telemetry.
	 */
	bool bAllowReentrantEmission = true;
};

// =============================================================================
// IEventRecorder - Pluggable event trace recorder (D1)
// =============================================================================

/**
 * Interface for event bus recording. Attach via FEventBusManager::SetEventRecorder().
 *
 * All methods may be called from different threads (one per shard bus) --
 * implementations must be internally thread-safe.
 *
 * Concrete implementation: FJsonEventRecorder (EventBus.cpp).
 * Activated via: g.eventbus.Record 1   Deactivated via: g.eventbus.Record 0
 */
class IEventRecorder
{
public:
	virtual ~IEventRecorder() = default;

	/** Called after metadata is stamped on the event, before it is queued or dispatched. */
	virtual void RecordEmit(const IEvent& Event, const FEventMetadata& Meta) = 0;

	/** Called after each handler invocation. HandlerId matches FEventHandlerHandle::HandlerId. */
	virtual void RecordDispatch(const IEvent& Event, uint64 HandlerId) = 0;
};

// =============================================================================
// ISerializableEvent - Optional payload serialization for tape recorder
// =============================================================================

/**
 * Optional mixin for simulation events that need to be recorded and replayed.
 * Only events dispatched with Queued* modes AND marked with a non-Internal
 * EEventOrigin in their metadata need to implement this interface.
 *
 * UI-only events (dispatched via EmitImmediate / Internal origin) do NOT need
 * to implement ISerializableEvent.
 *
 * Implementation pattern:
 *
 *   struct FMyCardPlayedEvent : public TEvent<FMyCardPlayedEvent>
 *                             , public ISerializableEvent
 *   {
 *       FCardId CardId = {};
 *       FEntityId TargetEntityId = {};
 *
 *       FString GetEventName() const override { return TEXT("MyCardPlayed"); }
 *
 *       // ISerializableEvent
 *       static FName StaticTypeTag()      { return TEXT("MyCardPlayed"); }
 *       FName         GetTypeTag()  const override { return StaticTypeTag(); }
 *
 *       void Serialize(FArchive& Ar) override
 *       {
 *           Ar << CardId.Value;
 *           Ar << TargetEntityId.Value;
 *       }
 *   };
 *
 *   // Register once, typically in your module startup:
 *   FEventTypeRegistry::Get().Register<FMyCardPlayedEvent>();
 *
 * TypeTag must be a stable ASCII FName. The GetTypeId() from TEvent<Derived>
 * is the fast runtime key; the TypeTag is the human-readable persistent key
 * stored in tape files and used for deserialization factory lookup.
 */
class ISerializableEvent
{
public:
	virtual ~ISerializableEvent() = default;

	/** Stable human-readable name stored in the tape file. Must be unique across
	 *  all registered event types. ASCII only; survives binary-format upgrades. */
	[[nodiscard]] virtual FName GetTypeTag() const = 0;

	/**
	 * Serialize or deserialize the event payload.
	 * Called with a writer archive when recording; called with a reader archive
	 * when deserializing during replay. Must be symmetric. Do not serialize
	 * FEventMetadata -- the tape recorder stores it separately.
	 */
	virtual void Serialize(FArchive& Ar) = 0;
};

// =============================================================================
// FEventTypeRegistry - Central factory for serializable event types
// =============================================================================

/**
 * Process-global registry mapping TypeTag FNames to factory functions.
 * Used by FTapeEventRecorder to look up a type tag at record time, and by
 * FReplayEventInjector to reconstruct events from payload bytes.
 *
 * Registration is permanent for the lifetime of the process. Registering
 * the same TypeTag twice is an error (asserted in Debug/Development builds).
 *
 * Thread safety: Register() should only be called during module startup
 * (single-threaded). Lookup via FindFactory() is read-only and safe from
 * any thread after startup.
 */
class FEventTypeRegistry
{
public:
	using FFactoryFn = TFunction<TUniquePtr<IEvent>()>;

	/** Get the singleton. */
	[[nodiscard]] GALAXYCORE_API static FEventTypeRegistry& Get();

	/**
	 * Register an event type. Derived must inherit both TEvent<Derived> and
	 * ISerializableEvent. The TypeTag comes from Derived::StaticTypeTag().
	 * Asserts if the tag is already taken (duplicate registration).
	 */
	template<typename Derived>
	void Register()
	{
		static_assert(std::is_base_of_v<ISerializableEvent, Derived>,
			"FEventTypeRegistry::Register<T>: T must derive ISerializableEvent");
		const FName Tag = Derived::StaticTypeTag();
		ensureMsgf(!Factories.Contains(Tag),
			TEXT("[EventTypeRegistry] Duplicate TypeTag '%s' -- check for double registration"),
			*Tag.ToString());
		Factories.Add(Tag, []() -> TUniquePtr<IEvent>
		{
			return MakeUnique<Derived>();
		});
		TypeIdToTag.Add(Derived::GetStaticTypeId(), Tag);
	}

	/** Look up the factory for a TypeTag. Returns nullptr if not registered. */
	[[nodiscard]] const FFactoryFn* FindFactory(FName Tag) const
	{
		return Factories.Find(Tag);
	}

	/** Look up the TypeTag for a runtime type ID. Returns NAME_None if not registered.
	 *  Used by FTapeEventRecorder to write a human-readable tag into the tape. */
	[[nodiscard]] FName FindTag(uint64 TypeId) const
	{
		const FName* Tag = TypeIdToTag.Find(TypeId);
		return Tag ? *Tag : NAME_None;
	}

	/** Returns true if the given runtime type ID has a registered serializable entry. */
	[[nodiscard]] bool IsRegistered(uint64 TypeId) const
	{
		return TypeIdToTag.Contains(TypeId);
	}

private:
	FEventTypeRegistry() = default;

	/** Tag -> factory function. */
	TMap<FName, FFactoryFn> Factories;

	/** TypeId (from TEvent<Derived>::GetStaticTypeId()) -> TypeTag FName. */
	TMap<uint64, FName> TypeIdToTag;
};

// =============================================================================
// Internal: Handler storage types
// =============================================================================

namespace EventBusInternal
{
	/**
	 * Type-erased handler wrapper.
	 */
	struct FHandlerEntry
	{
		uint64 HandlerId = 0;
		EEventPriority Priority = EEventPriority::Normal;
		uint64 RegistrationOrder = 0;
		TFunction<void(const IEvent&)> Handler;

		/** Tombstone flag. False means the entry was unsubscribed during dispatch and
		 *  is awaiting compaction. Skipped by DispatchEvent iteration. */
		bool bAlive = true;

		/** Optional type-erased event filter. If set, handler is only invoked when
		 *  this predicate returns true. Null (default) means always invoke. */
		TFunction<bool(const IEvent&)> Filter;

		/** Optional UObject lifetime guard (SubscribeWeak). If the pointer becomes
		 *  stale, the entry is tombstoned during the next dispatch. */
		TWeakObjectPtr<UObject> WeakOwner;
		bool bHasWeakOwner = false;

		[[nodiscard]] GALAXYCORE_API bool operator<(const FHandlerEntry& Other) const;
	};

	/**
	 * Queued event wrapper.
	 */
	struct FQueuedEvent
	{
		TUniquePtr<IEvent> Event;
		EEventDispatchMode Mode = EEventDispatchMode::Queued;

		FQueuedEvent() = default;
		FQueuedEvent(TUniquePtr<IEvent>&& InEvent, EEventDispatchMode InMode)
			: Event(MoveTemp(InEvent))
			, Mode(InMode)
		{
		}

		FQueuedEvent(FQueuedEvent&&) = default;
		FQueuedEvent& operator=(FQueuedEvent&&) = default;
	};

	/**
	 * Per-type handler list with dirty sorting flag.
	 */
	struct FHandlerList
	{
		TArray<FHandlerEntry> Handlers;
		bool bNeedsSorting = false;

		/** Number of dispatches currently in progress for this list. Tombstones may only
		 *  be compacted out when depth returns to 0 (safe re-entrant dispatch). */
		int32 DispatchDepth = 0;

		/** Tombstone count; cheap early-out for compaction. */
		int32 DeadCount = 0;

		/** Entries subscribed while a dispatch was active on this list. Appending to
		 *  Handlers mid-dispatch could reallocate the array while one of its TFunctions
		 *  is still executing; deferred entries join Handlers when depth returns to 0. */
		TArray<FHandlerEntry> PendingAdds;

		GALAXYCORE_API void EnsureSorted();

		GALAXYCORE_API void CompactDeadIfSafe();

		GALAXYCORE_API void DrainPendingIfSafe();
	};
}

// =============================================================================
// FOnEventBusFlush - Pre/post flush hook delegate (D3)
// =============================================================================

// FEventBusManager is forward-declared above; FEventBusManager& as a reference
// parameter requires only a forward declaration at the point of this macro.
DECLARE_MULTICAST_DELEGATE_OneParam(FOnEventBusFlush, FEventBusManager&);

// =============================================================================
// FEventBusManager - Centralized event dispatch
// =============================================================================

/**
 * Centralized event bus with tick-synchronized dispatch.
 *
 * Usage:
 *   FEventBusManager& Bus = GetEventBus();
 *
 *   // Subscribe to events
 *   auto Handle = Bus.Subscribe<FFleetMovedEvent>([](const FFleetMovedEvent& Event) {
 *       // Handle event
 *   });
 *
 *   // Emit events (queued by default)
 *   Bus.Emit(MakeUnique<FFleetMovedEvent>(FleetId, OldPos, NewPos));
 *
 *   // At tick boundary
 *   Bus.Flush();
 *
 *   // Cleanup
 *   Bus.Unsubscribe(Handle);
 *
 * Thread Safety: Single-writer/single-reader per bus instance.
 * Each PlanetBus instance is exclusively accessed by its shard's worker thread
 * (emit during Phase_LocalSim, flush during Phase_BuildSnapshot). Never share
 * one FEventBusManager instance across threads.
 */
class FEventBusManager
{
public:
	FEventBusManager() = default;
	~FEventBusManager() = default;

	// Non-copyable, non-movable
	FEventBusManager(const FEventBusManager&) = delete;
	FEventBusManager& operator=(const FEventBusManager&) = delete;

	// =========================================================================
	// Flush hooks (D3)
	// =========================================================================

	/** Fired at the very start of Flush(), before ring drain or dispatch.
	 *  Do NOT emit events from a pre-flush hook -- they will be processed in the
	 *  current flush (ring is drained AFTER this fires, so emits land in LocalQueue). */
	FOnEventBusFlush OnPreFlush;

	/** Fired at the end of Flush(), after all events have been dispatched and
	 *  telemetry has been updated. Re-entrant emits from post-flush hooks land
	 *  in the next flush (standard 1-tick latency). */
	FOnEventBusFlush OnPostFlush;

	/**
	 * Initialize the event bus with configuration.
	 */
	// Default ring buffer capacity when MaxQueueDepth is set to 0 (unlimited).
	static constexpr int32 DefaultRingCapacity = 65536;

	GALAXYCORE_API void Initialize(const FEventBusConfig& InConfig); // impl: EventBus.cpp

	/**
	 * Set the current simulation tick.
	 */
	GALAXYCORE_API void SetCurrentTick(uint64 Tick);

	/**
	 * Get the current simulation tick.
	 */
	[[nodiscard]] uint64 GetCurrentTick() const { return CurrentTick; }

	/**
	 * Tag all events emitted by this bus with the given shard ID.
	 * Called once at shard construction. Stamped into FEventMetadata::SourceShardId.
	 */
	void SetSourceShard(uint16 InShardId) { SourceShardId = InShardId; }

	/**
	 * Drain and return all events queued for cross-shard export.
	 * Called by UGlobalShardScheduler on the game thread after Phase_BuildSnapshots_All().
	 * Thread-safe: only call when the owning shard thread has already finished its phase.
	 */
	[[nodiscard]] GALAXYCORE_API TArray<TUniquePtr<IEvent>> StealExportedEvents();

	// =========================================================================
	// Subscription
	// =========================================================================

	/**
	 * Subscribe to an event type.
	 * @param Handler Function to call when event is dispatched
	 * @param Priority Handler execution priority
	 * @return Handle for unsubscribing
	 */
	template<typename EventType>
	FEventHandlerHandle Subscribe(
		TFunction<void(const EventType&)> Handler,
		EEventPriority Priority = EEventPriority::Normal)
	{
		static_assert(TIsDerivedFrom<EventType, IEvent>::Value, 
			"EventType must derive from IEvent");

		const uint64 TypeId = EventType::GetStaticTypeId();
		const uint64 HandlerId = NextHandlerId++;

		EventBusInternal::FHandlerEntry Entry;
		Entry.HandlerId = HandlerId;
		Entry.Priority = Priority;
		Entry.RegistrationOrder = NextRegistrationOrder++;
		Entry.Handler = [UserHandler = MoveTemp(Handler)](const IEvent& Event)
		{
			UserHandler(static_cast<const EventType&>(Event));
		};

		AddHandlerEntry(TypeId, MoveTemp(Entry));

		FEventHandlerHandle Handle;
		Handle.TypeId = TypeId;
		Handle.HandlerId = HandlerId;
		return Handle;
	}

	/**
	 * Subscribe to an event type with an optional filter predicate.
	 * Handler is only invoked when Filter returns true.
	 */
	template<typename EventType>
	FEventHandlerHandle Subscribe(
		TFunction<void(const EventType&)> Handler,
		EEventPriority Priority,
		TFunction<bool(const EventType&)> Filter)
	{
		static_assert(TIsDerivedFrom<EventType, IEvent>::Value,
			"EventType must derive from IEvent");

		const uint64 TypeId = EventType::GetStaticTypeId();
		const uint64 HandlerId = NextHandlerId++;

		EventBusInternal::FHandlerEntry Entry;
		Entry.HandlerId = HandlerId;
		Entry.Priority = Priority;
		Entry.RegistrationOrder = NextRegistrationOrder++;
		Entry.Handler = [UserHandler = MoveTemp(Handler)](const IEvent& Event)
		{
			UserHandler(static_cast<const EventType&>(Event));
		};
		if (Filter)
		{
			Entry.Filter = [TypedFilter = MoveTemp(Filter)](const IEvent& Event) -> bool
			{
				return TypedFilter(static_cast<const EventType&>(Event));
			};
		}

		AddHandlerEntry(TypeId, MoveTemp(Entry));

		FEventHandlerHandle Handle;
		Handle.TypeId = TypeId;
		Handle.HandlerId = HandlerId;
		return Handle;
	}

	/**
	 * Subscribe to an event type with a UObject lifetime guard.
	 * If Owner becomes garbage-collected, the entry is tombstoned at next dispatch.
	 */
	template<typename EventType>
	FEventHandlerHandle SubscribeWeak(
		UObject* Owner,
		TFunction<void(const EventType&)> Handler,
		EEventPriority Priority = EEventPriority::Normal)
	{
		static_assert(TIsDerivedFrom<EventType, IEvent>::Value,
			"EventType must derive from IEvent");

		const uint64 TypeId = EventType::GetStaticTypeId();
		const uint64 HandlerId = NextHandlerId++;

		EventBusInternal::FHandlerEntry Entry;
		Entry.HandlerId = HandlerId;
		Entry.Priority = Priority;
		Entry.RegistrationOrder = NextRegistrationOrder++;
		Entry.Handler = [UserHandler = MoveTemp(Handler)](const IEvent& Event)
		{
			UserHandler(static_cast<const EventType&>(Event));
		};
		Entry.WeakOwner = Owner;
		Entry.bHasWeakOwner = true;

		AddHandlerEntry(TypeId, MoveTemp(Entry));

		FEventHandlerHandle Handle;
		Handle.TypeId = TypeId;
		Handle.HandlerId = HandlerId;
		return Handle;
	}

	/**
	 * Subscribe to an event type using a lambda.
	 * Convenience overload that deduces the event type.
	 */
	template<typename Func>
	FEventHandlerHandle SubscribeLambda(
		Func&& Handler,
		EEventPriority Priority = EEventPriority::Normal)
	{
		return SubscribeLambdaImpl(Forward<Func>(Handler), Priority,
			&std::decay_t<Func>::operator());
	}

	/**
	 * Unsubscribe using a handle.
	 * @return true if handler was found and removed
	 */
	GALAXYCORE_API bool Unsubscribe(const FEventHandlerHandle& Handle); // impl: EventBus.cpp

	/**
	 * Unsubscribe every handler that was registered via SubscribeWeak(Owner, ...)
	 * for the given UObject. Handles the full subsystem-shutdown pattern without
	 * requiring callers to store every returned handle.
	 *
	 * Entries inside an active dispatch are tombstoned and compacted when the
	 * dispatch depth returns to 0; entries outside dispatch are removed immediately.
	 *
	 * @param Owner  Subsystem/actor whose handlers should be removed.
	 * @return Number of handler entries removed or tombstoned.
	 */
	GALAXYCORE_API int32 UnsubscribeByOwner(const UObject* Owner); // impl: EventBus.cpp

	// =========================================================================
	// Emission
	// =========================================================================

	/**
	 * Emit an event.
	 * @param Event Event to emit (ownership transferred)
	 * @param Mode Dispatch mode (Queued by default)
	 * @param SourceDomain Optional source domain for metadata
	 */
	GALAXYCORE_API void Emit(
		TUniquePtr<IEvent> Event,
		EEventDispatchMode Mode = EEventDispatchMode::Queued,
		FName SourceDomain = NAME_None); // impl: EventBus.cpp

	/**
	 * Convenience method to create and emit an event.
	 */
	template<typename EventType, typename... Args>
	void EmitNew(
		EEventDispatchMode Mode = EEventDispatchMode::Queued,
		FName SourceDomain = NAME_None,
		Args&&... InArgs)
	{
		Emit(MakeUnique<EventType>(Forward<Args>(InArgs)...), Mode, SourceDomain);
	}

	/**
	 * Emit an event with coalescing (C3). Multiple calls with the same CoalesceKey
	 * for the same EventType result in exactly one dispatch at the next Flush(),
	 * using the payload from the LATEST EmitCoalesce call (last-write wins).
	 *
	 * Coalesced events are dispatched before the regular ring queue in Flush().
	 * Sorted by (TypeId, CoalesceKey) for deterministic dispatch order.
	 *
	 * Use for high-frequency state-update events where only the latest value matters
	 * (e.g. FResourceTotalUpdated, FEntityPositionChanged).
	 *
	 * @param CoalesceKey   Caller-supplied dedup key (unique per logical entity/slot).
	 * @param SourceDomain  Optional source domain tag for metadata.
	 * @param InArgs        Constructor arguments forwarded to EventType.
	 */
	template<typename EventType, typename... Args>
	void EmitCoalesce(
		uint64 CoalesceKey,
		FName SourceDomain = NAME_None,
		Args&&... InArgs)
	{
		static_assert(TIsDerivedFrom<EventType, IEvent>::Value,
			"EventType must derive from IEvent");
		const uint64 TypeId = EventType::GetStaticTypeId();
		if (!HandlersByType.Contains(TypeId))
		{
			++Telemetry.DroppedNoSubscribers;
			return;
		}
		EmitCoalesceInternal(TypeId, MakeUnique<EventType>(Forward<Args>(InArgs)...), CoalesceKey, SourceDomain);
	}

	/**
	 * Emit an event immediately, bypassing the queue.
	 * Use sparingly - this is non-deterministic for simulation.
	 */
	void EmitImmediate(TUniquePtr<IEvent> Event, FName SourceDomain = NAME_None)
	{
		Emit(MoveTemp(Event), EEventDispatchMode::Immediate, SourceDomain);
	}

	// =========================================================================
	// Flush
	// =========================================================================

	/**
	 * Flush the event queue, dispatching all pending events.
	 * Typically called once per simulation tick.
	 * @return Number of events dispatched
	 */
	GALAXYCORE_API int32 Flush(); // impl: EventBus.cpp

	/**
	 * Clear all queued events without dispatching.
	 * @return Number of events dropped
	 */
	GALAXYCORE_API int32 ClearQueue(); // impl: EventBus.cpp

	// =========================================================================
	// Query
	// =========================================================================

	/**
	 * Get number of events currently in the ring buffer queue.
	 */
	[[nodiscard]] int32 GetQueuedCount() const { return RingCount; }

	/**
	 * Check if the ring buffer queue is empty.
	 */
	[[nodiscard]] bool IsQueueEmpty() const { return RingCount == 0; }

	/**
	 * Get telemetry data.
	 */
	[[nodiscard]] const FEventBusTelemetry& GetTelemetry() const { return Telemetry; }

	/**
	 * Get configuration.
	 */
	[[nodiscard]] const FEventBusConfig& GetConfig() const { return Config; }

	// =========================================================================
	// Recorder + domain telemetry (D1, D2)
	// =========================================================================

	/**
	 * Attach or detach an event recorder (D1).
	 * Pass nullptr to disable recording. The recorder is NOT owned by the bus.
	 * Thread-safety: call from the owning thread only. For shard buses on worker
	 * threads, stop recording before the next tick phase begins.
	 */
	void SetEventRecorder(IEventRecorder* InRecorder) { Recorder = InRecorder; }

	/** Current recorder, or nullptr if none is attached. */
	[[nodiscard]] IEventRecorder* GetEventRecorder() const { return Recorder; }

	/**
	 * Per-source-domain telemetry breakdown (D2).
	 * Keyed by the SourceDomain FName passed to Emit(). NAME_None entries are omitted.
	 */
	[[nodiscard]] const TMap<FName, FDomainStats>& GetDomainTelemetry() const
	{
		return Telemetry.DomainTelemetry;
	}

	/**
	 * Per-event-type telemetry breakdown (item 5).
	 * Keyed by IEvent::GetTypeId(). Includes byte-level counters (item 7).
	 */
	[[nodiscard]] const TMap<uint64, FTypeStats>& GetTypeTelemetry() const
	{
		return Telemetry.TypeTelemetry;
	}

	/**
	 * Register this bus in the global console-command registry.
	 * Enables g.eventbus.Stats and g.eventbus.Record to find this bus.
	 * Call from the game thread during initialization. BusName must be unique.
	 */
	GALAXYCORE_API static void RegisterForStats(FName BusName, FEventBusManager* Bus);

	/** Unregister from the global console-command registry. Game thread only. */
	GALAXYCORE_API static void UnregisterForStats(FName BusName);

	/** Enable or disable the shared debug recorder across all registered buses. */
	GALAXYCORE_API static void SetRecordingEnabled(bool bEnable, FOutputDevice& Out);

	/** Dump aggregate and per-domain telemetry for all registered buses. */
	GALAXYCORE_API static void DumpRegisteredBusStats(FOutputDevice& Out);

	/**
	 * Get number of handlers for a specific event type.
	 */
	template<typename EventType>
	[[nodiscard]] int32 GetHandlerCount() const
	{
		const uint64 TypeId = EventType::GetStaticTypeId();
		const EventBusInternal::FHandlerList* List = HandlersByType.Find(TypeId);
		// Tombstones awaiting compaction are not live; deferred mid-dispatch adds are
		return List ? (List->Handlers.Num() - List->DeadCount + List->PendingAdds.Num()) : 0;
	}

	/**
	 * Serialize replay-relevant bus state (current tick + tick sequence counter).
	 * Queue state is NOT serialized -- callers must Flush() before snapshotting so
	 * both the ring and the coalesce map are empty (ensure-guarded on save).
	 * Handlers (subscriptions) are not persisted because they are always rebuilt
	 * by live subsystems on load.
	 */
	GALAXYCORE_API void Serialize(FArchive& Ar); // impl: EventBus.cpp

private:
	GALAXYCORE_API void EmitCoalesceInternal(uint64 TypeId, TUniquePtr<IEvent> NewEvent, uint64 CoalesceKey, FName SourceDomain); // impl: EventBus.cpp
	GALAXYCORE_API void QueueEvent(TUniquePtr<IEvent>&& Event); // impl: EventBus.cpp
	GALAXYCORE_API void DispatchEvent(const IEvent& Event); // impl: EventBus.cpp
	GALAXYCORE_API void AddHandlerEntry(uint64 TypeId, EventBusInternal::FHandlerEntry&& Entry); // impl: EventBus.cpp

	// Helper for lambda subscription type deduction
	template<typename Func, typename Ret, typename Class, typename EventType>
	FEventHandlerHandle SubscribeLambdaImpl(
		Func&& Handler,
		EEventPriority Priority,
		Ret(Class::*)(const EventType&) const)
	{
		return Subscribe<EventType>(Forward<Func>(Handler), Priority);
	}

	template<typename Func, typename Ret, typename Class, typename EventType>
	FEventHandlerHandle SubscribeLambdaImpl(
		Func&& Handler,
		EEventPriority Priority,
		Ret(Class::*)(const EventType&))
	{
		return Subscribe<EventType>(Forward<Func>(Handler), Priority);
	}

private:
	FEventBusConfig Config;
	FEventBusTelemetry Telemetry;

	// ---- Ring buffer queue (C2) ----
	// Fixed-capacity circular buffer. RingHead points to the oldest element.
	// RingCount tracks how many elements are live. Writes go to (RingHead+RingCount)%Cap.
	TArray<EventBusInternal::FQueuedEvent> Ring;
	int32 RingHead  = 0;
	int32 RingCount = 0;

	/** Reusable drain buffer for Flush(); persists so steady-state flushes are allocation-free. */
	TArray<EventBusInternal::FQueuedEvent> FlushScratch;

	// ---- Coalesce map (C3) ----
	// Maps {TypeId, CoalesceKey} -> latest event. Drained deterministically in Flush()
	// before the regular ring queue. TMap iteration is sorted before dispatch.
	TMap<TPair<uint64, uint64>, TUniquePtr<IEvent>> CoalesceSlots;

	TMap<uint64, EventBusInternal::FHandlerList> HandlersByType;

	uint64 NextHandlerId = 1;
	uint64 NextRegistrationOrder = 1;
	uint64 CurrentTick = 0;
	uint32 TickSequence = 0;

	/** Shard ID stamped on every FEventMetadata emitted from this bus (0 = unset). */
	uint16 SourceShardId = 0;

	/** Events queued for cross-shard export (QueuedAndExport mode). Drained by
	 *  UGlobalShardScheduler on the game thread after Phase_BuildSnapshots_All(). */
	TArray<TUniquePtr<IEvent>> ExportQueue;

	/** True while Flush() is actively dispatching. Used to classify re-entrant emits. */
	bool bInFlush = false;

	/** Pluggable event recorder (D1). Null = no recording (zero-cost branch). Not owned by bus. */
	IEventRecorder* Recorder = nullptr;

	/** Global console-command registry used by g.eventbus.* helpers. */
	GALAXYCORE_API static TMap<FName, FEventBusManager*> RegisteredEventBuses;

	/** Shared lifetime for the active debug recorder while recording is enabled. */
	GALAXYCORE_API static TSharedPtr<IEventRecorder> ActiveEventRecorder;
};

// =============================================================================
// Common Event Base Classes
// =============================================================================

/**
 * Base for entity-related events.
 */
template<typename Derived>
struct TEntityEvent : public TEvent<Derived>
{
	uint64 EntityId = 0;

	TEntityEvent() = default;
	explicit TEntityEvent(uint64 InEntityId) : EntityId(InEntityId) {}
};

/**
 * Base for zone-related events.
 */
template<typename Derived>
struct TZoneEvent : public TEvent<Derived>
{
	int32 ZoneId = INDEX_NONE;

	TZoneEvent() = default;
	explicit TZoneEvent(int32 InZoneId) : ZoneId(InZoneId) {}
};

/**
 * Base for command completion events.
 */
template<typename Derived>
struct TCommandCompletedEvent : public TEvent<Derived>
{
	uint64 CommandSequence = 0;
	uint32 CommandTypeId = 0;
	bool bSuccess = true;

	TCommandCompletedEvent() = default;
	TCommandCompletedEvent(uint64 InSeq, uint32 InTypeId, bool InSuccess)
		: CommandSequence(InSeq)
		, CommandTypeId(InTypeId)
		, bSuccess(InSuccess)
	{
	}
};

#undef GALAXY_EVENTBUS_TYPENAME_STRING
