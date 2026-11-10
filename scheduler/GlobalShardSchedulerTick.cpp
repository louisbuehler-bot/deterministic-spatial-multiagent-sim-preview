// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
//
// THE TICK SPINE -- the deterministic game timeline.
//
// This TU holds ONLY the ordering control flow, extracted from
// GlobalShardScheduler.cpp so the determinism contract is one small, auditable
// unit. The "what each phase does" bodies (Phase_*_All) are
// GlobalShardSchedulerPhases.cpp (the table of the scheduler's TUs is at the top of
// GlobalShardScheduler.cpp); this file is the "in what order it all happens" surface:
//
//   Tick()               -- variable-rate frame adapter: wall-clock DeltaTime ->
//                           N fixed steps (spiral-of-death capped).
//   StepOneFixedTick()   -- one deterministic fixed step (membership snapshot,
//                           read-buffer freeze, phase run, boundary cascade,
//                           intent resolve, ++GlobalTick).
//   ProcessFrozenTickPath() -- reduced step during cosmic realignment freeze.
//   RunNormalFramePhases()  -- the per-frame phase sequence (Input -> Derive
//                           -> ClusterPreSim -> LocalSim -> Collect -> Orchestrate
//                           -> Dispatch -> ResolveDue -> BuildSnapshots -> Viz).
//   ProcessBoundaryTicks()  -- the Big/Super/MegaTick/Ultra/Omega cascade.
//   VerifyTickInvariants()  -- development guard that StepShards is in ShardId
//                           order, run once straight after the sort.
//
// ============================================================================
#include "CoreMinimal.h" // FIRST, not the scheduler header: see the TU table in GlobalShardScheduler.cpp
#include "GlobalShardScheduler.h"
#include "GlobalShardSchedulerLog.h" // GSS_VLOG_THIS / GSS_TLOG_THIS
#include "AttachedZoneManager.h" // ProcessBoundaryTicks flags MarkZoneRecalcDirty()
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "ECS/Systems/Core/GalaxyECSSubsystem.h"
#include "Logging/GalaxyLogThrottle.h" // GalaxyLog::ShouldLogThisTick
#include "PlayerCardDraftingManager.h" // IWYU pragma: keep -- DraftMgr-> dereferenced below; the TUniquePtr member is forward-declared in the scheduler header, so this TU needs the COMPLETE type while never naming it
#include "ECS/Validation/ECSValidation.h"
#include "Numeric/GalaxyDeterministicFP.h" // FGalaxyDeterministicFPScope -- pin MXCSR for the authoritative step
#include "DataRegistry/SUDistanceWorldScale.h" // Tick() refreshes the world<->SU-Distance snapshot before any phase launches
// Tape/deterministic-replay engines are TUniquePtr<incomplete> in the header; the spine
// dereferences them (InjectForTick / FlushTick), so it needs the full types.
#include "Infrastructure/Determinism/TapeEventRecorder.h"
#include "Infrastructure/Determinism/ReplayEventInjector.h" // IWYU pragma: keep -- Injector-> dereferenced below; the TUniquePtr member is forward-declared in the scheduler header, so this TU needs the COMPLETE type while never naming it
#include "EntityTransferCoordinator.h" // IWYU pragma: keep -- TransferCoordinator->ProcessTransfers() dereferenced below; same forward-declared-member shape as the two above

// CVarGSS_MaxSteps has external linkage (defined in GlobalShardScheduler.cpp); the
// spine re-reads it each frame for live catch-up tuning. FixedDt is intentionally
// NOT extern'd here -- it is read once at Initialize() to keep the sim timestep fixed
// for the whole session (replay determinism), so it stays static in the main TU.
extern TAutoConsoleVariable<int32> CVarGSS_MaxSteps;
extern TAutoConsoleVariable<float> CVarGSS_SlowStepWarnMs;
extern TAutoConsoleVariable<float> CVarGSS_SpikeStepWarnMs;

