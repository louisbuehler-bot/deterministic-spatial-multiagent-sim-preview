// Excerpt from a larger file: how a squad's fit for an objective becomes a bid.

bool FMissionDemandProfile::IsSatisfiedBy(const FSquadCapabilityProfile& Cap) const
{
	// Squad size check
	if (Cap.CurrentSize < MinSquadSize)
	{
		return false;
	}

	// Independence check
	if (bRequiresIndependentOperation && !Cap.bCanOperateIndependently)
	{
		return false;
	}

	// Stealth threshold check
	if (StealthRequirement > 0.0f && Cap.StealthRating < StealthRequirement)
	{
		return false;
	}

	// Detection signature ceiling
	if (MaxAcceptableDetectionRisk < 1.0f && Cap.DetectionSignature > MaxAcceptableDetectionRisk)
	{
		return false;
	}

	// Required tags -- all must be present
	for (const ESquadPreferenceTag& Tag : RequiredTags)
	{
		if (!Cap.HasTag(Tag))
		{
			return false;
		}
	}

	return true;
}

float FMissionDemandProfile::ScoreAgainst(const FSquadCapabilityProfile& Cap) const
{
	// Fail fast on hard requirements
	if (!IsSatisfiedBy(Cap))
	{
		return 0.0f;
	}

	float Score = 0.0f;
	float TotalWeight = 0.0f;

	// --- Mission alignment (weight: 0.30) ---
	{
		constexpr float Weight = 0.30f;
		float MissionScore = Cap.MissionFitScore(PreferredMission);
		Score += MissionScore * Weight;
		TotalWeight += Weight;
	}

	// --- Preferred tag overlap (weight: 0.20) ---
	{
		constexpr float Weight = 0.20f;
		if (PreferredTags.Num() > 0)
		{
			int32 Matches = 0;
			for (const ESquadPreferenceTag& Tag : PreferredTags)
			{
				if (Cap.HasTag(Tag))
				{
					++Matches;
				}
			}
			Score += (static_cast<float>(Matches) / static_cast<float>(PreferredTags.Num())) * Weight;
		}
		else
		{
			Score += 0.5f * Weight; // Neutral if no preferences
		}
		TotalWeight += Weight;
	}

	// --- Behavioral compatibility (weight: 0.20) ---
	{
		constexpr float Weight = 0.20f;
		float BehaviorScore = 1.0f;

		// Aggression range check
		if (Cap.AggressionFactor < MinAggressionFactor)
		{
			BehaviorScore -= (MinAggressionFactor - Cap.AggressionFactor);
		}
		if (MaxAggressionFactor < 1.0f && Cap.AggressionFactor > MaxAggressionFactor)
		{
			BehaviorScore -= (Cap.AggressionFactor - MaxAggressionFactor);
		}

		// Cohesion threshold
		if (MinCombatCohesion > 0.0f && Cap.CombatCohesion < MinCombatCohesion)
		{
			BehaviorScore -= (MinCombatCohesion - Cap.CombatCohesion) * 0.5f;
		}

		// Engagement doctrine alignment
		if (Cap.Engagement == PreferredEngagement)
		{
			BehaviorScore = FMath::Min(BehaviorScore + 0.1f, 1.0f);
		}

		Score += FMath::Clamp(BehaviorScore, 0.0f, 1.0f) * Weight;
		TotalWeight += Weight;
	}

	// --- Capability thresholds (weight: 0.15) ---
	{
		constexpr float Weight = 0.15f;
		float CapScore = 1.0f;
		int32 CapChecks = 0;

		if (MinIntelCapability > 0.0f)
		{
			CapScore *= FMath::Min(Cap.IntelCapability / MinIntelCapability, 1.0f);
			++CapChecks;
		}

		if (MinRangeCapability > 0.0f)
		{
			CapScore *= FMath::Min(Cap.MaxEngagementRange / MinRangeCapability, 1.0f);
			++CapChecks;
		}

		if (CapChecks == 0)
		{
			CapScore = 0.7f; // Neutral if no capability thresholds
		}

		Score += FMath::Clamp(CapScore, 0.0f, 1.0f) * Weight;
		TotalWeight += Weight;
	}

	// --- Readiness (weight: 0.15) ---
	{
		constexpr float Weight = 0.15f;
		float ReadinessScore = (Cap.HealthPercent * 0.5f) + (Cap.AmmoPercent * 0.3f);

		// Squad size surplus bonus
		if (MinSquadSize > 0 && Cap.CurrentSize > MinSquadSize)
		{
			float SizeSurplus = static_cast<float>(Cap.CurrentSize - MinSquadSize)
				/ static_cast<float>(FMath::Max(MinSquadSize, 1));
			ReadinessScore += FMath::Min(SizeSurplus * 0.2f, 0.2f);
		}

		Score += FMath::Clamp(ReadinessScore, 0.0f, 1.0f) * Weight;
		TotalWeight += Weight;
	}

	// Normalize (should be ~1.0 but guard against float drift)
	float SuitabilityScore = 0.0f;
	if (TotalWeight > 0.0f)
	{
		SuitabilityScore = FMath::Clamp(Score / TotalWeight, 0.0f, 1.0f);
	}

	const float CombatKnowledgeWeight = FMath::Clamp(
		Cap.CombatKnowledgeWeight,
		0.0f,
		MissionSuitabilityCombatKnowledgeWeightCap);
	if (CombatKnowledgeWeight <= 0.0f)
	{
		return SuitabilityScore;
	}

	const float CombatKnowledgeScore = FMath::Clamp(Cap.CombatKnowledgeSuitabilityScore, 0.0f, 1.0f);
	return FMath::Clamp(
		(SuitabilityScore * (1.0f - CombatKnowledgeWeight)) + (CombatKnowledgeScore * CombatKnowledgeWeight),
		0.0f,
		1.0f);
}

