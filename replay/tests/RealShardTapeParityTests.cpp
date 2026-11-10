// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// TAPE PARITY OVER A REAL GAMEPLAY SHARD.
//
// This test records a tape from a live PlanetShard (registry-bearing, real
// combatants) driven ONLY through UGlobalShardScheduler::Tick -- the production
// entry, not hand-called phases -- then replays the tape into a fresh world and
// requires bit-identical checkpoints.
//
// The units are stamped with CGalaxyStableId on purpose: it is the entity table
// of the authoritative payload (AuthoritativeRegistrySerializer), so an unstamped
// entity would be absent from the bootstrap snapshot and the replay world would
// silently start from an EMPTY planet -- a test that passes while comparing
// nothing.
// ============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "DeterminismTapeFixtures.h"                            // DeterminismTapeFixtures::FScopedTapeRootOverride
#include "Misc/AutomationTest.h"
#include "TestUtilities.h"

#include "ECS/Components/Core/ECSEmpireSlotComponents.h"       // CEmpireSlot
#include "ECS/Components/Core/ECSGalaxyTopologyComponents.h"   // CGalaxyStableId
#include "ECS/Components/Unit/ECSPositionComponents.h"          // CPositionPlanetSurface
#include "ECS/Components/Unit/UnitAvatar.h"                     // CUnitAvatar
#include "ECS/Serialization/ECSSerialization.h"                  // EShardType (combat-standup assert)
#include "ECS/Shards/CombatShard.h"                              // GetCombatId -- witnesses transient-id reuse
#include "ECS/Shards/GlobalShardScheduler.h"
#include "ECS/Shards/InterstellarSquadShard.h"                   // 2nd registry-bearing shard type
#include "ECS/Shards/PlanetShard.h"
#include "Infrastructure/Determinism/DeterminismReplayRunner.h"

#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/Paths.h"

namespace
{
	// The PlanetShard id is shared between the recording world and the replay
	// bootstrap: ApplyECSSnapshotBinary matches recorded payloads to REGISTERED
	// shards by id, so the bootstrap has to stand up the same id or the planet
	// payload lands nowhere.
	constexpr uint16 GRealTapePlanetShardId = 4301;

	// RAII flip of the live-combat-zone gate, so standup behaviour does not depend
	// on the shipping default or on test ordering.
	//
	// ALSO pins the entry-mode sub-gates to the values the recorded windows were authored
	// against, so a recording does not inherit them from ambient config:
	// g.combat.PerceptionGatedEntry=0 (the tape's combats stand up via the geometric
	// trigger) and g.combat.WildlifeZones=1.
	struct FScopedLiveZonesGate
	{
		IConsoleVariable* CVar = nullptr;
		IConsoleVariable* PerceptionCVar = nullptr;
		IConsoleVariable* WildlifeCVar = nullptr;
		int32 Prev = 0;
		int32 PrevPerception = 0;
		int32 PrevWildlife = 0;

