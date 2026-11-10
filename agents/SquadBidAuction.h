// Copyright (C) GALAXY_PROJECT. All Rights Reserved.
// SquadBidAuction.h - Objective bid/auction system for squad coordination
//
// Squads negotiate objectives via bid/auction over the comms network.
// Homebase coordinates reassignment between squads.

#pragma once

#include "CoreMinimal.h"
#include "CoreTypes/ColorTypes.h" // EResourceColor (haul objective color)
#include "ECS/Components/Combat/ECSTacticalDoctrineEnums.h" // EEngagementDoctrine only -- no doctrine COMPONENT is named here
#include "ECS/Types/StrategicOperationBinding.h"
#include "SquadTypes.h"
#include "SquadBidAuction.generated.h"

// Bidding/demand-profile diagnostics. Declared here rather than pulled from
// OperationLifecycleInternal.h: that header does not compile standalone in this
// translation unit, and a log category is not worth an include-order dependency.
DECLARE_LOG_CATEGORY_EXTERN(LogSquadBidAuction, Log, All);

// ============================================================================
// OBJECTIVE TYPES
// ============================================================================

/**
 * Type of objective being bid on.
 */
UENUM()
enum class EObjectiveType : uint8
{
	None				UMETA(DisplayName = "None"),
	
	// === Combat ===
	AttackTarget		UMETA(DisplayName = "Attack Target"),
	DefendPosition		UMETA(DisplayName = "Defend Position"),
	Intercept			UMETA(DisplayName = "Intercept"),
	Ambush				UMETA(DisplayName = "Ambush"),
	
	// === Patrol ===
	PatrolArea			UMETA(DisplayName = "Patrol Area"),
	PatrolRoute			UMETA(DisplayName = "Patrol Route"),
	ShowOfForce			UMETA(DisplayName = "Show of Force"),
	
	// === Reconnaissance ===
	ScoutArea			UMETA(DisplayName = "Scout Area"),
	ScoutTarget			UMETA(DisplayName = "Scout Target"),
	Surveillance		UMETA(DisplayName = "Surveillance"),
	
	// === Support ===
	Escort				UMETA(DisplayName = "Escort"),
	Resupply			UMETA(DisplayName = "Resupply"),
	Rescue				UMETA(DisplayName = "Rescue"),
	Reinforce			UMETA(DisplayName = "Reinforce"),
	
	// === Logistics ===
	Transport			UMETA(DisplayName = "Transport"),
	Mining				UMETA(DisplayName = "Mining"),
	Construction		UMETA(DisplayName = "Construction"),
	Collection			UMETA(DisplayName = "Collection"),

	// === Population ===
	// Settler party founding a population homestead. NEVER auctioned: posted
	// pre-claimed for its dedicated party by SettlerBehaviorSystem (RunAuction
	// and TickAssignments both skip it).
	Settle				UMETA(DisplayName = "Settle"),

	// === Hunting ===
	// Two-leg hunt circuit: drive to a wildlife hunting ground, drive-by-grab
	// slimes into member cargo (WildlifeHarvestSystem -- NO combat), haul home to
	// the settlement at the carry threshold. Emitted by BuildHuntCandidates under
	// g.hunt.Emission. APPENDED after Settle: the objective type persists on
	// CSquad.CurrentObjective, so this enum is append-only (save stability).
	HarvestWildlife		UMETA(DisplayName = "Harvest Wildlife"),

	// === Cave logistics ===
	// Own-economy squad jobs that drive a unit to the surface point above a tunnel
	// and advance the dig/pave on the observed cave dynamic state:
	//   RepairTunnel  -- excavate a collapsed tunnel back to Open (CaveNetworkSystem::SetRepairProgress)
	//   BuildCaveRoad -- pave an open RawGround tunnel to BuiltRoadTravel (SetRoadProgress)
	// Emitted (fog-gated) only under g.goals.CaveJobs. APPENDED after HarvestWildlife:
	// the objective type persists on CSquad.CurrentObjective, so this enum is append-only.
	RepairTunnel		UMETA(DisplayName = "Repair Tunnel"),
	BuildCaveRoad		UMETA(DisplayName = "Build Cave Road"),