void UGlobalShardScheduler::Tick(float DeltaTime)
{
	UWorld* W = GetWorld();
	if (!W) return;

	// Snapshot the world-units-per-SU-Distance factor every simulation reader uses, HERE: game thread,
	// before any phase ParallelFor launches (fights stand up on worker threads, and the Developer tab
	// writes the registry without a lock). Ahead of the client early-out so display readers get it too.
	SUDistanceWorldScale::RefreshFromRegistry();

    // Only the server advances the authoritative sim.
	if (W->GetNetMode() == NM_Client)
	{
		return;
	}

    // Sync the ECS world context (World ptr + CurrentGlobalTick) once per frame so
    // that bridges and systems that read FECSWorldContext::World for UI event dispatch
    // receive a valid pointer rather than the null initialized at startup.
	if (UGameInstance* GI = W->GetGameInstance())
	{
		if (UGalaxyECSSubsystem* ECS = GI->GetSubsystem<UGalaxyECSSubsystem>())
		{
			ECS->UpdateCurrentGlobalTick(static_cast<uint32>(GlobalTick));
		}
	}

    // Pull runtime tunable. NOTE: FixedDt is deliberately NOT re-read here -- it is
    // read once at Initialize() because it feeds the sim timestep; changing it
    // mid-session would diverge replays. (Cadence periods are compile-time
    // TickConstants.) MaxSteps only caps per-frame catch-up (never per-step
    // outcome), so live tuning stays safe.
	// Game-thread read: this is Tick() itself, the function that LAUNCHES
	// the phase ParallelFors -- it is never inside one.
	MaxStepsPerFrame = FMath::Max(1, CVarGSS_MaxSteps.GetValueOnGameThread()); // thread-safe-verified -- Tick() is the UTickableWorldSubsystem game-thread override; MaxStepsPerFrame is written/read only on that thread

    // SIMULATION-time vs REAL-WORLD-time bridge. FixedDt is a constant SIMULATION-second quantum
    // (SUTime::SecondsPerFrame -- 1/64 sim-s by default, and "Frame" here means one SIM STEP, not a
    // render frame); it is NOT real-world seconds. We accumulate the engine's variable real-world
    // DeltaTime, then spend it in whole FixedDt steps -- so the step SIZE never changes (determinism),
    // only the NUMBER of steps per frame does. This TRIES to keep sim-time tracking wall-clock time,
    // but does not force it: when the sim cannot keep up, the MaxStepsPerFrame cap below leaves the
    // remainder in the Accumulator -> fewer sim-seconds advance per real second -> graceful slow-mo
    // (sim-time lags wall-clock), never a stretched/distorted step. Render fps is only the
    // sampling heartbeat, never a sim input.
	Accumulator += FMath::Max(0.f, DeltaTime); // deltatime-safe -- engine Tick() override, converted to FixedDt steps below

    // Stall guard: cap banked wall-clock debt at a few catch-up frames so one long hitch
    // (debugger pause, blocking load, OS stall) cannot force minutes of forced catch-up
    // grind. Determinism-safe -- it changes how MANY steps run, never a step's outcome.
    // (Full rationale + the drop telemetry/log live in ClampBankedSimDebt.)
	ClampBankedSimDebt();

    // Step-duration watchdog: completes the overload story -- sim-time DEBT is
    // exposed via GetSimTimeDebtSeconds(); this names the CAUSE when individual
    // steps run hot. Wall-clock measurement is diagnostic only (never feeds sim).
	// Both traced game-thread, same reason as MaxStepsPerFrame above: still in Tick(), outside
	// every phase ParallelFor. Hoisted here rather than read per step, which is also what keeps
	// one frame's watchdog thresholds internally consistent.
	const float SlowStepWarnMs = CVarGSS_SlowStepWarnMs.GetValueOnGameThread(); // thread-safe-verified -- game-thread Tick(), above every phase ParallelFor; SlowStepWarnMs is a stack local passed by value to the watchdog
	const float SpikeStepWarnMs = CVarGSS_SpikeStepWarnMs.GetValueOnGameThread(); // thread-safe-verified -- game-thread Tick(), above every phase ParallelFor; SpikeStepWarnMs is a stack local passed by value to the spike log

#if !(UE_BUILD_SHIPPING)
	const bool bCollectStepTelemetry = bCollectShardPerf.load(std::memory_order_relaxed);
#else
	const bool bCollectStepTelemetry = false;
#endif
	const bool bMeasureSteps = (SlowStepWarnMs > 0.0f) || (SpikeStepWarnMs > 0.0f) || bCollectStepTelemetry;

	int32 StepsThisFrame = 0;
	while (Accumulator >= FixedDt && StepsThisFrame < MaxStepsPerFrame)
	{
		Accumulator -= FixedDt;
		const uint64 StepTick = static_cast<uint64>(GlobalTick); // tick this step simulates (pre-increment)
		const double StepStartSeconds = bMeasureSteps ? FPlatformTime::Seconds() : 0.0; // wallclock-safe -- watchdog diagnostics only
		StepOneFixedTick();
		++StepsThisFrame;

		if (bMeasureSteps)
		{
			const double StepMs = (FPlatformTime::Seconds() - StepStartSeconds) * 1000.0; // wallclock-safe -- watchdog diagnostics only
			ReportFixedStepDuration(StepTick, StepMs, SlowStepWarnMs, SpikeStepWarnMs);
		}
	}

#if !(UE_BUILD_SHIPPING)
	if (Accumulator >= FixedDt && StepsThisFrame >= MaxStepsPerFrame)
	{
		UE_LOG(LogGlobalShardScheduler, Verbose, TEXT("[GSS] Step cap hit. Acc=%.3f Step=%.3f"), Accumulator, FixedDt); // tick-log-safe: fires only on fixed-step cap-hit (overload indicator); Verbose + !UE_BUILD_SHIPPING
	}

    // Draw zone debug visualization (controlled by FDebugLoggingConfig toggles)
	DrawZoneDebug(W);
#endif
}

