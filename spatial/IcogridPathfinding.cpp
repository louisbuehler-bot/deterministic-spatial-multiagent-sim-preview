// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================

#include "IcogridPathfinding.h"
#include "ECS/Spatial/FineEdgeOverlay.h"
#include "ECS/Spatial/IcogridGroundLaw.h" // the one zeon-aware edge law
#include "Math/GalaxyMathRays.h"
#include "Math/GalaxyMathSpherical.h" // GalaxyMath::RobustSlerp
#include "CoreTypes/TerrainRoughnessScale.h"

// ============================================================================
// A* Implementation
// ============================================================================

namespace
{
	/** Great-circle arc distance (radians) between two unit directions. */
	FORCEINLINE float ArcDistance(const FVector3f& A, const FVector3f& B)
	{
		// Clamp dot product to avoid acos domain errors from floating-point noise
		const float Dot = FMath::Clamp(FVector3f::DotProduct(A, B), -1.0f, 1.0f);
		return FMath::Acos(Dot);
	}

	/** Legacy edge traversal cost with independent slope and roughness penalties.
	 *  When both weights are zero, operates in pure geodesic mode. This makes the
	 *  arc-distance heuristic nearly perfect, enabling efficient topology-only
	 *  queries like checkpoint baking.
	 */
	FORCEINLINE float EdgeCost(
		const FIcogrid& Grid,
		int32 From,
		int32 To,
		const FIcogridTraversalWeights& TraversalWeights)
	{
		const float Arc = ArcDistance(Grid.VertexDirections[From], Grid.VertexDirections[To]);

		// Pure geodesic mode: caller wants unweighted arc distance.
		if (TraversalWeights.SlopePenaltyFactor == 0.0f
			&& TraversalWeights.RoughnessPenaltyFactor == 0.0f)
		{
			return Arc;
		}

		float Penalty = 0.0f;

		// A border edge (one zeon end) pays its non-zeon end's own ground, as IcogridGroundLaw::SampleEdge
		// reads it; every other edge reads both ends.
		const int32 BorderGround = IcogridGroundLaw::BorderEdgeGroundVertex(Grid, From, To);
		const int32 GroundA = BorderGround != INDEX_NONE ? BorderGround : From;
		const int32 GroundB = BorderGround != INDEX_NONE ? BorderGround : To;

		if (TraversalWeights.SlopePenaltyFactor > 0.0f)
		{
			// Use baked per-vertex slope when available; fall back to raw height diff.
			if (Grid.HasSlopes())
			{
				const float MaxSlope = FMath::Max(Grid.Slopes[GroundA], Grid.Slopes[GroundB]);
				Penalty += TraversalWeights.SlopePenaltyFactor * MaxSlope;
			}
			else
			{
				const float HeightDiff = FMath::Abs(Grid.Heights[To] - Grid.Heights[From]);
				Penalty += TraversalWeights.SlopePenaltyFactor * HeightDiff;
			}
		}

		if (TraversalWeights.RoughnessPenaltyFactor > 0.0f && Grid.HasRoughness())
		{
			Penalty += TraversalWeights.RoughnessPenaltyFactor
				* TerrainRoughnessScale::ToOpenExposure(0.5f * (Grid.RoughnessGrade[GroundA] + Grid.RoughnessGrade[GroundB]));
		}

		// Zeon is the optimum: the edge moves toward the bare arc by its zeon share (the same
		// blend the time law uses). On a grid with no zeon the weighted arc is returned unchanged.
		return IcogridGroundLaw::BlendTowardPeak(Arc * (1.0f + Penalty), Arc, IcogridGroundLaw::EdgeBlueGroundFraction(Grid, From, To));
	}

	FORCEINLINE float EdgeTraversalTime(
		const FIcogrid& Grid,
		int32 From,
		int32 To,
		const FMobilityTimeProfile& TimeProfile)
	{
		const float Arc = ArcDistance(Grid.VertexDirections[From], Grid.VertexDirections[To]);
		// The one ground law (IcogridGroundLaw), shared with every checkpoint-bake sampler: slope,
		// roughness and slip as the profile prices them, moved toward the profile's peak on zeon.
		// Slipperiness only bites when the profile carries a SlipperinessPenalty (ground-contact
		// classes); road/hover profiles set it to 0, so their routes are unaffected.
		return IcogridGroundLaw::EdgeTravelTime(TimeProfile, Arc, IcogridGroundLaw::SampleEdge(Grid, From, To));
	}

	/** A* open-set node. */
	struct FAStarNode
	{
		int32 VertexId;
		float FCost; // g + h

		// UE5 heaps are MIN-heaps: HeapPop returns the element the predicate orders first (root).
		// A* must expand the LOWEST-FCost open node first, so order by ascending FCost. (A reversed
		// `FCost > Other.FCost` here makes HeapPop yield the most-expensive node, which pops the goal
		// via the first/expensive branch before a cheaper branch relaxes it -- returning suboptimal paths.)
		bool operator<(const FAStarNode& Other) const { return FCost < Other.FCost; }
	};

	/**
	 * Fine: the fine layer, or null for none. An edge it closes is skipped; what it adds to an edge (an arc) is
	 * charged at FineArcToCost per radian, the search's own cost units. With Fine null every cost is the edge cost
	 * function's alone, so the existing overloads are unchanged bit for bit.
	 */
	template <typename EdgeCostFuncType, typename HeuristicFuncType>
	FIcogridPathResult FindPathInternal(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		TFunctionRef<bool(int32)> AcceptPredicate,
		EdgeCostFuncType&& ComputeEdgeCost,
		HeuristicFuncType&& ComputeHeuristic,
		int32 MaxNodesToEvaluate,
		const FFineEdgeRouting* Fine = nullptr,
		float FineArcToCost = 1.0f)
	{
		FIcogridPathResult Result;

		if (!Grid.HasAdjacency())
		{
			return Result;
		}

		if (StartVertex < 0 || StartVertex >= Grid.VertexCount
			|| GoalVertex < 0 || GoalVertex >= Grid.VertexCount)
		{
			return Result;
		}

		if (!AcceptPredicate(StartVertex) || !AcceptPredicate(GoalVertex))
		{
			return Result;
		}

		if (StartVertex == GoalVertex)
		{
			Result.Waypoints.Add(StartVertex);
			Result.bSuccess = true;
			return Result;
		}

		const int32 NodeCap = (MaxNodesToEvaluate > 0) ? MaxNodesToEvaluate : Grid.VertexCount;

		TArray<float> GCost;
		GCost.SetNumUninitialized(Grid.VertexCount);
		for (int32 I = 0; I < Grid.VertexCount; ++I)
		{
			GCost[I] = TNumericLimits<float>::Max();
		}
		GCost[StartVertex] = 0.0f;

		TArray<int32> CameFrom;
		CameFrom.SetNumUninitialized(Grid.VertexCount);
		for (int32 I = 0; I < Grid.VertexCount; ++I)
		{
			CameFrom[I] = INDEX_NONE;
		}

		TArray<bool> Closed;
		Closed.SetNumZeroed(Grid.VertexCount);

		TArray<FAStarNode> OpenHeap;
		OpenHeap.Reserve(256);

		const float H0 = ComputeHeuristic(StartVertex);
		OpenHeap.Add({StartVertex, H0});

		int32 NodesEvaluated = 0;

		while (OpenHeap.Num() > 0 && NodesEvaluated < NodeCap)
		{
			FAStarNode CurrentNode;
			OpenHeap.HeapPop(CurrentNode);

			if (Closed[CurrentNode.VertexId])
			{
				continue;
			}
			Closed[CurrentNode.VertexId] = true;
			++NodesEvaluated;

			if (CurrentNode.VertexId == GoalVertex)
			{
				Result.bSuccess = true;
				Result.TotalCost = GCost[GoalVertex];
				Result.NodesEvaluated = NodesEvaluated;

				int32 V = GoalVertex;
				while (V != INDEX_NONE)
				{
					Result.Waypoints.Add(V);
					V = CameFrom[V];
				}
				Algo::Reverse(Result.Waypoints);
				return Result;
			}

			const TConstArrayView<int32> Neighbors = Grid.GetNeighbors(CurrentNode.VertexId);
			const int32 FirstSlot = Grid.AdjacencyOffsets[CurrentNode.VertexId];
			for (int32 NeighborIndex = 0; NeighborIndex < Neighbors.Num(); ++NeighborIndex)
			{
				const int32 Neighbor = Neighbors[NeighborIndex];
				if (Closed[Neighbor] || !AcceptPredicate(Neighbor))
				{
					continue;
				}

				float EdgeCostValue = ComputeEdgeCost(CurrentNode.VertexId, Neighbor);
				if (Fine != nullptr)
				{
					const int32 Slot = FirstSlot + NeighborIndex;
					if (!Fine->CanTake(Slot))
					{
						continue;
					}
					EdgeCostValue += static_cast<float>(Fine->CrossingArc(Slot)) * FineArcToCost;
				}

				const float TentativeG = GCost[CurrentNode.VertexId] + EdgeCostValue;

				if (TentativeG < GCost[Neighbor])
				{
					GCost[Neighbor] = TentativeG;
					CameFrom[Neighbor] = CurrentNode.VertexId;

					const float H = ComputeHeuristic(Neighbor);
					OpenHeap.HeapPush({Neighbor, TentativeG + H});
				}
			}
		}

		Result.NodesEvaluated = NodesEvaluated;
		return Result;
	}
}