		explicit FScopedLiveZonesGate(int32 Value)
		{
			CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("g.combat.LiveZones"));
			if (CVar)
			{
				Prev = CVar->GetInt();
				CVar->Set(Value, ECVF_SetByCode);
			}
			PerceptionCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("g.combat.PerceptionGatedEntry"));
			if (PerceptionCVar)
			{
				PrevPerception = PerceptionCVar->GetInt();
				PerceptionCVar->Set(0, ECVF_SetByCode); // legacy geometric trigger -- what the tapes record
			}
			WildlifeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("g.combat.WildlifeZones"));
			if (WildlifeCVar)
			{
				PrevWildlife = WildlifeCVar->GetInt();
				WildlifeCVar->Set(1, ECVF_SetByCode); // wildlife may seed/join zones
			}
		}

		~FScopedLiveZonesGate()
		{
			if (WildlifeCVar)
			{
				WildlifeCVar->Set(PrevWildlife, ECVF_SetByCode);
			}
			if (PerceptionCVar)
			{
				PerceptionCVar->Set(PrevPerception, ECVF_SetByCode);
			}
			if (CVar)
			{
				CVar->Set(Prev, ECVF_SetByCode);
			}
		}

		FScopedLiveZonesGate(const FScopedLiveZonesGate&) = delete;
		FScopedLiveZonesGate& operator=(const FScopedLiveZonesGate&) = delete;
	};

	// A combat-eligible planet unit that SURVIVES serialization. CGalaxyStableId is
	// the entity-table key (pass 1 of the authoritative payload); CUnitAvatar,
	// CEmpireSlot and CPositionPlanetSurface are all on the authoritative component
	// roster, so the whole unit round-trips through the bootstrap snapshot.
	void MakeSerializablePlanetUnit(
		entt::registry& Registry,
		int64 StableId,
		int32 EntityId,
		int32 EmpireSlotId,
		const FVector3d& SurfaceDirection)
	{
		const entt::entity Entity = Registry.create();
		Registry.emplace<CGalaxyStableId>(Entity, CGalaxyStableId{ StableId });

		CUnitAvatar Avatar;
		Avatar.EntityId = EntityId;
		Avatar.UnitName = FString::Printf(TEXT("TapeParityUnit_%d"), EntityId);
		Avatar.LifecycleState = EUnitLifecycleState::Idle;
		Registry.emplace<CUnitAvatar>(Entity, MoveTemp(Avatar));

		Registry.emplace<CEmpireSlot>(Entity).EmpireSlotId = EmpireSlotId;
		Registry.emplace<CPositionPlanetSurface>(Entity).UnitDirection = SurfaceDirection.GetSafeNormal();
	}

	// Populate a PlanetShard with two hostile combatants (slot 1 vs slot 2), close
	// enough to each other that the proximity pass groups them into one engagement.
	void PopulateTapeParityPlanet(PlanetShard& Planet)
	{
		entt::registry& Registry = Planet.GetRegistry();
		MakeSerializablePlanetUnit(Registry, /*StableId*/ 43001, /*EntityId*/ 10, /*Slot*/ 1, FVector3d(1.0, 0.0, 0.0));
		MakeSerializablePlanetUnit(Registry, /*StableId*/ 43002, /*EntityId*/ 11, /*Slot*/ 2, FVector3d(1.0, 0.0, 0.0));
	}

	// Register an EMPTY PlanetShard under the recorded id. State arrives afterwards
	// via ApplyECSSnapshotBinary -- the same split the fixture-based TapeParity test
	// uses (register structure in bootstrap, restore state from the snapshot).
	bool BootstrapRealPlanetShard(UWorld* World, UGlobalShardScheduler* Scheduler, FString& OutError)
	{
		if (World == nullptr || Scheduler == nullptr)
		{
			OutError = TEXT("Real-shard replay bootstrap received a null world or scheduler.");
			return false;
		}

		AActor* Host = World->SpawnActor<AActor>();
		if (Host == nullptr)
		{
			OutError = TEXT("Real-shard replay bootstrap could not spawn the planet host actor.");
			return false;
		}

		TSharedPtr<PlanetShard> Planet =
			MakeShared<PlanetShard>(GRealTapePlanetShardId, TWeakObjectPtr<AActor>(Host));
		Scheduler->RegisterShard(Planet);
		Scheduler->FlushPendingShardOpsForTests();
		return true;
	}

	// ------------------------------------------------------------------------
	// SECOND registry-bearing shard type, for the multi-shard parity test below.
	// ------------------------------------------------------------------------
	// InterstellarSquadShard is the natural pair for PlanetShard: its own header
	// describes it as holding per-unit entities in its own entt::registry,
	// "mirroring PlanetShard + its 1:1 AttachedZone". So it drives the SAME
	// serialization spine through a DIFFERENT shard type -- which is exactly the
	// axis the single-shard test above cannot cover.
	constexpr uint16 GRealTapeSquadShardId = 4302;
	constexpr uint16 GRealTapeSquadParentZoneId = 4303;
	constexpr int32  GRealTapeSquadId = 77;

	// Same authoritative component set as the planet units: CGalaxyStableId is the
	// entity-table key, so an unstamped entity is absent from the bootstrap snapshot
	// and the replay world would silently start from an EMPTY shard -- a test that
	// passes while comparing nothing.
	void MakeSerializableSquadUnit(
		entt::registry& Registry,
		int64 StableId,
		int32 EntityId,
		int32 EmpireSlotId)
	{
		const entt::entity Entity = Registry.create();
		Registry.emplace<CGalaxyStableId>(Entity, CGalaxyStableId{ StableId });

		CUnitAvatar Avatar;
		Avatar.EntityId = EntityId;
		Avatar.UnitName = FString::Printf(TEXT("TapeParitySquadUnit_%d"), EntityId);
		Avatar.LifecycleState = EUnitLifecycleState::Idle;
		Registry.emplace<CUnitAvatar>(Entity, MoveTemp(Avatar));

		Registry.emplace<CEmpireSlot>(Entity).EmpireSlotId = EmpireSlotId;
	}

	// A WIDER planet population than PopulateTapeParityPlanet's two units.
	// WHY WIDER: with 2 entities the authoritative payload's entity table is
	// trivially ordered, so an iteration-order hazard has almost no room to show
	// up -- any permutation of a 2-element table that differs is a coin flip the
	// test would fail loudly on anyway. Six units across two slots gives the
	// entity table, the per-slot grouping and the engagement proximity pass enough
	// material for an ordering difference to actually be observable.
	void PopulateWideTapeParityPlanet(PlanetShard& Planet)
	{
		entt::registry& Registry = Planet.GetRegistry();
		for (int32 Index = 0; Index < 6; ++Index)
		{
			const int32 SlotId = (Index % 2) + 1;                 // alternate slot 1 / slot 2
			const double Lateral = 0.01 * static_cast<double>(Index);
			MakeSerializablePlanetUnit(
				Registry,
				/*StableId*/ 43100 + Index,
				/*EntityId*/ 20 + Index,
				/*Slot*/ SlotId,
				FVector3d(1.0, Lateral, 0.0));
		}
	}

	void PopulateTapeParitySquad(InterstellarSquadShard& Squad)
	{
		entt::registry& Registry = Squad.GetRegistry();
		for (int32 Index = 0; Index < 4; ++Index)
		{
			MakeSerializableSquadUnit(
				Registry,
				/*StableId*/ 43200 + Index,
				/*EntityId*/ 40 + Index,
				/*Slot*/ (Index % 2) + 1);
		}
	}

	// Register BOTH registry-bearing shards, empty, under their recorded ids.
	// State arrives afterwards via ApplyECSSnapshotBinary, which matches payloads
	// to REGISTERED shards by id -- so a shard the bootstrap forgets receives
	// nothing and its payload lands nowhere, silently.
	bool BootstrapRealMultiShard(UWorld* World, UGlobalShardScheduler* Scheduler, FString& OutError)
	{
		if (World == nullptr || Scheduler == nullptr)
		{
			OutError = TEXT("Multi-shard replay bootstrap received a null world or scheduler.");
			return false;
		}

		AActor* Host = World->SpawnActor<AActor>();
		if (Host == nullptr)
		{
			OutError = TEXT("Multi-shard replay bootstrap could not spawn the planet host actor.");
			return false;
		}

		TSharedPtr<PlanetShard> Planet =
			MakeShared<PlanetShard>(GRealTapePlanetShardId, TWeakObjectPtr<AActor>(Host));
		Scheduler->RegisterShard(Planet);

		TSharedPtr<InterstellarSquadShard> Squad = MakeShared<InterstellarSquadShard>(
			static_cast<FShardId>(GRealTapeSquadShardId),
			static_cast<FShardId>(GRealTapeSquadParentZoneId),
			GRealTapeSquadId,
			static_cast<FShardId>(GRealTapePlanetShardId));
		Scheduler->RegisterShard(Squad);

		Scheduler->FlushPendingShardOpsForTests();
		return true;
	}

	// Does this checkpoint carry a checksum for the given shard? The tape writer
	// drops any shard whose ComputeShardChecksum is 0, so presence here is the
	// direct proof that a REAL gameplay shard reached the tape.
	bool CheckpointCoversShard(const GalaxyDeterminism::FTapeCheckpointEntry& Entry, FShardId ShardId)
	{
		for (const GalaxyDeterminism::FTapeShardChecksum& Shard : Entry.ShardChecksums)
		{
			if (Shard.ShardId == static_cast<uint16>(ShardId))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealShardTapeParityTest,
	"GALAXY_PROJECT.Determinism.Replay.Functional.RealShardTapeParity",
	GALAXY_TEST_FLAGS)

bool FRealShardTapeParityTest::RunTest(const FString& Parameters)
{
	TestUtilities::FScopedTestIsolation Isolation(*this);
	(void)Parameters;

	TestUtilities::FScopedAutomationDirectory TapeDirScope(TEXT("RealShardTapeParity"));
	DeterminismTapeFixtures::FScopedTapeRootOverride TapeRootOverride(FPaths::Combine(TapeDirScope.GetPath(), TEXT("DeterminismTapes")));

	// Held across the replay too: the replay world re-runs the same standup logic
	// and must see the same gate value the recording did.
	FScopedLiveZonesGate LiveZones(1);

	TestUtilities::FScopedTestWorld RecordingWorld;
	UWorld* World = RecordingWorld.Get();
	if (!TestNotNull(TEXT("Recording world created"), World))
	{
		return false;
	}

	UGlobalShardScheduler* RecordingScheduler = World->GetSubsystem<UGlobalShardScheduler>();
	if (!TestNotNull(TEXT("Recording world provides UGlobalShardScheduler"), RecordingScheduler))
	{
		return false;
	}

	constexpr int32 RootSeed = 20260730;
	RecordingScheduler->SetDeterminismSeed(RootSeed);
	RecordingScheduler->SetExpectedDeterminismSeed(RootSeed);

	AActor* Host = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Planet host actor spawned"), Host))
	{
		return false;
	}

	TSharedPtr<PlanetShard> Planet =
		MakeShared<PlanetShard>(GRealTapePlanetShardId, TWeakObjectPtr<AActor>(Host));
	RecordingScheduler->RegisterShard(Planet);
	RecordingScheduler->FlushPendingShardOpsForTests();
	PopulateTapeParityPlanet(*Planet);

	// --- Bootstrap snapshot: the replay world's starting state. ---
	TArray<uint8> SnapshotBytes;
	if (!RecordingScheduler->BuildECSSnapshotBinary(SnapshotBytes, false))
	{
		AddError(TEXT("BuildECSSnapshotBinary failed for a registry-bearing PlanetShard; ")
			TEXT("without a bootstrap snapshot the replay world cannot start from the recorded state."));
		return false;
	}
	TestTrue(TEXT("Bootstrap snapshot is non-empty"), SnapshotBytes.Num() > 0);

	const FString TapeId = FString::Printf(TEXT("automation_realshard_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString TapeFolder = FPaths::Combine(TapeDirScope.GetPath(), TEXT("DeterminismTapes"), TapeId);
	const FString SnapshotPath = FPaths::Combine(TapeFolder, TEXT("ECSSnapshot.bin"));
	const auto CleanupTape = [&TapeFolder]()
	{
		IFileManager::Get().DeleteDirectory(*TapeFolder, false, true);
	};
	CleanupTape();

	FOutputDeviceNull NullAr;
	if (!RecordingScheduler->StartTapeRecording(TapeId, NullAr))
	{
		AddError(TEXT("Failed to start tape recording for the real-shard parity run."));
		CleanupTape();
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(SnapshotBytes, *SnapshotPath))
	{
		AddError(FString::Printf(TEXT("Failed to save the bootstrap snapshot: %s"), *SnapshotPath));
		RecordingScheduler->StopTapeRecording(NullAr);
		CleanupTape();
		return false;
	}

	// --- Drive the sim through the PRODUCTION entry only. Scheduler::Tick runs
	//     Phase_Strategic_All at MegaTick cadence, which is what fires PlanetShard
	//     combat detection AND LaunchPendingCombatShards -- so anything the loop
	//     stands up is recorded without the test hand-calling phases. ---
	const float FixedDt = RecordingScheduler->GetFixedDt();
	const int32 RecordedTicks = TickConstants::FramesPerMegaTick * 3;
	bool bSawCombatShard = false;
	for (int32 TickIndex = 0; TickIndex < RecordedTicks; ++TickIndex)
	{
		RecordingScheduler->Tick(FixedDt);

		// Sample per tick: a CombatShard is TRANSIENT (created by the standup, torn
		// down by CleanupCompletedCombatShards), so checking only after the loop
		// could miss it entirely and leave the claim below unverifiable.
		for (const TPair<FShardId, TSharedPtr<IShardBridge>>& Pair : RecordingScheduler->GetShards())
		{
			if (Pair.Value.IsValid()
				&& Pair.Value->GetShardType() == ECSSerialization::EShardType::Combat)
			{
				bSawCombatShard = true;
			}
		}
	}

	// Asserted rather than left incidental: the recorded window really does contain a
	// combat the production loop stood up, so tape parity covers the standup path and
	// not just a planet ticking in isolation. If standup ever stops firing for this
	// fixture, this test would quietly shrink to less than its name claims.
	// NOTE for whoever sees the log: this fixture legitimately emits
	// "[BattlespaceLifecycle] ... no valid spatial anchor ... falling back to legacy
	// entry projection" -- a mock PlanetShard has no terrain context, and
	// BattlespaceInstanceLifecycle.cpp documents that fallback as well-defined for exactly
	// this case. It is expected output here, not a defect.
	TestTrue(
		TEXT("The recorded window includes a CombatShard the production loop stood up"),
		bSawCombatShard);

	// The whole point: a REAL gameplay shard must be present in the tape's
	// per-shard checksum list, not just the synthetic fixture shards.
	const GalaxyDeterminism::FTapeCheckpointEntry RecordedEntry =
		RecordingScheduler->BuildTapeCheckpointEntry(
			static_cast<uint64>(RecordingScheduler->GetGlobalTick()),
			static_cast<uint64>(RecordingScheduler->GetDeterminismChecksum()));
	TestTrue(
		TEXT("The tape checkpoint covers the registry-bearing PlanetShard (real gameplay shard, not a fixture)"),
		CheckpointCoversShard(RecordedEntry, static_cast<FShardId>(GRealTapePlanetShardId)));

	RecordingScheduler->StopTapeRecording(NullAr);

	// --- Replay into a fresh world and require bit-identical checkpoints. ---
	GalaxyDeterminism::FReplayRunConfig Config;
	Config.TapeFolder = TapeFolder;
	Config.Bootstrap = &BootstrapRealPlanetShard;

	const GalaxyDeterminism::FReplayResult Result = GalaxyDeterminism::FDeterminismReplayRunner::RunReplay(Config);

	if (!Result.Succeeded())
	{
		AddError(FString::Printf(
			TEXT("Real-shard replay failed: status=%s error=%s"),
			GalaxyDeterminism::ReplayRunStatusToString(Result.Status),
			Result.Error.IsEmpty() ? TEXT("<none>") : *Result.Error));

		// Name the diverging shard: that localization is exactly what the per-shard
		// ComputeShardChecksum overrides exist to provide.
		for (const GalaxyDeterminism::FReplayCheckpointMismatch& Mismatch : Result.Mismatches)
		{
			AddError(FString::Printf(
				TEXT("  tick %llu: registry expected=%llu actual=%llu (%d expected / %d actual shard checksums)"),
				Mismatch.Tick,
				Mismatch.ExpectedRegistryChecksum,
				Mismatch.ActualRegistryChecksum,
				Mismatch.ExpectedShardChecksums.Num(),
				Mismatch.ActualShardChecksums.Num()));

			// The counts alone never localized anything. Diff the per-shard lists and
			// NAME the shards that actually moved -- that is the whole reason
			// ComputeShardChecksum has per-shard overrides. A shard present on one side
			// only is reported too: an absent shard is a different (standup-ordering)
			// defect from a shard whose contents drifted, and the two look identical
			// when you only print totals.
			for (const GalaxyDeterminism::FTapeShardChecksum& Expected : Mismatch.ExpectedShardChecksums)
			{
				const GalaxyDeterminism::FTapeShardChecksum* Actual =
					Mismatch.ActualShardChecksums.FindByPredicate(
						[&Expected](const GalaxyDeterminism::FTapeShardChecksum& Candidate)
						{ return Candidate.ShardId == Expected.ShardId; });

				if (!Actual)
				{
					AddError(FString::Printf(
						TEXT("    shard %u: present in the tape, ABSENT on replay"), Expected.ShardId));
				}
				else if (Actual->Checksum != Expected.Checksum)
				{
					AddError(FString::Printf(
						TEXT("    shard %u: DIVERGED expected=%llu actual=%llu"),
						Expected.ShardId, Expected.Checksum, Actual->Checksum));
				}
			}
			for (const GalaxyDeterminism::FTapeShardChecksum& Actual : Mismatch.ActualShardChecksums)
			{
				const bool bInTape = Mismatch.ExpectedShardChecksums.ContainsByPredicate(
					[&Actual](const GalaxyDeterminism::FTapeShardChecksum& Candidate)
					{ return Candidate.ShardId == Actual.ShardId; });
				if (!bInTape)
				{
					AddError(FString::Printf(
						TEXT("    shard %u: stood up on replay, ABSENT from the tape"), Actual.ShardId));
				}
			}
		}
	}

	TestTrue(TEXT("Real-shard replay succeeds"), Result.Succeeded());
	TestTrue(TEXT("Real-shard replay verified at least one checkpoint"), Result.CheckpointsVerified > 0);
	TestEqual(TEXT("Real-shard replay mismatch count is zero"), Result.Mismatches.Num(), 0);
	TestEqual(TEXT("Real-shard replay simulated the recorded tick count"),
		Result.TicksSimulated, static_cast<uint64>(RecordedTicks));

	CleanupTape();

	// Detach the PlanetShard before the world (and its registry) tears down.
	RecordingScheduler->UnregisterShard(static_cast<FShardId>(GRealTapePlanetShardId));
	RecordingScheduler->FlushPendingShardOpsForTests();

	return Result.Succeeded();
}

// ============================================================================
// Transient shard-id RECYCLING across a tape boundary.
// ============================================================================
// CombatShard ids are TRANSIENT -- CleanupCompletedCombatShards
// hands a completed combat's id back to a reuse pool (quarantined for
// ShardTimingWheel::StandardSize ticks first), so a later combat is handed the SAME
// id. Every id-keyed mechanism therefore has to stay correct across that reuse:
// snapshot apply matches payloads to shards BY ID, and the tape checkpoint records
// per-shard checksums BY ID.
//
// WHAT THIS ASSERTS -- deliberately NOT a pinned id assignment:
// pinning "the second combat gets id N" would cement whichever id the current
// allocator happens to hand out, and would have to be re-cut by any legitimate
// change to the pool. What matters is that the reuse is REPRODUCIBLE: record a
// window containing a recycle, replay it, and require bit-identical checkpoints.
// That stays valid whatever the allocator does.
//
// This guards the release order in CleanupCompletedCombatShards: with 2+ combats
// completing on the same tick, that order decides which id the LIFO pool hands the
// next combat, hence its ShardSig, hence the determinism checksum.
//
// SELF-DIAGNOSING BY DESIGN: if the recorded window contains no recycle, the test
// FAILS with what it actually observed rather than passing over an unexercised
// path -- the false-green shape this suite exists to avoid.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTransientShardIdRecycleTapeParityTest,
	"GALAXY_PROJECT.Determinism.Replay.Functional.TransientShardIdRecycleTapeParity",
	GALAXY_TEST_FLAGS)

bool FTransientShardIdRecycleTapeParityTest::RunTest(const FString& Parameters)
{
	TestUtilities::FScopedTestIsolation Isolation(*this);
	(void)Parameters;

	TestUtilities::FScopedAutomationDirectory TapeDirScope(TEXT("TransientShardIdRecycleTapeParity"));
	DeterminismTapeFixtures::FScopedTapeRootOverride TapeRootOverride(FPaths::Combine(TapeDirScope.GetPath(), TEXT("DeterminismTapes")));
	FScopedLiveZonesGate LiveZones(1);

	TestUtilities::FScopedTestWorld RecordingWorld;
	UWorld* World = RecordingWorld.Get();
	if (!TestNotNull(TEXT("Recording world created"), World))
	{
		return false;
	}

	UGlobalShardScheduler* RecordingScheduler = World->GetSubsystem<UGlobalShardScheduler>();
	if (!TestNotNull(TEXT("Recording world provides UGlobalShardScheduler"), RecordingScheduler))
	{
		return false;
	}

	constexpr int32 RootSeed = 20260731;
	RecordingScheduler->SetDeterminismSeed(RootSeed);
	RecordingScheduler->SetExpectedDeterminismSeed(RootSeed);

	AActor* Host = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Planet host actor spawned"), Host))
	{
		return false;
	}

	TSharedPtr<PlanetShard> Planet =
		MakeShared<PlanetShard>(GRealTapePlanetShardId, TWeakObjectPtr<AActor>(Host));
	RecordingScheduler->RegisterShard(Planet);
	RecordingScheduler->FlushPendingShardOpsForTests();
	PopulateTapeParityPlanet(*Planet);

	TArray<uint8> SnapshotBytes;
	if (!RecordingScheduler->BuildECSSnapshotBinary(SnapshotBytes, false))
	{
		AddError(TEXT("BuildECSSnapshotBinary failed; the replay world cannot start from the recorded state."));
		return false;
	}

	const FString TapeId = FString::Printf(TEXT("automation_recycle_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString TapeFolder = FPaths::Combine(TapeDirScope.GetPath(), TEXT("DeterminismTapes"), TapeId);
	const FString SnapshotPath = FPaths::Combine(TapeFolder, TEXT("ECSSnapshot.bin"));
	const auto CleanupTape = [&TapeFolder]()
	{
		IFileManager::Get().DeleteDirectory(*TapeFolder, false, true);
	};
	CleanupTape();

	FOutputDeviceNull NullAr;
	if (!RecordingScheduler->StartTapeRecording(TapeId, NullAr))
	{
		AddError(TEXT("Failed to start tape recording for the recycle parity run."));
		CleanupTape();
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(SnapshotBytes, *SnapshotPath))
	{
		AddError(FString::Printf(TEXT("Failed to save the bootstrap snapshot: %s"), *SnapshotPath));
		RecordingScheduler->StopTapeRecording(NullAr);
		CleanupTape();
		return false;
	}

	// WINDOW SIZING -- derived, not guessed. A recycle needs three things in sequence,
	// and the first dominates:
	//   (1) the first combat must COMPLETE. This fixture takes the full-simulation path
	//       (a battlespace exists), so CombatShard::MaxCombatRounds never applies --
	//       completion comes from FBattlespaceInstance::ShouldEndCombat, and with two
	//       stat-less mock units neither side is ever eliminated or routed, so the
	//       binding condition is its CombatTick >= MaxCombatTicks timeout.
	//       MaxCombatTicks is 120 Command Ticks (2 minutes).
	//   (2) the released id must clear the ShardTimingWheel::StandardSize quarantine.
	//   (3) a second combat must stand up and draw the recycled id.
	// The window is DERIVED from those three, not pinned, so a retune of the tick ladder
	// cannot silently shrink it below what a recycle needs.
	const float FixedDt = RecordingScheduler->GetFixedDt();
	constexpr int32 MaxCombatCommandTicks = 120;  // mirrors FBattlespaceInstance::MaxCombatTicks
	const int32 CombatFrames = MaxCombatCommandTicks * TickConstants::FramesPerSuperTick;
	const int32 RecordedTicks = CombatFrames
		+ ShardTimingWheel::StandardSize          // (2) quarantine
		+ 6 * TickConstants::FramesPerMegaTick;       // (3) slack, matching the old margin

	// Per shard id, every DISTINCT CombatId ever bound to it. An id carrying two
	// different CombatIds is a recycle, observed purely through the public shard map
	// -- no reaching into the allocator's pools, so the assertion survives any
	// internal change to how reuse is implemented.
	TMap<FShardId, TSet<int64>> CombatIdsByShardId;
	int32 DistinctCombatsSeen = 0;
	TSet<int64> AllCombatIds;

	for (int32 TickIndex = 0; TickIndex < RecordedTicks; ++TickIndex)
	{
		RecordingScheduler->Tick(FixedDt);

		for (const TPair<FShardId, TSharedPtr<IShardBridge>>& Pair : RecordingScheduler->GetShards()) // tmap-order-safe -- observation only; feeds a set, no outcome
		{
			if (!Pair.Value.IsValid()
				|| Pair.Value->GetShardType() != ECSSerialization::EShardType::Combat)
			{
				continue;
			}
			const CombatShard* Combat = static_cast<const CombatShard*>(Pair.Value.Get());
			const int64 CombatId = Combat->GetCombatId();
			CombatIdsByShardId.FindOrAdd(Pair.Key).Add(CombatId);
			bool bAlreadySeen = false;
			AllCombatIds.Add(CombatId, &bAlreadySeen);
			if (!bAlreadySeen)
			{
				++DistinctCombatsSeen;
			}
		}
	}

	FShardId RecycledShardId = 0;
	int32 RecycledIdCount = 0;
	for (const TPair<FShardId, TSet<int64>>& Pair : CombatIdsByShardId) // tmap-order-safe -- counting + first-witness for a diagnostic message, not an outcome
	{
		if (Pair.Value.Num() > 1)
		{
			++RecycledIdCount;
			if (RecycledShardId == 0)
			{
				RecycledShardId = Pair.Key;
			}
		}
	}

	RecordingScheduler->StopTapeRecording(NullAr);

	// HARD requirement, not an incidental observation: without a recycle in the
	// window this test silently degrades into a duplicate of RealShardTapeParity.
	if (RecycledIdCount == 0)
	{
		AddError(FString::Printf(
			TEXT("No transient shard-id RECYCLE occurred in %d recorded ticks, so the reuse path went ")
			TEXT("untested (observed %d distinct CombatShard ids carrying %d distinct CombatIds). ")
			TEXT("The window must exceed the ShardTimingWheel::StandardSize quarantine AND contain a ")
			TEXT("combat that completes and is followed by another standing up. Lengthen the window or ")
			TEXT("make the fixture churn combats rather than deleting this assertion."),
			RecordedTicks,
			CombatIdsByShardId.Num(),
			DistinctCombatsSeen));
		CleanupTape();
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("Observed %d recycled shard id(s) across %d distinct combats; first witness: shard id %d."),
		RecycledIdCount, DistinctCombatsSeen, RecycledShardId));

	// The payoff: a tape spanning the reuse must replay bit-identically.
	GalaxyDeterminism::FReplayRunConfig Config;
	Config.TapeFolder = TapeFolder;
	Config.Bootstrap = &BootstrapRealPlanetShard;

	const GalaxyDeterminism::FReplayResult Result = GalaxyDeterminism::FDeterminismReplayRunner::RunReplay(Config);

	if (!Result.Succeeded())
	{
		AddError(FString::Printf(
			TEXT("Recycle-spanning replay failed: status=%s error=%s"),
			GalaxyDeterminism::ReplayRunStatusToString(Result.Status),
			Result.Error.IsEmpty() ? TEXT("<none>") : *Result.Error));

		for (const GalaxyDeterminism::FReplayCheckpointMismatch& Mismatch : Result.Mismatches)
		{
			AddError(FString::Printf(
				TEXT("  tick %llu: registry expected=%llu actual=%llu (%d expected / %d actual shard checksums). ")
				TEXT("A mismatch HERE points at transient-id reuse: the release order in ")
				TEXT("CleanupCompletedCombatShards decides which id the next combat receives, and that id ")
				TEXT("feeds ShardSig."),
				Mismatch.Tick,
				Mismatch.ExpectedRegistryChecksum,
				Mismatch.ActualRegistryChecksum,
				Mismatch.ExpectedShardChecksums.Num(),
				Mismatch.ActualShardChecksums.Num()));

			// The counts alone never localized anything. Diff the per-shard lists and
			// NAME the shards that actually moved -- that is the whole reason
			// ComputeShardChecksum has per-shard overrides. A shard present on one side
			// only is reported too: an absent shard is a different (standup-ordering)
			// defect from a shard whose contents drifted, and the two look identical
			// when you only print totals.
			for (const GalaxyDeterminism::FTapeShardChecksum& Expected : Mismatch.ExpectedShardChecksums)
			{
				const GalaxyDeterminism::FTapeShardChecksum* Actual =
					Mismatch.ActualShardChecksums.FindByPredicate(
						[&Expected](const GalaxyDeterminism::FTapeShardChecksum& Candidate)
						{ return Candidate.ShardId == Expected.ShardId; });

				if (!Actual)
				{
					AddError(FString::Printf(
						TEXT("    shard %u: present in the tape, ABSENT on replay"), Expected.ShardId));
				}
				else if (Actual->Checksum != Expected.Checksum)
				{
					AddError(FString::Printf(
						TEXT("    shard %u: DIVERGED expected=%llu actual=%llu"),
						Expected.ShardId, Expected.Checksum, Actual->Checksum));
				}
			}
			for (const GalaxyDeterminism::FTapeShardChecksum& Actual : Mismatch.ActualShardChecksums)
			{
				const bool bInTape = Mismatch.ExpectedShardChecksums.ContainsByPredicate(
					[&Actual](const GalaxyDeterminism::FTapeShardChecksum& Candidate)
					{ return Candidate.ShardId == Actual.ShardId; });
				if (!bInTape)
				{
					AddError(FString::Printf(
						TEXT("    shard %u: stood up on replay, ABSENT from the tape"), Actual.ShardId));
				}
			}
		}
	}

	TestTrue(TEXT("Replay across a transient shard-id recycle succeeds"), Result.Succeeded());
	TestTrue(TEXT("Recycle replay verified at least one checkpoint"), Result.CheckpointsVerified > 0);
	TestEqual(TEXT("Recycle replay mismatch count is zero"), Result.Mismatches.Num(), 0);
	TestEqual(TEXT("Recycle replay simulated the recorded tick count"),
		Result.TicksSimulated, static_cast<uint64>(RecordedTicks));

	CleanupTape();

	RecordingScheduler->UnregisterShard(static_cast<FShardId>(GRealTapePlanetShardId));
	RecordingScheduler->FlushPendingShardOpsForTests();

	return Result.Succeeded();
}

// ============================================================================
// TWO registry-bearing shard TYPES in one tape, over a wider population.
// ============================================================================
// RealShardTapeParity above covers one registry-bearing shard type holding two units.
// TWO axes widen here, and they are different in kind:
//
//   SHARD TYPE. InterstellarSquadShard drives the same serialization spine
//   (entity table keyed on CGalaxyStableId, authoritative component payloads,
//   per-shard tape checksums) through a second type. A defect that lives in
//   "how a shard type is written" rather than "how serialization works" is
//   invisible to a single-type fixture. It also puts TWO shards in the
//   scheduler's registered set, so cross-shard ORDERING is exercised: the
//   checkpoint has to fold more than one registry-bearing shard's checksum in a
//   stable sequence.
//
//   POPULATION. Six planet units (two slots) and four squad units, against the
//   previous two. A 2-entity table is trivially ordered; an iteration-order
//   hazard needs enough entities that a permutation is actually reachable.
//
// WHAT IT ASSERTS, and deliberately does not. Same contract as its sibling:
// record through the PRODUCTION scheduler entry, replay into a fresh world,
// require bit-identical checkpoints. It does NOT pin entity counts, ids, or
// checksums -- those are outputs, and pinning them would turn every legitimate
// content change into a re-cut. It DOES assert that the checkpoint covers BOTH
// shards, because a tape that quietly dropped one would still replay perfectly
// while testing half of what the name claims.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMultiShardTapeParityTest,
	"GALAXY_PROJECT.Determinism.Replay.Functional.MultiShardTapeParity",
	GALAXY_TEST_FLAGS)

bool FMultiShardTapeParityTest::RunTest(const FString& Parameters)
{
	TestUtilities::FScopedTestIsolation Isolation(*this);
	(void)Parameters;

	TestUtilities::FScopedAutomationDirectory TapeDirScope(TEXT("MultiShardTapeParity"));
	DeterminismTapeFixtures::FScopedTapeRootOverride TapeRootOverride(FPaths::Combine(TapeDirScope.GetPath(), TEXT("DeterminismTapes")));
	FScopedLiveZonesGate LiveZones(1);

	TestUtilities::FScopedTestWorld RecordingWorld;
	UWorld* World = RecordingWorld.Get();
	if (!TestNotNull(TEXT("Recording world created"), World))
	{
		return false;
	}

	UGlobalShardScheduler* RecordingScheduler = World->GetSubsystem<UGlobalShardScheduler>();
	if (!TestNotNull(TEXT("Recording world provides UGlobalShardScheduler"), RecordingScheduler))
	{
		return false;
	}

	constexpr int32 RootSeed = 20260814;
	RecordingScheduler->SetDeterminismSeed(RootSeed);
	RecordingScheduler->SetExpectedDeterminismSeed(RootSeed);

	AActor* Host = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Planet host actor spawned"), Host))
	{
		return false;
	}

	TSharedPtr<PlanetShard> Planet =
		MakeShared<PlanetShard>(GRealTapePlanetShardId, TWeakObjectPtr<AActor>(Host));
	RecordingScheduler->RegisterShard(Planet);

	TSharedPtr<InterstellarSquadShard> Squad = MakeShared<InterstellarSquadShard>(
		static_cast<FShardId>(GRealTapeSquadShardId),
		static_cast<FShardId>(GRealTapeSquadParentZoneId),
		GRealTapeSquadId,
		static_cast<FShardId>(GRealTapePlanetShardId));
	RecordingScheduler->RegisterShard(Squad);

	RecordingScheduler->FlushPendingShardOpsForTests();

	PopulateWideTapeParityPlanet(*Planet);
	PopulateTapeParitySquad(*Squad);

	// Guards the fixture itself: if either populate helper silently stopped
	// producing entities, everything downstream would still pass while comparing
	// two empty registries.
	TestEqual(TEXT("Planet shard holds the wider population"), Planet->GetRegistryEntityCount(), 6);
	TestEqual(TEXT("Squad shard holds its population"), Squad->GetRegistryEntityCount(), 4);

	// --- Bootstrap snapshot: the replay world's starting state, both shards. ---
	TArray<uint8> SnapshotBytes;
	if (!RecordingScheduler->BuildECSSnapshotBinary(SnapshotBytes, false))
	{
		AddError(TEXT("BuildECSSnapshotBinary failed with a PlanetShard and an InterstellarSquadShard ")
			TEXT("registered; without a bootstrap snapshot the replay world cannot start from the recorded state."));
		return false;
	}
	TestTrue(TEXT("Bootstrap snapshot is non-empty"), SnapshotBytes.Num() > 0);

	const FString TapeId = FString::Printf(TEXT("automation_multishard_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString TapeFolder = FPaths::Combine(TapeDirScope.GetPath(), TEXT("DeterminismTapes"), TapeId);
	const FString SnapshotPath = FPaths::Combine(TapeFolder, TEXT("ECSSnapshot.bin"));
	const auto CleanupTape = [&TapeFolder]()
	{
		IFileManager::Get().DeleteDirectory(*TapeFolder, false, true);
	};
	CleanupTape();

	FOutputDeviceNull NullAr;
	if (!RecordingScheduler->StartTapeRecording(TapeId, NullAr))
	{
		AddError(TEXT("Failed to start tape recording for the multi-shard parity run."));
		CleanupTape();
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(SnapshotBytes, *SnapshotPath))
	{
		AddError(FString::Printf(TEXT("Failed to save the bootstrap snapshot: %s"), *SnapshotPath));
		RecordingScheduler->StopTapeRecording(NullAr);
		CleanupTape();
		return false;
	}

	// Production entry only -- Scheduler::Tick, never hand-called phases.
	const float FixedDt = RecordingScheduler->GetFixedDt();
	const int32 RecordedTicks = TickConstants::FramesPerMegaTick * 3;
	for (int32 TickIndex = 0; TickIndex < RecordedTicks; ++TickIndex)
	{
		RecordingScheduler->Tick(FixedDt);
	}

	const GalaxyDeterminism::FTapeCheckpointEntry RecordedEntry =
		RecordingScheduler->BuildTapeCheckpointEntry(
			static_cast<uint64>(RecordingScheduler->GetGlobalTick()),
			static_cast<uint64>(RecordingScheduler->GetDeterminismChecksum()));

	// BOTH, separately. A tape covering only the planet would replay perfectly
	// and prove nothing about the second shard type -- the false-green this test
	// exists to rule out.
	TestTrue(
		TEXT("Tape checkpoint covers the PlanetShard"),
		CheckpointCoversShard(RecordedEntry, static_cast<FShardId>(GRealTapePlanetShardId)));
	TestTrue(
		TEXT("Tape checkpoint covers the InterstellarSquadShard (2nd registry-bearing shard type)"),
		CheckpointCoversShard(RecordedEntry, static_cast<FShardId>(GRealTapeSquadShardId)));

	RecordingScheduler->StopTapeRecording(NullAr);

	// --- Replay into a fresh world and require bit-identical checkpoints. ---
	GalaxyDeterminism::FReplayRunConfig Config;
	Config.TapeFolder = TapeFolder;
	Config.Bootstrap = &BootstrapRealMultiShard;

	const GalaxyDeterminism::FReplayResult Result = GalaxyDeterminism::FDeterminismReplayRunner::RunReplay(Config);

	if (!Result.Succeeded())
	{
		for (const GalaxyDeterminism::FReplayCheckpointMismatch& Mismatch : Result.Mismatches)
		{
			AddError(FString::Printf(
				TEXT("Multi-shard replay mismatch at tick %llu: registry expected=%llu actual=%llu"),
				Mismatch.Tick,
				Mismatch.ExpectedRegistryChecksum,
				Mismatch.ActualRegistryChecksum));

			// Name the SHARD that diverged. With two registry-bearing shard types in
			// the tape, "the checksums differ" is not actionable on its own -- which
			// of the two drifted is the whole diagnostic value of this test.
			for (const GalaxyDeterminism::FTapeShardChecksum& Expected : Mismatch.ExpectedShardChecksums)
			{
				const GalaxyDeterminism::FTapeShardChecksum* Actual =
					Mismatch.ActualShardChecksums.FindByPredicate(
						[&Expected](const GalaxyDeterminism::FTapeShardChecksum& Candidate)
						{ return Candidate.ShardId == Expected.ShardId; });
				if (Actual == nullptr)
				{
					AddError(FString::Printf(
						TEXT("  shard %u present in the tape but ABSENT on replay"),
						static_cast<uint32>(Expected.ShardId)));
				}
				else if (Actual->Checksum != Expected.Checksum)
				{
					AddError(FString::Printf(
						TEXT("  shard %u diverged: expected=%llu actual=%llu"),
						static_cast<uint32>(Expected.ShardId),
						Expected.Checksum,
						Actual->Checksum));
				}
			}
		}
		if (!Result.Error.IsEmpty())
		{
			AddError(FString::Printf(TEXT("Multi-shard replay error: %s"), *Result.Error));
		}
	}

	TestTrue(TEXT("Two-shard-type replay succeeds"), Result.Succeeded());
	TestTrue(TEXT("Two-shard-type replay verified at least one checkpoint"), Result.CheckpointsVerified > 0);
	TestEqual(TEXT("Two-shard-type replay mismatch count is zero"), Result.Mismatches.Num(), 0);
	TestEqual(TEXT("Two-shard-type replay simulated the recorded tick count"),
		Result.TicksSimulated, static_cast<uint64>(RecordedTicks));

	CleanupTape();

	RecordingScheduler->UnregisterShard(static_cast<FShardId>(GRealTapeSquadShardId));
	RecordingScheduler->UnregisterShard(static_cast<FShardId>(GRealTapePlanetShardId));
	RecordingScheduler->FlushPendingShardOpsForTests();

	return Result.Succeeded();
}

// ============================================================================
// ENTITY-ORDER SHUFFLE PARITY -- what a tape test cannot see
// ============================================================================
// A tape test records world A and replays into world B. Both build their
// registries through paths that yield the SAME entt iteration order -- probing
// the live fixture logged the identical order (5,4,3,2,1,0) in all 40 sampled
// iterations across every recording AND replay world. So a genuine
// order-dependent bug computes the same wrong answer on both sides and the tape
// reports parity. Proven by mutation: folding each entity's VIEW POSITION into
// compared registry state left all four tape tests green, while folding a plain
// counter into the same field failed three of them. The net is sharp on values
// and blind on order.
//
// Blindness follows from the replay path being FAITHFUL, so more tape tests
// cannot fix it. Breaking the symmetry can: build the same logical state twice
// with the entity creation order REVERSED, tick both, and require identical
// checksums. Order-independent code is unaffected; anything that reads iteration
// order as if it were meaningful diverges immediately.
//
// WHY THE COMPARISON IS CLEAN (and not a false alarm generator). The
// authoritative registry serializer is CANONICAL by stable id, not by entt
// handle -- the entity table does `Ids.Sort()` and every component block goes
// through `CollectSorted<T>`. Two registries holding the same logical state
// therefore serialize to identical bytes no matter what order they were built
// in. A failure here is a real order dependency in the SIMULATION, never a
// serialization artifact. If that canonicalization is ever weakened this test
// starts failing for the wrong reason -- fix the serializer, do not relax this.
//
// THE FIXTURE GUARD IS LOAD-BEARING. The test asserts the two worlds actually
// iterate DIFFERENTLY before it compares anything. Without that, a future change
// making both orders identical again would leave this test passing while
// proving nothing.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEntityOrderShuffleParityTest,
	"GALAXY_PROJECT.Determinism.Replay.Functional.EntityOrderShuffleParity",
	GALAXY_TEST_FLAGS)

namespace
{
	/** Entt view order for the shard's stable-id'd units, as a readable signature. */
	FString CaptureViewOrder(PlanetShard& Planet)
	{
		FString Order;
		entt::registry& Registry = Planet.GetRegistry();
		auto View = Registry.view<const CGalaxyStableId>();
		for (const entt::entity Entity : View) // entt-order-safe: capturing the order IS the measurement
		{
			Order += FString::Printf(TEXT("%lld,"), View.get<const CGalaxyStableId>(Entity).Id);
		}
		return Order;
	}

	/**
	 * Same six units, same stable ids, same data -- built in the caller's order.
	 * Ascending vs descending is the entire difference between the two worlds.
	 */
	void PopulateShufflePlanet(PlanetShard& Planet, bool bDescending)
	{
		entt::registry& Registry = Planet.GetRegistry();
		for (int32 Step = 0; Step < 6; ++Step)
		{
			const int32 Index = bDescending ? (5 - Step) : Step;
			MakeSerializablePlanetUnit(
				Registry,
				/*StableId*/ 43100 + Index,
				/*EntityId*/ 20 + Index,
				/*Slot*/ (Index % 2) + 1,
				FVector3d(1.0, 0.01 * static_cast<double>(Index), 0.0));
		}
	}

	/** Build a world, populate in the given order, tick, and return its checksum. */
	uint64 RunShuffleWorld(
		FAutomationTestBase& Test, bool bDescending, int32 TickCount, FString& OutViewOrder, bool& bOutOk)
	{
		bOutOk = false;
		TestUtilities::FScopedTestWorld ScopedWorld;
		UWorld* World = ScopedWorld.Get();
		if (!Test.TestNotNull(TEXT("Shuffle world created"), World))
		{
			return 0;
		}

		UGlobalShardScheduler* Scheduler = World->GetSubsystem<UGlobalShardScheduler>();
		if (!Test.TestNotNull(TEXT("Shuffle world provides UGlobalShardScheduler"), Scheduler))
		{
			return 0;
		}

		// IDENTICAL seed in both worlds -- the creation order is the ONLY difference.
		constexpr int32 RootSeed = 20260814;
		Scheduler->SetDeterminismSeed(RootSeed);
		Scheduler->SetExpectedDeterminismSeed(RootSeed);

		AActor* Host = World->SpawnActor<AActor>();
		if (!Test.TestNotNull(TEXT("Shuffle planet host spawned"), Host))
		{
			return 0;
		}

		TSharedPtr<PlanetShard> Planet =
			MakeShared<PlanetShard>(GRealTapePlanetShardId, TWeakObjectPtr<AActor>(Host));
		Scheduler->RegisterShard(Planet);
		Scheduler->FlushPendingShardOpsForTests();

		PopulateShufflePlanet(*Planet, bDescending);
		Test.TestEqual(TEXT("Shuffle world holds the full population"), Planet->GetRegistryEntityCount(), 6);

		OutViewOrder = CaptureViewOrder(*Planet);

		// Production entry only -- Scheduler::Tick, never hand-called phases.
		const float FixedDt = Scheduler->GetFixedDt();
		for (int32 TickIndex = 0; TickIndex < TickCount; ++TickIndex)
		{
			Scheduler->Tick(FixedDt);
		}

		const uint64 Checksum = static_cast<uint64>(Scheduler->GetDeterminismChecksum());

		Scheduler->UnregisterShard(static_cast<FShardId>(GRealTapePlanetShardId));
		Scheduler->FlushPendingShardOpsForTests();

		bOutOk = true;
		return Checksum;
	}
}

bool FEntityOrderShuffleParityTest::RunTest(const FString& Parameters)
{
	TestUtilities::FScopedTestIsolation Isolation(*this);
	(void)Parameters;

	FScopedLiveZonesGate LiveZones(1);

	const int32 TickCount = TickConstants::FramesPerMegaTick * 3;

	bool bAscendingOk = false;
	FString AscendingOrder;
	const uint64 AscendingChecksum =
		RunShuffleWorld(*this, /*bDescending=*/false, TickCount, AscendingOrder, bAscendingOk);
	if (!bAscendingOk)
	{
		return false;
	}

	bool bDescendingOk = false;
	FString DescendingOrder;
	const uint64 DescendingChecksum =
		RunShuffleWorld(*this, /*bDescending=*/true, TickCount, DescendingOrder, bDescendingOk);
	if (!bDescendingOk)
	{
		return false;
	}

	// FIXTURE GUARD, checked BEFORE the comparison that matters. If both worlds
	// iterate the same way the shuffle did not happen, and a passing parity
	// assertion below would mean nothing at all.
	TestNotEqual(
		FString::Printf(
			TEXT("The two worlds must actually iterate differently (asc=%s desc=%s) -- ")
			TEXT("without a real permutation this test cannot detect an order dependency"),
			*AscendingOrder, *DescendingOrder),
		AscendingOrder, DescendingOrder);

	// Neither world may be empty: a zero checksum would compare equal to another
	// zero checksum and pass while simulating nothing.
	TestNotEqual(TEXT("Ascending world produced a checksum"), AscendingChecksum, static_cast<uint64>(0));
	TestNotEqual(TEXT("Descending world produced a checksum"), DescendingChecksum, static_cast<uint64>(0));

	// [law] THE CLAIM. Identical logical state, opposite construction order, same
	// seed and tick count -- the authoritative checksum must be bit-identical.
	// A failure names a real iteration-order dependency in the ticked simulation;
	// the registry serializer is canonical by stable id, so it cannot be an
	// artifact of how the two registries were laid out.
	TestEqual(
		FString::Printf(
			TEXT("Reversing entity creation order must not change the authoritative checksum ")
			TEXT("(asc=%llu desc=%llu). A tape test cannot see this class of bug."),
			AscendingChecksum, DescendingChecksum),
		AscendingChecksum, DescendingChecksum);

	return AscendingChecksum == DescendingChecksum;
}

#endif // WITH_DEV_AUTOMATION_TESTS