// STALL GUARD. A single long hitch (debugger pause,
// blocking load, OS stall) can bank many seconds of debt at once; without a cap the sim then
// grinds at MaxStepsPerFrame for minutes of forced catch-up. Sustained mild overload still
// degrades to slow-mo as designed -- only debt beyond a few full catch-up frames is discarded,
// and the drop is counted (GetDroppedSimTimeSeconds) and logged (throttled <=1/sec) so it stays
// visible to telemetry. Dropping wall-clock debt is deterministic-safe: it changes how many steps
// run, never a step's outcome. Runs AFTER MaxStepsPerFrame is refreshed for the frame.
void UGlobalShardScheduler::ClampBankedSimDebt()
{
	const float MaxBankedSimSeconds = FixedDt * static_cast<float>(MaxStepsPerFrame) * 4.0f;
	if (Accumulator <= MaxBankedSimSeconds)
	{
		return;
	}

	const float Dropped = Accumulator - MaxBankedSimSeconds;
	DroppedSimTimeSeconds += Dropped;
	Accumulator = MaxBankedSimSeconds;

#if !(UE_BUILD_SHIPPING)
    // Step-spike telemetry: keep the tick identity of each drop so harness
    // runs can correlate drops with boundary cadences (window-gated, capped).
	if (bCollectShardPerf.load(std::memory_order_relaxed))
	{
		constexpr int32 MaxStallDropSamples = 64;
		if (StallDropSamples.Num() >= MaxStallDropSamples)
		{
			StallDropSamples.RemoveAt(0, 1, EAllowShrinking::No);
		}
		StallDropSamples.Add({ static_cast<uint64>(GlobalTick), Dropped });
	}
#endif

	if (GalaxyLog::ShouldLogThisTick(true, static_cast<uint64>(GlobalTick), LastStallDropLogTick,
			static_cast<uint64>(TickConstants::FramesPerSuperTick)))
	{
		UE_LOG(LogGlobalShardScheduler, Warning,
			TEXT("[GlobalShardScheduler][StallGuard] Dropped %.1fms of banked sim debt after a stall (session total %.2fs, Tick=%u)"),
			Dropped * 1000.0f, DroppedSimTimeSeconds, (uint32)GlobalTick);
	}
}