float FIcogridPathfinding::ComputeEdgeEffectiveArc(
	const FIcogrid& Grid,
	int32 From,
	int32 To,
	const FMobilityTimeProfile& TimeProfile)
{
	if (!Grid.VertexDirections.IsValidIndex(From)
		|| !Grid.VertexDirections.IsValidIndex(To)
		|| !TimeProfile.IsValid())
	{
		return 0.0f;
	}

	// time * BaseSpeed = arc * terrain multiplier: the profile's class base speed
	// cancels, leaving only the terrain law (see the header contract).
	return EdgeTraversalTime(Grid, From, To, TimeProfile) * TimeProfile.BaseSpeed;
}

int32 FIcogridPathfinding::FindEdgeSlot(const FIcogrid& Grid, int32 From, int32 To)
{
	if (!Grid.HasAdjacency() || From < 0 || From >= Grid.VertexCount)
	{
		return INDEX_NONE;
	}
	for (int32 Slot = Grid.AdjacencyOffsets[From]; Slot < Grid.AdjacencyOffsets[From + 1]; ++Slot)
	{
		if (Grid.AdjacencyNeighbors[Slot] == To)
		{
			return Slot;
		}
	}
	return INDEX_NONE;
}

float FIcogridPathfinding::ComputeEdgeEffectiveArc(
	const FIcogrid& Grid,
	int32 From,
	int32 To,
	const FMobilityTimeProfile& TimeProfile,
	const FFineEdgeRouting& Fine)
{
	const float EffectiveArc = ComputeEdgeEffectiveArc(Grid, From, To, TimeProfile);
	if (!Fine.IsActive() || EffectiveArc <= 0.0f)
	{
		return EffectiveArc;
	}
	// The fine search charges CrossingArc / BaseSpeed of time; as an arc that is CrossingArc itself.
	return EffectiveArc + static_cast<float>(Fine.CrossingArc(FindEdgeSlot(Grid, From, To)));
}

FIcogridPathResult FIcogridPathfinding::FindPath(
	const FIcogrid& Grid,
	int32 StartVertex,
	int32 GoalVertex,
	TFunctionRef<bool(int32)> AcceptPredicate,
	const FMobilityTimeProfile& TimeProfile,
	const FFineEdgeRouting& Fine,
	int32 MaxNodesToEvaluate)
{
	auto ComputeEdgeCost = [&Grid, &TimeProfile](int32 From, int32 To)
	{
		return EdgeTraversalTime(Grid, From, To, TimeProfile);
	};

	auto ComputeHeuristic = [&Grid, GoalVertex, &TimeProfile](int32 VertexIndex)
	{
		return TimeProfile.ComputeHeuristicTime(
			ArcDistance(Grid.VertexDirections[VertexIndex], Grid.VertexDirections[GoalVertex]));
	};

	// An added arc is walked at the profile's base speed, as ComputeTravelTime walks a flat one.
	return FindPathInternal(
		Grid,
		StartVertex,
		GoalVertex,
		AcceptPredicate,
		ComputeEdgeCost,
		ComputeHeuristic,
		MaxNodesToEvaluate,
		Fine.IsActive() ? &Fine : nullptr,
		1.0f / FMath::Max(TimeProfile.BaseSpeed, KINDA_SMALL_NUMBER));
}

FIcogridPathResult FIcogridPathfinding::FindPath(
	const FIcogrid& Grid,
	int32 StartVertex,
	int32 GoalVertex,
	float SlopePenaltyFactor,
	const FFineEdgeRouting& Fine,
	int32 MaxNodesToEvaluate)
{
	const FIcogridTraversalWeights TraversalWeights = {
		SlopePenaltyFactor,
		SlopePenaltyFactor == 0.0f ? 0.0f : 1.0f
	};

	auto AcceptAllVertices = [](int32)
	{
		return true;
	};

	auto ComputeEdgeCost = [&Grid, &TraversalWeights](int32 From, int32 To)
	{
		return EdgeCost(Grid, From, To, TraversalWeights);
	};

	auto ComputeHeuristic = [&Grid, GoalVertex](int32 VertexIndex)
	{
		return ArcDistance(Grid.VertexDirections[VertexIndex], Grid.VertexDirections[GoalVertex]);
	};

	return FindPathInternal(
		Grid,
		StartVertex,
		GoalVertex,
		AcceptAllVertices,
		ComputeEdgeCost,
		ComputeHeuristic,
		MaxNodesToEvaluate,
		Fine.IsActive() ? &Fine : nullptr,
		1.0f);
}

FIcogridPathResult FIcogridPathfinding::FindPath(
	const FIcogrid& Grid,
	int32 StartVertex,
	int32 GoalVertex,
	const FMobilityTimeProfile& TimeProfile,
	int32 MaxNodesToEvaluate)
{
	auto AcceptAllVertices = [](int32)
	{
		return true;
	};

	return FindPath(
		Grid,
		StartVertex,
		GoalVertex,
		AcceptAllVertices,
		TimeProfile,
		MaxNodesToEvaluate);
}

FIcogridPathResult FIcogridPathfinding::FindPath(
	const FIcogrid& Grid,
	int32 StartVertex,
	int32 GoalVertex,
	TFunctionRef<bool(int32)> AcceptPredicate,
	const FMobilityTimeProfile& TimeProfile,
	int32 MaxNodesToEvaluate)
{
	auto ComputeEdgeCost = [&Grid, &TimeProfile](int32 From, int32 To)
	{
		return EdgeTraversalTime(Grid, From, To, TimeProfile);
	};

	auto ComputeHeuristic = [&Grid, GoalVertex, &TimeProfile](int32 VertexIndex)
	{
		return TimeProfile.ComputeHeuristicTime(
			ArcDistance(Grid.VertexDirections[VertexIndex], Grid.VertexDirections[GoalVertex]));
	};

	return FindPathInternal(
		Grid,
		StartVertex,
		GoalVertex,
		AcceptPredicate,
		ComputeEdgeCost,
		ComputeHeuristic,
		MaxNodesToEvaluate);
}

FIcogridPathResult FIcogridPathfinding::FindPath(
	const FIcogrid& Grid,
	int32 StartVertex,
	int32 GoalVertex,
	TFunctionRef<bool(int32)> AcceptPredicate,
	const FMobilityTimeProfile& TimeProfile,
	TFunctionRef<float(int32)> VertexArrivalCostMultiplier,
	int32 MaxNodesToEvaluate)
{
	auto ComputeEdgeCost = [&Grid, &TimeProfile, &VertexArrivalCostMultiplier](int32 From, int32 To)
	{
		return EdgeTraversalTime(Grid, From, To, TimeProfile) * FMath::Clamp(VertexArrivalCostMultiplier(To), 0.0f, 1.0f);
	};

	auto ComputeHeuristic = [&Grid, GoalVertex, &TimeProfile](int32 VertexIndex)
	{
		return TimeProfile.ComputeHeuristicTime(
			ArcDistance(Grid.VertexDirections[VertexIndex], Grid.VertexDirections[GoalVertex]));
	};

	return FindPathInternal(
		Grid,
		StartVertex,
		GoalVertex,
		AcceptPredicate,
		ComputeEdgeCost,
		ComputeHeuristic,
		MaxNodesToEvaluate);
}

FIcogridPathResult FIcogridPathfinding::FindPath(
	const FIcogrid& Grid,
	int32 StartVertex,
	int32 GoalVertex,
	const FIcogridTraversalWeights& TraversalWeights,
	int32 MaxNodesToEvaluate)
{
	auto AcceptAllVertices = [](int32)
	{
		return true;
	};

	auto ComputeEdgeCost = [&Grid, &TraversalWeights](int32 From, int32 To)
	{
		return EdgeCost(Grid, From, To, TraversalWeights);
	};

	auto ComputeHeuristic = [&Grid, GoalVertex](int32 VertexIndex)
	{
		return ArcDistance(Grid.VertexDirections[VertexIndex], Grid.VertexDirections[GoalVertex]);
	};

	return FindPathInternal(
		Grid,
		StartVertex,
		GoalVertex,
		AcceptAllVertices,
		ComputeEdgeCost,
		ComputeHeuristic,
		MaxNodesToEvaluate);
}

