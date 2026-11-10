// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================

#include "Infrastructure/Determinism/ReplayEventInjector.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Infrastructure/EventBus.h"

DEFINE_LOG_CATEGORY(LogDeterminismReplay);

namespace GalaxyDeterminism
{
	// -------------------------------------------------------------------------
	// Construction / Loading
	// -------------------------------------------------------------------------

	FReplayEventInjector::FReplayEventInjector(const FString& InTapeFolder)
		: TapeFolder(InTapeFolder)
	{
		const FString FilePath = FPaths::Combine(TapeFolder, GalaxyDeterminism::EventsFileName());
		bLoaded = LoadFromFile(FilePath);

		if (bLoaded)
		{
			UE_LOG(LogDeterminismReplay, Log,
				TEXT("[DeterminismReplay][Injector] Loaded %d events across %d ticks from: %s"),
				TotalEventCount, RecordsByTick.Num(), *FilePath);
		}
	}

	bool FReplayEventInjector::LoadFromFile(const FString& FilePath)
	{
		TArray<uint8> FileBytes;
		if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath))
		{
			UE_LOG(LogDeterminismReplay, Error,
				TEXT("[DeterminismReplay][Injector] Failed to read events.bin: %s"), *FilePath);
			return false;
		}

		FMemoryReader Reader(FileBytes, /*bIsPersistent=*/true);

		// Validate magic + version header.
		uint32 Magic   = 0;
		uint32 Version = 0;
		Reader << Magic;
		Reader << Version;

		if (Reader.IsError())
		{
			UE_LOG(LogDeterminismReplay, Error,
				TEXT("[DeterminismReplay][Injector] events.bin too short to read header: %s"), *FilePath);
			return false;
		}

		if (Magic != GalaxyDeterminism::EventsFileMagic)
		{
			UE_LOG(LogDeterminismReplay, Error,
				TEXT("[DeterminismReplay][Injector] events.bin magic mismatch (got 0x%08X, expected 0x%08X): %s"),
				Magic, GalaxyDeterminism::EventsFileMagic, *FilePath);
			return false;
		}

		if (Version > GalaxyDeterminism::TapeFormatVersion)
		{
			UE_LOG(LogDeterminismReplay, Warning,
				TEXT("[DeterminismReplay][Injector] events.bin format version %u > supported %u -- "
				     "some records may not parse correctly."),
				Version, GalaxyDeterminism::TapeFormatVersion);
		}

		// Read all records. Layout per record:
		//   uint64   Tick
		//   uint32   TickSequence
		//   uint16   SourceShardId
		//   uint8    Origin
		//   uint8    Reserved
		//   uint64   TypeTag
		//   uint32   PayloadBytes
		//   [PayloadBytes] raw payload

		while (!Reader.AtEnd() && !Reader.IsError())
		{
			FTapeEventRecord Record;

			uint8 OriginByte = 0;
			uint8 Reserved   = 0;
			uint32 PayloadSize = 0;

			Reader.Serialize(&Record.Tick,          sizeof(Record.Tick));
			Reader.Serialize(&Record.TickSequence,  sizeof(Record.TickSequence));
			Reader.Serialize(&Record.SourceShardId, sizeof(Record.SourceShardId));
			Reader.Serialize(&OriginByte,            sizeof(OriginByte));
			Reader.Serialize(&Reserved,              sizeof(Reserved));
			Reader.Serialize(&Record.TypeTag,        sizeof(Record.TypeTag));
			Reader.Serialize(&PayloadSize,           sizeof(PayloadSize));

			if (Reader.IsError())
			{
				break;
			}

			Record.Origin = static_cast<EEventOrigin>(OriginByte);

			if (PayloadSize > 0)
			{
				Record.Payload.SetNumUninitialized(static_cast<int32>(PayloadSize));
				Reader.Serialize(Record.Payload.GetData(), static_cast<int64>(PayloadSize));
			}

			if (Reader.IsError())
			{
				UE_LOG(LogDeterminismReplay, Warning,
					TEXT("[DeterminismReplay][Injector] Truncated record at tick %llu -- stopping load."),
					Record.Tick);
				break;
			}

			// Track tick range.
			if (Record.Tick < FirstTick) { FirstTick = Record.Tick; }
			if (Record.Tick > LastTick)  { LastTick  = Record.Tick; }

			RecordsByTick.FindOrAdd(Record.Tick).Add(MoveTemp(Record));
			++TotalEventCount;
		}

		// Sort each tick bucket by (TickSequence, SourceShardId) to preserve
		// original emission order.
		for (TPair<uint64, TArray<FTapeEventRecord>>& Pair : RecordsByTick) // tmap-order-safe: sorts each tick's bucket on its own
		{
			Pair.Value.Sort([](const FTapeEventRecord& A, const FTapeEventRecord& B)
			{
				if (A.TickSequence != B.TickSequence) { return A.TickSequence < B.TickSequence; }
				return A.SourceShardId < B.SourceShardId;
			});
		}

		return TotalEventCount > 0 || FileBytes.Num() >= 8; // header-only tape is still valid
	}

	// -------------------------------------------------------------------------
	// Registration
	// -------------------------------------------------------------------------

	void FReplayEventInjector::RegisterShardBus(uint16 ShardId, FEventBusManager* Bus)
	{
		if (!ensureMsgf(Bus != nullptr,
			TEXT("[DeterminismReplay][Injector] RegisterShardBus: null bus for ShardId %u"), ShardId))
		{
			return;
		}
		ShardBuses.Add(ShardId, Bus);
	}

	// -------------------------------------------------------------------------
	// Per-tick injection
	// -------------------------------------------------------------------------

	int32 FReplayEventInjector::InjectForTick(uint64 Tick)
	{
		const TArray<FTapeEventRecord>* Records = RecordsByTick.Find(Tick);
		if (Records == nullptr)
		{
			return 0;
		}

		int32 Injected = 0;
		for (const FTapeEventRecord& Record : *Records)
		{
			if (ReplayOneRecord(Record))
			{
				++Injected;
			}
		}
		return Injected;
	}

	bool FReplayEventInjector::ReplayOneRecord(const FTapeEventRecord& Record)
	{
		// Step 1: TypeId -> FName tag.
		const FName TypeTag = FEventTypeRegistry::Get().FindTag(Record.TypeTag);
		if (TypeTag.IsNone())
		{
			UE_LOG(LogDeterminismReplay, Warning,
				TEXT("[DeterminismReplay][Injector] Unknown TypeTag 0x%llX at tick %llu -- "
				     "event dropped. Register the type before replay."),
				Record.TypeTag, Record.Tick);
			return false;
		}

		// Step 2: FName tag -> factory function.
		const FEventTypeRegistry::FFactoryFn* FactoryPtr = FEventTypeRegistry::Get().FindFactory(TypeTag);
		if (!ensureMsgf(FactoryPtr != nullptr,
			TEXT("[DeterminismReplay][Injector] FindTag succeeded for '%s' but FindFactory returned null -- "
			     "registry inconsistency."),
			*TypeTag.ToString()))
		{
			return false;
		}

		// Step 3: Create a fresh event instance.
		TUniquePtr<IEvent> EventPtr = (*FactoryPtr)();
		if (!ensureMsgf(EventPtr.IsValid(),
			TEXT("[DeterminismReplay][Injector] Factory for '%s' returned null."),
			*TypeTag.ToString()))
		{
			return false;
		}

		// Step 4: Deserialize payload into the new event.
		if (Record.Payload.Num() > 0)
		{
			// AsSerializable() is a RTTI-free virtual on IEvent (TEvent<Derived> overrides
			// it to return 'this' when Derived derives ISerializableEvent).
			ISerializableEvent* Serializable = EventPtr->AsSerializable();
			if (!ensureMsgf(Serializable != nullptr,
				TEXT("[DeterminismReplay][Injector] Factory for '%s' produced a non-serializable event. "
				     "Ensure the type derives ISerializableEvent before calling Register<T>()."),
				*TypeTag.ToString()))
			{
				return false;
			}

			FMemoryReader Reader(Record.Payload, /*bIsPersistent=*/true);
			Serializable->Serialize(Reader);

			if (!ensureMsgf(!Reader.IsError(),
				TEXT("[DeterminismReplay][Injector] Payload deserialization failed for '%s' at tick %llu."),
				*TypeTag.ToString(), Record.Tick))
			{
				return false;
			}
		}

		// Step 5: Find the target bus and emit.
		FEventBusManager** BusPtr = ShardBuses.Find(Record.SourceShardId);
		if (BusPtr == nullptr || *BusPtr == nullptr)
		{
			UE_LOG(LogDeterminismReplay, Warning,
				TEXT("[DeterminismReplay][Injector] No bus registered for ShardId %u -- "
				     "dropping event '%s' at tick %llu. Call RegisterShardBus() before InjectForTick()."),
				Record.SourceShardId, *TypeTag.ToString(), Record.Tick);
			return false;
		}

		(*BusPtr)->Emit(MoveTemp(EventPtr), EEventDispatchMode::Queued);
		return true;
	}
}
