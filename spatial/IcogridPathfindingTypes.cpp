// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// The out-of-line members of IcogridPathfinding.h's data types, kept apart so
// IcogridPathfinding.cpp holds only the algorithms.
// ============================================================================

#include "IcogridPathfinding.h"

// ============================================================================
// FIcogridTriangleTopology
// ============================================================================

bool FIcogridTriangleTopology::IsValidForGrid(const FIcogrid& Grid) const
{
	return VertexCount == Grid.VertexCount
		&& TriangleCount == Grid.TriangleCount
		&& VertexCount > 0
		&& TriangleCount > 0
		&& IncidentTriangleOffsets.Num() == VertexCount + 1
		&& TriangleNeighborIndices.Num() == TriangleCount * 3;
}

bool FIcogridTriangleTopology::HasIncidentTriangles(int32 VertexIndex) const
{
	return VertexIndex >= 0
		&& VertexIndex < VertexCount
		&& IncidentTriangleOffsets.IsValidIndex(VertexIndex + 1);
}

TConstArrayView<int32> FIcogridTriangleTopology::GetIncidentTriangles(int32 VertexIndex) const
{
	const int32 Start = IncidentTriangleOffsets[VertexIndex];
	const int32 End = IncidentTriangleOffsets[VertexIndex + 1];
	return TConstArrayView<int32>(IncidentTriangleIndices.GetData() + Start, End - Start);
}

int32 FIcogridTriangleTopology::GetTriangleNeighbor(int32 TriangleIndex, int32 LocalEdgeIndex) const
{
	const int32 FlatIndex = TriangleIndex * 3 + LocalEdgeIndex;
	return TriangleNeighborIndices.IsValidIndex(FlatIndex)
		? TriangleNeighborIndices[FlatIndex]
		: INDEX_NONE;
}

void FIcogridTriangleTopology::Clear()
{
	VertexCount = 0;
	TriangleCount = 0;
	IncidentTriangleOffsets.Empty();
	IncidentTriangleIndices.Empty();
	TriangleNeighborIndices.Empty();
}


// ============================================================================
// FIcogridSurfacePathResult
// ============================================================================

void FIcogridSurfacePathResult::Clear()
{
	SourceVertexWaypoints.Empty();
	CorridorTriangleIndices.Empty();
	WaypointDirections.Empty();
	TotalArcLength = 0.0f;
	bSuccess = false;
}


// ============================================================================
// FIcogridHorizonData
// ============================================================================

bool FIcogridHorizonData::IsValid() const
{
	return VertexCount > 0 && MaxHops > 0
		&& HorizonHops.Num() == VertexCount * HORIZON_SPOKE_COUNT
		&& (HorizonTransitionFractions.IsEmpty()
			|| HorizonTransitionFractions.Num() == VertexCount * HORIZON_SPOKE_COUNT);
}

uint8 FIcogridHorizonData::GetBlockedHopFractionByte(int32 VertexIndex, int32 SpokeIndex) const
{
	if (!HasTransitionFractions())
	{
		return HORIZON_DEFAULT_TRANSITION_FRACTION_BYTE;
	}

	return HorizonTransitionFractions[VertexIndex * HORIZON_SPOKE_COUNT + SpokeIndex];
}

float FIcogridHorizonData::GetBlockedHopFraction(int32 VertexIndex, int32 SpokeIndex) const
{
	if (GetBlockedHop(VertexIndex, SpokeIndex) == 0)
	{
		return 0.0f;
	}

	return static_cast<float>(GetBlockedHopFractionByte(VertexIndex, SpokeIndex)) / 255.0f;
}

float FIcogridHorizonData::GetBlockedEdgeHop(int32 VertexIndex, int32 SpokeIndex, float ClearEdge) const
{
	const uint8 BlockedHop = GetBlockedHop(VertexIndex, SpokeIndex);
	if (BlockedHop == 0)
	{
		return ClearEdge;
	}

	const int32 ClampedHop = FMath::Clamp(static_cast<int32>(BlockedHop), 1, MaxHops);
	const float EdgeHop = static_cast<float>(ClampedHop - 1) + GetBlockedHopFraction(VertexIndex, SpokeIndex);
	return FMath::Clamp(EdgeHop, 0.0f, ClearEdge);
}