FIcogridPathResult FIcogridPathfinding::FindPath(
	const FIcogrid& Grid,
	int32 StartVertex,
	int32 GoalVertex,
	float SlopePenaltyFactor,
	int32 MaxNodesToEvaluate)
	{
	const FIcogridTraversalWeights TraversalWeights = {
		SlopePenaltyFactor,
		SlopePenaltyFactor == 0.0f ? 0.0f : 1.0f
	};
	return FindPath(Grid, StartVertex, GoalVertex, TraversalWeights, MaxNodesToEvaluate);
	}

FIcogridPathResult FIcogridPathfinding::FindPath(
	const FIcogrid& Grid,
	int32 StartVertex,
	int32 GoalVertex,
	TFunctionRef<bool(int32)> AcceptPredicate,
	const FIcogridTraversalWeights& TraversalWeights,
	int32 MaxNodesToEvaluate)
{
	auto ComputeEdgeCost = [&Grid, &TraversalWeights](int32 From, int32 To)
	{
		return EdgeCost(Grid, From, To, TraversalWeights);
	};

	auto ComputeHeuristic = [&Grid, GoalVertex](int32 VertexIndex)
	{
		return ArcDistance(Grid.VertexDirections[VertexIndex], Grid.VertexDirections[GoalVertex]);
	};

	return FindPathInternal(
		Grid,
		StartVertex,
		GoalVertex,
		AcceptPredicate,
		ComputeEdgeCost,
		ComputeHeuristic,
		MaxNodesToEvaluate);
}

FIcogridPathResult FIcogridPathfinding::FindPath(
	const FIcogrid& Grid,
	int32 StartVertex,
	int32 GoalVertex,
	TFunctionRef<bool(int32)> AcceptPredicate,
	float SlopePenaltyFactor,
	int32 MaxNodesToEvaluate)
{
	const FIcogridTraversalWeights TraversalWeights = {
		SlopePenaltyFactor,
		SlopePenaltyFactor == 0.0f ? 0.0f : 1.0f
	};
	return FindPath(Grid, StartVertex, GoalVertex, AcceptPredicate, TraversalWeights, MaxNodesToEvaluate);
}

// ============================================================================
// FindNearestVertex
// ============================================================================

int32 FIcogridPathfinding::FindNearestVertex(
	const FIcogrid& Grid,
	const FVector3f& Direction)
{
	if (Grid.VertexCount <= 0)
	{
		return INDEX_NONE;
	}

	// Dispatch to hierarchical path when accelerator is available
	if (Grid.DescentAccelerator.IsValid())
	{
		return FindNearestVertexHierarchical(Grid, Direction, /*bExactMode=*/ true);
	}

	const FVector3f Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return INDEX_NONE;
	}

	int32 BestIdx = 0;
	float BestDot = FVector3f::DotProduct(Dir, Grid.VertexDirections[0]);

	for (int32 I = 1; I < Grid.VertexCount; ++I)
	{
		const float Dot = FVector3f::DotProduct(Dir, Grid.VertexDirections[I]);
		if (Dot > BestDot)
		{
			BestDot = Dot;
			BestIdx = I;
		}
	}

	return BestIdx;
}

// ============================================================================
// FindNearestVertexHierarchical
// ============================================================================

int32 FIcogridPathfinding::FindNearestVertexHierarchical(
	const FIcogrid& Grid,
	const FVector3f& Direction,
	bool bExactMode)
{
	if (Grid.VertexCount <= 0 || !Grid.DescentAccelerator.IsValid())
	{
		return INDEX_NONE;
	}

	const FVector3f Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return INDEX_NONE;
	}

	const FIcogridDescentAccelerator& Accel = Grid.DescentAccelerator;
	const int32 LeafId = Accel.DescendToLeaf(Dir);
	if (LeafId < 0 || LeafId >= Accel.NumLeaves)
	{
		return INDEX_NONE;
	}

	int32 BestIdx = INDEX_NONE;
	float BestDot = -2.0f;

	// Scan the primary leaf
	auto ScanLeaf = [&](int32 Leaf)
	{
		if (Leaf < 0 || Leaf >= Accel.NumLeaves) return;
		const TConstArrayView<int32> Verts = Accel.GetLeafVertices(Leaf);
		for (const int32 V : Verts)
		{
			const float Dot = FVector3f::DotProduct(Dir, Grid.VertexDirections[V]);
			if (Dot > BestDot)
			{
				BestDot = Dot;
				BestIdx = V;
			}
		}
	};

	ScanLeaf(LeafId);

	// In exact mode, also scan adjacent leaves for a wider initial candidate pool
	if (bExactMode && Accel.HasLeafAdjacency())
	{
		int32 Neighbors[3];
		Accel.GetLeafNeighbors(LeafId, Neighbors);
		for (int32 I = 0; I < 3; ++I)
		{
			ScanLeaf(Neighbors[I]);
		}
	}

	// Greedy walk refinement (both modes): walk the grid adjacency from
	// BestIdx until no closer vertex is found. On an Icogrid Voronoi
	// diagram, this converges to the nearest vertex in 1-2 hops from a
	// good starting point (~6-12 extra dot products, negligible cost).
	if (BestIdx != INDEX_NONE && Grid.HasAdjacency())
	{
		bool bImproved = true;
		while (bImproved)
		{
			bImproved = false;
			const TConstArrayView<int32> Adj = Grid.GetNeighbors(BestIdx);
			for (const int32 N : Adj)
			{
				const float Dot = FVector3f::DotProduct(Dir, Grid.VertexDirections[N]);
				if (Dot > BestDot)
				{
					BestDot = Dot;
					BestIdx = N;
					bImproved = true;
				}
			}
		}
	}

	return BestIdx;
}

// ============================================================================
// FindVerticesInRadius
// ============================================================================

FIcogridRadiusResult FIcogridPathfinding::FindVerticesInRadius(
	const FIcogrid& Grid,
	const FVector3f& CenterDirection,
	float RadiusRadians)
{
	FIcogridRadiusResult Result;

	if (!Grid.HasAdjacency() || RadiusRadians <= 0.0f)
	{
		return Result;
	}

	const FVector3f Dir = CenterDirection.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return Result;
	}

	const int32 Seed = FindNearestVertex(Grid, Dir);
	if (Seed == INDEX_NONE)
	{
		return Result;
	}
	Result.SeedVertex = Seed;

	// Dot-product threshold equivalent to the angular radius.
	// dot(v, dir) >= cos(R)  <=>  angle(v, dir) <= R
	const float CosThreshold = FMath::Cos(RadiusRadians);

	// Seed must be inside the cap. Can only fail if RadiusRadians is extremely
	// small and no vertex falls within it.
	if (FVector3f::DotProduct(Dir, Grid.VertexDirections[Seed]) < CosThreshold)
	{
		return Result;
	}

	// Solid-angle fraction of a spherical cap: (1 - cos R) / 2.
	// Pre-reserve storage to avoid repeated reallocation on large radii.
	const float CapFraction = (1.0f - FMath::Cos(RadiusRadians)) * 0.5f;
	const int32 ExpectedCount = FMath::Clamp(
		FMath::CeilToInt(Grid.VertexCount * CapFraction * 1.1f), 8, Grid.VertexCount);
	Result.VertexIndices.Reserve(ExpectedCount);

	// Visited array: O(1) lookup per vertex, no hashing overhead.
	TArray<bool> Visited;
	Visited.SetNumZeroed(Grid.VertexCount);

	// Use the result array itself as the BFS queue to avoid a second allocation.
	// Head advances through already-collected results; tail grows as we accept more.
	Result.VertexIndices.Add(Seed);
	Visited[Seed] = true;

	int32 Head = 0;
	while (Head < Result.VertexIndices.Num())
	{
		const int32 V = Result.VertexIndices[Head++];
		const TConstArrayView<int32> Neighbors = Grid.GetNeighbors(V);
		for (const int32 N : Neighbors)
		{
			if (Visited[N])
			{
				continue;
			}
			Visited[N] = true;
			// Only collect and expand further from vertices inside the cap.
			// Correctness: the spherical cap is convex, so its induced subgraph
			// on the Icogrid is connected -- no inside vertex is reachable
			// only through outside vertices.
			if (FVector3f::DotProduct(Dir, Grid.VertexDirections[N]) >= CosThreshold)
			{
				Result.VertexIndices.Add(N);
			}
		}
	}

	return Result;
}

// ============================================================================
// FloodFill
// ============================================================================

