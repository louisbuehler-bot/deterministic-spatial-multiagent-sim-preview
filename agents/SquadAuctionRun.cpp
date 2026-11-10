// Excerpt from a larger file: one auction pass, where squads bid for open objectives.

namespace StrategicGoalEmissionSystem::AuctionPass
{
	using StrategicGoalEmissionInternal::ArcBetween;
	using StrategicGoalEmissionInternal::DeriveSquadFacts;
	using StrategicGoalEmissionInternal::FSquadDerivedFacts;

	/** One eligible bidder: an idle/moving squad of the board's empire. */
	struct FBidderEntry
	{
		entt::entity SquadEntity = entt::null;
		int64 SquadId = 0;
		int32 PlanetId = INDEX_NONE;
		FVector3d CentroidN = FVector3d::ZeroVector;
		FSquadCapabilityProfile Capability;
		bool bAssignedThisPass = false;
	};

	/** Eligible bidders: this empire's idle/moving squads with a valid centroid
	 *  and no current objective. Sorted by SquadId for deterministic award order. */
	TArray<FBidderEntry> CollectBidders(entt::registry& Registry, const CStrategicGoalBoard& Board)
	{
		const TMap<int64, FSquadDerivedFacts> SquadFacts = DeriveSquadFacts(Registry);
		TArray<FBidderEntry> Bidders;
		auto SquadView = Registry.view<CSquad, CSquadCentroid>();
		for (auto SquadEntity : SquadView) // entt-order-safe -- collected, sorted below
		{
			const CSquad& Squad = SquadView.get<CSquad>(SquadEntity);
			// SquadId 0 = uninitialized. NEGATIVE ids are legitimate reserved ranges
			// (scenario squads -3000000..; assembly temps -1000000..); demo squads
			// (-9999) self-exclude because their raw-emplace membership never writes
			// the CEmpireSlot.SquadId reverse index the facts lookup below requires.
			if (Squad.SquadId == 0 || Squad.CurrentObjective.HasObjective())
			{
				continue;
			}
			if (Squad.State != ESquadState::Idle && Squad.State != ESquadState::Moving)
			{
				continue;
			}
			if (Registry.any_of<CSettlerParty>(SquadEntity))
			{
				continue; // settler parties are single-purpose: they never bid on military goals
			}
			const FSquadDerivedFacts* Facts = SquadFacts.Find(Squad.SquadId);
			if (!Facts || Facts->EmpireSlotId != Board.EmpireSlotId)
			{
				continue;
			}
			const CSquadCentroid& Centroid = SquadView.get<CSquadCentroid>(SquadEntity);
			if (Centroid.CentroidN.IsNearlyZero())
			{
				continue;
			}

			FBidderEntry Entry;
			Entry.SquadEntity = SquadEntity;
			Entry.SquadId = Squad.SquadId;
			Entry.PlanetId = Facts->PlanetId;
			Entry.CentroidN = Centroid.CentroidN.GetSafeNormal();
			Entry.Capability = FSquadCapabilityProfile::Compute(Registry, SquadEntity);
			Bidders.Add(MoveTemp(Entry));
		}
		Bidders.Sort([](const FBidderEntry& A, const FBidderEntry& B) { return A.SquadId < B.SquadId; });
		return Bidders;
	}

	/** Open objectives, highest priority first (ties by id) -- the more
	 *  important goal gets first pick of the squad pool. */
	TArray<int32> CollectOpenObjectiveIndices(const CStrategicGoalBoard& Board, float TimeSeconds)
	{
		TArray<int32> OpenObjectiveIndices;
		for (int32 Index = 0; Index < Board.Auction.AvailableObjectives.Num(); ++Index)
		{
			const FObjectiveDefinition& Objective = Board.Auction.AvailableObjectives[Index];
			if (Objective.bIsOpen && Board.Auction.CanReassign(Objective.ObjectiveID, TimeSeconds))
			{
				OpenObjectiveIndices.Add(Index);
			}
		}
		OpenObjectiveIndices.Sort([&Board](int32 A, int32 B)
		{
			const FObjectiveDefinition& ObjA = Board.Auction.AvailableObjectives[A];
			const FObjectiveDefinition& ObjB = Board.Auction.AvailableObjectives[B];
			if (ObjA.Priority != ObjB.Priority)
			{
				return ObjA.Priority > ObjB.Priority;
			}
			return ObjA.ObjectiveID < ObjB.ObjectiveID;
		});
		return OpenObjectiveIndices;
	}

	/** Whether a bidder may bid on the objective at all: still unassigned this pass, meeting the hard
	 *  demand requirements, with hold space for a haul and grab room for a hunt. */
	bool CanBid(entt::registry& Registry, const FBidderEntry& Bidder, const FObjectiveDefinition& Objective)
	{
		if (Bidder.bAssignedThisPass)
		{
			return false;
		}
		if (!Objective.DemandProfile.IsSatisfiedBy(Bidder.Capability))
		{
			return false; // hard requirements (size, stealth, independence) not met
		}
		if (Objective.ObjectiveType == EObjectiveType::Transport
			&& SupplyTransferSystem::SquadCargoHeadroom(Registry, Bidder.SquadEntity) <= KINDA_SMALL_NUMBER)
		{
			return false; // hauls need hold space -- full/holdless squads cannot bid
		}
		if (Objective.ObjectiveType == EObjectiveType::HarvestWildlife
			&& !WildlifeHarvestSystem::SquadCanStillGrab(Registry, Bidder.SquadEntity))
		{
			// Hunts need GRAB room, which is per member: squad-summed headroom let a squad whose room was
			// scattered below HuntGrabHeadroomMin win a hunt, then turn home the moment it arrived.
			return false;
		}
		return true;
	}