float FIcogridHorizonData::GetClearFraction(int32 VertexIndex) const
{
	if (!IsValid() || VertexIndex < 0 || VertexIndex >= VertexCount)
	{
		return 0.0f;
	}

	int32 ClearSpokes = 0;
	for (int32 SpokeIndex = 0; SpokeIndex < HORIZON_SPOKE_COUNT; ++SpokeIndex)
	{
		if (GetBlockedHop(VertexIndex, SpokeIndex) == 0)
		{
			++ClearSpokes;
		}
	}
	return static_cast<float>(ClearSpokes) / static_cast<float>(HORIZON_SPOKE_COUNT);
}

int32 FIcogridHorizonData::GetNearestBlockedHop(int32 VertexIndex) const
{
	if (!IsValid() || VertexIndex < 0 || VertexIndex >= VertexCount)
	{
		return 0;
	}

	int32 Nearest = 0;
	for (int32 SpokeIndex = 0; SpokeIndex < HORIZON_SPOKE_COUNT; ++SpokeIndex)
	{
		const uint8 BlockedHop = GetBlockedHop(VertexIndex, SpokeIndex);
		if (BlockedHop == 0)
		{
			continue;
		}
		const int32 Hop = FMath::Clamp(static_cast<int32>(BlockedHop), 1, MaxHops);
		Nearest = (Nearest == 0) ? Hop : FMath::Min(Nearest, Hop);
	}
	return Nearest;
}

FIcogridHorizonSummary FIcogridHorizonData::SummarizeVertex(int32 VertexIndex) const
{
	FIcogridHorizonSummary Summary;
	if (!IsValid() || VertexIndex < 0 || VertexIndex >= VertexCount)
	{
		return Summary;
	}

	int32 ClearSpokes = 0;
	int32 NearestBlocked = 0;
	float HopSum = 0.0f;
	float HopSqSum = 0.0f;
	for (int32 SpokeIndex = 0; SpokeIndex < HORIZON_SPOKE_COUNT; ++SpokeIndex)
	{
		const uint8 BlockedHop = GetBlockedHop(VertexIndex, SpokeIndex);
		if (BlockedHop == 0)
		{
			++ClearSpokes;
		}
		else
		{
			const int32 Hop = FMath::Clamp(static_cast<int32>(BlockedHop), 1, MaxHops);
			NearestBlocked = (NearestBlocked == 0) ? Hop : FMath::Min(NearestBlocked, Hop);
		}

		const float VisibleHop = static_cast<float>(GetVisibleHop(VertexIndex, SpokeIndex));
		HopSum += VisibleHop;
		HopSqSum += VisibleHop * VisibleHop;
	}

	Summary.ClearFraction = static_cast<float>(ClearSpokes) / static_cast<float>(HORIZON_SPOKE_COUNT);
	Summary.MeanVisibleHop = HopSum / static_cast<float>(HORIZON_SPOKE_COUNT);
	Summary.NearestBlockedHop = NearestBlocked;
	Summary.BlockedSpokeCount = HORIZON_SPOKE_COUNT - ClearSpokes;

	const float MeanSq = HopSqSum / static_cast<float>(HORIZON_SPOKE_COUNT);
	const float Variance = FMath::Max(0.0f, MeanSq - Summary.MeanVisibleHop * Summary.MeanVisibleHop);
	Summary.DirectionalAnisotropy = (MaxHops > 0) ? FMath::Sqrt(Variance) / static_cast<float>(MaxHops) : 0.0f;
	return Summary;
}

float FIcogridHorizonData::GetCheckpointPriorityOpenness(int32 VertexIndex) const
{
	if (!IsValid() || VertexIndex < 0 || VertexIndex >= VertexCount)
	{
		return 0.5f;
	}

	const FIcogridHorizonSummary Summary = SummarizeVertex(VertexIndex);
	const float MeanVisibleScore = MaxHops > 0
		? FMath::Clamp(Summary.MeanVisibleHop / static_cast<float>(MaxHops), 0.0f, 1.0f)
		: 0.5f;
	return FMath::Clamp(0.55f * MeanVisibleScore + 0.45f * Summary.ClearFraction, 0.0f, 1.0f);
}

