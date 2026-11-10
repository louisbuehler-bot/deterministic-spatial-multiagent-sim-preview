// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
//
// UGlobalShardScheduler, CROSS-SHARD MESSAGE ROUTING: outbox collection, cross-interaction
// orchestration, deterministic dispatch to destinations, externally injected messages, and
// dead-letter logging and recovery.
//
// Part of the scheduler's split into one TU per concern -- the table is at the top of
// GlobalShardScheduler.cpp. Leads with CoreMinimal.h, not the scheduler header, so under adaptive
// unity an edit to GlobalShardScheduler.h ejects only the owner from its unity blob.
// ============================================================================
#include "CoreMinimal.h"
#include "GlobalShardScheduler.h"
#include "Logging/GalaxyLogThrottle.h" // GalaxyLog::ShouldLogThisTick
#include "Async/ParallelFor.h"
#include "GlobalShardSchedulerLog.h" // GSS_VLOG_THIS / GSS_TLOG_THIS

namespace {
	static constexpr int32 MaxDeadLetterRetryQueueSize = 4096;
	static constexpr int32 MaxDeadLetterRetryQueueCritical = 1024;
	static constexpr int32 MaxDeadLetterRetryQueueNormal = 2048;
	static constexpr int32 MaxDeadLetterRetryQueueDebug = 1024;
	static constexpr int32 DeadLetterDrainQuotaCritical = 128;
	static constexpr int32 DeadLetterDrainQuotaNormal = 64;
	static constexpr int32 DeadLetterDrainQuotaDebug = 16;

    // Named reason string for invalid-envelope dead letters -- used in dispatch AND routing to prevent string drift
	static const TCHAR* const DeadLetterReasonInvalidEnvelope = TEXT("InvalidEnvelope");

	static bool IsMessageHigherDispatchPriority(const FShardMessage& A, const FShardMessage& B)
	{
		if (A.Priority != B.Priority)
		{
			return A.Priority > B.Priority;
		}
		if (A.DueTick != B.DueTick)
		{
			return A.DueTick < B.DueTick;
		}
		if (A.EmitTick != B.EmitTick)
		{
			return A.EmitTick < B.EmitTick;
		}
		if (A.MessageId != B.MessageId)
		{
			return A.MessageId < B.MessageId;
		}
		if (A.Source != B.Source)
		{
			return A.Source < B.Source;
		}
		return A.Destination < B.Destination;
	}

