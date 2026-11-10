// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================

#include "Infrastructure/Determinism/TapeCheckpointWriter.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogDeterminismCheckpoint);

namespace
{
	// FArchive::Serialize requires non-const void*. This helper lets callers
	// pass const struct fields without casting at every call site.
	FORCEINLINE void ArchiveWrite(FArchive* Ar, const void* Data, int64 Size)
	{
		Ar->Serialize(const_cast<void*>(Data), Size);
	}

	FORCEINLINE void ArchiveWrite(const TUniquePtr<FArchive>& Ar, const void* Data, int64 Size)
	{
		Ar->Serialize(const_cast<void*>(Data), Size);
	}
}

namespace GalaxyDeterminism
{
	FTapeCheckpointWriter::FTapeCheckpointWriter(const FString& InTapeFolder)
		: TapeFolder(InTapeFolder)
	{
		const FString FilePath = FPaths::Combine(TapeFolder, GalaxyDeterminism::ChecksumsFileName());

		IFileManager::Get().MakeDirectory(*TapeFolder, /*Tree=*/true);

		ChecksumsFile.Reset(IFileManager::Get().CreateFileWriter(*FilePath, /*Flags=*/0));
		if (!ensureMsgf(ChecksumsFile != nullptr,
			TEXT("[DeterminismTape][CheckpointWriter] Failed to open checksums.bin: %s"), *FilePath))
		{
			return;
		}

		// Write magic + version header.
		uint32 Magic   = GalaxyDeterminism::ChecksumFileMagic;
		uint32 Version = GalaxyDeterminism::TapeFormatVersion;
		ArchiveWrite(ChecksumsFile, &Magic,   sizeof(Magic));
		ArchiveWrite(ChecksumsFile, &Version, sizeof(Version));
	}

	FTapeCheckpointWriter::~FTapeCheckpointWriter()
	{
		Finalize();
	}

	void FTapeCheckpointWriter::Finalize()
	{
		if (bFinalized)
		{
			return;
		}
		bFinalized = true;

		if (ChecksumsFile != nullptr)
		{
			ChecksumsFile->Flush();
			ChecksumsFile.Reset();
		}

		UE_LOG(LogDeterminismCheckpoint, Log,
			TEXT("[DeterminismTape][CheckpointWriter] Finalized. Total checkpoints: %u"),
			CheckpointCount);
	}

	void FTapeCheckpointWriter::AppendCheckpoint(const FTapeCheckpointEntry& Entry)
	{
		if (bFinalized || ChecksumsFile == nullptr)
		{
			return;
		}
		WriteEntry(Entry);
	}

	void FTapeCheckpointWriter::WriteEntry(const FTapeCheckpointEntry& Entry)
	{
		// On-disk layout per entry (all little-endian):
		//   uint64   Tick
		//   uint64   RegistryChecksum
		//   uint32   ShardChecksumCount
		//   [ShardChecksumCount] { uint16 ShardId; uint64 Checksum; }

		const uint32 ShardCount = static_cast<uint32>(Entry.ShardChecksums.Num());

		ArchiveWrite(ChecksumsFile, &Entry.Tick,             sizeof(Entry.Tick));
		ArchiveWrite(ChecksumsFile, &Entry.RegistryChecksum, sizeof(Entry.RegistryChecksum));
		ArchiveWrite(ChecksumsFile, &ShardCount,             sizeof(ShardCount));

		for (const FTapeShardChecksum& SC : Entry.ShardChecksums)
		{
			ArchiveWrite(ChecksumsFile, &SC.ShardId,  sizeof(SC.ShardId));
			ArchiveWrite(ChecksumsFile, &SC.Checksum, sizeof(SC.Checksum));
		}

		++CheckpointCount;
	}
}