void FIcogridHorizonData::Clear()
{
	HorizonHops.Empty();
	HorizonTransitionFractions.Empty();
	VertexCount = 0;
	ObserverHeight = 0.0f;
	MaxHops = 0;
	OcclusionGraceDegrees = 0.5f;
	bUsesWeldedMeshOcclusion = false;
	CoverageTransitionHalfWidthHops = 0.5f;
	bCoverageUseCrossSpokeSmoothing = true;
	bCoverageFillEnclosedHoles = true;
	bCoverageRemoveDisconnectedVisibleRegions = true;
	CoverageHoleFillMaxVertices = 4;
	CoverageGapFillMaxThicknessHops = 0;
	CoverageNeckCloseMaxThicknessHops = 0;
	bUsesNeighborRelaxation = false;
	NeighborRelaxExpandStrength = 0.33f;
	NeighborRelaxExpandDecayHops = 3.0f;
	NeighborRelaxExpandMaxDeltaHops = 2.0f;
	NeighborRelaxCompressStrength = 0.2f;
	NeighborRelaxCompressDecayHops = 2.0f;
	NeighborRelaxCompressMaxDeltaHops = 1.0f;
	bUsesObserverFootprintAggregation = false;
	ObserverFootprintMaxExtendHops = 2.0f;
	bUsesSoftVisibilityPressure = false;
	SoftPressureSearchHops = 2;
	SoftPressureStrength = 0.2f;
	SoftPressureFreeSlope = 0.2f;
	SoftPressureSlopeExponent = 3.0f;
	SoftPressureResistanceScale = 4.0f;
	SoftPressureMaxDeltaHops = 1.0f;
	SoftPressureBlockSlope = 0.95f;
}


// ============================================================================
// FPlanetBasinData
// ============================================================================

int32 FPlanetBasinData::GetMemoryBytes() const
{
	int32 Bytes = static_cast<int32>(sizeof(*this));
	for (const FBasinBoundaryLoop& L : BoundaryLoops)
	{
		Bytes += static_cast<int32>(L.PointsUV.GetAllocatedSize());
	}
	Bytes += VertexBasinAssignment.GetAllocatedSize();
	return Bytes;
}

void FPlanetBasinData::Clear()
{
	BoundaryLoops.Empty();
	TotalBasins = 0;
	VertexBasinAssignment.Empty();
	DepthThresholdFraction = 0.0f;
	MinVertices = 0;
}


// ============================================================================
// FPlanetRidgeData
// ============================================================================

int32 FPlanetRidgeData::GetMemoryBytes() const
{
	int32 Bytes = static_cast<int32>(sizeof(*this));
	for (const FRidgeLineSegment& S : Segments)
	{
		Bytes += static_cast<int32>(S.PointsUV.GetAllocatedSize());
	}
	return Bytes;
}

void FPlanetRidgeData::Clear()
{
	Segments.Empty();
	TotalSegments = 0;
	MinElevationFraction = 0.0f;
}


// ============================================================================
// FPlanetTerrainMetrics
// ============================================================================

int32 FPlanetTerrainMetrics::GetMemoryBytes() const
{
	return static_cast<int32>(sizeof(*this))
		+ static_cast<int32>(SlopeMagnitude.GetAllocatedSize())
		+ static_cast<int32>(FlowAccumulation.GetAllocatedSize())
		+ static_cast<int32>(FlowTarget.GetAllocatedSize())
		+ static_cast<int32>(SaddlePoints.GetAllocatedSize());
}

void FPlanetTerrainMetrics::Clear()
{
	SlopeMagnitude.Empty();
	FlowAccumulation.Empty();
	FlowTarget.Empty();
	MaxSlope = 0.0f;
	MaxAccumulation = 0.0f;
	VertexCount = 0;
	SaddlePoints.Empty();
}