	// === Salvage ===
	// Drive to a remembered wreck and work it: SalvageSystem::TickSalvage claims and
	// breaks down (or refits) by proximity, exactly as Mining collects from a pit by
	// standing on it. This is what makes "holding the field converts a kill into
	// material" true for an AI empire and not only for the human player. Emitted by
	// BuildCandidates under g.goals.Salvage. APPENDED after BuildCaveRoad: the
	// objective type persists on CSquad.CurrentObjective, so this enum is append-only.
	Salvage				UMETA(DisplayName = "Salvage"),

	MAX					UMETA(Hidden)
};

/**
 * Preference tags that affect bid strength.
 * Squads with matching tags bid higher for certain objectives.
 */
UENUM()
enum class ESquadPreferenceTag : uint8
{
	None				UMETA(DisplayName = "None"),
	
	// === Combat Style ===
	Aggressive			UMETA(DisplayName = "Aggressive"),
	Defensive			UMETA(DisplayName = "Defensive"),
	Balanced			UMETA(DisplayName = "Balanced"),
	
	// === Specialty ===
	Stealth				UMETA(DisplayName = "Stealth"),

	// Simulation-test tags, not authorable in game data.
	Artillery			UMETA(DisplayName = "Artillery"),
	AntiAir				UMETA(DisplayName = "Anti-Air"),
	AntiArmor			UMETA(DisplayName = "Anti-Armor"),
	AntiInfantry		UMETA(DisplayName = "Anti-Infantry"),
	
	// === Role ===
	Scout				UMETA(DisplayName = "Scout"),
	Assault				UMETA(DisplayName = "Assault"),
	Support				UMETA(DisplayName = "Support"),
	Logistics			UMETA(DisplayName = "Logistics"),

	// === Tech Tier (retired; kept so later values do not shift) ===
	LowTech				UMETA(DisplayName = "Low Tech"),
	MidTech				UMETA(DisplayName = "Mid Tech"),
	HighTech			UMETA(DisplayName = "High Tech"),
	UltraTech			UMETA(DisplayName = "Ultra Tech"),
	ApexTech			UMETA(DisplayName = "Apex Tech"),

	// === Unit Metadata ===
	Small				UMETA(DisplayName = "Small"),
	Biological			UMETA(DisplayName = "Biological"),
	Augmented			UMETA(DisplayName = "Augmented"),
	Commander			UMETA(DisplayName = "Commander"),
	ElectronicWarfare	UMETA(DisplayName = "Electronic Warfare"),
	// Retired; kept so later values do not shift.
	IonizationImmune			UMETA(DisplayName = "IonicPulse Immune"),
	FullColorLoadout	UMETA(DisplayName = "Full Color Loadout"),
	FireAnchor			UMETA(DisplayName = "Fire Anchor"),
	Generalist			UMETA(DisplayName = "Generalist"),
	Infiltrator			UMETA(DisplayName = "Infiltrator"),
	Precision			UMETA(DisplayName = "Precision"),
	SensorFighter		UMETA(DisplayName = "Sensor Fighter"),
	Suppressor			UMETA(DisplayName = "Suppressor"),
	Breacher			UMETA(DisplayName = "Breacher"),
	CannonFodder		UMETA(DisplayName = "Cannon Fodder"),
	
	MAX					UMETA(Hidden)
};

// Forward declarations for FMissionDemandProfile (implementations in .cpp)
struct FOperationDefinition;
struct FSquadCapabilityProfile;
enum class EOperationCategory : uint8;

// ============================================================================
// MISSION DEMAND PROFILE
// ============================================================================
// Structured descriptor of what an operation requires from a squad.
// Replaces the shallow RequiredUnitTags -> PreferenceTags mapping with
// a rich, multi-axis demand surface that can be scored against a
// FSquadCapabilityProfile.
//
// Built from FOperationDefinition via FromDefinition(). No JSON changes
// needed -- all fields are derived from existing definition data.
// ============================================================================