	/** The strongest bid on the objective, or nullptr when no bidder can take it this pass. */
	FBidderEntry* FindBestBidder(
		entt::registry& Registry,
		TArray<FBidderEntry>& Bidders,
		const FObjectiveDefinition& Objective,
		float PlanetRadius,
		float TimeSeconds,
		FObjectiveBid& BestBid)
	{
		FBidderEntry* Best = nullptr;
		for (FBidderEntry& Bidder : Bidders)
		{
			if (!CanBid(Registry, Bidder, Objective))
			{
				continue;
			}
			// Hauls bid on distance to the PICKUP (their first leg); everything
			// else on distance to the target.
			const FVector3d& BidLegDirection =
				(Objective.ObjectiveType == EObjectiveType::Transport && !Objective.SourceDirectionN.IsNearlyZero())
					? Objective.SourceDirectionN
					: Objective.TargetDirectionN;
			const float DistanceWorld = BidLegDirection.IsNearlyZero()
				? 0.0f
				: ArcBetween(Bidder.CentroidN, BidLegDirection) * PlanetRadius;

			FSquadID BidderId;
			BidderId.TacticalCardEntityID = static_cast<int32>(Bidder.SquadId);
			const FObjectiveBid Bid = BidCalculation::BuildBid(
				BidderId, Objective.ObjectiveID, Objective.DemandProfile,
				Bidder.Capability, DistanceWorld, TimeSeconds);

			// Deterministic best: higher strength wins; ties go to the lower SquadId
			// (bidders are iterated in ascending SquadId order, so first wins ties).
			if (Bid.BidStrength <= 0.0f)
			{
				continue;
			}
			if (Best == nullptr || Bid.BidStrength > BestBid.BidStrength)
			{
				Best = &Bidder;
				BestBid = Bid;
			}
		}
		return Best;
	}

	/** Award the objective to the winning bidder and send its squad on the way. */
	void AwardObjective(
		entt::registry& Registry,
		CStrategicGoalBoard& Board,
		FBidderEntry& Winner,
		const FObjectiveDefinition& Objective,
		const FObjectiveBid& BestBid,
		float TimeSeconds)
	{
		Board.Auction.SubmitBid(BestBid);
		Board.Auction.AwardClaim(Objective.ObjectiveID, BestBid.BiddingSquadID, TimeSeconds, BestBid.BidStrength);
		if (FObjectiveClaim* Claim = Board.Auction.ActiveClaims.Find(Objective.ObjectiveID))
		{
			Claim->bAcknowledged = true; // shard-local synchronous assignment
		}

		Winner.bAssignedThisPass = true;
		CSquad& Squad = Registry.get<CSquad>(Winner.SquadEntity);
		AssignObjectiveToSquad(Registry, Winner.SquadEntity, Squad, Objective, Winner.PlanetId);

		UE_LOG(LogStrategicGoals, Log,
			TEXT("[StrategicGoals][Award] Empire=%d Objective=%lld Type=%d -> Squad=%lld (bid=%.2f)"),
			Board.EmpireSlotId, Objective.ObjectiveID,
			static_cast<int32>(Objective.ObjectiveType), Winner.SquadId, BestBid.BidStrength);
	}
}

int32 StrategicGoalEmissionSystem::RunAuction(
	entt::registry& Registry,
	CStrategicGoalBoard& Board,
	float PlanetRadius,
	float TimeSeconds)
{
	TArray<AuctionPass::FBidderEntry> Bidders = AuctionPass::CollectBidders(Registry, Board);
	if (Bidders.Num() == 0)
	{
		return 0;
	}

	const TArray<int32> OpenObjectiveIndices = AuctionPass::CollectOpenObjectiveIndices(Board, TimeSeconds);

	int32 Assigned = 0;
	for (const int32 ObjectiveIndex : OpenObjectiveIndices)
	{
		// Index stays valid: nothing in this loop adds/removes objectives.
		const FObjectiveDefinition& Objective = Board.Auction.AvailableObjectives[ObjectiveIndex];

		if (Objective.ObjectiveType == EObjectiveType::Settle)
		{
			// Settle goals are pre-assigned to their dedicated settler party at
			// post time; one reopened by the dead-squad release pass is an ORPHAN
			// the settler pass removes -- never re-auction it to a military squad.
			continue;
		}

		FObjectiveBid BestBid;
		AuctionPass::FBidderEntry* Best =
			AuctionPass::FindBestBidder(Registry, Bidders, Objective, PlanetRadius, TimeSeconds, BestBid);
		if (Best == nullptr)
		{
			continue; // nobody can take this objective this pass
		}

		AuctionPass::AwardObjective(Registry, Board, *Best, Objective, BestBid, TimeSeconds);
		++Assigned;
	}

	return Assigned;
}
