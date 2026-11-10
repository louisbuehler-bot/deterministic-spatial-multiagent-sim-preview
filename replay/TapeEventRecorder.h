// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// FTapeEventRecorder -- IEventRecorder implementation that serialises
// external-origin events to events.bin inside a determinism tape folder.
//
// Lifecycle:
//   1. UGlobalShardScheduler::OnStartRecording() creates an instance and calls
//      FEventBusManager::SetEventRecorder(&Recorder) on the shard's bus.
//   2. RecordEmit() is called for every emission. Events with a non-replayable
//      origin (Internal) are skipped; all others are serialised and buffered.
//   3. FlushTick() is called by the scheduler after Phase_BuildSnapshot to
//      flush the per-tick buffer to the binary writer.
//   4. Finalize() is called by g.det.StopRecord to flush remainders and close.
//
// Thread safety:
//   The scheduler drives all phases single-threaded from the game thread.
//   The EventBus may call RecordEmit from shard threads (one per shard bus).
//   Access to TickBuffer is therefore protected by a TArray-per-tick design:
//   each call to RecordEmit appends to a thread-local staging buffer, and
//   FlushTick() (game-thread only) drains all staging buffers under a lock.
//   This mirrors the approach used by FJsonEventRecorder in EventBus.cpp.
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Infrastructure/EventBus.h"
#include "Infrastructure/Determinism/DeterminismTapeFormat.h"

DECLARE_LOG_CATEGORY_EXTERN(LogDeterminismTape, Log, All);

namespace GalaxyDeterminism
{
	/**
	 * Writes replayable (external-origin) events to events.bin inside a tape folder.
	 * Attach to an FEventBusManager via SetEventRecorder(); call FlushTick() after
	 * each Phase_BuildSnapshot; call Finalize() when recording stops.
	 *
	 * Only events whose FEventMetadata::Origin is non-Internal AND whose type is
	 * registered in FEventTypeRegistry are written to disk. Unregistered external
	 * events emit a one-time warning and are dropped from the tape.
	 */
	class FTapeEventRecorder final : public IEventRecorder
	{
	public:
		/**
		 * Construct and open the events.bin file for writing.
		 * @param InTapeFolder  Absolute path to the tape folder (must already exist).
		 * @param InStartTick   The first scheduler tick that will be recorded.
		 */
		GALAXYCORE_API FTapeEventRecorder(const FString& InTapeFolder, uint64 InStartTick);

		/**
		 * Finalize and close. Safe to call more than once; subsequent calls are no-ops.
		 * Writes the final record count to EventRecordCount so the manifest can be
		 * updated by the scheduler.
		 */
		GALAXYCORE_API void Finalize();

		GALAXYCORE_API ~FTapeEventRecorder() override;

		// ----------------------------------------------------------------
		// IEventRecorder
		// ----------------------------------------------------------------

		/**
		 * Called by the EventBusManager after metadata is stamped on the event.
		 * May be called from any thread. Thread-safe.
		 */
		void RecordEmit(const IEvent& Event, const FEventMetadata& Meta) override;

		/** Not used by the tape recorder (dispatch order is internal). */
		void RecordDispatch(const IEvent& Event, uint64 HandlerId) override { (void)Event; (void)HandlerId; }

		// ----------------------------------------------------------------
		// Scheduler API (game-thread only)
		// ----------------------------------------------------------------

		/**
		 * Flush the per-tick staging buffer to disk.
		 * Called by UGlobalShardScheduler after Phase_BuildSnapshot.
		 * Game-thread only.
		 */
		GALAXYCORE_API void FlushTick(uint64 CurrentTick);

		/** Total event records written so far (final value after Finalize()). */
		[[nodiscard]] uint64 GetEventRecordCount() const { return EventRecordCount; }

		/** Returns true after Finalize() has been called. */
		[[nodiscard]] bool IsFinalized() const { return bFinalized; }

	private:
		// ----------------------------------------------------------------
		// Serialization helpers
		// ----------------------------------------------------------------

		/** Write one FTapeEventRecord to the binary stream. Game-thread only. */
		void WriteRecord(const FTapeEventRecord& Record);

		/** Serialize an ISerializableEvent payload to bytes. Returns false if
		 *  the archive write fails; caller should drop the record. */
		bool SerializePayload(ISerializableEvent& SerializableEvent, TArray<uint8>& OutBytes);

		// ----------------------------------------------------------------
		// State
		// ----------------------------------------------------------------

		/** Absolute path to the tape folder (e.g. .../Saved/DeterminismTapes/test01/). */
		FString TapeFolder;

		/** First scheduled tick that recording covers. Written into the tape manifest. */
		uint64 StartTick = 0;

		/** File handle for events.bin. Null if not open or already finalized. */
		TUniquePtr<FArchive> EventsFile;

		/** Guard for StagingBuffer -- written from (potentially) multiple threads. */
		FCriticalSection StagingLock;

		/** Per-tick staging buffer. RecordEmit appends here; FlushTick drains it. */
		TArray<FTapeEventRecord> StagingBuffer;

		/** Total event records serialized and flushed to disk. */
		uint64 EventRecordCount = 0;

		/** Set in Finalize(). */
		bool bFinalized = false;

		/** Set of type IDs for which we have already emitted a "not registered" warning.
		 *  Prevents O(N) log spam for high-frequency unregistered events. */
		TSet<uint64> WarnedUnregisteredTypes;

		/** Lock for WarnedUnregisteredTypes (accessed from any emit thread). */
		FCriticalSection WarnLock;
	};
}
