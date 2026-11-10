// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Minimal bridge interface that all ECS shards implement.
// The scheduler drives phases through this interface without knowing concrete
// shard types. Extracted from GlobalShardScheduler.h.
//
// No UE reflection (UINTERFACE) by design -- keeps it fast and non-UObject.
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "ShardMessageTypes.h"  // FShardMessage
#include "ShardPerfTypes.h"     // FShardDebugView
#include "ShardZoneRecalcTypes.h"  // FShardId, FGlobalTick, FZoneRecalcRequest -- NOT the 460-line zone vocabulary
#include "CoreTypes/SchedulerConstants.h" // FMegaTickCount (strong MegaTick-domain type for Phase_Strategic)

// Forward declarations for serialization helpers used in virtual interface
namespace ECSSerialization
{
	enum class EShardType : uint8;
}

// Forward declarations for the data-integrity (NaN/Inf) sentinel; the override
// includes ECS/Validation/ECSValidation.h in its .cpp.
namespace ECSValidation
{
	struct FECSValidationReport;
	struct FECSValidationOptions;
}

// Forward-declared so callers do not need to include the full EventBus header.
// Both are used here ONLY as pointers (GetShardEventBus / SetEventRecorder), so a
// declaration is sufficient and the definition is not.
class IEventRecorder;
class FEventBusManager;

/**
 * Minimal bridge interface shards must implement.
 * No UE reflection -- keep it fast and non-UObject.
 */
struct IShardBridge
{
	virtual ~IShardBridge() = default;

	[[nodiscard]] virtual FShardId GetShardId() const = 0;

	[[nodiscard]] virtual int32 GetRegistryEntityCount() const { return -1; }

	/**
	 * PER-SYSTEM step-cost telemetry (diagnostic-only, opt-in, non-shipping).
	 *
	 * Default no-ops so a shard opts in by overriding rather than by being edited:
	 * shard-level timing already exists (FShardPerfStats), and it can only ever say
	 * WHICH SHARD is hot. Naming the system inside that shard is the rung below.
	 *
	 * COLLECTION IS PER-SHARD BY DESIGN. Phases run under ParallelFor across shard
	 * groups, so a shard appends to a sink IT owns (one worker touches it) and the
	 * scheduler drains serially after the step. Enable/drain are both called from the
	 * scheduler on the game thread, never mid-phase.
	 */
	virtual void SetSystemPerfCollectionEnabled(bool bEnabled) { (void)bEnabled; }

	/** Move out everything sampled since the last drain. Default: nothing to give. */
	virtual void DrainSystemPerfSamples(TArray<FECSSystemPerfStats>& Out) { (void)Out; }

	// Local phases (no cross-shard access inside these):
	virtual void             Phase_Input(FGlobalTick Now) = 0;
	virtual void             Phase_Derive(FGlobalTick Now) = 0;
	virtual void             Phase_LocalSim(FGlobalTick Now, float FixedDt) = 0;

	// Outbox ownership: shard returns all messages produced so far and clears its outbox.
	virtual void             StealOutbox(TArray<FShardMessage>& Out) = 0;

	// Inbox enqueue: scheduler delivers to shard. Shard decides when to apply (usually via a timing wheel).
	virtual void             EnqueueInbound(const FShardMessage& M) = 0;

	// Resolve all messages that are due at 'Now'. (Shard drains its timing wheel bucket for 'Now').
	virtual void             Phase_ResolveDue(FGlobalTick Now) = 0;

	// Optional: build render/net snapshots after world-state settles for this tick.
	virtual void             Phase_BuildSnapshot(FGlobalTick Now) = 0;

	// Development-only data-integrity sentinel: scan this shard's float-bearing
	// authoritative state for NaN/Inf and append Data_NaN/Data_Infinite issues
	// (via ECSValidation::CheckFinite*). The scheduler calls this at the
	// BuildSnapshot barrier under the bEnableECSValidation toggle, so a poisoned
	// value is caught before it reaches the snapshot/checksum. Default no-op;
	// shards with float members or registries opt in by overriding.
	virtual void             ScanDataIntegrity(ECSValidation::FECSValidationReport& Report,
	                                           const ECSValidation::FECSValidationOptions& Options) const
	{
		(void)Report; (void)Options;
	}