struct FMissionDemandProfile
{
	// =========================================================================
	// Objective Classification
	// =========================================================================

	/** Resolved objective type for the bid/auction system */
	EObjectiveType ObjectiveType = EObjectiveType::None;

	/** Preferred tactical mission the squad should adopt */
	ESquadMission PreferredMission = ESquadMission::Patrol;

	/** Desired engagement posture (e.g., Stealth for covert ops) */
	EEngagementDoctrine PreferredEngagement = EEngagementDoctrine::Opportunistic;

	// =========================================================================
	// Behavioral Requirements
	// =========================================================================

	/** Minimum acceptable aggression factor (0-1) */
	float MinAggressionFactor = 0.0f;

	/** Maximum acceptable aggression factor (0-1) */
	float MaxAggressionFactor = 1.0f;

	/** Minimum formation discipline needed (0-1) */
	float MinCombatCohesion = 0.0f;

	// =========================================================================
	// Composition Requirements
	// =========================================================================

	/** Minimum squad headcount */
	int32 MinSquadSize = 1;

	/** Hard requirements -- squad MUST have all of these tags */
	TArray<ESquadPreferenceTag> RequiredTags;

	/** Soft preferences -- boost bid score but not mandatory */
	TArray<ESquadPreferenceTag> PreferredTags;

	// =========================================================================
	// Capability Thresholds
	// =========================================================================

	/** Minimum intel-gathering capability (0-1, for recon/intel ops) */
	float MinIntelCapability = 0.0f;

	/** Stealth requirement (0 = irrelevant, 1 = mandatory stealth approach) */
	float StealthRequirement = 0.0f;

	/** Minimum effective engagement range (for artillery/long-range ops) */
	float MinRangeCapability = 0.0f;

	/** Maximum acceptable detection signature (1.0 = no constraint) */
	float MaxAcceptableDetectionRisk = 1.0f;

	/** Must the squad be able to operate without command link? */
	bool bRequiresIndependentOperation = false;

	/** Estimated mission duration in ticks (for supply endurance checks) */
	int32 EstimatedDurationTicks = 0;

	// =========================================================================
	// Construction
	// =========================================================================

	/**
	 * Build a demand profile from an operation definition.
	 * Replaces the hardcoded if-else chains in PostSquadObjective.
	 */
	GALAXY_PROJECT_API static FMissionDemandProfile FromDefinition(const FOperationDefinition& Def);

	// =========================================================================
	// Scoring
	// =========================================================================

	/**
	 * Binary pass/fail: does the capability profile satisfy all hard
	 * requirements (size, required tags, independence, stealth)?
	 */
	[[nodiscard]] GALAXY_PROJECT_API bool IsSatisfiedBy(const FSquadCapabilityProfile& Cap) const;

	/**
	 * Soft scoring (0-1) for bid suitability. Higher = better fit.
	 * Considers mission alignment, tag overlap, behavioral compatibility,
	 * and capability thresholds.
	 */
	[[nodiscard]] GALAXY_PROJECT_API float ScoreAgainst(const FSquadCapabilityProfile& Cap) const;

	// =========================================================================
	// Internal Mapping Helpers
	// =========================================================================

	/** Maps RequiredUnitTag FNames to ESquadPreferenceTag (extensible table) */
	GALAXY_PROJECT_API static const TMap<FName, ESquadPreferenceTag>& GetTagMapping();

	/** Category -> objective type (replaces MapCategoryToObjectiveType) */
	GALAXY_PROJECT_API static EObjectiveType CategoryToObjectiveType(EOperationCategory Category);

	/** Category -> preferred tactical mission */
	GALAXY_PROJECT_API static ESquadMission CategoryToPreferredMission(EOperationCategory Category);

	/** Category -> preferred engagement doctrine */
	GALAXY_PROJECT_API static EEngagementDoctrine CategoryToPreferredEngagement(EOperationCategory Category);
};

// ============================================================================
// OBJECTIVE DEFINITION
// ============================================================================

/**
 * An objective available for bidding.
 * Created by Purple strategic command.
 */