// ============================================================================
// BID SCORING FUNCTIONS (BidCalculation namespace)
// ============================================================================

float BidCalculation::ComputeSuitabilityScore(
	const FMissionDemandProfile& Demand,
	const FSquadCapabilityProfile& Cap)
{
	// Delegate to the demand profile's rich scoring
	return Demand.ScoreAgainst(Cap);
}

float BidCalculation::ComputePreferenceScore(
	const FMissionDemandProfile& Demand,
	const FSquadCapabilityProfile& Cap)
{
	if (Demand.PreferredTags.Num() == 0 && Demand.RequiredTags.Num() == 0)
	{
		return 0.5f; // Neutral if no tag preferences
	}

	int32 TotalTags = Demand.PreferredTags.Num() + Demand.RequiredTags.Num();
	int32 Matches = 0;

	for (const ESquadPreferenceTag& Tag : Demand.RequiredTags)
	{
		if (Cap.HasTag(Tag))
		{
			++Matches;
		}
	}

	for (const ESquadPreferenceTag& Tag : Demand.PreferredTags)
	{
		if (Cap.HasTag(Tag))
		{
			++Matches;
		}
	}

	// Mission alignment bonus
	float MissionBonus = Cap.MissionFitScore(Demand.PreferredMission) * 0.2f;

	float TagScore = static_cast<float>(Matches) / static_cast<float>(FMath::Max(TotalTags, 1));
	return FMath::Clamp(TagScore + MissionBonus, 0.0f, 1.0f);
}

float BidCalculation::ComputeAvailabilityScore(const FSquadCapabilityProfile& Cap)
{
	// Health component (50% weight)
	float HealthScore = Cap.HealthPercent;

	// Ammo component (30% weight)
	float AmmoScore = Cap.AmmoPercent;

	// Size ratio component (20% weight) -- fuller squads are more available
	float SizeRatio = (Cap.MaxCapacity > 0)
		? static_cast<float>(Cap.CurrentSize) / static_cast<float>(Cap.MaxCapacity)
		: 0.0f;

	return FMath::Clamp(
		HealthScore * 0.5f + AmmoScore * 0.3f + SizeRatio * 0.2f,
		0.0f, 1.0f);
}

FObjectiveBid BidCalculation::BuildBid(
	const FSquadID& SquadId,
	int64 ObjectiveId,
	const FMissionDemandProfile& Demand,
	const FSquadCapabilityProfile& Cap,
	float DistanceToObjective,
	float CurrentTime)
{
	FObjectiveBid Bid;
	Bid.BiddingSquadID = SquadId;
	Bid.ObjectiveID = ObjectiveId;
	Bid.BidTime = CurrentTime;
	Bid.DistanceToObjective = DistanceToObjective;

	Bid.SuitabilityScore = ComputeSuitabilityScore(Demand, Cap);
	Bid.DistanceScore = CalculateDistanceScore(DistanceToObjective);
	Bid.PreferenceScore = ComputePreferenceScore(Demand, Cap);
	Bid.AvailabilityScore = ComputeAvailabilityScore(Cap);
	Bid.CombatKnowledgeScore = FMath::Clamp(
		Cap.CombatKnowledgeWeight > 0.0f ? Cap.CombatKnowledgeSuitabilityScore : 0.0f,
		0.0f,
		1.0f);
	Bid.CombatKnowledgeExplanationTags = Cap.CombatKnowledgeExplanationTags;
	Bid.CombatKnowledgeFailureModes = Cap.CombatKnowledgeFailureModes;
	Bid.EstimatedTravelTime = DistanceToObjective / 100.0f; // Placeholder velocity estimate

	Bid.BidStrength = CalculateBidStrength(Bid);

	return Bid;
}