void UGlobalShardScheduler::StepOneFixedTick()
{
	// Pin the SSE control word (FTZ + DAZ + round-to-nearest-even)
	// for the whole authoritative fixed step, restoring the caller's prior word on
	// return. This guards against engine/render/middleware threads leaving a
	// divergent denormal/rounding mode in the thread-local MXCSR, which could make
	// otherwise-identical binaries produce bit-divergent floats -> desync. Scoped
	// at the step boundary so every entry (live Tick loop, replay runner, tests)
	// and every early-return path (e.g. the frozen-tick path) is covered.
	// MXCSR is per thread, so this covers only the thread running the step: each phase
	// ParallelFor pins its own workers (RunPhaseParallel_Impl, Phase_LocalSim_All).
	const FGalaxyDeterministicFPScope DeterministicFPScope;

	ApplyPendingShardOps();
    // Promote any transient-shard IDs whose quarantine has elapsed back into the reuse pool.
	DrainTransientShardIdReclaim();

    // Build per-step snapshot (strong refs) so membership is stable for the whole step.
    // Always sort after rebuilding: StepShards is reconstructed from TMap each tick,
    // so the previous tick's sorted order is not preserved. Sorting ~10-20 shards
    // is negligible compared to the phases that follow.
	StepShards.Reset();
	StepShards.Reserve(Shards.Num());
	for (auto& KV : Shards) { StepShards.Add(KV.Value); } // tmap-order-safe -- sorted by ShardId below
	StepShards.Sort([](const TSharedPtr<IShardBridge>& A, const TSharedPtr<IShardBridge>& B)
	{
		const int32 IdA = A.IsValid() ? A->GetShardId() : MAX_int32;
		const int32 IdB = B.IsValid() ? B->GetShardId() : MAX_int32;
		return IdA < IdB;
	});
	VerifyTickInvariants(TEXT("AfterSort"));
#if !(UE_BUILD_SHIPPING)
    // Arm per-system collection every tick, not when the switch is thrown: a shard can still be
    // in PendingRegisterShards at that moment, and arming once would miss it. Re-arming is
    // idempotent (it sets a bool and leaves the sink to the end-of-step drain).
	if (bCollectECSPerf.load(std::memory_order_relaxed))
	{
		for (const TSharedPtr<IShardBridge>& Shard : StepShards)
		{
			if (Shard.IsValid())
			{
				Shard->SetSystemPerfCollectionEnabled(true);
			}
		}
	}
#endif

    // Every phase ParallelFor iterates the partition, not StepShards -- rebuild it in
    // lockstep. Must follow the Sort: group membership order inherits StepShards order.
	BuildRegistryExclusionGroups();
	GSS_VLOG_THIS("GSS Tick=%u StepShards=%d", (uint32)GlobalTick, StepShards.Num());

#if !(UE_BUILD_SHIPPING)
	if (bGSSTrace.load(std::memory_order_relaxed))
	{
		FString Ids; Ids.Reserve(StepShards.Num() * 6);
		for (const TSharedPtr<IShardBridge>& S : StepShards)
		{
			if (S.IsValid())
			{
				Ids += FString::Printf(TEXT("%u,"), (uint32)S->GetShardId());
			}
		}
		GSS_TLOG_THIS("GSS Tick=%u SnapshotShardIds=[%s]", (uint32)GlobalTick, *Ids);
	}
#endif

    // Freeze per-tick read snapshot for all shards
	GlobalRef.BeginTick();

    // --- Per-tick budget reset (must happen every fixed step) ---
	for (TPair<FName, int32>& It : PerTagIntentCountsThisTick)
	{
		It.Value = 0;
	}

#if !(UE_BUILD_SHIPPING)
    // Sample storage-backed debug flags for this tick
	{
		auto R = GlobalRef.Read();

		const bool v = R.GetBool(DebugKeys::GSSVerbose(), false);
		const bool t = R.GetBool(DebugKeys::GSSTrace(), false);
		bGSSVerbose.store(v, std::memory_order_relaxed);
		bGSSTrace.store(t, std::memory_order_relaxed);

		const bool PS_V = R.GetBool(DebugKeys::PSVerbose(), false);   // or GlobalKeys::DToggle(TEXT("PlanetShardVerbose"))
		const bool PS_T = R.GetBool(DebugKeys::PSTrace(), false);   // or GlobalKeys::DToggle(TEXT("PlanetShardTrace"))
		const int32 WarnB = R.GetInt(DebugKeys::PayloadWarnB(), 0);   // or GlobalKeys::DThreshold(TEXT("ECSMessagePayloadWarnBytes"))

		const bool SP_V = R.GetBool(DebugKeys::SPVerbose(), false);
		const bool SP_T = R.GetBool(DebugKeys::SPTrace(), false);

		for (const TSharedPtr<IShardBridge>& S : StepShards)
		{
			if (!S.IsValid()) continue;
            // Use virtual methods on IShardBridge - works for all shard types
			S->SetLogConfig(PS_V, PS_T, WarnB);
			S->SetPlannerLog(SP_V, SP_T);
		}
	}
#endif

    // =========================================================================
    // FRAME TICK PHASES (run every fixed step)
    // =========================================================================

    // -------------------------------------------------------------------------
    // COSMIC REALIGNMENT FREEZE CHECK
    // -------------------------------------------------------------------------
    // During realignment, skip gameplay phases but continue:
    //   - Input processing (so UI remains responsive)
    //   - Visualization updates (so players see planet movement)
    //   - Snapshot building (so render state stays valid)
    //
    // When frozen, we tick the CosmicRealignmentCoordinator instead.
    // -------------------------------------------------------------------------

	if (ProcessFrozenTickPath())
	{
		return;
	}

    // -------------------------------------------------------------------------
    // NORMAL GAMEPLAY PHASES (when not frozen)
    // -------------------------------------------------------------------------
	RunNormalFramePhases();

    // =========================================================================
    // TICK HIERARCHY BOUNDARY PROCESSING
    // =========================================================================
    // Boundaries are checked BEFORE incrementing counters so that:
    //   - Frame 0 is Big Tick 0, Super Tick 0, MegaTick 0 (all boundaries fire on first frame)
    //   - Counters represent "how many complete intervals have passed"

	ProcessBoundaryTicks();

    // Close out the fixed step (single source of truth -- see EndFixedStep).
	EndFixedStep();
}