struct FObjectiveDefinition
{
	/** Unique ID for this objective (side-scoped: [16-bit EmpireSlotId16 | 48-bit local counter]) */
	int64 ObjectiveID = INDEX_NONE;

	/** Purple strategy this objective belongs to */
	int32 StrategyID = INDEX_NONE;

	/** Type of objective */
	EObjectiveType ObjectiveType = EObjectiveType::None;

	/** Priority from Purple strategy (0-1, higher = more important) */
	float Priority = 0.5f;

	/** Target position (world space) */
	FVector TargetPosition = FVector::ZeroVector;

	/** Target entity ID (if applicable) */
	int32 TargetEntityID = INDEX_NONE;

	/** Area radius (for area-based objectives) */
	float AreaRadius = 500.0f;

	/** Preference tags - squads matching these bid higher */
	TArray<ESquadPreferenceTag> PreferenceTags;

	/**
	 * Structured mission demand profile (ECS-layer data).
	 * Carries rich multi-axis requirements from the operation definition.
	 * Populated by OperationLifecycleSystem::PostSquadObjective().
	 * PreferenceTags above is kept in sync for backward compatibility.
	 */
	FMissionDemandProfile DemandProfile;

	/**
	 * Optional exact leader-operation play selected by the Plexus strategic
	 * pipeline. Empty means this remains a generic system objective. A complete
	 * binding is provenance only; CardManagement revalidates its generation and
	 * ownership before any activation or resource mutation.
	 */
	FStrategicOperationBinding OperationBinding;

	/**
	 * Planet-surface goal direction (ECS-layer data).
	 * Unit N-vector for objectives on a planet surface; zero when the
	 * objective is not surface-anchored. Populated by the planet goal
	 * emission pass so squad assignment can issue CSquadRouteRequest
	 * goals without re-deriving the direction from TargetPosition.
	 */
	FVector3d TargetDirectionN = FVector3d::ZeroVector;

	/**
	 * Stable id (CGalaxyStableId) of the target entity for entity-anchored
	 * objectives (ECS-layer data; 0 = area objective).
	 * Used by goal-emission assignment ticking to detect a destroyed target
	 * (an arrived squad observing the target's absence completes the goal).
	 */
	int64 TargetStableId = 0;

	/**
	 * Haul objectives (Transport/Resupply) only: the PICKUP anchor
	 * (ECS-layer data). The squad routes here first, loads from
	 * the source carrier, then hauls to TargetDirectionN (the delivery anchor).
	 * Zero / 0 for every non-haul objective.
	 */
	FVector3d SourceDirectionN = FVector3d::ZeroVector;
	int64 SourceStableId = 0;

	/** Haul objectives only: the resource color hauled. */
	EResourceColor HaulColor = EResourceColor::Orange;

	/**
	 * Cave-job target tunnel (ECS-layer data). For RepairTunnel /
	 * BuildCaveRoad the squad works the tunnel (SystemIndex, EdgeIndex) on the cave
	 * dynamic state; INDEX_NONE for every other objective. Transient board record only.
	 */
	int32 CaveSystemIndex = INDEX_NONE;
	int32 CaveEdgeIndex = INDEX_NONE;

	/** Time objective was created */
	float CreationTime = 0.0f;

	/** Optional: Expiration time (0 = no expiration) */
	float ExpirationTime = 0.0f;

	/** Is this objective still available for bidding? */
	bool bIsOpen = true;
};

// ============================================================================
// BID SYSTEM
// ============================================================================

/**
 * A squad's bid on an objective.
 * Bid strength is calculated from multiple factors.
 */
struct FObjectiveBid
{
	/** Squad placing the bid */
	FSquadID BiddingSquadID;

	/** Objective being bid on */
	int64 ObjectiveID = INDEX_NONE;

	/** 
	 * Final bid strength (0-1). Calculated from:
	 * - Equipment suitability
	 * - Distance to objective (inverse)
	 * - Squad-medium card bid aggressiveness
	 * - Preference tag matches
	 * - Current availability (not engaged elsewhere)
	 */
	float BidStrength = 0.0f;