	// Strategic-layer processing: fires once per MegaTick.
	// CONTRACT: MegaTick is the MEGATICK COUNTER, not GlobalTick -- unlike every other
	// phase callback, which receives the frame tick. The strong FMegaTickCount type
	// makes that unit mix-up a compile error. Gate slower cadences with
	// MegaTick.IsBoundaryOf(TickConstants::MegaTicksPerUltraTick / MegaTicksPerOmegaTick).
	// Use for intel decay, fleet order evaluation, resource accrual, etc.
	// Default implementation is empty; shards opt-in by overriding.
	virtual void             Phase_Strategic(FMegaTickCount MegaTick) { (void)MegaTick; }

	// Parent zone for child shards (such as PolymassShard).
	// Returns 0 for shards that are not zone children (SpaceZoneShard, PlanetShard).
	// Used by scheduler to query tick authorization from the parent zone.
	// NOTE: CombatShard also overrides this -- it reports its parent zone so the
	// scheduler can mediate results back. Zone AUTHORIZATION is a separate axis
	// from registry EXCLUSIVITY; see GetRegistryOwnerShardId below.
	[[nodiscard]] virtual FShardId GetParentZoneId() const { return 0; }

	// ========================================================================
	// Registry exclusivity -- which registry this shard's phases MUTATE.
	// ========================================================================
	// The scheduler runs phases across shards with ParallelFor. That is only
	// sound while no two concurrently-ticked shards write the same
	// entt::registry, and "one shard, one registry" is NOT universally true:
	// CombatShard's FBattlespaceInstance holds ParentRegistryWrite, a pointer
	// into the PARENT zone's registry, and CombatTickProcessor writes through it
	// (emplace_or_replace<CSubstrateAttachmentGraph>, barrier depletion,
	// UAttackResolution::DestroyUnit).
	//
	// This returns the id of the shard that OWNS the registry this shard
	// mutates. The scheduler partitions each phase into groups sharing an owner,
	// runs groups in parallel and members within a group serially, so a
	// registry is never touched by two threads at once. Default: itself.
	//
	// Overriding is mandatory for any shard that reaches into another's
	// registry. Getting it wrong is a data race, not a slowdown -- a non-const
	// registry.view<T>() calls assure<T>(), which CREATES the component pool on
	// first touch and mutates the pool container itself, so even a "read" of a
	// never-before-used component type races.
	[[nodiscard]] virtual FShardId GetRegistryOwnerShardId() const { return GetShardId(); }

	// ========================================================================
	// Serialization (scheduler-owned)
	// ========================================================================

	[[nodiscard]] virtual ECSSerialization::EShardType GetShardType() const { return static_cast<ECSSerialization::EShardType>(0); }

	GALAXY_PROJECT_API virtual bool SerializeShardState(TArray<uint8>& OutBytes, uint16& OutSchemaVersion) const;

	GALAXY_PROJECT_API virtual bool DeserializeShardState(const TArray<uint8>& InBytes, uint16 SchemaVersion);

	// ------------------------------------------------------------------------
	// Determinism checksum (record/deterministic-replay harness)
	// ------------------------------------------------------------------------
	// Called by the scheduler at every MegaTick boundary and folded into the
	// simulation-wide checksum. Every shard class overrides this as a hash of its
	// SerializeShardState bytes, so the checksum is a pure function of the payload
	// that feeds save, replay and resync, and can never disagree with those bytes.
	//
	// The default of 0 means the shard sits outside checksum comparison, and that
	// is absence of coverage, so a new shard must override this.
	//
	// Implementations must be deterministic: no wall-clock, no unseeded RNG,
	// no TMap/TSet iteration without sorting.
	[[nodiscard]] virtual uint64 ComputeShardChecksum() const { return 0; }

	/**
	 * Install or remove a determinism tape event recorder on this shard's
	 * FEventBusManager (if it owns one). Pass nullptr to remove.
	 * Called by UGlobalShardScheduler when g.det.Record / g.det.StopRecord
	 * arms or disarms tape recording. Default no-op for shards without a bus.
	 */
	virtual void InstallEventRecorder(IEventRecorder* Recorder) { (void)Recorder; }

	/**
	 * Return this shard's local FEventBusManager, or nullptr if none.
	 * Used by UGlobalShardScheduler to register shard buses with
	 * FReplayEventInjector when starting a replay session.
	 * Default returns nullptr.
	 */
	[[nodiscard]] virtual FEventBusManager* GetShardEventBus() { return nullptr; }