TArray<int32> FIcogridPathfinding::FloodFill(
	const FIcogrid& Grid,
	int32 SeedVertex,
	TFunctionRef<bool(int32)> AcceptPredicate,
	int32 MaxVertices)
{
	TArray<int32> Result;

	if (!Grid.HasAdjacency()
		|| SeedVertex < 0 || SeedVertex >= Grid.VertexCount
		|| !AcceptPredicate(SeedVertex))
	{
		return Result;
	}

	const int32 Cap = MaxVertices > 0 ? MaxVertices : Grid.VertexCount;

	TArray<bool> Visited;
	Visited.SetNumZeroed(Grid.VertexCount);

	// Use result as the BFS queue -- same trick as FindVerticesInRadius.
	Result.Reserve(FMath::Min(Cap, 256));
	Result.Add(SeedVertex);
	Visited[SeedVertex] = true;

	int32 Head = 0;
	while (Head < Result.Num() && Result.Num() < Cap)
	{
		const int32 V = Result[Head++];
		const TConstArrayView<int32> Neighbors = Grid.GetNeighbors(V);
		for (const int32 N : Neighbors)
		{
			if (Visited[N])
			{
				continue;
			}
			Visited[N] = true;
			if (AcceptPredicate(N))
			{
				Result.Add(N);
				if (Result.Num() >= Cap)
				{
					break;
				}
			}
		}
	}

	return Result;
}

// ============================================================================
// RayIntersect
// ============================================================================

FIcogridRayResult FIcogridPathfinding::RayIntersect(
	const FIcogrid& Grid,
	const FVector3f& RayOrigin,
	const FVector3f& RayDir,
	float SphereRadius)
{
	FIcogridRayResult Result;

	const float T = GalaxyMath::RaySphereIntersect(
		RayOrigin, RayDir, FVector3f::ZeroVector, SphereRadius);

	if (T < 0.0f)
	{
		return Result;
	}

	const FVector3f HitPoint = RayOrigin + T * RayDir;
	const FVector3f HitDirection = HitPoint.GetSafeNormal();

	if (HitDirection.IsNearlyZero())
	{
		// Degenerate: ray origin exactly at sphere center
		return Result;
	}

	Result.bHit = true;
	Result.T = T;
	Result.HitDirection = HitDirection;
	Result.NearestVertex = FindNearestVertex(Grid, HitDirection);

	return Result;
}

// ============================================================================
// FindContainingTriangle
// ============================================================================

FIcogridTriangleResult FIcogridPathfinding::FindContainingTriangle(
	const FIcogrid& Grid,
	const FVector3f& Direction)
{
	FIcogridTriangleResult Result;

	if (!Grid.HasAdjacency() || Grid.TriangleIndices.Num() == 0)
	{
		return Result;
	}

	const FVector3f Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return Result;
	}

	const int32 Center = FindNearestVertex(Grid, Dir);
	if (Center == INDEX_NONE)
	{
		return Result;
	}

	// Enumerate the triangle fan around Center.
	// A triangle (Center, Ni, Nj) exists iff Ni and Nj are mutual neighbors.
	const TConstArrayView<int32> CenterNeighbors = Grid.GetNeighbors(Center);
	const int32 Degree = CenterNeighbors.Num();

	// Build a small set of Center's neighbors for O(1) lookup.
	// Max degree on an Icogrid is 6, so a flat scan is fine.

	// For each pair of Center's neighbors, check if they share an edge.
	// On an Icogrid, each vertex has 5 or 6 incident triangles, and the
	// neighbors form a ring. We check all pairs (not just consecutive) to be
	// robust against arbitrary adjacency ordering.

	// Lambda: check if vertex A is a neighbor of vertex B.
	auto AreNeighbors = [&Grid](int32 A, int32 B) -> bool
	{
		const TConstArrayView<int32> Adj = Grid.GetNeighbors(A);
		for (const int32 N : Adj)
		{
			if (N == B)
			{
				return true;
			}
		}
		return false;
	};

	// Lambda: compute barycentrics of Dir projected onto the tangent plane of
	// triangle (A, B, C) on the unit sphere. Returns barycentrics in OutBary.
	// Returns true if all barycentrics are >= -epsilon (point inside triangle).
	auto ComputeBarycentrics = [&Grid, &Dir](int32 A, int32 B, int32 C, FVector3f& OutBary) -> bool
	{
		const FVector3f& PA = Grid.VertexDirections[A];
		const FVector3f& PB = Grid.VertexDirections[B];
		const FVector3f& PC = Grid.VertexDirections[C];

		// Project all four directions onto the tangent plane at the triangle
		// centroid. For small spherical triangles (L5+: ~1-2 deg), distortion
		// is negligible.
		const FVector3f Centroid = ((PA + PB + PC) / 3.0f).GetSafeNormal();

		// Gnomonic projection: project from sphere center onto the tangent
		// plane at Centroid. For a point P on the unit sphere:
		//   projected = P / dot(P, Centroid) - Centroid
		// This maps the spherical triangle to a planar triangle with minimal
		// distortion for small triangles.
		const float DotA = FVector3f::DotProduct(PA, Centroid);
		const float DotB = FVector3f::DotProduct(PB, Centroid);
		const float DotC = FVector3f::DotProduct(PC, Centroid);
		const float DotQ = FVector3f::DotProduct(Dir, Centroid);

		// All dots should be positive for front-facing vertices near the query.
		// If any is <= 0 the vertex wraps around the sphere -- ignore.
		if (DotA <= 0.0f || DotB <= 0.0f || DotC <= 0.0f || DotQ <= 0.0f)
		{
			return false;
		}

		const FVector3f ProjA = PA / DotA - Centroid;
		const FVector3f ProjB = PB / DotB - Centroid;
		const FVector3f ProjC = PC / DotC - Centroid;
		const FVector3f ProjQ = Dir / DotQ - Centroid;

		// 2D barycentric solve: use the two tangent-plane axes.
		// v0 = B - A, v1 = C - A, v2 = Q - A
		const FVector3f V0 = ProjB - ProjA;
		const FVector3f V1 = ProjC - ProjA;
		const FVector3f V2 = ProjQ - ProjA;

		const float D00 = FVector3f::DotProduct(V0, V0);
		const float D01 = FVector3f::DotProduct(V0, V1);
		const float D11 = FVector3f::DotProduct(V1, V1);
		const float D20 = FVector3f::DotProduct(V2, V0);
		const float D21 = FVector3f::DotProduct(V2, V1);

		const float Denom = D00 * D11 - D01 * D01;
		// Relative degeneracy test: an absolute epsilon under-scales as subdivision
		// grows (edge lengths shrink, D00*D11 falls quadratically).
		if (Denom < 1.0e-6f * D00 * D11)
		{
			return false; // Degenerate triangle
		}

		const float InvDenom = 1.0f / Denom;
		const float BaryB = (D11 * D20 - D01 * D21) * InvDenom;
		const float BaryC = (D00 * D21 - D01 * D20) * InvDenom;
		const float BaryA = 1.0f - BaryB - BaryC;

		OutBary = FVector3f(BaryA, BaryB, BaryC);

		// Small negative tolerance for edge cases / floating point.
		constexpr float Eps = -1e-4f;
		return BaryA >= Eps && BaryB >= Eps && BaryC >= Eps;
	};

	// Try each triangle in the fan around Center.
	FVector3f BestBary = FVector3f::ZeroVector;
	for (int32 I = 0; I < Degree; ++I)
	{
		for (int32 J = I + 1; J < Degree; ++J)
		{
			const int32 Ni = CenterNeighbors[I];
			const int32 Nj = CenterNeighbors[J];

			if (!AreNeighbors(Ni, Nj))
			{
				continue;
			}

			// (Center, Ni, Nj) is a triangle.
			FVector3f Bary = FVector3f::ZeroVector;
			if (ComputeBarycentrics(Center, Ni, Nj, Bary))
			{
				Result.V0 = Center;
				Result.V1 = Ni;
				Result.V2 = Nj;
				Result.Barycentrics = Bary;
				Result.bFound = true;
				return Result;
			}
		}
	}

	// Fallback: the query point is at a triangle edge and floating-point
	// placed us in none of the fan triangles. Return the triangle with the
	// least-negative minimum barycentric (closest to containment).
	float BestMinBary = -FLT_MAX;
	for (int32 I = 0; I < Degree; ++I)
	{
		for (int32 J = I + 1; J < Degree; ++J)
		{
			const int32 Ni = CenterNeighbors[I];
			const int32 Nj = CenterNeighbors[J];

			if (!AreNeighbors(Ni, Nj))
			{
				continue;
			}

			FVector3f Bary = FVector3f::ZeroVector;
			ComputeBarycentrics(Center, Ni, Nj, Bary);
			const float MinB = FMath::Min3(Bary.X, Bary.Y, Bary.Z);
			if (MinB > BestMinBary)
			{
				BestMinBary = MinB;
				BestBary = Bary;
				Result.V0 = Center;
				Result.V1 = Ni;
				Result.V2 = Nj;
			}
		}
	}

	if (Result.V0 != INDEX_NONE)
	{
		Result.Barycentrics = BestBary;
		Result.bFound = true;
	}

	return Result;
}

