// Copyright (C) GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Definitions for SquadBidAuction.h.

#include "SquadBidAuction.h"
#include "Containers/DeterministicIteration.h" // GalaxyDeterminism::SortedKeys

// ============================================================================
// BidCalculation
// ============================================================================

float BidCalculation::CalculateBidStrength(const FObjectiveBid& Bid)
{
	return (Bid.SuitabilityScore * SUITABILITY_WEIGHT) +
	       (Bid.DistanceScore * DISTANCE_WEIGHT) +
	       (Bid.PreferenceScore * PREFERENCE_WEIGHT) +
	       (Bid.AvailabilityScore * AVAILABILITY_WEIGHT);
}

float BidCalculation::CalculateDistanceScore(float Distance, float MaxDistance)
{
	if (Distance <= 0.0f) return 1.0f;
	if (Distance >= MaxDistance) return 0.0f;
	return 1.0f - (Distance / MaxDistance);
}


// ============================================================================
// FBidAuctionState
// ============================================================================

void FBidAuctionState::SubmitBid(const FObjectiveBid& Bid)
{
	TArray<FObjectiveBid>& Bids = CurrentBids.FindOrAdd(Bid.ObjectiveID);
	
	// Replace existing bid from same squad
	Bids.RemoveAll([&](const FObjectiveBid& Existing) {
		return Existing.BiddingSquadID == Bid.BiddingSquadID;
	});
	
	Bids.Add(Bid);
}

const FObjectiveBid* FBidAuctionState::FindBestBid(int64 ObjectiveID) const
{
	const TArray<FObjectiveBid>* Bids = CurrentBids.Find(ObjectiveID);
	if (!Bids || Bids->Num() == 0) return nullptr;

	const FObjectiveBid* Best = &(*Bids)[0];
	for (const FObjectiveBid& Bid : *Bids)
	{
		if (Bid.BidStrength > Best->BidStrength)
		{
			Best = &Bid;
		}
	}
	return Best;
}

void FBidAuctionState::AwardClaim(int64 ObjectiveID, const FSquadID& WinningSquad, float CurrentTime, float BidStrength)
{
	FObjectiveClaim Claim;
	Claim.ClaimingSquadID = WinningSquad;
	Claim.ObjectiveID = ObjectiveID;
	Claim.ClaimTime = CurrentTime;
	Claim.CooldownExpiration = CurrentTime + DefaultClaimCooldown;
	Claim.WinningBidStrength = BidStrength;
	Claim.bAcknowledged = false;
	Claim.bIsActive = true;

	ActiveClaims.Add(ObjectiveID, Claim);

	// Mark objective as no longer open
	for (FObjectiveDefinition& Obj : AvailableObjectives)
	{
		if (Obj.ObjectiveID == ObjectiveID)
		{
			Obj.bIsOpen = false;
			break;
		}
	}

	// Clear bids for this objective
	CurrentBids.Remove(ObjectiveID);
}

void FBidAuctionState::ReleaseClaim(int64 ObjectiveID, EClaimReleaseReason Reason)
{
	FObjectiveClaim* Claim = ActiveClaims.Find(ObjectiveID);
	if (Claim)
	{
		Claim->bIsActive = false;
	}
	ActiveClaims.Remove(ObjectiveID);

	// If objective still valid, reopen for bidding
	if (Reason != EClaimReleaseReason::ObjectiveComplete &&
	    Reason != EClaimReleaseReason::ObjectiveFailed)
	{
		for (FObjectiveDefinition& Obj : AvailableObjectives)
		{
			if (Obj.ObjectiveID == ObjectiveID)
			{
				Obj.bIsOpen = true;
				break;
			}
		}
	}
}

const FObjectiveClaim* FBidAuctionState::GetClaimForSquad(const FSquadID& SquadID) const
{
	// AwardClaim does not refuse a squad a second claim, so this first-match must not be decided
	// by hash order: walk ascending ObjectiveID and return the lowest-id active claim.
	for (const int64 ObjectiveID : GalaxyDeterminism::SortedKeys(ActiveClaims))
	{
		const FObjectiveClaim& Claim = ActiveClaims.FindChecked(ObjectiveID);
		if (Claim.ClaimingSquadID == SquadID && Claim.bIsActive)
		{
			return &Claim;
		}
	}
	return nullptr;
}

bool FBidAuctionState::CanReassign(int64 ObjectiveID, float CurrentTime) const
{
	const FObjectiveClaim* Claim = ActiveClaims.Find(ObjectiveID);
	if (!Claim) return true; // No claim = can assign
	return !Claim->IsInCooldown(CurrentTime);
}