void UGlobalShardScheduler::EndFixedStep()
{
    // Drop per-step refs; shards removed this frame will now be destroyed if no other owners remain.
	StepShards.Reset();
    // Indices into an emptied array. Clear together so nothing can dispatch off a stale partition.
	StepShardGroups.Reset();

    // Merge intents deterministically into the Next buffer (visible next tick).
	GlobalRef.EndTickResolve();

    // Player draft pass: per-tick cooldowns; independent of gameplay-frozen state.
	if (DraftMgr)
	{
		DraftMgr->OnEndOfGlobalTick(GlobalTick);
	}

    // Advance authoritative time.
	++GlobalTick;
}

// Development guard on the order every phase relies on. StepShards is written only by the per-tick
// rebuild, and Shards only by ApplyPendingShardOps before it, so one check after the sort covers
// the whole step.
void UGlobalShardScheduler::VerifyTickInvariants(const TCHAR* StageTag)
{
#if DO_CHECK
	checkf(StepShards.Num() <= Shards.Num(), TEXT("[GlobalShardScheduler][%s] StepShards exceeds Shards size (%d > %d)"),
		StageTag, StepShards.Num(), Shards.Num());

	int32 LastId = MIN_int32;
	for (const TSharedPtr<IShardBridge>& Shard : StepShards)
	{
		if (!Shard.IsValid())
		{
			continue;
		}

		const int32 CurrentId = static_cast<int32>(Shard->GetShardId());
		checkf(CurrentId >= LastId,
			TEXT("[GlobalShardScheduler][%s] StepShards is not sorted by ShardId (%d after %d)"),
			StageTag, CurrentId, LastId);
		LastId = CurrentId;
	}
#endif
}