float FIcogridPathfinding::SampleHeightFromDirection(
	const FIcogrid& Grid,
	const FVector3f& Direction)
{
	if (Grid.Heights.Num() == 0 || Grid.Heights.Num() != Grid.VertexDirections.Num())
	{
		return 0.0f;
	}

	const FIcogridTriangleResult Tri = FindContainingTriangle(Grid, Direction);
	if (!Tri.bFound)
	{
		return 0.0f;
	}

	return Grid.Heights[Tri.V0] * Tri.Barycentrics.X
		+ Grid.Heights[Tri.V1] * Tri.Barycentrics.Y
		+ Grid.Heights[Tri.V2] * Tri.Barycentrics.Z;
}

// ============================================================================
// Triangle Topology and Any-Angle Surface Paths
// ============================================================================

namespace
{
	struct FIcogridEdgeOwner
	{
		int32 TriangleIndex = INDEX_NONE;
		int32 LocalEdgeIndex = INDEX_NONE;
	};

	FORCEINLINE uint64 MakeUndirectedEdgeKey(int32 A, int32 B)
	{
		const uint32 Lo = static_cast<uint32>(FMath::Min(A, B));
		const uint32 Hi = static_cast<uint32>(FMath::Max(A, B));
		return (static_cast<uint64>(Lo) << 32) | static_cast<uint64>(Hi);
	}

	float EstimateAverageIcogridEdgeArc(const FIcogrid& Grid)
	{
		if (Grid.SubdivisionLevel > 0)
		{
			return UE_PI / FMath::Sqrt(5.0f * FMath::Pow(4.0f, static_cast<float>(Grid.SubdivisionLevel)));
		}

		if (Grid.HasAdjacency() && Grid.VertexCount > 0)
		{
			for (int32 VertexIndex = 0; VertexIndex < Grid.VertexCount; ++VertexIndex)
			{
				const TConstArrayView<int32> Neighbors = Grid.GetNeighbors(VertexIndex);
				if (Neighbors.Num() <= 0)
				{
					continue;
				}

				return ArcDistance(Grid.VertexDirections[VertexIndex], Grid.VertexDirections[Neighbors[0]]);
			}
		}

		return 0.1f;
	}

	FVector3f SlerpUnitDirection(const FVector3f& A, const FVector3f& B, float Alpha)
	{
		// Forwards to the canonical slerp (kills the hand-rolled weight variant).
		// RobustSlerp assumes unit inputs, so normalize here as the original did.
		return GalaxyMath::RobustSlerp(A.GetSafeNormal(), B.GetSafeNormal(), Alpha);
	}

	bool ComputeSurfaceTriangleBarycentrics(
		const FIcogrid& Grid,
		const FVector3f& Dir,
		int32 A,
		int32 B,
		int32 C,
		FVector3f& OutBary)
	{
		if (!Grid.VertexDirections.IsValidIndex(A)
			|| !Grid.VertexDirections.IsValidIndex(B)
			|| !Grid.VertexDirections.IsValidIndex(C))
		{
			return false;
		}

		const FVector3f& PA = Grid.VertexDirections[A];
		const FVector3f& PB = Grid.VertexDirections[B];
		const FVector3f& PC = Grid.VertexDirections[C];
		const FVector3f Centroid = ((PA + PB + PC) / 3.0f).GetSafeNormal();
		if (Centroid.IsNearlyZero())
		{
			return false;
		}

		const float DotA = FVector3f::DotProduct(PA, Centroid);
		const float DotB = FVector3f::DotProduct(PB, Centroid);
		const float DotC = FVector3f::DotProduct(PC, Centroid);
		const float DotQ = FVector3f::DotProduct(Dir, Centroid);
		if (DotA <= 0.0f || DotB <= 0.0f || DotC <= 0.0f || DotQ <= 0.0f)
		{
			return false;
		}

		const FVector3f ProjA = PA / DotA - Centroid;
		const FVector3f ProjB = PB / DotB - Centroid;
		const FVector3f ProjC = PC / DotC - Centroid;
		const FVector3f ProjQ = Dir / DotQ - Centroid;

		const FVector3f V0 = ProjB - ProjA;
		const FVector3f V1 = ProjC - ProjA;
		const FVector3f V2 = ProjQ - ProjA;

		const float D00 = FVector3f::DotProduct(V0, V0);
		const float D01 = FVector3f::DotProduct(V0, V1);
		const float D11 = FVector3f::DotProduct(V1, V1);
		const float D20 = FVector3f::DotProduct(V2, V0);
		const float D21 = FVector3f::DotProduct(V2, V1);

		const float Denom = D00 * D11 - D01 * D01;
		// Relative degeneracy test -- see the fan-walk barycentric solve above.
		if (Denom < 1.0e-6f * D00 * D11)
		{
			return false;
		}

		const float InvDenom = 1.0f / Denom;
		const float BaryB = (D11 * D20 - D01 * D21) * InvDenom;
		const float BaryC = (D00 * D21 - D01 * D20) * InvDenom;
		const float BaryA = 1.0f - BaryB - BaryC;
		OutBary = FVector3f(BaryA, BaryB, BaryC);

		constexpr float Eps = -1e-4f;
		return BaryA >= Eps && BaryB >= Eps && BaryC >= Eps;
	}

	int32 FindContainingTriangleIndexWithTopology(
		const FIcogrid& Grid,
		const FIcogridTriangleTopology& Topology,
		const FVector3f& Direction)
	{
		if (!Topology.IsValidForGrid(Grid))
		{
			return INDEX_NONE;
		}

		const FVector3f Dir = Direction.GetSafeNormal();
		if (Dir.IsNearlyZero())
		{
			return INDEX_NONE;
		}

		const int32 Center = FIcogridPathfinding::FindNearestVertex(Grid, Dir);
		if (!Topology.HasIncidentTriangles(Center))
		{
			return INDEX_NONE;
		}

		float BestMinBary = -FLT_MAX;
		int32 BestTriangle = INDEX_NONE;
		for (const int32 TriangleIndex : Topology.GetIncidentTriangles(Center))
		{
			const int32 TriBase = TriangleIndex * 3;
			if (!Grid.TriangleIndices.IsValidIndex(TriBase + 2))
			{
				continue;
			}

			FVector3f Bary = FVector3f::ZeroVector;
			const bool bInside = ComputeSurfaceTriangleBarycentrics(
				Grid,
				Dir,
				Grid.TriangleIndices[TriBase + 0],
				Grid.TriangleIndices[TriBase + 1],
				Grid.TriangleIndices[TriBase + 2],
				Bary);
			if (bInside)
			{
				return TriangleIndex;
			}

			const float MinBary = FMath::Min3(Bary.X, Bary.Y, Bary.Z);
			if (MinBary > BestMinBary)
			{
				BestMinBary = MinBary;
				BestTriangle = TriangleIndex;
			}
		}

		return BestMinBary >= -1e-3f ? BestTriangle : INDEX_NONE;
	}

	bool SurfaceSegmentStaysInsideCorridor(
		const FIcogrid& Grid,
		const FIcogridTriangleTopology& Topology,
		const TSet<int32>& CorridorTriangles,
		const FVector3f& From,
		const FVector3f& To,
		int32 MaxShortcutSamples)
	{
		if (CorridorTriangles.Num() <= 0)
		{
			return false;
		}

		const float SegmentArc = ArcDistance(From.GetSafeNormal(), To.GetSafeNormal());
		if (SegmentArc <= 1e-6f)
		{
			return true;
		}

		const float AverageEdgeArc = FMath::Max(EstimateAverageIcogridEdgeArc(Grid), 1e-4f);
		const int32 SampleSteps = FMath::Clamp(
			FMath::CeilToInt(SegmentArc / (AverageEdgeArc * 0.5f)),
			2,
			FMath::Max(2, MaxShortcutSamples));

		for (int32 SampleIndex = 1; SampleIndex < SampleSteps; ++SampleIndex)
		{
			const float Alpha = static_cast<float>(SampleIndex) / static_cast<float>(SampleSteps);
			const FVector3f SampleDirection = SlerpUnitDirection(From, To, Alpha);
			const int32 TriangleIndex = FindContainingTriangleIndexWithTopology(Grid, Topology, SampleDirection);
			if (!CorridorTriangles.Contains(TriangleIndex))
			{
				return false;
			}
		}

		return true;
	}