	static void SortMessagesForDispatch(TArray<FShardMessage>& Messages)
	{
		const int32 Count = Messages.Num();
		if (Count <= 1)
		{
			return;
		}

        // Small-N fast path: insertion sort avoids Sort() overhead for tiny batches.
		if (Count <= 32)
		{
			for (int32 i = 1; i < Count; ++i)
			{
				FShardMessage Value = MoveTemp(Messages[i]);
				int32 j = i - 1;
				while (j >= 0 && IsMessageHigherDispatchPriority(Value, Messages[j]))
				{
					Messages[j + 1] = MoveTemp(Messages[j]);
					--j;
				}
				Messages[j + 1] = MoveTemp(Value);
			}
			return;
		}

		Messages.Sort([](const FShardMessage& A, const FShardMessage& B)
		{
			return IsMessageHigherDispatchPriority(A, B);
		});
	}
}
void UGlobalShardScheduler::Phase_CollectOutboxes_All(TArray<FShardMessage>& OutCollected)
{
	const int32 N = StepShards.Num();
	if (N == 0) return;

	OutboxScratch.SetNum(N, EAllowShrinking::No);
	for (int32 i = 0; i < N; ++i)
	{
		OutboxScratch[i].Reset();
	}

    // Grouped like every other phase. StealOutbox only moves a shard's own message
    // array today, so per-shard would still be safe here -- but a phase loop that
    // disagrees with the others about what "a parallel unit" is invites the next
    // reader to widen it back. One partition, used everywhere.
	ParallelFor(StepShardGroups.Num(), [this](int32 g) // thread-safe-verified -- groups partition registry ownership; OutboxScratch writes hit distinct pre-sized indices
		{
			for (const int32 i : StepShardGroups[g])
			{
				const TSharedPtr<IShardBridge>& Shard = StepShards[i];
				if (Shard.IsValid())
				{
					Shard->StealOutbox(OutboxScratch[i]);
				}
			}
		});

	for (int32 i = 0; i < N; ++i)
	{
		OutCollected.Append(OutboxScratch[i]);
	}

#if !(UE_BUILD_SHIPPING)
	if (bGSSTrace.load(std::memory_order_relaxed))
	{
		UE_LOG(LogGlobalShardScheduler, VeryVerbose, TEXT("[GlobalShardScheduler] GSS Phase_CollectOutboxes N=%d collected %d messages (Tick=%u)"),
			N, OutCollected.Num(), (uint32)GlobalTick);
	}
#endif
}
void UGlobalShardScheduler::Phase_Orchestrate_CrossInteractions(TArray<FShardMessage>& InOut)
{

    // A DueTick that is unset (0) or already past moves to the next tick, so a message
    // can never apply in the tick that sent it.

	int32 FixedDue = 0, SetEmit = 0, SetIds = 0, ClampedPastDue = 0, Staged = 0, Released = 0;

    // Wheel horizon: per-shard timing wheels hold ShardTimingWheel::StandardSize
    // ticks. Messages due beyond it are STAGED here (not clamped, not delivered)
    // and re-enter the dispatch stream once within horizon, which lifts the wheel's
    // ceiling to the full tick range without touching per-shard wheels.
	const int32 MaxDueTick = static_cast<int32>(GlobalTick) + ShardTimingWheel::StandardSize - 1;

    // 1) Release staged messages that are now within horizon. The staging array is
    // kept sorted by (DueTick, MessageId), so the due ones are a prefix -- release
    // order is deterministic.
	if (FarFutureStagedMessages.Num() > 0)
	{
		int32 ReleaseCount = 0;
		while (ReleaseCount < FarFutureStagedMessages.Num()
			&& FarFutureStagedMessages[ReleaseCount].DueTick <= MaxDueTick)
		{
			++ReleaseCount;
		}
		if (ReleaseCount > 0)
		{
			for (int32 i = 0; i < ReleaseCount; ++i)
			{
				InOut.Add(MoveTemp(FarFutureStagedMessages[i]));
			}
			FarFutureStagedMessages.RemoveAt(0, ReleaseCount, EAllowShrinking::No);
			Released = ReleaseCount;
		}
	}

    // 2) Stamp pass (EmitTick/MessageId/DueTick defaults + past-due protection).
	for (FShardMessage& M : InOut)
	{
		if (M.DueTick == 0)
		{
			M.DueTick = GlobalTick + 1; // "not before next tick"
			++FixedDue;
		}
		else if (M.DueTick <= (int32)GlobalTick)
		{
            // Past-due protection: never apply in the same or past tick
			M.DueTick = (int32)GlobalTick + 1;
			++ClampedPastDue;
		}
		if (M.EmitTick == 0)
		{
			M.EmitTick = GlobalTick;
			++SetEmit;
		}
		if (M.MessageId == 0)
		{
			M.MessageId = ++MessageSeq;
			++SetIds;
		}
	}

    // 3) Stage pass: pull anything still beyond horizon out of the dispatch stream
    // (after stamping, so a staged message already carries EmitTick + MessageId).
	for (int32 i = InOut.Num() - 1; i >= 0; --i)
	{
		if (InOut[i].DueTick > MaxDueTick)
		{
			FarFutureStagedMessages.Add(MoveTemp(InOut[i]));
			InOut.RemoveAt(i, 1, EAllowShrinking::No);
			++Staged;
		}
	}
	if (Staged > 0)
	{
		FarFutureStagedMessages.Sort([](const FShardMessage& A, const FShardMessage& B)
		{
			if (A.DueTick != B.DueTick) { return A.DueTick < B.DueTick; }
			return A.MessageId < B.MessageId;
		});

        // Soft backpressure signal: a very large staging set means some sender is
        // scheduling unbounded far-future work -- surface it before memory does.
		if (FarFutureStagedMessages.Num() > 10000
			&& GalaxyLog::ShouldLogThisTick(true, static_cast<uint64>(GlobalTick), LastHorizonClampLogTick,
				static_cast<uint64>(TickConstants::FramesPerSuperTick)))
		{
			UE_LOG(LogGlobalShardScheduler, Warning,
				TEXT("[GlobalShardScheduler][Orchestrate] Far-future staging holds %d messages (Tick=%u) -- check senders for runaway scheduling"),
				FarFutureStagedMessages.Num(), (uint32)GlobalTick);
		}
	}

	if ((FixedDue | SetEmit | SetIds | ClampedPastDue | Staged | Released) != 0)
	{
		GSS_TLOG_THIS("GSS Orchestrate: fixedDue=%d clampedPastDue=%d staged=%d released=%d stagedTotal=%d setEmit=%d setIds=%d (Tick=%u)",
			FixedDue, ClampedPastDue, Staged, Released, FarFutureStagedMessages.Num(), SetEmit, SetIds, (uint32)GlobalTick);
	}

}
void UGlobalShardScheduler::Phase_Dispatch_ToDestinations(const TArray<FShardMessage>& Collected)
{
	int32 Delivered = 0;
	int32 Dropped = 0;
	int32 Invalid = 0;
	int32 RetriedFromLastTick = 0;

    // Optional capture settings
#if !(UE_BUILD_SHIPPING)
	const bool bCaptureDead = bGSSTrace.load(std::memory_order_relaxed);
	constexpr int32 MaxDeadLetterCaptures = 256;
#endif

    // Every undeliverable message takes this one route: captured on a trace run, then offered to recovery.
	const auto HandleUndeliverable = [&](const FShardMessage& M, const TCHAR* Reason)
	{
#if !(UE_BUILD_SHIPPING)
		if (bCaptureDead && DebugDeadLetters.Num() < MaxDeadLetterCaptures)
		{
			DebugDeadLetters.Add(M);
			LogDeadLetterWithContext(M, Reason);
		}
#endif
		ProcessDeadLetter(M, Reason);
	};

    // Build combined dispatch list: retried dead letters (tiered quotas) + new messages
	TArray<FShardMessage> ToDispatch;
	ToDispatch.Reserve(Collected.Num() + DeadLetterRetryQueueCritical.Num() + DeadLetterRetryQueueNormal.Num() + DeadLetterRetryQueueDebug.Num());

	auto DrainRetryQueue = [&ToDispatch, &RetriedFromLastTick](TArray<FShardMessage>& Queue, int32 Quota)
	{
		if (Quota <= 0 || Queue.Num() == 0)
		{
			return;
		}

		const int32 DrainCount = FMath::Min(Quota, Queue.Num());
		for (int32 i = 0; i < DrainCount; ++i)
		{
			ToDispatch.Add(MoveTemp(Queue[i]));
		}
		Queue.RemoveAt(0, DrainCount, EAllowShrinking::No);
		RetriedFromLastTick += DrainCount;
	};

	DrainRetryQueue(DeadLetterRetryQueueCritical, DeadLetterDrainQuotaCritical);
	DrainRetryQueue(DeadLetterRetryQueueNormal, DeadLetterDrainQuotaNormal);
	DrainRetryQueue(DeadLetterRetryQueueDebug, DeadLetterDrainQuotaDebug);

    // Add new messages
	ToDispatch.Append(Collected);

#if !(UE_BUILD_SHIPPING)
	RecordDispatchBatchSample(static_cast<float>(ToDispatch.Num()));
#endif

    // Sort by priority (descending) with deterministic tie-breakers
	SortMessagesForDispatch(ToDispatch);

	for (const FShardMessage& M : ToDispatch)
	{
        // Basic envelope sanity
		if (M.Destination <= 0 || M.Source <= 0)
		{
			++Invalid;
            // The recovery hook may request a retry (though invalid envelopes usually can't be fixed)
			HandleUndeliverable(M, DeadLetterReasonInvalidEnvelope);
			continue;
		}

		if (TSharedPtr<IShardBridge>* Dest = Shards.Find(static_cast<FShardId>(M.Destination)))
		{
			if (Dest->IsValid())
			{
				(*Dest)->EnqueueInbound(M);
				++Delivered;
#if !(UE_BUILD_SHIPPING)
				GSS_TLOG_THIS("GSS Dispatch msgId=%lld src=%d dst=%d due=%d type=%s prio=%d",
					(long long)M.MessageId, M.Source, M.Destination, M.DueTick, *M.TypeTag.ToString(), M.Priority);
#endif
			}
			else
			{
				++Dropped;
				HandleUndeliverable(M, TEXT("DestShardInvalid"));
			}
		}
		else
		{
			++Dropped;
			HandleUndeliverable(M, TEXT("DestShardNotFound"));
		}
	}

	if ((Delivered | Dropped | Invalid | RetriedFromLastTick) != 0)
	{
		GSS_VLOG_THIS("GSS Dispatched=%d Dropped=%d Invalid=%d Retried=%d PendingRetry=%d (Tick=%u)",
			Delivered,
			Dropped,
			Invalid,
			RetriedFromLastTick,
			DeadLetterRetryQueueCritical.Num() + DeadLetterRetryQueueNormal.Num() + DeadLetterRetryQueueDebug.Num(),
			(uint32)GlobalTick);
	}

	StatTotalDelivered += Delivered;
	StatTotalDropped += Dropped;
}