	/** Time bid was placed */
	float BidTime = 0.0f;

	// === Bid Components (for debugging/tuning) ===

	/** Suitability score (equipment match) */
	float SuitabilityScore = 0.0f;

	/** Distance score (closer = higher) */
	float DistanceScore = 0.0f;

	/** Preference match score */
	float PreferenceScore = 0.0f;

	/** Availability score (not busy) */
	float AvailabilityScore = 0.0f;

	/** Learned planning score blended into suitability for this bidder. */
	float CombatKnowledgeScore = 0.0f;

	/** Explanation tags carried with the learned planning bias. */
	TArray<FString> CombatKnowledgeExplanationTags;

	/** Failure modes carried with the learned planning bias. */
	TArray<FString> CombatKnowledgeFailureModes;

	/** Distance to objective in meters */
	float DistanceToObjective = 0.0f;

	/** Estimated time to reach objective */
	float EstimatedTravelTime = 0.0f;
};

/**
 * Calculate bid strength from components.
 * Weights can be tuned for balance.
 */
namespace BidCalculation
{
	// Default weights for bid components
	constexpr float SUITABILITY_WEIGHT = 0.35f;
	constexpr float DISTANCE_WEIGHT = 0.30f;
	constexpr float PREFERENCE_WEIGHT = 0.20f;
	constexpr float AVAILABILITY_WEIGHT = 0.15f;

	GALAXY_PROJECT_API float CalculateBidStrength(const FObjectiveBid& Bid);

	/**
	 * Calculate distance score (inverse relationship).
	 * @param Distance Distance in meters
	 * @param MaxDistance Distance at which score becomes 0
	 */
	GALAXY_PROJECT_API float CalculateDistanceScore(float Distance, float MaxDistance = 10000.0f);

	/**
	 * Compute suitability score from demand profile vs squad capability.
	 * Measures how well the squad's composition and doctrine match the
	 * operation's requirements.
	 * @return Score 0-1 (higher = better fit)
	 */
	GALAXY_PROJECT_API float ComputeSuitabilityScore(
		const FMissionDemandProfile& Demand,
		const FSquadCapabilityProfile& Cap);

	/**
	 * Compute preference score from tag overlap and mission alignment.
	 * @return Score 0-1 (higher = more matching preferences)
	 */
	GALAXY_PROJECT_API float ComputePreferenceScore(
		const FMissionDemandProfile& Demand,
		const FSquadCapabilityProfile& Cap);

	/**
	 * Compute availability score from squad readiness state.
	 * Considers health, ammo, supply endurance, and current commitment.
	 * @return Score 0-1 (higher = more available/ready)
	 */
	GALAXY_PROJECT_API float ComputeAvailabilityScore(
		const FSquadCapabilityProfile& Cap);

	/**
	 * Build a complete FObjectiveBid from demand profile + capability profile.
	 * Computes all four component scores and the final BidStrength.
	 */
	GALAXY_PROJECT_API FObjectiveBid BuildBid(
		const FSquadID& SquadId,
		int64 ObjectiveId,
		const FMissionDemandProfile& Demand,
		const FSquadCapabilityProfile& Cap,
		float DistanceToObjective,
		float CurrentTime);
}

// ============================================================================
// CLAIM SYSTEM
// ============================================================================

/**
 * An awarded objective claim.
 * Once claimed, objective belongs to squad until released or cooldown expires.
 */
struct FObjectiveClaim
{
	/** Squad that owns the claim */
	FSquadID ClaimingSquadID;

	/** Objective that was claimed */
	int64 ObjectiveID = INDEX_NONE;

	/** Time claim was awarded */
	float ClaimTime = 0.0f;

	/** Cooldown expiration time (cannot be reassigned until then) */
	float CooldownExpiration = 0.0f;

	/** Winning bid strength (for reference) */
	float WinningBidStrength = 0.0f;

	/** Has the squad acknowledged the claim? */
	bool bAcknowledged = false;

	/** Is this claim still active? */
	bool bIsActive = true;