	float ComputePolylineArcLength(const TArray<FVector3f>& WaypointDirections)
	{
		float Total = 0.0f;
		for (int32 PointIndex = 1; PointIndex < WaypointDirections.Num(); ++PointIndex)
		{
			Total += ArcDistance(
				WaypointDirections[PointIndex - 1].GetSafeNormal(),
				WaypointDirections[PointIndex].GetSafeNormal());
		}
		return Total;
	}
}

FIcogridTriangleTopology FIcogridPathfinding::BuildTriangleTopology(const FIcogrid& Grid)
{
	FIcogridTriangleTopology Result;
	if (!Grid.HasMeshData())
	{
		return Result;
	}

	Result.VertexCount = Grid.VertexCount;
	Result.TriangleCount = Grid.TriangleCount;

	TArray<int32> IncidentCounts;
	IncidentCounts.Init(0, Grid.VertexCount);

	for (int32 TriangleIndex = 0; TriangleIndex < Grid.TriangleCount; ++TriangleIndex)
	{
		const int32 TriBase = TriangleIndex * 3;
		const int32 V0 = Grid.TriangleIndices[TriBase + 0];
		const int32 V1 = Grid.TriangleIndices[TriBase + 1];
		const int32 V2 = Grid.TriangleIndices[TriBase + 2];
		if (!Grid.VertexDirections.IsValidIndex(V0)
			|| !Grid.VertexDirections.IsValidIndex(V1)
			|| !Grid.VertexDirections.IsValidIndex(V2))
		{
			Result.Clear();
			return Result;
		}

		++IncidentCounts[V0];
		++IncidentCounts[V1];
		++IncidentCounts[V2];
	}

	Result.IncidentTriangleOffsets.SetNumUninitialized(Grid.VertexCount + 1);
	int32 TotalIncidentTriangles = 0;
	for (int32 VertexIndex = 0; VertexIndex < Grid.VertexCount; ++VertexIndex)
	{
		Result.IncidentTriangleOffsets[VertexIndex] = TotalIncidentTriangles;
		TotalIncidentTriangles += IncidentCounts[VertexIndex];
	}
	Result.IncidentTriangleOffsets[Grid.VertexCount] = TotalIncidentTriangles;
	Result.IncidentTriangleIndices.SetNumUninitialized(TotalIncidentTriangles);

	TArray<int32> WriteOffsets = Result.IncidentTriangleOffsets;
	for (int32 TriangleIndex = 0; TriangleIndex < Grid.TriangleCount; ++TriangleIndex)
	{
		const int32 TriBase = TriangleIndex * 3;
		const int32 V0 = Grid.TriangleIndices[TriBase + 0];
		const int32 V1 = Grid.TriangleIndices[TriBase + 1];
		const int32 V2 = Grid.TriangleIndices[TriBase + 2];
		Result.IncidentTriangleIndices[WriteOffsets[V0]++] = TriangleIndex;
		Result.IncidentTriangleIndices[WriteOffsets[V1]++] = TriangleIndex;
		Result.IncidentTriangleIndices[WriteOffsets[V2]++] = TriangleIndex;
	}

	Result.TriangleNeighborIndices.Init(INDEX_NONE, Grid.TriangleCount * 3);
	TMap<uint64, FIcogridEdgeOwner> EdgeOwners;
	EdgeOwners.Reserve(Grid.TriangleCount * 3);

	for (int32 TriangleIndex = 0; TriangleIndex < Grid.TriangleCount; ++TriangleIndex)
	{
		const int32 TriBase = TriangleIndex * 3;
		const int32 V[3] = {
			Grid.TriangleIndices[TriBase + 0],
			Grid.TriangleIndices[TriBase + 1],
			Grid.TriangleIndices[TriBase + 2]
		};
		const int32 EdgeA[3] = { V[0], V[1], V[2] };
		const int32 EdgeB[3] = { V[1], V[2], V[0] };

		for (int32 LocalEdgeIndex = 0; LocalEdgeIndex < 3; ++LocalEdgeIndex)
		{
			const uint64 EdgeKey = MakeUndirectedEdgeKey(EdgeA[LocalEdgeIndex], EdgeB[LocalEdgeIndex]);
			if (const FIcogridEdgeOwner* ExistingOwner = EdgeOwners.Find(EdgeKey))
			{
				const int32 ThisFlatIndex = TriangleIndex * 3 + LocalEdgeIndex;
				const int32 OtherFlatIndex = ExistingOwner->TriangleIndex * 3 + ExistingOwner->LocalEdgeIndex;
				if (Result.TriangleNeighborIndices.IsValidIndex(OtherFlatIndex)
					&& Result.TriangleNeighborIndices[OtherFlatIndex] == INDEX_NONE)
				{
					Result.TriangleNeighborIndices[ThisFlatIndex] = ExistingOwner->TriangleIndex;
					Result.TriangleNeighborIndices[OtherFlatIndex] = TriangleIndex;
				}
			}
			else
			{
				FIcogridEdgeOwner Owner;
				Owner.TriangleIndex = TriangleIndex;
				Owner.LocalEdgeIndex = LocalEdgeIndex;
				EdgeOwners.Add(EdgeKey, Owner);
			}
		}
	}

	return Result;
}

FIcogridSurfacePathResult FIcogridPathfinding::BuildSurfacePathFromVertexPath(
	const FIcogrid& Grid,
	const FIcogridTriangleTopology& Topology,
	const TArray<int32>& VertexWaypoints,
	int32 MaxShortcutSamples)
{
	FIcogridSurfacePathResult Result;
	Result.SourceVertexWaypoints = VertexWaypoints;

	if (!Grid.HasMeshData()
		|| !Topology.IsValidForGrid(Grid)
		|| VertexWaypoints.Num() <= 0)
	{
		return Result;
	}

	TSet<int32> CorridorTriangleSet;
	CorridorTriangleSet.Reserve(VertexWaypoints.Num() * 6);
	Result.CorridorTriangleIndices.Reserve(VertexWaypoints.Num() * 4);

	for (const int32 VertexIndex : VertexWaypoints)
	{
		if (!Grid.VertexDirections.IsValidIndex(VertexIndex) || !Topology.HasIncidentTriangles(VertexIndex))
		{
			Result.Clear();
			Result.SourceVertexWaypoints = VertexWaypoints;
			return Result;
		}

		for (const int32 TriangleIndex : Topology.GetIncidentTriangles(VertexIndex))
		{
			if (TriangleIndex >= 0
				&& TriangleIndex < Topology.TriangleCount
				&& !CorridorTriangleSet.Contains(TriangleIndex))
			{
				CorridorTriangleSet.Add(TriangleIndex);
				Result.CorridorTriangleIndices.Add(TriangleIndex);
			}
		}
	}

	if (Result.CorridorTriangleIndices.Num() <= 0)
	{
		return Result;
	}

	Result.WaypointDirections.Reserve(VertexWaypoints.Num());
	Result.WaypointDirections.Add(Grid.VertexDirections[VertexWaypoints[0]].GetSafeNormal());

	int32 CurrentPathIndex = 0;
	while (CurrentPathIndex < VertexWaypoints.Num() - 1)
	{
		const FVector3f FromDirection = Grid.VertexDirections[VertexWaypoints[CurrentPathIndex]].GetSafeNormal();
		int32 BestPathIndex = CurrentPathIndex + 1;

		for (int32 CandidatePathIndex = VertexWaypoints.Num() - 1; CandidatePathIndex > CurrentPathIndex + 1; --CandidatePathIndex)
		{
			const FVector3f ToDirection = Grid.VertexDirections[VertexWaypoints[CandidatePathIndex]].GetSafeNormal();
			if (SurfaceSegmentStaysInsideCorridor(
				Grid,
				Topology,
				CorridorTriangleSet,
				FromDirection,
				ToDirection,
				MaxShortcutSamples))
			{
				BestPathIndex = CandidatePathIndex;
				break;
			}
		}

		Result.WaypointDirections.Add(Grid.VertexDirections[VertexWaypoints[BestPathIndex]].GetSafeNormal());
		CurrentPathIndex = BestPathIndex;
	}

	Result.TotalArcLength = ComputePolylineArcLength(Result.WaypointDirections);
	Result.bSuccess = Result.WaypointDirections.Num() > 0;
	return Result;
}

FIcogridSurfacePathResult FIcogridPathfinding::BuildSurfacePathFromVertexPath(
	const FIcogrid& Grid,
	const TArray<int32>& VertexWaypoints,
	int32 MaxShortcutSamples)
{
	const FIcogridTriangleTopology Topology = BuildTriangleTopology(Grid);
	return BuildSurfacePathFromVertexPath(Grid, Topology, VertexWaypoints, MaxShortcutSamples);
}

