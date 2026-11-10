// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// On-disk format for deterministic replay tapes.
//
// A tape folder lives at Saved/DeterminismTapes/<TapeId>/ and contains:
//   - manifest.json    Human-auditable header (see FDeterminismTapeManifest).
//   - events.bin       Length-prefixed per-tick event records (input tape).
//   - checksums.bin    Per-MegaTick checksum entries (registry + per-shard).
//   - shard_snapshots/ Optional keyframe full-state dumps (future extension).
//
// Binary payloads keep the hot-path write cheap; the JSON manifest stays
// human-auditable for golden regeneration review.
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "Infrastructure/EventOriginTypes.h"   // EEventOrigin, IsReplayableEventOrigin

namespace GalaxyDeterminism
{
	// ------------------------------------------------------------------------
	// Format version. Bump when the on-disk layout changes in a breaking way.
	// Additive field changes should also bump and be handled by readers.
	// ------------------------------------------------------------------------
	inline constexpr uint32 TapeFormatVersion = 1;

	// ------------------------------------------------------------------------
	// File-level magic numbers. Little-endian, checked at the head of each
	// binary file to catch wrong-file/wrong-format mix-ups before parsing.
	// ------------------------------------------------------------------------
	inline constexpr uint32 EventsFileMagic    = 0x47585445; // "GXTE"
	inline constexpr uint32 ChecksumFileMagic  = 0x47584354; // "GXCT"

	// ------------------------------------------------------------------------
	// Origin tag attached to every recorded event. Only external-origin events
	// are replayed; internally-derived events are expected to reproduce from
	// the deterministic sim starting from the same seed + inputs.
	// Definition lives in EventBus.h (EEventOrigin). Re-exported here so
	// tape consumers need only include this header.
	// ------------------------------------------------------------------------
	using EEventOrigin = ::EEventOrigin;
	using ::IsReplayableEventOrigin;

	// ------------------------------------------------------------------------
	// FDeterminismTapeManifest
	//
	// Serialized as JSON at manifest.json. All fields are required.
	// ------------------------------------------------------------------------
	struct FDeterminismTapeManifest
	{
		/** Format version from TapeFormatVersion at record time. */
		uint32 FormatVersion = TapeFormatVersion;

		/** Opaque tape identifier (matches folder name). */
		FString TapeId;

		/** Engine build identifier (FEngineVersion::Current().ToString()). */
		FString EngineBuildId;

		/** GALAXY_SAVE_VERSION at record time. */
		uint32 SaveVersion = 0;

		/** Root RNG seed. Replay sets this via UGlobalShardScheduler::SetDeterminismSeed. */
		int32 RootSeed = 0;

		/** First scheduler tick represented in the tape (usually the tick after Record was armed). */
		uint64 StartTick = 0;

		/** Total tick count recorded (inclusive of StartTick). */
		uint64 TickCount = 0;

		/** Fixed dt at record time (from UGlobalShardScheduler::GetFixedDt()). */
		float FixedDt = 0.0f;

		/** Number of MegaTick-boundary checksum entries written to checksums.bin. */
		uint32 CheckpointCount = 0;

		/** Number of event records written to events.bin. */
		uint64 EventRecordCount = 0;

		/** Unix-epoch seconds at record start. Informational only. */
		int64 RecordedAtUnixSeconds = 0;

		/** Human-readable description written by whoever invoked g.det.Record. */
		FString Description;
	};

	// ------------------------------------------------------------------------
	// FTapeEventRecord
	//
	// One entry in events.bin. Emitted in tick-and-sequence order. The scheduler
	// plays these back before Phase_Input at the matching tick during replay.
	//
	// On-disk layout (all little-endian):
	//   uint64   Tick
	//   uint32   TickSequence
	//   uint16   SourceShardId
	//   uint8    Origin (EEventOrigin)
	//   uint8    Reserved
	//   uint64   TypeTag (from FEventTypeRegistry)
	//   uint32   PayloadBytes
	//   [PayloadBytes] Raw bytes from ISerializableEvent::Serialize().
	// ------------------------------------------------------------------------
	struct FTapeEventRecord
	{
		uint64       Tick = 0;
		uint32       TickSequence = 0;
		uint16       SourceShardId = 0;
		EEventOrigin Origin = EEventOrigin::Internal;
		uint64       TypeTag = 0;
		TArray<uint8> Payload;
	};

	// ------------------------------------------------------------------------
	// FTapeCheckpointEntry
	//
	// One entry in checksums.bin. Written at every MegaTick boundary while recording.
	//
	// On-disk layout (all little-endian):
	//   uint64   Tick
	//   uint64   RegistryChecksum
	//   uint32   ShardChecksumCount
	//   [ShardChecksumCount] { uint16 ShardId; uint64 Checksum; }
	// ------------------------------------------------------------------------
	struct FTapeShardChecksum
	{
		uint16 ShardId = 0;
		uint64 Checksum = 0;
	};

	struct FTapeCheckpointEntry
	{
		uint64 Tick = 0;
		uint64 RegistryChecksum = 0;
		TArray<FTapeShardChecksum> ShardChecksums;
	};

	// ------------------------------------------------------------------------
	// Relative filenames inside a tape folder. Kept as free functions so
	// consumers never hardcode string literals.
	// ------------------------------------------------------------------------
	[[nodiscard]] inline FString ManifestFileName()  { return TEXT("manifest.json"); }
	[[nodiscard]] inline FString EventsFileName()    { return TEXT("events.bin"); }
	[[nodiscard]] inline FString ChecksumsFileName() { return TEXT("checksums.bin"); }
	[[nodiscard]] inline FString SnapshotsDirName()  { return TEXT("shard_snapshots"); }
}
