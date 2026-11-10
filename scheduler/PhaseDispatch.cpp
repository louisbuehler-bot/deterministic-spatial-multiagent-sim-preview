// Excerpt from a larger file: how one phase runs across worker threads.

// -- Registry-exclusion partition: the precondition every phase ParallelFor rests on --
//
// One group per distinct registry owner. Shards that can write the same entt::registry
// share a group and therefore run serially; groups run concurrently. Built once per tick
// off the already-sorted StepShards, so both the group order (first-seen owner) and the
// within-group order (ShardId ascending) are deterministic.
//
// Over-grouping costs parallelism; under-grouping is a data race. When a shard's answer is
// uncertain, GetRegistryOwnerShardId is expected to over-approximate -- CombatShard does.
void UGlobalShardScheduler::BuildRegistryExclusionGroups()
{
    // The partition itself is a pure function of IShardBridge and lives beside that
    // contract (ShardBridgeInterface.h) so it can be tested without a scheduler.
    // StepShards is sorted by ShardId here -- VerifyTickInvariants enforces it -- which
    // is what makes the serialized within-group order deterministic.
	StepShardGroups = ShardScheduling::BuildRegistryExclusionGroups(StepShards);

#if !(UE_BUILD_SHIPPING)
	if (bGSSTrace.load(std::memory_order_relaxed))
	{
		UE_LOG(LogGlobalShardScheduler, VeryVerbose,
			TEXT("[GlobalShardScheduler] GSS Tick=%u RegistryExclusionGroups=%d over %d shards"),
			(uint32)GlobalTick, StepShardGroups.Num(), StepShards.Num());
	}
#endif
}

// -- Shared helper: ParallelFor over registry-exclusion groups, per-shard perf collection --
//
// Parallelism is across GROUPS, not shards. Two shards share a group exactly when they
// can write the same entt::registry (IShardBridge::GetRegistryOwnerShardId), so a group
// is the unit within which concurrency would be a data race. Members run serially,
// registry owner FIRST, then borrowers in StepShards order -- owner-first is a rule,
// not an accident of ShardId comparison, because combat ids are transient and recycled
// and would otherwise flip the parent-vs-combat order on id reuse. See
// BuildRegistryExclusionGroups.
template<typename FPhaseCall, typename FStatsMerge>
void UGlobalShardScheduler::RunPhaseParallel_Impl(
	FGlobalTick PhaseTick,
	FPhaseCall&& PhaseCall,
	FStatsMerge&& StatsMerge,
	const TCHAR* PhaseName)
{
	const int32 N = StepShards.Num();
	if (N == 0) return;

	const int32 GroupCount = StepShardGroups.Num();
	if (GroupCount == 0) return;

#if !(UE_BUILD_SHIPPING)
	const bool bCollectPerf = bCollectShardPerf.load(std::memory_order_relaxed);
	TArray<float> PerShardMs;
	if (bCollectPerf) { PerShardMs.SetNumZeroed(N); }
	const uint64 BatchStartCycles = bCollectPerf ? FPlatformTime::Cycles64() : 0ull; // wallclock-safe -- batch start mark under bCollectPerf; its delta DECIDES nothing, it is only reported
#endif

	ParallelFor(GroupCount, [this // thread-safe-verified -- groups partition registry ownership; PerShardMs writes hit distinct pre-sized indices
#if !(UE_BUILD_SHIPPING)
		, bCollectPerf, &PerShardMs
#endif
		, PhaseTick, &PhaseCall](int32 g)
		{
			// MXCSR is per thread: the pin StepOneFixedTick holds covers only the thread running the
			// step, so every thread a group lands on pins its own, and where it ran changes no bit.
			const FGalaxyDeterministicFPScope DeterministicFPScope;
			for (const int32 i : StepShardGroups[g])
			{
				const TSharedPtr<IShardBridge>& Shard = StepShards[i];
				if (Shard.IsValid())
				{
#if !(UE_BUILD_SHIPPING)
					const uint64 StartCycles = bCollectPerf ? FPlatformTime::Cycles64() : 0ull; // wallclock-safe -- perf start mark under bCollectPerf; its delta DECIDES only the hottest-shard id named in the step-spike warning
#endif
					PhaseCall(*Shard, PhaseTick);
#if !(UE_BUILD_SHIPPING)
					if (bCollectPerf)
					{
						PerShardMs[i] = static_cast<float>(FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles)); // wallclock-safe -- elapsed into PerShardMs then ShardPerfStats; DECIDES only which shard GetHottestShard names in the watchdog log
					}
#endif
				}
			}
		});

#if !(UE_BUILD_SHIPPING)
	const double WallMs = bCollectPerf ? FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - BatchStartCycles) : 0.0; // wallclock-safe -- batch elapsed under bCollectPerf; DECIDES nothing, it goes only into the perf report and the trace log
	double WorkMs = 0.0;
	for (float V : PerShardMs) { WorkMs += V; }

	if (bCollectPerf)
	{
		RecordPhaseMsSample(FName(PhaseName), static_cast<float>(WorkMs));
		RecordPhaseParallelism(FName(PhaseName), WorkMs, WallMs, GroupCount);
	}

	if (bCollectPerf)
	{
		for (int32 i = 0; i < N; ++i)
		{
			if (StepShards[i].IsValid())
			{
				StatsMerge(StepShards[i]->GetShardId(), PerShardMs[i]);
			}
		}
	}

	if (bGSSTrace.load(std::memory_order_relaxed))
	{
		UE_LOG(LogGlobalShardScheduler, VeryVerbose, TEXT("[GlobalShardScheduler] GSS %s N=%d work=%.3f ms wall=%.3f ms (PhaseTick=%u)"),
			PhaseName, N, WorkMs, WallMs, (uint32)PhaseTick);
	}
#else
	(void)PhaseName;
#endif
}