void FIcogridPathfinding::RasterizeSurfacePathToVertices(
	const FIcogrid& Grid,
	const FIcogridSurfacePathResult& SurfacePath,
	TArray<float>& InOutVertexScalars,
	float Weight,
	float RadiusRadians)
{
	if (!Grid.HasAdjacency()
		|| Grid.VertexCount <= 0
		|| InOutVertexScalars.Num() != Grid.VertexCount
		|| !SurfacePath.bSuccess
		|| SurfacePath.WaypointDirections.Num() <= 0
		|| Weight <= 0.0f)
	{
		return;
	}

	const float AverageEdgeArc = FMath::Max(EstimateAverageIcogridEdgeArc(Grid), 1e-4f);
	const float CorridorRadius = RadiusRadians > 0.0f
		? RadiusRadians
		: AverageEdgeArc * 0.75f;
	const float SampleArc = FMath::Max(CorridorRadius * 0.5f, AverageEdgeArc * 0.25f);

	auto AccumulateSample = [&Grid, &InOutVertexScalars, Weight, CorridorRadius](const FVector3f& Direction)
	{
		const FIcogridRadiusResult RadiusResult = FIcogridPathfinding::FindVerticesInRadius(
			Grid,
			Direction,
			CorridorRadius);
		if (RadiusResult.VertexIndices.Num() <= 0)
		{
			const int32 NearestVertex = FIcogridPathfinding::FindNearestVertex(Grid, Direction);
			if (InOutVertexScalars.IsValidIndex(NearestVertex))
			{
				InOutVertexScalars[NearestVertex] += Weight;
			}
			return;
		}

		for (const int32 VertexIndex : RadiusResult.VertexIndices)
		{
			if (InOutVertexScalars.IsValidIndex(VertexIndex))
			{
				InOutVertexScalars[VertexIndex] += Weight;
			}
		}
	};

	if (SurfacePath.WaypointDirections.Num() == 1)
	{
		AccumulateSample(SurfacePath.WaypointDirections[0]);
		return;
	}

	for (int32 PointIndex = 1; PointIndex < SurfacePath.WaypointDirections.Num(); ++PointIndex)
	{
		const FVector3f From = SurfacePath.WaypointDirections[PointIndex - 1].GetSafeNormal();
		const FVector3f To = SurfacePath.WaypointDirections[PointIndex].GetSafeNormal();
		const float SegmentArc = ArcDistance(From, To);
		const int32 SampleSteps = FMath::Clamp(
			FMath::CeilToInt(SegmentArc / SampleArc),
			1,
			128);
		for (int32 SampleIndex = 0; SampleIndex <= SampleSteps; ++SampleIndex)
		{
			const float Alpha = static_cast<float>(SampleIndex) / static_cast<float>(SampleSteps);
			AccumulateSample(SlerpUnitDirection(From, To, Alpha));
		}
	}
}

// ============================================================================
// Horizon Profile Baking
// ============================================================================

namespace
{
	/**
	 * Build a local tangent-plane basis at a vertex on the unit sphere.
	 * East = cross(Up, Normal), North = cross(Normal, East).
	 * Degeneracy at poles handled by fallback to Forward.
	 */
	void BuildTangentBasis(const FVector3f& Normal, FVector3f& OutEast, FVector3f& OutNorth)
	{
		const FVector3f Up(0.0f, 0.0f, 1.0f);
		OutEast = FVector3f::CrossProduct(Up, Normal);
		const float EastLen = OutEast.Size();
		if (EastLen < 1e-6f)
		{
			// Near pole -- use Forward as fallback
			const FVector3f Fwd(1.0f, 0.0f, 0.0f);
			OutEast = FVector3f::CrossProduct(Fwd, Normal);
			OutEast.Normalize();
		}
		else
		{
			OutEast /= EastLen;
		}
		OutNorth = FVector3f::CrossProduct(Normal, OutEast);
		OutNorth.Normalize();
	}

	/**
	 * Compute the compass spoke direction in 3D from a local tangent basis.
	 * SpokeIdx in [0, HORIZON_SPOKE_COUNT). Angle = SpokeIdx * SpokeSpacing.
	 * Returns a unit direction in the tangent plane (perpendicular to Normal).
	 */
	FVector3f SpokeDirection3D(const FVector3f& East, const FVector3f& North, int32 SpokeIdx)
	{
		const float Angle = static_cast<float>(SpokeIdx) * HORIZON_SPOKE_SPACING;
		float SinA, CosA;
		FMath::SinCos(&SinA, &CosA, Angle);
		FVector3f Dir = East * CosA + North * SinA;
		Dir.Normalize();
		return Dir;
	}

	/**
	 * Walk greedily along the graph in a compass direction.
	 * At each step, pick the neighbor whose direction most closely matches
	 * the target compass direction (highest dot product with SpokeDir).
	 *
	 * Returns the sequence of vertex indices visited (excluding the start vertex).
	 */
	TArray<int32> GreedySpokeWalk(
		const FIcogrid& Grid,
		int32 StartVertex,
		const FVector3f& SpokeDir,
		int32 MaxHops)
	{
		TArray<int32> Path;
		Path.Reserve(MaxHops);

		int32 Current = StartVertex;
		int32 Previous = INDEX_NONE;

		for (int32 Hop = 0; Hop < MaxHops; ++Hop)
		{
			TConstArrayView<int32> Neighbors = Grid.GetNeighbors(Current);
			int32 BestNeighbor = INDEX_NONE;
			float BestDot = -2.0f;

			for (const int32 N : Neighbors)
			{
				// Never walk backward to the previous vertex
				if (N == Previous)
				{
					continue;
				}

				// Direction from current to neighbor on the sphere
				FVector3f ToN = Grid.VertexDirections[N] - Grid.VertexDirections[Current];
				const float Len = ToN.Size();
				if (Len < 1e-8f)
				{
					continue;
				}
				ToN /= Len;

				const float Dot = FVector3f::DotProduct(ToN, SpokeDir);
				if (Dot > BestDot)
				{
					BestDot = Dot;
					BestNeighbor = N;
				}
			}

			if (BestNeighbor == INDEX_NONE)
			{
				break;
			}

			Path.Add(BestNeighbor);
			Previous = Current;
			Current = BestNeighbor;
		}

		return Path;
	}

	/** Elevation angle from the observer's local tangent plane to a terrain point. */
	float ComputeTerrainElevationAngle(
		const FIcogrid& Grid,
		float ObserverHeight,
		int32 ObserverVertex,
		int32 TargetVertex)
	{
		const FVector3f ObserverNormal = Grid.VertexDirections[ObserverVertex].GetSafeNormal();
		const FVector3f ObserverPos = ObserverNormal * (Grid.Heights[ObserverVertex] + ObserverHeight);
		const FVector3f TargetPos = Grid.VertexDirections[TargetVertex].GetSafeNormal() * Grid.Heights[TargetVertex];

		FVector3f ToTarget = TargetPos - ObserverPos;
		const float Distance = ToTarget.Size();
		if (Distance < 1e-6f)
		{
			return UE_HALF_PI;
		}

		ToTarget /= Distance;
		const float Vertical = FMath::Clamp(FVector3f::DotProduct(ToTarget, ObserverNormal), -1.0f, 1.0f);
		const float Tangent = FMath::Sqrt(FMath::Max(0.0f, 1.0f - Vertical * Vertical));
		return FMath::Atan2(Vertical, Tangent);
	}

	/** Conservative smooth-sphere visibility limit for a target from an elevated observer. */
	float ComputeGeometricHorizonArcLimit(
		const FIcogrid& Grid,
		float ObserverHeight,
		int32 ObserverVertex,
		int32 TargetVertex)
	{
		const float OccluderRadius = FMath::Max(
			1.0f,
			FMath::Min(Grid.Heights[ObserverVertex], Grid.Heights[TargetVertex]));
		const float ObserverRadius = FMath::Max(OccluderRadius + KINDA_SMALL_NUMBER, Grid.Heights[ObserverVertex] + ObserverHeight);
		const float TargetRadius = FMath::Max(OccluderRadius + KINDA_SMALL_NUMBER, Grid.Heights[TargetVertex]);

		const float ObserverArc = FMath::Acos(FMath::Clamp(OccluderRadius / ObserverRadius, -1.0f, 1.0f));
		const float TargetArc = FMath::Acos(FMath::Clamp(OccluderRadius / TargetRadius, -1.0f, 1.0f));
		return ObserverArc + TargetArc;
	}