void UGlobalShardScheduler::InjectMessage(const FShardMessage& M)
{
	if (UWorld* W = GetWorld())
	{
		if (W->GetNetMode() == NM_Client)
		{
			UE_LOG(LogGlobalShardScheduler, Warning, TEXT("[GlobalShardScheduler] InjectMessage on client -- ignored."));
			return;
		}
	}
	InjectedMessages.Add(M);
}

#if !(UE_BUILD_SHIPPING)
void UGlobalShardScheduler::LogDeadLetterWithContext(const FShardMessage& M, const TCHAR* Reason)
{
    // Build human-readable shard labels: Shard#N:TypeName if alive, Shard#N(gone) if not.
	auto MakeShardLabel = [this](uint32 ShardId) -> FString
	{
		if (ShardId == 0) { return TEXT("None"); }
		if (const TSharedPtr<IShardBridge>* Ptr = Shards.Find(static_cast<FShardId>(ShardId)))
		{
			const int32 TypeOrdinal = static_cast<int32>((*Ptr)->GetShardType());
			return FString::Printf(TEXT("Shard#%u:T%d"), ShardId, TypeOrdinal);
		}
		return FString::Printf(TEXT("Shard#%u(gone)"), ShardId);
	};

	UE_LOG(LogGlobalShardScheduler, Warning,
		TEXT("[DeadLetter] %s | MsgId=%lld Type=%s | Src=%s Dst=%s | EmitTick=%d DueTick=%d CurrentTick=%u | PayloadBytes=%d"),
		Reason,
		(long long)M.MessageId,
        *M.TypeTag.ToString(),
        *MakeShardLabel(M.Source),
        *MakeShardLabel(M.Destination),
		M.EmitTick,
		M.DueTick,
		(uint32)GlobalTick,
		M.Payload.Num());
}
#endif

