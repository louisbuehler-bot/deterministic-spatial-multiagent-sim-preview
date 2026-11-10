// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// FTapeCheckpointWriter -- writes per-MegaTick checksum entries to checksums.bin.
//
// Called by UGlobalShardScheduler::ComputeDeterminismChecksum() when tape
// recording is armed (bRecordingTape == true).
//
// Lifecycle mirrors FTapeEventRecorder:
//   1. Created by the scheduler when g.det.Record is invoked.
//   2. AppendCheckpoint() called once per MegaTick while recording.
//   3. Finalize() called by g.det.StopRecord.
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Infrastructure/Determinism/DeterminismTapeFormat.h"

DECLARE_LOG_CATEGORY_EXTERN(LogDeterminismCheckpoint, Log, All);

namespace GalaxyDeterminism
{
	/**
	 * Appends FTapeCheckpointEntry records to checksums.bin.
	 * Game-thread only -- no locking required.
	 */
	class FTapeCheckpointWriter
	{
	public:
		/**
		 * Open checksums.bin for writing inside InTapeFolder.
		 * The folder must already exist (created by FTapeEventRecorder or the
		 * scheduler before constructing this).
		 */
		GALAXYCORE_API explicit FTapeCheckpointWriter(const FString& InTapeFolder);

		GALAXYCORE_API void Finalize();
		GALAXYCORE_API ~FTapeCheckpointWriter();

		// ----------------------------------------------------------------
		// Scheduler API (game-thread only)
		// ----------------------------------------------------------------

		/**
		 * Append one MegaTick-boundary entry.
		 * @param Entry  Registry + per-shard checksums for this MegaTick.
		 */
		GALAXYCORE_API void AppendCheckpoint(const FTapeCheckpointEntry& Entry);

		/** Total entries written (valid after each AppendCheckpoint or Finalize). */
		[[nodiscard]] uint32 GetCheckpointCount() const { return CheckpointCount; }

		[[nodiscard]] bool IsFinalized() const { return bFinalized; }

	private:
		void WriteEntry(const FTapeCheckpointEntry& Entry);

		FString TapeFolder;
		TUniquePtr<FArchive> ChecksumsFile;
		uint32 CheckpointCount = 0;
		bool bFinalized = false;
	};
}