bool UGlobalShardScheduler::ProcessFrozenTickPath()
{
	if (!bGameplayFrozen)
	{
		return false;
	}

    // Determinism replay injection -- mirrors RunNormalFramePhases. LocalSim still
    // runs while frozen, so a tape recorded across a freeze carries events for these
    // ticks; skipping injection here would silently drop them on replay.
	if (bReplayingTape && ReplayInjector.IsValid())
	{
		const int32 Injected = ReplayInjector->InjectForTick(static_cast<uint64>(GlobalTick));
		(void)Injected; // logged per-record inside injector on warning/error only
	}

    // Input: UI remains responsive
	Phase_Input_All();

    // Local sim stays active during gameplay freeze. That keeps planet surface
    // motion responsive, lets fleet shards apply anchor/drift behavior, and lets
    // the scheduler-owned CosmicRealignmentShard continue advancing the realignment
    // coordinator. Cross-shard message routing is still paused -- outboxes
    // accumulate and flush on the first normal tick after realignment ends.
	Phase_LocalSim_All();

    // Big Tick boundary: card cooldowns tick down even during realignment.
    // Same wall-of-time resync as the normal path (ProcessBoundaryTicks).
	if (IsBigTickBoundary())
	{
		BigTickCounter = GlobalTick / static_cast<FGlobalTick>(TickConstants::FramesPerBigTick);
		Phase_CardSystems_BigTick();
		++BigTickCounter;
	}

    // Snapshots for rendering
	Phase_BuildSnapshots_All();

    // Flush the tape event recorder for this tick -- mirrors RunNormalFramePhases.
    // Without this, events queued by the still-running LocalSim during a freeze
    // would stage unflushed and get stamped onto the next normal tick.
	if (bRecordingTape && TapeEventRecorder.IsValid())
	{
		TapeEventRecorder->FlushTick(static_cast<uint64>(GlobalTick));
	}

    // Visualization delegate
	Phase_Visualization();

    // Derive, message routing, strategic, zone recalc, entity transfers, and
    // SuperTick/MegaTick/UltraTick/OmegaTick counters are all frozen. Close out the
    // fixed step exactly as the normal path does (single source of truth -- the
    // draft-cooldown pass runs inside EndFixedStep, independent of freeze state).
	EndFixedStep();
	return true;
}

void UGlobalShardScheduler::RunNormalFramePhases()
{
    // 0) Determinism replay injection -- emit replayed events before input.
	if (bReplayingTape && ReplayInjector.IsValid())
	{
		const int32 Injected = ReplayInjector->InjectForTick(static_cast<uint64>(GlobalTick));
		(void)Injected; // logged per-record inside injector on warning/error only
	}

    // 1) Local Input (parallel) - process player/AI input
	Phase_Input_All();
	RunECSValidation(ECSValidation::EECSValidationPhase::Phase_Input);

    // 2) Derive (parallel) - recompute per-step derived state
	Phase_Derive_All();
	RunECSValidation(ECSValidation::EECSValidationPhase::Phase_Derive);

    // 2.5) Cluster pre-sim snapshot (single-threaded) - cross-zone visibility
	Phase_ClusterPreSim();

    // 3) Local Simulation (parallel) - physics, movement, continuous updates
    //    Note: Shards internally gate combat to Big/Super Tick boundaries
	Phase_LocalSim_All();
	RunECSValidation(ECSValidation::EECSValidationPhase::Phase_LocalSim);

    // 4) Collect outboxes (barrier)
	TArray<FShardMessage> Collected;
	Phase_CollectOutboxes_All(Collected);
	StatTotalCollected += Collected.Num();
	GSS_TLOG_THIS("GSS Tick=%u CollectedOutboxMsgs=%d", (uint32)GlobalTick, Collected.Num());

	int32 InjectN = 0;

    // Include externally injected messages (tests/tools)
	if (!InjectedMessages.IsEmpty())
	{
		InjectN = InjectedMessages.Num();
		GSS_TLOG_THIS("GSS Tick=%u InjectedExternalMsgs=%d", (uint32)GlobalTick, InjectN);
		Collected.Append(InjectedMessages);
		InjectedMessages.Reset();
	}
	StatTotalInjected += InjectN;

    // 5) Cross-orchestration hook (domains/tokens/ETAs)
	Phase_Orchestrate_CrossInteractions(Collected);

    // 6) Dispatch to destinations (barrier)
	Phase_Dispatch_ToDestinations(Collected);

    // 7) Shards resolve only messages due at GlobalTick (parallel)
	Phase_ResolveDue_All();
	RunECSValidation(ECSValidation::EECSValidationPhase::Phase_ResolveDue);

    // 8) Snapshots (parallel)
	Phase_BuildSnapshots_All();
	RunECSValidation(ECSValidation::EECSValidationPhase::Phase_BuildSnapshot);

    // Flush the tape event recorder's staging buffer after all shards have
    // completed Phase_BuildSnapshot (all Queued* events for this tick are settled).
	if (bRecordingTape && TapeEventRecorder.IsValid())
	{
		TapeEventRecorder->FlushTick(static_cast<uint64>(GlobalTick));
	}

    // 9) Visualization phase - fires delegate for overlay/UI updates
	Phase_Visualization();
}