// ============================================================================
// Dead Letter Recovery
// ============================================================================

bool UGlobalShardScheduler::ProcessDeadLetter(const FShardMessage& M, const TCHAR* Reason)
{
    // Call the recovery hook (a C++ virtual) so a subclass can request a retry or handle recovery
	bool bShouldRetry = false;
	OnDeadLetterDropped(M, FString(Reason), bShouldRetry);

	if (bShouldRetry)
	{
		if (M.MaxRetries <= 0 || M.RetryCount >= M.MaxRetries)
		{
#if !(UE_BUILD_SHIPPING)
			GSS_VLOG_THIS("[DeadLetter] Retry limit reached for MsgId=%lld (Reason=%s) retries=%d max=%d", (long long)M.MessageId, Reason, M.RetryCount, M.MaxRetries);
#endif
			return false;
		}

        // Re-queue for next tick (bump DueTick to avoid immediate re-drop)
		FShardMessage RetryMsg = M;
		RetryMsg.RetryCount = FMath::Max(0, RetryMsg.RetryCount + 1);
		RetryMsg.DueTick = static_cast<int32>(GlobalTick) + 1;

		TArray<FShardMessage>* TargetQueue = &DeadLetterRetryQueueNormal;
		int32 QueueCap = MaxDeadLetterRetryQueueNormal;

		if (RetryMsg.Priority >= 200)
		{
			TargetQueue = &DeadLetterRetryQueueCritical;
			QueueCap = MaxDeadLetterRetryQueueCritical;
		}
		else if (FCString::Stricmp(Reason, DeadLetterReasonInvalidEnvelope) == 0)
		{
			TargetQueue = &DeadLetterRetryQueueDebug;
			QueueCap = MaxDeadLetterRetryQueueDebug;
		}

		const int32 TotalQueued = DeadLetterRetryQueueCritical.Num() + DeadLetterRetryQueueNormal.Num() + DeadLetterRetryQueueDebug.Num();
		if (TotalQueued >= MaxDeadLetterRetryQueueSize || TargetQueue->Num() >= QueueCap)
		{
#if !(UE_BUILD_SHIPPING)
			GSS_VLOG_THIS("[DeadLetter] Retry queue full for MsgId=%lld (Reason=%s) total=%d/%d tier=%d/%d",
				(long long)M.MessageId,
				Reason,
				TotalQueued,
				MaxDeadLetterRetryQueueSize,
				TargetQueue->Num(),
				QueueCap);
#endif
			return false;
		}

#if !(UE_BUILD_SHIPPING)
		GSS_VLOG_THIS("[DeadLetter] Re-queued MsgId=%lld for retry (Reason=%s) retries=%d/%d", (long long)M.MessageId, Reason, RetryMsg.RetryCount, RetryMsg.MaxRetries);
#endif

		TargetQueue->Add(MoveTemp(RetryMsg));
	}

	return bShouldRetry;
}

void UGlobalShardScheduler::OnDeadLetterDropped(const FShardMessage& DeadLetter, const FString& Reason, bool& bShouldRetry)
{
    // Default implementation: do not retry. A C++ subclass can override to implement recovery logic.
    // Example override strategies:
    // - Re-route to a fallback shard (by emitting a new message; the dead letter itself is const)
    // - Log to telemetry for debugging
    // - Accumulate for batch recovery at MegaTick boundaries
	bShouldRetry = false;
}