	GALAXY_PROJECT_API virtual void ResolveSnapshotReferences(const TMap<FShardId, TSharedPtr<IShardBridge>>& InShards);

	// ========================================================================
	// Zone Recalculation (SuperTick boundary)
	// ========================================================================

	/**
	 * Check for zone topology changes and return a recalc request if needed.
	 * Called by scheduler at SuperTick boundaries (every 1s).
	 *
	 * AttachedZones check if their bubble overlaps with other bubbles have changed.
	 * Returns a valid request if zone topology may need updating.
	 *
	 * Default: No recalc needed (non-zone shards).
	 */
	[[nodiscard]] virtual FZoneRecalcRequest GetZoneRecalcRequest() const { return FZoneRecalcRequest{}; }

	/**
	 * Clear the zone recalc request after scheduler has processed it.
	 * Called by scheduler after collecting all requests.
	 */
	virtual void ClearZoneRecalcRequest() {}

	/**
	 * Get the world position of this shard (for bubble overlap calculations).
	 * Default: Zero vector (non-spatial shards).
	 */
	[[nodiscard]] virtual FVector GetWorldPosition() const { return FVector::ZeroVector; }

	/**
	 * Get the bubble radius for zone overlap calculations.
	 * For fleets: max(DefaultBubble, MaxWeaponRange, MaxSensorRange)
	 * For planets: PlanetRadius + AnchorBufferRadius
	 * Default: 0 (non-bubble shards).
	 */
	[[nodiscard]] virtual float GetBubbleRadius() const { return 0.0f; }

	GALAXY_PROJECT_API virtual void BuildDebugView(FShardDebugView& Out) const;

	/**
	 * Set logging flags for shards that support verbose/trace logging.
	 * Default implementation does nothing - shards opt-in by overriding.
	 * @param bVerbose Enable verbose logging
	 * @param bTrace Enable trace logging
	 * @param WarnAtBytes Warn on payloads exceeding this size (0 = disabled)
	 */
	virtual void SetLogConfig(bool bVerbose, bool bTrace, int32 WarnAtBytes) { (void)bVerbose; (void)bTrace; (void)WarnAtBytes; }

	/**
	 * Set planner logging flags for shards that have strategic planners.
	 * Default implementation does nothing - only PlanetShard overrides this.
	 * @param bVerbose Enable planner verbose logging
	 * @param bTrace Enable planner trace logging
	 */
	virtual void SetPlannerLog(bool bVerbose, bool bTrace) { (void)bVerbose; (void)bTrace; }
};

namespace ShardScheduling
{
	/**
	 * Partition shards into registry-exclusion groups -- the precondition every
	 * phase ParallelFor in UGlobalShardScheduler rests on.
	 *
	 * Two shards land in the same group exactly when GetRegistryOwnerShardId()
	 * agrees, i.e. exactly when their phases can write the same entt::registry.
	 * Callers run groups concurrently and members within a group serially, so no
	 * registry is ever touched by two threads at once.
	 *
	 * Returns indices INTO `Shards`, so the result is invalidated by any mutation
	 * of that array and must be rebuilt alongside it.
	 *
	 * Determinism: group order is first-seen owner; within a group the registry
	 * OWNER runs first, then the remaining members in the caller's order. The
	 * scheduler passes a StepShards array already sorted by ShardId, which is what
	 * makes the serialized case reproducible rather than merely race-free. The
	 * owner-first hoist is a RULE, not an optimization: without it the
	 * parent-vs-borrower order would ride on ShardId comparison, and combat ids
	 * are transient and RECYCLED -- whether a combat ticked before or after its
	 * parent would flip on id reuse. Deterministic either way; a stated semantic
	 * ("borrowers read a settled owner tick") only this way. The internal map is
	 * keyed lookup only -- never iterated -- so it contributes no ordering.
	 *
	 * Lives beside the interface rather than inside the scheduler because it is a
	 * pure function of the contract above; that keeps it directly testable without
	 * standing up a scheduler and a world.
	 */
	[[nodiscard]] GALAXY_PROJECT_API TArray<TArray<int32>> BuildRegistryExclusionGroups(
		TArrayView<const TSharedPtr<IShardBridge>> Shards);
}
