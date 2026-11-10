// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// FReplayEventInjector -- reads events.bin from a tape and re-emits recorded
// events onto the correct shard bus at the matching tick during replay.
//
// Usage:
//   1. Construct with the tape folder path. Check IsLoaded() before using.
//   2. Register every shard's bus with RegisterShardBus(ShardId, Bus).
//   3. Call InjectForTick(GlobalTick) BEFORE Phase_Input each scheduler tick.
//   4. Destroy or Reset() when replay ends.
//
// Event reconstruction:
//   - TypeTag (uint64 FNV TypeId) -> FName via FEventTypeRegistry::FindTag()
//   - FName -> FFactoryFn via FEventTypeRegistry::FindFactory()
//   - Payload bytes deserialized into the new event via FMemoryReader +
//     ISerializableEvent::Serialize(). All fields come from the tape.
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Infrastructure/Determinism/DeterminismTapeFormat.h"

class FEventBusManager;

DECLARE_LOG_CATEGORY_EXTERN(LogDeterminismReplay, Log, All);

namespace GalaxyDeterminism
{
	/**
	 * Loads events.bin from a tape folder and replays recorded events tick by tick.
	 *
	 * Thread safety: game-thread only (all methods).
	 * Memory: all records are loaded into memory on construction (bounded by tape size).
	 */
	class FReplayEventInjector
	{
	public:
		/**
		 * Load events.bin from InTapeFolder.
		 * Check IsLoaded() to confirm success before calling other methods.
		 */
		GALAXYCORE_API explicit FReplayEventInjector(const FString& InTapeFolder);

		/**
		 * Register the event bus for a specific shard.
		 * Events with matching SourceShardId will be emitted on this bus.
		 * Register all expected shards before calling InjectForTick().
		 * The bus must outlive this injector.
		 */
		GALAXYCORE_API void RegisterShardBus(uint16 ShardId, FEventBusManager* Bus);

		/**
		 * Inject all recorded events for the given tick onto their registered buses.
		 * Events are injected in their original TickSequence order.
		 * Call before Phase_Input each tick during replay.
		 * @return Number of events successfully injected.
		 */
		GALAXYCORE_API int32 InjectForTick(uint64 Tick);

		// ----------------------------------------------------------------
		// State queries
		// ----------------------------------------------------------------

		/** True if events.bin was loaded and validated successfully. */
		[[nodiscard]] bool IsLoaded() const { return bLoaded; }

		/** First tick that has recorded events (UINT64_MAX if no events loaded). */
		[[nodiscard]] uint64 GetFirstTick() const { return FirstTick; }

		/** Last tick that has recorded events (0 if no events loaded). */
		[[nodiscard]] uint64 GetLastTick() const { return LastTick; }

		/** Total number of event records loaded from disk. */
		[[nodiscard]] int32 GetTotalEventCount() const { return TotalEventCount; }

		/** Number of ticks that have at least one recorded event. */
		[[nodiscard]] int32 GetActiveTicks() const { return RecordsByTick.Num(); }

	private:
		/** Read and validate events.bin, populating RecordsByTick. */
		bool LoadFromFile(const FString& FilePath);

		/**
		 * Reconstruct and emit a single recorded event.
		 * @param Record   The tape record to replay.
		 * @return true if the event was successfully reconstructed and emitted.
		 */
		bool ReplayOneRecord(const FTapeEventRecord& Record);

		// ----------------------------------------------------------------
		// State
		// ----------------------------------------------------------------

		/** Tape folder path for logging. */
		FString TapeFolder;

		/** ShardId -> FEventBusManager* registered by the scheduler. */
		TMap<uint16, FEventBusManager*> ShardBuses;

		/**
		 * Records grouped and sorted by tick.
		 * Within each tick bucket, records are sorted by (TickSequence, SourceShardId)
		 * to preserve emission order across shards.
		 */
		TMap<uint64, TArray<FTapeEventRecord>> RecordsByTick;

		uint64 FirstTick       = UINT64_MAX;
		uint64 LastTick        = 0;
		int32  TotalEventCount = 0;
		bool   bLoaded         = false;
	};
}