	/** Is claim in cooldown (cannot be reassigned)? */
	[[nodiscard]] bool IsInCooldown(float CurrentTime) const
	{
		return CurrentTime < CooldownExpiration;
	}
};

/**
 * Claim release reason.
 */
UENUM()
enum class EClaimReleaseReason : uint8
{
	None				UMETA(DisplayName = "None"),
	ObjectiveComplete	UMETA(DisplayName = "Objective Complete"),
	ObjectiveFailed		UMETA(DisplayName = "Objective Failed"),
	SquadDestroyed		UMETA(DisplayName = "Squad Destroyed"),
	ManualRelease		UMETA(DisplayName = "Manual Release"),
	Reassigned			UMETA(DisplayName = "Reassigned to Better Squad"),
	CommsLost			UMETA(DisplayName = "Communications Lost"),
	CooldownExpired		UMETA(DisplayName = "Cooldown Expired"),
	MAX					UMETA(Hidden)
};

// ============================================================================
// AUCTION COORDINATOR (Homebase)
// ============================================================================

/**
 * State of the bid/auction system for a homebase (Purple command).
 * Coordinates objective assignment between squads.
 */
struct FBidAuctionState
{
	/** Purple card entity ID that owns this auction state */
	int32 OwnerPurpleCardID = INDEX_NONE;

	/** All available objectives */
	TArray<FObjectiveDefinition> AvailableObjectives;

	/** Current bids on objectives (ObjectiveID -> Array of bids) */
	TMap<int64, TArray<FObjectiveBid>> CurrentBids;

	/** Active claims (ObjectiveID -> Claim) */
	TMap<int64, FObjectiveClaim> ActiveClaims;

	/** Default claim cooldown in seconds */
	float DefaultClaimCooldown = 30.0f;

	/** Last time auction was evaluated */
	float LastEvaluationTime = 0.0f;

	/** Auction evaluation interval in seconds */
	float EvaluationInterval = 1.0f;

	// === Methods ===

	/** Add a new objective for bidding */
	void AddObjective(const FObjectiveDefinition& Objective)
	{
		AvailableObjectives.Add(Objective);
	}

	/** Submit a bid on an objective */
	GALAXY_PROJECT_API void SubmitBid(const FObjectiveBid& Bid);

	/** Find best bid for an objective */
	GALAXY_PROJECT_API const FObjectiveBid* FindBestBid(int64 ObjectiveID) const;

	/** Award claim to winning bidder */
	GALAXY_PROJECT_API void AwardClaim(int64 ObjectiveID, const FSquadID& WinningSquad, float CurrentTime, float BidStrength);

	/** Release a claim */
	GALAXY_PROJECT_API void ReleaseClaim(int64 ObjectiveID, EClaimReleaseReason Reason);

	/** Get claim for a squad (if any) */
	GALAXY_PROJECT_API const FObjectiveClaim* GetClaimForSquad(const FSquadID& SquadID) const;

	/** Check if an objective can be reassigned (not in cooldown) */
	[[nodiscard]] GALAXY_PROJECT_API bool CanReassign(int64 ObjectiveID, float CurrentTime) const;
};

// ============================================================================
// BID/AUCTION MESSAGES
// ============================================================================

/**
 * Message types for bid/auction system.
 */
UENUM()
enum class EBidAuctionMessageType : uint8
{
	None				UMETA(DisplayName = "None"),
	
	// === From Homebase ===
	ObjectiveAvailable	UMETA(DisplayName = "Objective Available"),
	ClaimAwarded		UMETA(DisplayName = "Claim Awarded"),
	ClaimRevoked		UMETA(DisplayName = "Claim Revoked"),
	ObjectiveCancelled	UMETA(DisplayName = "Objective Cancelled"),
	
	// === From Squad ===
	BidSubmitted		UMETA(DisplayName = "Bid Submitted"),
	ClaimAcknowledged	UMETA(DisplayName = "Claim Acknowledged"),
	ClaimReleased		UMETA(DisplayName = "Claim Released"),
	StatusUpdate		UMETA(DisplayName = "Status Update"),
	
	MAX					UMETA(Hidden)
};