	uint8 EncodeHorizonTransitionFraction(float Fraction)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(FMath::Clamp(Fraction, 0.0f, 1.0f) * 255.0f), 0, 255));
	}

	float EstimateEnvelopeTransitionFraction(
		float PreviousVisibleAdjustedAngle,
		float CurrentHiddenAdjustedAngle,
		float OccludingEnvelopeAngle)
	{
		const float Denominator = PreviousVisibleAdjustedAngle - CurrentHiddenAdjustedAngle;
		if (FMath::Abs(Denominator) <= KINDA_SMALL_NUMBER)
		{
			return static_cast<float>(HORIZON_DEFAULT_TRANSITION_FRACTION_BYTE) / 255.0f;
		}

		const float Transition = (PreviousVisibleAdjustedAngle - OccludingEnvelopeAngle) / Denominator;
		return FMath::Clamp(Transition, 0.0f, 1.0f);
	}

}

FIcogridHorizonData FIcogridPathfinding::BakeHorizonProfiles(
	const FIcogrid& Grid,
	float PlanetRadius,
	float ObserverHeight,
	int32 MaxHops,
	float OcclusionGraceDegrees,
	float CoverageTransitionHalfWidthHops,
	int32 CoverageHoleFillMaxVertices,
	int32 CoverageGapFillMaxThicknessHops,
	int32 CoverageNeckCloseMaxThicknessHops)
{
	FIcogridHorizonData Result;
	(void)PlanetRadius;

	if (!Grid.HasAdjacency()
		|| Grid.VertexCount == 0
		|| MaxHops <= 0
		|| Grid.VertexDirections.Num() != Grid.VertexCount
		|| Grid.Heights.Num() != Grid.VertexCount)
	{
		return Result;
	}

	Result.ObserverHeight = ObserverHeight;
	Result.MaxHops = MaxHops;
	Result.OcclusionGraceDegrees = FMath::Max(0.0f, OcclusionGraceDegrees);
	Result.CoverageTransitionHalfWidthHops = FMath::Max(0.0f, CoverageTransitionHalfWidthHops);
	Result.CoverageHoleFillMaxVertices = FMath::Max(0, CoverageHoleFillMaxVertices);
	Result.CoverageGapFillMaxThicknessHops = FMath::Max(0, CoverageGapFillMaxThicknessHops);
	Result.CoverageNeckCloseMaxThicknessHops = FMath::Max(0, CoverageNeckCloseMaxThicknessHops);
	Result.VertexCount = Grid.VertexCount;
	Result.HorizonHops.SetNumZeroed(Grid.VertexCount * HORIZON_SPOKE_COUNT);
	Result.HorizonTransitionFractions.Init(HORIZON_DEFAULT_TRANSITION_FRACTION_BYTE, Grid.VertexCount * HORIZON_SPOKE_COUNT);
	const float HorizonOcclusionGraceRadians = FMath::DegreesToRadians(Result.OcclusionGraceDegrees);

	for (int32 V = 0; V < Grid.VertexCount; ++V)
	{
		const FVector3f& Normal = Grid.VertexDirections[V];

		// Build local tangent basis for spoke directions
		FVector3f East, North;
		BuildTangentBasis(Normal, East, North);

		for (int32 S = 0; S < HORIZON_SPOKE_COUNT; ++S)
		{
			const FVector3f SpokeDir = SpokeDirection3D(East, North, S);

			// Walk along the graph in this compass direction.
			TArray<int32> WalkPath = GreedySpokeWalk(Grid, V, SpokeDir, MaxHops);

			// Find the first terrain horizon along this spoke. The running max is
			// the highest elevation angle already seen from the observer; once a
			// farther target is at or below it, that target is hidden behind the
			// prior terrain envelope.
			uint8 FirstBlocked = 0; // 0 = clear
			uint8 TransitionFraction = HORIZON_DEFAULT_TRANSITION_FRACTION_BYTE;
			float HighestPreviousAngle = -FLT_MAX;
			float PreviousVisibleAdjustedAngle = UE_HALF_PI;
			for (int32 HopIdx = 0; HopIdx < WalkPath.Num(); ++HopIdx)
			{
				const int32 TargetVertex = WalkPath[HopIdx];
				const float TargetAngle = ComputeTerrainElevationAngle(Grid, ObserverHeight, V, TargetVertex);
				const float AdjustedTargetAngle = TargetAngle + HorizonOcclusionGraceRadians;

				if (HopIdx > 0 && AdjustedTargetAngle <= HighestPreviousAngle)
				{
					const int32 HopDist = HopIdx + 1; // 1-based
					FirstBlocked = static_cast<uint8>(FMath::Min(HopDist, 255));
					TransitionFraction = EncodeHorizonTransitionFraction(EstimateEnvelopeTransitionFraction(
						PreviousVisibleAdjustedAngle,
						AdjustedTargetAngle,
						HighestPreviousAngle));
					break;
				}

				PreviousVisibleAdjustedAngle = AdjustedTargetAngle;
				HighestPreviousAngle = FMath::Max(HighestPreviousAngle, TargetAngle);
			}

			const int32 EntryIndex = V * HORIZON_SPOKE_COUNT + S;
			Result.HorizonHops[EntryIndex] = FirstBlocked;
			Result.HorizonTransitionFractions[EntryIndex] = TransitionFraction;
		}
	}

	return Result;
}

// ============================================================================
// Horizon Line-of-Sight Query
// ============================================================================

bool FIcogridPathfinding::HasLineOfSight(
	const FIcogrid& Grid,
	const FIcogridHorizonData& HorizonData,
	int32 FromVertex,
	int32 ToVertex)
{
	if (!HorizonData.IsValid())
	{
		return true; // No data -- assume clear
	}

	if (FromVertex < 0 || FromVertex >= Grid.VertexCount
		|| ToVertex < 0 || ToVertex >= Grid.VertexCount
		|| HorizonData.VertexCount != Grid.VertexCount)
	{
		return false;
	}

	if (FromVertex == ToVertex)
	{
		return true;
	}

	// Determine compass direction from observer to target
	const FVector3f& FromDir = Grid.VertexDirections[FromVertex];
	const FVector3f& ToDir = Grid.VertexDirections[ToVertex];
	const float ArcDist = FMath::Acos(FMath::Clamp(
		FVector3f::DotProduct(FromDir, ToDir), -1.0f, 1.0f));
	const float GeometricHorizonArcLimit = ComputeGeometricHorizonArcLimit(
		Grid,
		HorizonData.ObserverHeight,
		FromVertex,
		ToVertex);

	// If the target lies beyond the smooth-sphere horizon even before terrain
	// envelope blocking is considered, it cannot be visible.
	if (ArcDist > GeometricHorizonArcLimit + 1e-4f)
	{
		return false;
	}

	const float AvgHopArc = (Grid.SubdivisionLevel > 0)
		? (UE_PI / (FMath::Sqrt(5.0f * FMath::Pow(4.0f, static_cast<float>(Grid.SubdivisionLevel)))))
		: 0.1f; // Fallback for level 0

	const float EstimatedHop = ArcDist / AvgHopArc;

	// Horizon profiles are only sampled out to MaxHops. Treat farther targets as
	// not visible rather than incorrectly assuming a clear spoke stays clear forever.
	if (EstimatedHop > static_cast<float>(HorizonData.MaxHops) + KINDA_SMALL_NUMBER)
	{
		return false;
	}

	// Build tangent basis at observer
	FVector3f East, North;
	BuildTangentBasis(FromDir, East, North);

	// Project target direction onto tangent plane
	FVector3f Delta = ToDir - FromDir;
	const float EastComp = FVector3f::DotProduct(Delta, East);
	const float NorthComp = FVector3f::DotProduct(Delta, North);

	// Compass angle (0 = East, counter-clockwise)
	float Angle = FMath::Atan2(NorthComp, EastComp);
	if (Angle < 0.0f)
	{
		Angle += UE_TWO_PI;
	}

	// Find nearest spoke
	const int32 SpokeIdx = FMath::RoundToInt32(Angle / HORIZON_SPOKE_SPACING) % HORIZON_SPOKE_COUNT;
	const uint8 FirstBlocked = HorizonData.GetBlockedHop(FromVertex, SpokeIdx);

	// 0 = clear -- this spoke has no blockage within MaxHops
	if (FirstBlocked == 0)
	{
		return true;
	}

	const float BlockedEdgeHop = HorizonData.GetBlockedEdgeHop(
		FromVertex,
		SpokeIdx,
		static_cast<float>(HorizonData.MaxHops));

	// If the target lies beyond the visible edge on this spoke, LoS is blocked.
	return EstimatedHop <= BlockedEdgeHop + KINDA_SMALL_NUMBER;
}
