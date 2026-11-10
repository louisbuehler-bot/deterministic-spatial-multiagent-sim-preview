// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================

#include "Infrastructure/Determinism/TapeEventRecorder.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryWriter.h"

DEFINE_LOG_CATEGORY(LogDeterminismTape);

namespace
{
	// FArchive::Serialize requires non-const void*. This helper lets callers
	// pass const struct fields without casting at every call site.
	FORCEINLINE void ArchiveWriteRecorder(FArchive* Ar, const void* Data, int64 Size)
	{
		Ar->Serialize(const_cast<void*>(Data), Size);
	}

	FORCEINLINE void ArchiveWriteRecorder(const TUniquePtr<FArchive>& Ar, const void* Data, int64 Size)
	{
		Ar->Serialize(const_cast<void*>(Data), Size);
	}
}

namespace GalaxyDeterminism
{
	// -------------------------------------------------------------------------
	// Construction / destruction
	// -------------------------------------------------------------------------

	FTapeEventRecorder::FTapeEventRecorder(const FString& InTapeFolder, uint64 InStartTick)
		: TapeFolder(InTapeFolder)
		, StartTick(InStartTick)
	{
		const FString FilePath = FPaths::Combine(TapeFolder, GalaxyDeterminism::EventsFileName());

		// Ensure the tape folder exists.
		IFileManager::Get().MakeDirectory(*TapeFolder, /*Tree=*/true);

		EventsFile.Reset(IFileManager::Get().CreateFileWriter(*FilePath, /*Flags=*/0));
		if (!ensureMsgf(EventsFile != nullptr,
			TEXT("[DeterminismTape][TapeEventRecorder] Failed to open events.bin for writing: %s"),
			*FilePath))
		{
			return;
		}

		// Write magic number so readers can detect format mismatches early.
		uint32 Magic = GalaxyDeterminism::EventsFileMagic;
		uint32 Version = GalaxyDeterminism::TapeFormatVersion;
		ArchiveWriteRecorder(EventsFile, &Magic,   sizeof(Magic));
		ArchiveWriteRecorder(EventsFile, &Version, sizeof(Version));

		UE_LOG(LogDeterminismTape, Log, TEXT("[DeterminismTape][TapeEventRecorder] Opened %s (startTick=%llu)"),
			*FilePath, StartTick);
	}

	FTapeEventRecorder::~FTapeEventRecorder()
	{
		Finalize();
	}

	void FTapeEventRecorder::Finalize()
	{
		if (bFinalized)
		{
			return;
		}
		bFinalized = true;

		// Drain any staging records that arrived after the last FlushTick().
		{
			FScopeLock Lock(&StagingLock);
			for (const FTapeEventRecord& Record : StagingBuffer)
			{
				WriteRecord(Record);
			}
			StagingBuffer.Reset();
		}

		if (EventsFile != nullptr)
		{
			EventsFile->Flush();
			EventsFile.Reset();
		}

		UE_LOG(LogDeterminismTape, Log,
			TEXT("[DeterminismTape][TapeEventRecorder] Finalized. Total events recorded: %llu"),
			EventRecordCount);
	}

	// -------------------------------------------------------------------------
	// IEventRecorder
	// -------------------------------------------------------------------------

	void FTapeEventRecorder::RecordEmit(const IEvent& Event, const FEventMetadata& Meta)
	{
		if (bFinalized)
		{
			return;
		}

		// Only record events that need to be replayed verbatim.
		if (!IsReplayableEventOrigin(Meta.Origin))
		{
			return;
		}

		// Only record events that have a registered serialization entry.
		const FName TypeTag = FEventTypeRegistry::Get().FindTag(Event.GetTypeId());
		if (TypeTag.IsNone())
		{
			FScopeLock WLock(&WarnLock);
			if (!WarnedUnregisteredTypes.Contains(Event.GetTypeId()))
			{
				WarnedUnregisteredTypes.Add(Event.GetTypeId());
				UE_LOG(LogDeterminismTape, Warning,
					TEXT("[DeterminismTape][TapeEventRecorder] External-origin event '%s' (typeId=0x%llX) "
					     "has no FEventTypeRegistry entry -- dropped from tape. "
					     "Add Register<T>() in module startup to include it."),
					*Event.GetEventName(), Event.GetTypeId());
			}
			return;
		}

		// The event must also implement ISerializableEvent.
		// AsSerializable() is a RTTI-free virtual on IEvent (TEvent<Derived> overrides
		// it to return 'this' when Derived derives ISerializableEvent).
		const ISerializableEvent* SerializablePtr = Event.AsSerializable();
		if (!ensureMsgf(SerializablePtr != nullptr,
			TEXT("[DeterminismTape][TapeEventRecorder] Event '%s' is registered but does not "
			     "implement ISerializableEvent -- dropped from tape."),
			*Event.GetEventName()))
		{
			return;
		}

		TArray<uint8> PayloadBytes;
		// const_cast is intentional: FArchive serialize is logically read-only when
		// the archive is in write mode. The event's data is not modified.
		ISerializableEvent* MutableSerializable = const_cast<ISerializableEvent*>(SerializablePtr);
		if (!SerializePayload(*MutableSerializable, PayloadBytes))
		{
			return;
		}

		FTapeEventRecord Record;
		Record.Tick          = Meta.EmitTick;
		Record.TickSequence  = Meta.TickSequence;
		Record.SourceShardId = Meta.SourceShardId;
		Record.Origin        = Meta.Origin;
		Record.TypeTag       = Event.GetTypeId();
		Record.Payload       = MoveTemp(PayloadBytes);

		FScopeLock Lock(&StagingLock);
		StagingBuffer.Add(MoveTemp(Record));
	}