void UGlobalShardScheduler::ProcessBoundaryTicks()
{
	AttachedZoneMgr->MarkZoneRecalcDirty();

    // WALL-OF-TIME COUNTER MODEL: each counter is RESYNCED from GlobalTick at its
    // boundary before events fire (see the TICK HIERARCHY comment in the header).
    // Boundaries skipped by a freeze lose their WORK, never their NUMBER -- "MegaTick N"
    // always means the boundary at GlobalTick == N * FramesPerMegaTick, in live play
    // and replay logs alike.

    // --- Big Tick (every TickConstants::FramesPerBigTick frames) ---
	const bool bIsBigTick = IsBigTickBoundary();
	if (bIsBigTick)
	{
		BigTickCounter = GlobalTick / static_cast<FGlobalTick>(TickConstants::FramesPerBigTick);

        // Card cooldowns tick at Big Tick cadence
		Phase_CardSystems_BigTick();

        // Refresh cached squad capability profiles for bid scoring
		RefreshSquadCapabilityCaches();

        // Counter = completed intervals
		++BigTickCounter;
	}

    // --- Super Tick (every TickConstants::FramesPerSuperTick frames) ---
	const bool bIsSuperTick = IsSuperTickBoundary();
	if (bIsSuperTick)
	{
		SuperTickCounter = GlobalTick / static_cast<FGlobalTick>(TickConstants::FramesPerSuperTick);

        // Combat resolution happens at Super Tick cadence
        // (Shards handle this in Phase_LocalSim via IsSuperTickBoundary check)

        // Zone recalculation: detect bubble overlaps and manage MergeZones
        // Only runs if any shard flagged a recalc request (position change, bubble resize)
		if (AnyZoneRecalcRequestPending())
		{
			Phase_ZoneRecalculation_All();
		}

		++SuperTickCounter;
	}

    // --- MegaTick (every TickConstants::FramesPerMegaTick frames) ---
	const bool bIsMegaTick = IsMegaTickBoundary();
	if (bIsMegaTick)
	{
		MegaTickCounter = GlobalTick / static_cast<FGlobalTick>(TickConstants::FramesPerMegaTick);

        // Notify native subscribers of the MegaTick boundary
		FireMegaTickBoundaryEvents();

        // Strategic phase runs at MegaTick boundaries
		Phase_Strategic_All();
		RunECSValidation(ECSValidation::EECSValidationPhase::Phase_Strategic);

        // Process pending entity transfers (one per MegaTick for determinism)
		TransferCoordinator->ProcessTransfers();

        // Compute determinism checksum at MegaTick boundary for replay verification
		ComputeDeterminismChecksum();

        // Deep clean maintenance (safety cleanup before next tick)
		Phase_DeepCleanMaintenance_MegaTick();

        // Shrink PerTagIntentCountsThisTick if it has accumulated many stale keys
		if (PerTagIntentCountsThisTick.Num() > 64)
		{
			PerTagIntentCountsThisTick.Empty();
		}

        // Increment AFTER all MegaTick processing
		++MegaTickCounter;

        // --- UltraTick (every TickConstants::MegaTicksPerUltraTick MegaTicks) ---
        // GlobalTick-anchored predicate (every UltraTick boundary tick is also a MegaTick
        // boundary tick), so this agrees with the static TickBoundaries:: helpers and
        // fires on frame 0 like Big/Super/MegaTick -- position relative to ++MegaTickCounter
        // above no longer matters.
		if (IsUltraTickBoundary())
		{
			UltraTickCounter = GlobalTick / static_cast<FGlobalTick>(TickConstants::FramesPerUltraTick);
			FireUltraTickBoundaryEvents();
			++UltraTickCounter;
		}

        // --- OmegaTick (every TickConstants::MegaTicksPerOmegaTick MegaTicks) ---
		if (IsOmegaTickBoundary())
		{
			OmegaTickCounter = GlobalTick / static_cast<FGlobalTick>(TickConstants::FramesPerOmegaTick);
			FireOmegaTickBoundaryEvents();
			++OmegaTickCounter;
		}
	}
}