	// -------------------------------------------------------------------------
	// FlushTick -- game-thread only
	// -------------------------------------------------------------------------

	void FTapeEventRecorder::FlushTick(uint64 CurrentTick)
	{
		if (bFinalized || EventsFile == nullptr)
		{
			return;
		}

		TArray<FTapeEventRecord> LocalRecords;
		{
			FScopeLock Lock(&StagingLock);
			LocalRecords = MoveTemp(StagingBuffer);
			StagingBuffer.Reset();
		}

		// Records should already arrive in (Tick, TickSequence) order because the
		// EventBus stamps sequence numbers monotonically within a tick. Sort anyway
		// for safety (e.g. multiple shard buses racing).
		LocalRecords.Sort([](const FTapeEventRecord& A, const FTapeEventRecord& B)
		{
			if (A.Tick != B.Tick) { return A.Tick < B.Tick; }
			if (A.TickSequence != B.TickSequence) { return A.TickSequence < B.TickSequence; }
			return A.SourceShardId < B.SourceShardId;
		});

		for (const FTapeEventRecord& Record : LocalRecords)
		{
			WriteRecord(Record);
		}

		(void)CurrentTick; // kept as parameter for future per-tick flush metadata
	}

	// -------------------------------------------------------------------------
	// Private helpers
	// -------------------------------------------------------------------------

	void FTapeEventRecorder::WriteRecord(const FTapeEventRecord& Record)
	{
		if (EventsFile == nullptr)
		{
			return;
		}

		// On-disk layout per record (all little-endian):
		//   uint64   Tick
		//   uint32   TickSequence
		//   uint16   SourceShardId
		//   uint8    Origin
		//   uint8    Reserved (padding, written as 0)
		//   uint64   TypeTag
		//   uint32   PayloadBytes
		//   [PayloadBytes] Raw bytes

		const uint8  OriginByte   = static_cast<uint8>(Record.Origin);
		const uint8  Reserved     = 0;
		const uint32 PayloadSize  = static_cast<uint32>(Record.Payload.Num());

		ArchiveWriteRecorder(EventsFile, &Record.Tick,          sizeof(Record.Tick));
		ArchiveWriteRecorder(EventsFile, &Record.TickSequence,  sizeof(Record.TickSequence));
		ArchiveWriteRecorder(EventsFile, &Record.SourceShardId, sizeof(Record.SourceShardId));
		ArchiveWriteRecorder(EventsFile, &OriginByte,           sizeof(OriginByte));
		ArchiveWriteRecorder(EventsFile, &Reserved,             sizeof(Reserved));
		ArchiveWriteRecorder(EventsFile, &Record.TypeTag,        sizeof(Record.TypeTag));
		ArchiveWriteRecorder(EventsFile, &PayloadSize,           sizeof(PayloadSize));
		if (PayloadSize > 0)
		{
			ArchiveWriteRecorder(EventsFile, Record.Payload.GetData(), static_cast<int64>(PayloadSize));
		}

		++EventRecordCount;
	}

	bool FTapeEventRecorder::SerializePayload(ISerializableEvent& SerializableEvent, TArray<uint8>& OutBytes)
	{
		FMemoryWriter Writer(OutBytes, /*bIsPersistent=*/true);
		SerializableEvent.Serialize(Writer);
		return !Writer.IsError();
	}
}
