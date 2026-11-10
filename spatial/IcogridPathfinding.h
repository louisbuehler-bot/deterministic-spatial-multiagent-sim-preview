// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// A* pathfinding on the unified planet icogrid vertex graph.
//
// Operates on FIcogrid's CSR adjacency data. Pure C++, no UObject
// dependency, deterministic (no wall-clock, no unseeded RNG, no TMap iteration
// for outcomes).
//
// Edge cost = great-circle arc distance between vertex directions, modulated by
// height difference (slope penalty). Optional per-vertex cost callback for
// terrain type modifiers.
//
// It reads only the grid, the mobility profile and the cube-sphere math -- never a
// face asset or an actor.
// ============================================================================
#pragma once

#include "CoreMinimal.h"
#include "CoreTypes/PlanetFaceAddressing.h" // EPlanetFace (was reached through SphereFacePortalMath.h, which the header never used)
#include "ECS/Types/FaceIcogridData.h"
#include "ECS/Spatial/MobilityTimeProfile.h"

struct FFineEdgeRouting;

// ============================================================================
// FIcogridPathResult -- Output of a pathfinding query
// ============================================================================

struct FIcogridPathResult
{
	/** Ordered vertex indices from start to goal (inclusive). Empty on failure. */
	TArray<int32> Waypoints;

	/** Total path cost (sum of edge costs). */
	float TotalCost = 0.0f;

	/** Number of vertices evaluated during the search. */
	int32 NodesEvaluated = 0;

	/** Whether a valid path was found. */
	bool bSuccess = false;
};

struct FIcogridTraversalWeights
{
	/** Multiplier for slope-derived edge penalties. 0 = ignore slope. */
	float SlopePenaltyFactor = 1.0f;

	/** Multiplier for roughness-derived edge penalties. 0 = ignore roughness. */
	float RoughnessPenaltyFactor = 1.0f;
};

// ============================================================================
// FIcogridRadiusResult -- Output of a spherical-cap (radius) query
// ============================================================================

struct FIcogridRadiusResult
{
	/**
	 * All unified vertex indices whose angular distance from CenterDirection
	 * is <= the query radius. Includes the seed vertex. Order is BFS order
	 * (innermost ring first), not sorted by distance.
	 */
	TArray<int32> VertexIndices;

	/**
	 * The BFS seed: the grid vertex nearest to CenterDirection.
	 * INDEX_NONE if the query could not be seeded (empty grid, zero direction).
	 */
	int32 SeedVertex = INDEX_NONE;
};

// ============================================================================
// FIcogridRayResult -- Output of a ray-sphere intersection query
// ============================================================================

struct FIcogridRayResult
{
	/**
	 * Normalized direction from the planet center to the hit point (N-vector).
	 * Valid only when bHit is true.
	 */
	FVector3f HitDirection = FVector3f::ZeroVector;

	/**
	 * Unified vertex index nearest to the hit point.
	 * INDEX_NONE if the ray missed or the grid is empty.
	 */
	int32 NearestVertex = INDEX_NONE;

	/** Parametric distance: HitPoint = RayOrigin + T * RayDir. -1 if no hit. */
	float T = -1.0f;

	/** True if the ray intersected the sphere. */
	bool bHit = false;
};

// ============================================================================
// FIcogridTriangleResult -- Output of a containing-triangle query
// ============================================================================

struct FIcogridTriangleResult
{
	/** Triangle index in FIcogrid::TriangleIndices / 3. INDEX_NONE when unknown. */
	int32 TriangleIndex = INDEX_NONE;

	/** Vertex indices of the containing triangle (CCW winding). */
	int32 V0 = INDEX_NONE;
	int32 V1 = INDEX_NONE;
	int32 V2 = INDEX_NONE;

	/**
	 * Barycentric coordinates relative to (V0, V1, V2).
	 * X = weight of V0, Y = weight of V1, Z = weight of V2.
	 * Sum is ~1.0 for interior points.
	 */
	FVector3f Barycentrics = FVector3f::ZeroVector;

	/** True if a containing triangle was found. */
	bool bFound = false;
};

/**
 * Deterministic triangle-neighborhood cache for any-angle surface routing.
 *
 * This is deliberately separate from the vertex path API: callers can build it
 * once for bake-time work, then use it for triangle corridor extraction and
 * point-in-triangle checks without mutating FIcogrid.
 */
struct FIcogridTriangleTopology
{
	/** Number of vertices this topology was built for. */
	int32 VertexCount = 0;

	/** Number of triangles this topology was built for. */
	int32 TriangleCount = 0;

	/** CSR offsets into IncidentTriangleIndices. Length = VertexCount + 1. */
	TArray<int32> IncidentTriangleOffsets;

	/** Packed triangle indices incident to each vertex. */
	TArray<int32> IncidentTriangleIndices;

	/** Triangle adjacency across each local edge. Length = TriangleCount * 3. */
	TArray<int32> TriangleNeighborIndices;

	[[nodiscard]] GALAXY_PROJECT_API bool IsValidForGrid(const FIcogrid& Grid) const;

	[[nodiscard]] GALAXY_PROJECT_API bool HasIncidentTriangles(int32 VertexIndex) const;

	[[nodiscard]] GALAXY_PROJECT_API TConstArrayView<int32> GetIncidentTriangles(int32 VertexIndex) const;

	[[nodiscard]] GALAXY_PROJECT_API int32 GetTriangleNeighbor(int32 TriangleIndex, int32 LocalEdgeIndex) const;

	GALAXY_PROJECT_API void Clear();
};

/**
 * Any-angle surface polyline derived from a vertex path and its triangle corridor.
 *
 * SourceVertexWaypoints preserves the graph path contract for debugging. The
 * renderable / reusable route is WaypointDirections, whose segments may cross
 * triangle interiors instead of stepping vertex-to-vertex.
 */
struct FIcogridSurfacePathResult
{
	/** Original vertex path used to derive this surface route. */
	TArray<int32> SourceVertexWaypoints;

	/** Triangle ids forming the extracted surface corridor. */
	TArray<int32> CorridorTriangleIndices;

	/** Ordered unit directions of the any-angle polyline. */
	TArray<FVector3f> WaypointDirections;

	/** Total angular length of WaypointDirections, in radians. */
	float TotalArcLength = 0.0f;

	/** True when at least one valid surface waypoint was produced. */
	bool bSuccess = false;

	GALAXY_PROJECT_API void Clear();
};

// ============================================================================
// Horizon Profile types -- per-vertex 360-degree terrain visibility
// ============================================================================

/** Number of compass spokes per vertex horizon profile. */
inline constexpr int32 HORIZON_SPOKE_COUNT = 16;

/** Angular spacing between spokes in radians (2*PI / 16 = 22.5 degrees). */
inline constexpr float HORIZON_SPOKE_SPACING = UE_TWO_PI / static_cast<float>(HORIZON_SPOKE_COUNT);

/** Default sub-hop fraction used by legacy baked data that only stored whole-hop blockers. */
inline constexpr uint8 HORIZON_DEFAULT_TRANSITION_FRACTION_BYTE = 128;

/** Derived per-vertex horizon profile metrics for debug views and terrain analysis. */
struct FIcogridHorizonSummary
{
	/** Fraction of spokes that remain clear through MaxHops. */
	float ClearFraction = 0.0f;

	/** Average first-horizon distance in graph hops. Clear spokes contribute MaxHops. */
	float MeanVisibleHop = 0.0f;

	/** Nearest blocked hop among blocked spokes. 0 if every spoke is clear. */
	int32 NearestBlockedHop = 0;

	/** Number of spokes blocked before MaxHops. */
	int32 BlockedSpokeCount = 0;

	/** Normalized standard deviation of visible hop distance. Higher means more directional terrain. */
	float DirectionalAnisotropy = 0.0f;
};

/**
 * Compact baked horizon data for an entire icogrid.
 *
 * Layout: flat TArray<uint8> with HORIZON_SPOKE_COUNT entries per vertex.
 * HorizonHops[VertexIndex * HORIZON_SPOKE_COUNT + SpokeIndex] stores the
 * graph-hop distance of the first terrain horizon along that spoke.
 * HorizonTransitionFractions stores the optional sub-hop boundary fraction
 * between the previous visible hop and the first blocked hop.
 *
 * Values:
 *   0     = clear -- no horizon detected within MaxHops
 *   1-254 = first horizon at that hop distance
 *   255   = first horizon beyond the uint8 range (capped)
 *
 * Memory: 163,842 vertices * 16 spokes = ~2.6 MB for whole-hop data,
 * or ~5.2 MB when sub-hop transition fractions are populated.
 */
struct FIcogridHorizonData
{
	/** Flat array: VertexCount * HORIZON_SPOKE_COUNT entries. */
	TArray<uint8> HorizonHops;

	/** Optional flat array: VertexCount * HORIZON_SPOKE_COUNT fraction bytes for the visible->blocked transition. */
	TArray<uint8> HorizonTransitionFractions;

	/** Number of vertices this data covers. */
	int32 VertexCount = 0;

	/** Observer height above terrain used during bake (world units). */
	float ObserverHeight = 0.0f;

	/** Maximum graph hops traced per spoke. */
	int32 MaxHops = 0;

	/** Grace angle added to horizon occlusion comparisons during bake (degrees). */
	float OcclusionGraceDegrees = 0.5f;

	/** True when this bake used the welded 3D mesh for occlusion tests. */
	bool bUsesWeldedMeshOcclusion = false;

	/** Half-width of the soft HorizonCoverage transition band, measured in graph hops. */
	float CoverageTransitionHalfWidthHops = 0.5f;

	/** Blend HorizonCoverage across neighboring spokes. Disable to inspect raw nearest-spoke edges. */
	bool bCoverageUseCrossSpokeSmoothing = true;

	/** Fill hidden HorizonCoverage holes that are fully enclosed by visible area. */
	bool bCoverageFillEnclosedHoles = true;

	/** Remove visible HorizonCoverage regions that are not connected back to the observer vertex. */
	bool bCoverageRemoveDisconnectedVisibleRegions = true;

	/** Fill enclosed unseen islands up to this many vertices in HorizonCoverage rendering. */
	int32 CoverageHoleFillMaxVertices = 4;

	/** Fill thin unseen bands up to this many graph hops thick in HorizonCoverage rendering. */
	int32 CoverageGapFillMaxThicknessHops = 0;

	/** Close narrow hidden necks and inlets up to this many graph hops thick in HorizonCoverage rendering. */
	int32 CoverageNeckCloseMaxThicknessHops = 0;

	/** True when this baked horizon was post-processed by the authoritative neighbor-relaxation pass. */
	bool bUsesNeighborRelaxation = false;

	/** Base expansion strength used by the authoritative neighbor-relaxation pass. */
	float NeighborRelaxExpandStrength = 0.33f;

	/** Exponential decay scale, in hop difference, for farther-neighbor expansion. */
	float NeighborRelaxExpandDecayHops = 3.0f;

	/** Maximum hop movement allowed in one bake pass when expanding toward a farther neighbor. */
	float NeighborRelaxExpandMaxDeltaHops = 2.0f;

	/** Base compression strength used by the authoritative neighbor-relaxation pass. */
	float NeighborRelaxCompressStrength = 0.2f;

	/** Exponential decay scale, in hop difference, for closer-neighbor compression. */
	float NeighborRelaxCompressDecayHops = 2.0f;

	/** Maximum hop movement allowed in one bake pass when compressing toward a closer neighbor. */
	float NeighborRelaxCompressMaxDeltaHops = 1.0f;

	/** True when this baked horizon was post-processed by observer-footprint aggregation. */
	bool bUsesObserverFootprintAggregation = false;

	/** Maximum optimistic extension, in hops, that footprint aggregation can add beyond the center sample. */
	float ObserverFootprintMaxExtendHops = 2.0f;

	/** True when this baked horizon was post-processed by the soft visibility pressure pass. */
	bool bUsesSoftVisibilityPressure = false;

	/** Maximum support radius, in unified-graph hops, used by the soft visibility pressure pass. */
	int32 SoftPressureSearchHops = 2;

	/** Base expansion strength used by the soft visibility pressure pass. */
	float SoftPressureStrength = 0.2f;

	/** Slopes at or below this value contribute essentially no resistance to soft visibility pressure. */
	float SoftPressureFreeSlope = 0.2f;

	/** Exponent applied to normalized slope resistance before path accumulation. */
	float SoftPressureSlopeExponent = 3.0f;

	/** Exponential falloff applied to accumulated slope resistance along a support path. */
	float SoftPressureResistanceScale = 4.0f;

	/** Maximum hop movement allowed in one bake pass when soft visibility pressure expands a spoke. */
	float SoftPressureMaxDeltaHops = 1.0f;

	/** Normalized slope [0,1] at or above which soft visibility pressure treats terrain as a hard stop. */
	float SoftPressureBlockSlope = 0.95f;

	[[nodiscard]] GALAXY_PROJECT_API bool IsValid() const;

	[[nodiscard]] bool HasTransitionFractions() const
	{
		return HorizonTransitionFractions.Num() == VertexCount * HORIZON_SPOKE_COUNT;
	}

	/** Get the first-blocked hop for a vertex/spoke. 0 = clear. */
	[[nodiscard]] uint8 GetBlockedHop(int32 VertexIndex, int32 SpokeIndex) const
	{
		return HorizonHops[VertexIndex * HORIZON_SPOKE_COUNT + SpokeIndex];
	}

	/** Get the encoded sub-hop transition fraction for a blocked spoke. */
	[[nodiscard]] GALAXY_PROJECT_API uint8 GetBlockedHopFractionByte(int32 VertexIndex, int32 SpokeIndex) const;

	/** Fraction [0,1] of the transition between the previous visible hop and the first blocked hop. */
	[[nodiscard]] GALAXY_PROJECT_API float GetBlockedHopFraction(int32 VertexIndex, int32 SpokeIndex) const;

	/** Horizon edge position in graph hops, with a caller-provided value for clear spokes. */
	[[nodiscard]] GALAXY_PROJECT_API float GetBlockedEdgeHop(int32 VertexIndex, int32 SpokeIndex, float ClearEdge) const;

	/** Returns true if the spoke is blocked at any distance. */
	[[nodiscard]] bool IsSpokeBlocked(int32 VertexIndex, int32 SpokeIndex) const
	{
		return GetBlockedHop(VertexIndex, SpokeIndex) != 0;
	}

	/** First-horizon distance for a spoke, treating clear spokes as MaxHops. */
	[[nodiscard]] int32 GetVisibleHop(int32 VertexIndex, int32 SpokeIndex) const
	{
		const uint8 BlockedHop = GetBlockedHop(VertexIndex, SpokeIndex);
		return (BlockedHop == 0) ? MaxHops : FMath::Clamp(static_cast<int32>(BlockedHop), 1, MaxHops);
	}

	/** Fraction of spokes that remain clear through MaxHops. */
	[[nodiscard]] GALAXY_PROJECT_API float GetClearFraction(int32 VertexIndex) const;

	/** Nearest blocked hop among blocked spokes. 0 if every spoke is clear. */
	[[nodiscard]] GALAXY_PROJECT_API int32 GetNearestBlockedHop(int32 VertexIndex) const;

	/** Compute derived metrics for one vertex without allocating. */
	[[nodiscard]] GALAXY_PROJECT_API FIcogridHorizonSummary SummarizeVertex(int32 VertexIndex) const;

	/** Normalized horizon openness used by checkpoint representative scoring. */
	[[nodiscard]] GALAXY_PROJECT_API float GetCheckpointPriorityOpenness(int32 VertexIndex) const;

	/** Bipolar checkpoint horizon bias: -1 enclosed, +1 open. */
	[[nodiscard]] float GetCheckpointPriorityBias(int32 VertexIndex) const
	{
		return FMath::Clamp(GetCheckpointPriorityOpenness(VertexIndex) * 2.0f - 1.0f, -1.0f, 1.0f);
	}

	[[nodiscard]] int32 GetMemoryBytes() const
	{
		return static_cast<int32>(sizeof(*this) + HorizonHops.GetAllocatedSize() + HorizonTransitionFractions.GetAllocatedSize());
	}

	GALAXY_PROJECT_API void Clear();
};

// ============================================================================
// FPlanetBasinData -- Watershed basin boundaries from unified icogrid
// ============================================================================

/**
 * A single basin boundary loop on a specific cube face.
 * Points are face-local UV in [0,1], ordered to form a closed polygon
 * following icogrid edge topology rather than grid staircases.
 */
struct FBasinBoundaryLoop
{
	/** Which basin this boundary encloses (1-based, matches watershed assignment) */
	int32 BasinID = 0;

	/** Which cube face this loop segment lies on */
	EPlanetFace Face = EPlanetFace::PlusX;

	/** CCW-ordered face-local UV points forming a closed boundary loop.
	 *  Each point is the midpoint of an icogrid edge crossing the basin boundary,
	 *  projected to the face's UV space. */
	TArray<FVector2D> PointsUV;

	/** Number of unified icogrid vertices inside this basin (total, not per-face) */
	int32 InteriorVertexCount = 0;

	/** Depth of this basin: pour-point height minus sink height (world units) */
	float Depth = 0.0f;
};

/**
 * Watershed basin boundary data computed on the unified planet icogrid.
 *
 * Algorithm:
 * 1. Watershed: each unified icogrid vertex flows to its steepest-descent
 *    neighbor via GetNeighbors() CSR adjacency. Follow chains to local
 *    height minima (sinks). All vertices draining to the same sink share
 *    a basin ID.
 * 2. Filter: remove basins shallower than DepthThresholdFraction of global
 *    height range, or with fewer vertices than MinVertices.
 * 3. Boundary: walk triangle edges where basin IDs differ, compute edge
 *    midpoints, chain into closed loops via shared-triangle adjacency.
 * 4. UV: project midpoints to face-local UV via SpherePortals::UnitDirToFaceUV,
 *    group loops by face for per-face overlay rendering.
 *
 * Unlike per-face bake, this captures basins spanning face boundaries correctly.
 */
struct FPlanetBasinData
{
	/** All boundary loops across all faces (grouped by face for display) */
	TArray<FBasinBoundaryLoop> BoundaryLoops;

	/** Number of distinct basins surviving the depth/size filter */
	int32 TotalBasins = 0;

	/** Per-vertex basin assignment on the unified icogrid (1-based, 0=unassigned/filtered).
	 *  Indexed by unified vertex index. Filtered basins are zeroed out.
	 *  Preserved from the watershed computation for zone fill rendering. */
	TArray<int32> VertexBasinAssignment;

	/** Depth threshold fraction used during computation */
	float DepthThresholdFraction = 0.0f;

	/** Minimum vertex count used during computation */
	int32 MinVertices = 0;

	[[nodiscard]] bool IsValid() const { return TotalBasins >= 0 && DepthThresholdFraction > 0.0f; }

	[[nodiscard]] GALAXY_PROJECT_API int32 GetMemoryBytes() const;

	GALAXY_PROJECT_API void Clear();
};

// ============================================================================
// FRidgeLineSegment / FPlanetRidgeData -- Ridge lines (dual of basin boundaries)
// ============================================================================

/**
 * A single ridge line segment on a specific cube face.
 * Points are face-local UV in [0,1], forming an open polyline along
 * high-elevation basin boundaries.
 */
struct FRidgeLineSegment
{
	/** Which cube face this segment lies on */
	EPlanetFace Face = EPlanetFace::PlusX;

	/** Ordered face-local UV points forming an open polyline.
	 *  Each point is the midpoint of an icogrid edge where basin IDs differ,
	 *  filtered to high-elevation boundaries, projected to face UV. */
	TArray<FVector2D> PointsUV;

	/** Average normalized elevation (0..1 in planet height range) along this segment */
	float AvgElevation = 0.0f;

	/** Number of icogrid edges composing this segment */
	int32 EdgeCount = 0;
};

/**
 * Ridge line data computed on the unified planet icogrid.
 *
 * Ridges are the topographic dual of basins: boundary edges between
 * watershed basins that lie at high elevation. Where basin boundaries
 * encircle drainage lows, ridge lines trace the elevated spines that
 * separate drainage areas.
 *
 * Algorithm:
 * 1. Run shared watershed (same FlowTo/BasinAssignment as basin code).
 * 2. Collect ALL edges where BasinAssignment differs (no basin survival filter).
 * 3. Filter: keep edges whose average vertex height exceeds MinElevationFraction
 *    of the global height range.
 * 4. Chain surviving edges into open polylines via shared-triangle adjacency.
 * 5. Project midpoints to face-local UV, split by face.
 */
struct FPlanetRidgeData
{
	/** All ridge segments across all faces */
	TArray<FRidgeLineSegment> Segments;

	/** Number of ridge line segments */
	int32 TotalSegments = 0;

	/** Elevation fraction threshold used during computation */
	float MinElevationFraction = 0.0f;

	[[nodiscard]] bool IsValid() const { return MinElevationFraction > 0.0f; }

	[[nodiscard]] GALAXY_PROJECT_API int32 GetMemoryBytes() const;

	GALAXY_PROJECT_API void Clear();
};

// ============================================================================
// FTerrainSaddlePoint / FPlanetTerrainMetrics -- Per-vertex terrain analysis
// ============================================================================

/**
 * A saddle point (col / pass) on the terrain surface.
 * Detected by counting angular transitions in neighbor height ordering:
 * a vertex whose neighbors alternate above/below at least twice (in angular
 * order around the vertex) is a saddle. Higher transition counts indicate
 * monkey saddles or more complex junctions.
 */
struct FTerrainSaddlePoint
{
	/** Unified icogrid vertex index */
	int32 VertexIndex = INDEX_NONE;

	/** Face this saddle projects to */
	EPlanetFace Face = EPlanetFace::PlusX;

	/** Face-local UV of the saddle vertex */
	FVector2D UV = FVector2D::ZeroVector;

	/** Normalized elevation (0..1 in planet height range) */
	float Elevation = 0.0f;

	/** Height difference between saddle and lowest surrounding peak (world units) */
	float Prominence = 0.0f;

	/** Number of above/below transitions in angular neighbor ordering.
	 *  2 = standard saddle (pass), 4+ = monkey saddle or higher-order junction. */
	int32 TransitionCount = 0;
};

/**
 * Per-vertex terrain analysis metrics computed on the unified planet icogrid.
 *
 * Contains slope magnitude, flow accumulation, flow targets, and detected
 * saddle points -- all derived from the same watershed FlowTo array.
 *
 * Slope: maximum |dH| / arc_distance among all neighbors for each vertex.
 * Flow accumulation: number of vertices upstream of each vertex in the
 * steepest-descent flow graph (computed via topological sort, O(V)).
 * Saddle detection: angular ordering of neighbors, counting above/below
 * transitions to identify strategic chokepoints.
 */
struct FPlanetTerrainMetrics
{
	/** Per-vertex maximum slope magnitude (unitless: dH / arc_distance) */
	TArray<float> SlopeMagnitude;

	/** Per-vertex flow accumulation count (upstream drainage area proxy) */
	TArray<float> FlowAccumulation;

	/** Per-vertex steepest-descent target index (same as watershed FlowTo) */
	TArray<int32> FlowTarget;

	/** Maximum slope across all vertices */
	float MaxSlope = 0.0f;

	/** Maximum flow accumulation across all vertices */
	float MaxAccumulation = 0.0f;

	/** Number of vertices in this dataset */
	int32 VertexCount = 0;

	/** Detected saddle points on the terrain surface */
	TArray<FTerrainSaddlePoint> SaddlePoints;

	[[nodiscard]] bool IsValid() const { return VertexCount > 0 && SlopeMagnitude.Num() == VertexCount; }

	[[nodiscard]] GALAXY_PROJECT_API int32 GetMemoryBytes() const;

	GALAXY_PROJECT_API void Clear();
};

// ============================================================================
// FIcogridPathfinding -- Static A* pathfinding on unified icogrid
// ============================================================================

/**
 * Pure-function A* pathfinding on FIcogrid.
 *
 * Heuristic: great-circle arc distance (admissible on a sphere -- never
 * overestimates the true shortest geodesic distance).
 *
 * Edge cost: arc distance between vertices * (1 + SlopePenaltyFactor * |dH|).
 * Height differences penalize edges proportionally, making steep climbs costlier.
 */
class GALAXY_PROJECT_API FIcogridPathfinding
{
public:
	/**
	 * Find shortest path between two unified vertex indices using explicit
	 * locomotion travel-time math.
	 */
	static FIcogridPathResult FindPath(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		const FMobilityTimeProfile& TimeProfile,
		int32 MaxNodesToEvaluate = 0);

	/**
	 * Find shortest path between two unified vertex indices while restricting
	 * traversal to vertices accepted by the supplied predicate and charging
	 * explicit travel time per edge.
	 */
	static FIcogridPathResult FindPath(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		TFunctionRef<bool(int32)> AcceptPredicate,
		const FMobilityTimeProfile& TimeProfile,
		int32 MaxNodesToEvaluate = 0);

	/**
	 * Find shortest path with a per-vertex arrival cost multiplier.
	 *
	 * VertexArrivalCostMultiplier(V) returns a scalar [0,1] applied to the
	 * edge cost of arriving at V. Values below 1.0 make that vertex cheaper to
	 * enter, biasing A* to prefer it over equally-terrain-suited alternatives.
	 * The heuristic remains admissible as long as no multiplier exceeds 1.0.
	 *
	 * Primary use case: route magnetism -- discount vertices already on a baked
	 * route so subsequent routes snap to them where they run close, producing a
	 * merged corridor instead of two parallel lines.
	 */
	static FIcogridPathResult FindPath(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		TFunctionRef<bool(int32)> AcceptPredicate,
		const FMobilityTimeProfile& TimeProfile,
		TFunctionRef<float(int32)> VertexArrivalCostMultiplier,
		int32 MaxNodesToEvaluate = 0);

	/**
	 * The AcceptPredicate + TimeProfile search reading the fine layer (FineEdgeOverlay.h): an edge Fine's requester
	 * may not cross is never taken; a cliff crossing adds Fine.CliffCrossingRad at the profile's base speed.
	 * An inactive Fine gives the overload without it, bit for bit.
	 */
	static FIcogridPathResult FindPath(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		TFunctionRef<bool(int32)> AcceptPredicate,
		const FMobilityTimeProfile& TimeProfile,
		const FFineEdgeRouting& Fine,
		int32 MaxNodesToEvaluate = 0);

	/** The legacy SlopePenaltyFactor search reading the fine layer the same way; a cliff crossing adds its arc. */
	static FIcogridPathResult FindPath(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		float SlopePenaltyFactor,
		const FFineEdgeRouting& Fine,
		int32 MaxNodesToEvaluate = 0);

	/** The directed edge slot From -> To (AdjacencyOffsets[From] + i), or INDEX_NONE when they are not neighbours. */
	[[nodiscard]] static int32 FindEdgeSlot(const FIcogrid& Grid, int32 From, int32 To);

	/**
	 * Effective arc length (radians of flat-equivalent ground) of one icogrid edge
	 * under a locomotion time profile.
	 *
	 * This is the SAME per-edge law the profile-based FindPath overloads charge
	 * during route selection, re-expressed as a distance: effective arc =
	 * geometric arc * the profile's terrain multiplier (slope, roughness,
	 * coupling, collapse curvature, slope-coupled grip loss), moved toward the
	 * bare arc by the edge's zeon share (IcogridGroundLaw). Equivalently the
	 * edge's travel time * Profile.BaseSpeed, so the profile's class base
	 * speed cancels out: the multiplier is >= 1 (exactly 1 across zeon) and the unit's own
	 * CMovement.EffectiveMaxSpeed stays the sole speed authority (class base
	 * speed already lives in the locomotion archetype's TopSpeed -- folding the
	 * profile's BaseSpeed in as well would double-count it).
	 *
	 * Used by strategic movement to pace path traversal with the same law that
	 * priced the route, so realized travel time and route cost stop being two
	 * different models (a road leg is actually faster to WALK, not just cheaper
	 * to PICK).
	 */
	[[nodiscard]] static float ComputeEdgeEffectiveArc(
		const FIcogrid& Grid,
		int32 From,
		int32 To,
		const FMobilityTimeProfile& TimeProfile);

	/** The same, plus what the fine layer adds to the edge (FFineEdgeRouting::CrossingArc) -- pace matches the fine search's cost. */
	[[nodiscard]] static float ComputeEdgeEffectiveArc(
		const FIcogrid& Grid,
		int32 From,
		int32 To,
		const FMobilityTimeProfile& TimeProfile,
		const FFineEdgeRouting& Fine);

	/**
	 * Find shortest path between two unified vertex indices.
	 *
	 * @param Grid         The unified icogrid (must have adjacency built).
	 * @param StartVertex  Unified vertex index to start from.
	 * @param GoalVertex   Unified vertex index to reach.
	 * @param SlopePenaltyFactor  Multiplier for height-difference cost (0 = ignore height).
	 * @param MaxNodesToEvaluate  Safety cap to prevent runaway searches (0 = unlimited).
	 * @return Path result with waypoints and cost.
	 */
	static FIcogridPathResult FindPath(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		const FIcogridTraversalWeights& TraversalWeights,
		int32 MaxNodesToEvaluate = 0);

	/**
	 * Find shortest path between two unified vertex indices.
	 *
	 * Legacy overload that preserves the historical contract:
	 * SlopePenaltyFactor == 0 implies pure geodesic mode with no roughness cost.
	 */
	static FIcogridPathResult FindPath(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		float SlopePenaltyFactor = 1.0f,
		int32 MaxNodesToEvaluate = 0);

	/**
	 * Find shortest path between two unified vertex indices while restricting
	 * traversal to vertices accepted by the supplied predicate.
	 */
	static FIcogridPathResult FindPath(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		TFunctionRef<bool(int32)> AcceptPredicate,
		const FIcogridTraversalWeights& TraversalWeights,
		int32 MaxNodesToEvaluate = 0);

	/**
	 * Find shortest path between two unified vertex indices while restricting
	 * traversal to vertices accepted by the supplied predicate.
	 *
	 * Legacy overload that preserves the historical contract:
	 * SlopePenaltyFactor == 0 implies pure geodesic mode with no roughness cost.
	 */
	static FIcogridPathResult FindPath(
		const FIcogrid& Grid,
		int32 StartVertex,
		int32 GoalVertex,
		TFunctionRef<bool(int32)> AcceptPredicate,
		float SlopePenaltyFactor,
		int32 MaxNodesToEvaluate = 0);

	/**
	 * Find the nearest unified vertex to a given unit direction (N-vector).
	 * Brute-force dot-product scan -- O(VertexCount).
	 *
	 * If the grid has a valid DescentAccelerator, automatically dispatches to
	 * the hierarchical accelerated path instead.
	 *
	 * @param Grid       The unified icogrid.
	 * @param Direction  Unit direction from planet center.
	 * @return Unified vertex index, or INDEX_NONE if grid is empty.
	 */
	static int32 FindNearestVertex(
		const FIcogrid& Grid,
		const FVector3f& Direction);

	/**
	 * Hierarchical accelerated nearest-vertex lookup -- O(sqrt(V)) amortized.
	 * Uses the FIcogridDescentAccelerator to narrow the search to one or a
	 * few leaf triangles, then scans only those vertices.
	 *
	 * @param Grid       The unified icogrid (must have DescentAccelerator built).
	 * @param Direction  Unit direction from planet center.
	 * @param bExactMode If true, also scans adjacent leaf neighbors (~32 vertices).
	 *                   If false, scans only the target leaf (~8 vertices).
	 * @return Unified vertex index, or INDEX_NONE on failure.
	 */
	static int32 FindNearestVertexHierarchical(
		const FIcogrid& Grid,
		const FVector3f& Direction,
		bool bExactMode = false);

	/**
	 * Find all vertices within an angular radius of a center direction.
	 *
	 * Mathematically optimal: BFS from the nearest vertex, expanding only from
	 * inside-cap vertices. Visits exactly O(k + sqrt(k)) vertices where k is the
	 * result count (k results + one-ring border). This is information-theoretically
	 * optimal -- you cannot report k results without visiting at least k vertices.
	 *
	 * Correctness guarantee: a spherical cap is a convex region on the sphere, and
	 * the Icogrid's induced subgraph on any convex spherical region is connected
	 * (by the Delaunay convexity property). BFS from the seed therefore finds every
	 * vertex in the cap without missing any.
	 *
	 * @param Grid            The unified icogrid (must have adjacency built).
	 * @param CenterDirection Unit direction defining the cap center.
	 * @param RadiusRadians   Angular radius in radians. Use FMath::DegreesToRadians
	 *                        to convert from degrees.
	 * @return Result with all vertex indices inside the cap and the BFS seed.
	 */
	static FIcogridRadiusResult FindVerticesInRadius(
		const FIcogrid& Grid,
		const FVector3f& CenterDirection,
		float RadiusRadians);

	/**
	 * Flood-fill: find the maximal connected component of vertices satisfying a
	 * predicate, reachable from a seed vertex.
	 *
	 * BFS expanding only from accepted vertices -- finds contiguous accepted regions.
	 * Suitable for: territory claiming, fog-of-war reveal, paint-bucket area marking,
	 * slope-constrained region growing.
	 *
	 * NOTE: only explores the connected component of accepted vertices that contains
	 * the seed. Accepted vertices isolated by a ring of rejected vertices are not
	 * returned. This is intentional -- callers that need all accepted vertices
	 * regardless of connectivity should call FindVerticesInRadius or scan directly.
	 *
	 * @param Grid            The unified icogrid (must have adjacency built).
	 * @param SeedVertex      Starting vertex index. Must satisfy AcceptPredicate.
	 * @param AcceptPredicate Returns true for vertices to include and expand from.
	 * @param MaxVertices     Safety cap (0 = unlimited). Stops after collecting
	 *                        this many accepted vertices.
	 * @return All accepted vertex indices in the connected component (BFS order).
	 */
	static TArray<int32> FloodFill(
		const FIcogrid& Grid,
		int32 SeedVertex,
		TFunctionRef<bool(int32)> AcceptPredicate,
		int32 MaxVertices = 0);

	/**
	 * Ray-sphere intersection, snapped to the nearest unified icogrid vertex.
	 *
	 * Intersects the ray against the sphere, normalizes the hit point to an
	 * N-vector, then dispatches to FindNearestVertex to resolve the nearest vertex.
	 *
	 * SphereRadius should be 1.0f when operating on normalized vertex directions.
	 * For world-space picking, pass the planet world radius as SphereRadius.
	 *
	 * @param Grid         The unified icogrid (adjacency not required, but without it
	 *                     FindNearestVertex falls back to brute-force scan).
	 * @param RayOrigin    Ray origin in the same coordinate space as SphereRadius.
	 * @param RayDir       Ray direction (need not be normalized).
	 * @param SphereRadius Sphere radius (default 1.0f for unit Icogrid).
	 * @return Result with hit information and nearest vertex. bHit=false on miss.
	 */
	static FIcogridRayResult RayIntersect(
		const FIcogrid& Grid,
		const FVector3f& RayOrigin,
		const FVector3f& RayDir,
		float SphereRadius = 1.0f);

	/**
	 * Find the triangle containing a given direction on the Icogrid surface.
	 *
	 * Uses the Delaunay property: the nearest vertex is always a vertex of the
	 * containing triangle. Enumerates the triangle fan around the nearest vertex
	 * (~5-6 triangles) and tests each via barycentric coordinates on the tangent
	 * plane. O(1) after the FindNearestVertex lookup.
	 *
	 * Barycentrics enable smooth interpolation of per-vertex data (height, color,
	 * temperature, etc.) at arbitrary surface points.
	 *
	 * @param Grid       The unified icogrid (must have adjacency and TriangleIndices).
	 * @param Direction  Unit direction from planet center.
	 * @return Triangle result with vertex indices and barycentrics. bFound=false on failure.
	 */
	static FIcogridTriangleResult FindContainingTriangle(
		const FIcogrid& Grid,
		const FVector3f& Direction);

	/**
	 * Sample the interpolated terrain height (radius from planet center) at an
	 * arbitrary unit direction: containing triangle + barycentric blend of
	 * FIcogrid::Heights. O(sqrt(V)) via the descent accelerator when built.
	 *
	 * This is the RUNTIME hot-path height sampler. Face-local layer sampling
	 * (FFaceIcogridHeightmapLayer::SampleFromDirection) is a brute-force
	 * O(triangles) scan reserved for bake/assembly/editor paths.
	 *
	 * @param Grid       The unified icogrid (adjacency built; accelerator preferred).
	 * @param Direction  Unit direction from planet center.
	 * @return Interpolated height (radius from planet center), or 0 when the lookup fails.
	 */
	static float SampleHeightFromDirection(
		const FIcogrid& Grid,
		const FVector3f& Direction);

	/**
	 * Build deterministic triangle topology from FIcogrid::TriangleIndices.
	 *
	 * The resulting cache exposes vertex->incident-triangle lookup and
	 * triangle-to-triangle edge neighbors for bake-time surface routing.
	 */
	static FIcogridTriangleTopology BuildTriangleTopology(const FIcogrid& Grid);

	/**
	 * Convert a vertex waypoint path into a corridor-constrained any-angle
	 * surface polyline. Existing vertex path callers are unaffected.
	 *
	 * The implementation extracts all triangles incident to the source path and
	 * greedily string-pulls across that corridor, accepting shortcuts only when
	 * sampled points remain inside corridor triangles.
	 */
	static FIcogridSurfacePathResult BuildSurfacePathFromVertexPath(
		const FIcogrid& Grid,
		const FIcogridTriangleTopology& Topology,
		const TArray<int32>& VertexWaypoints,
		int32 MaxShortcutSamples = 24);

	/** Convenience overload that builds a temporary topology for one-off use. */
	static FIcogridSurfacePathResult BuildSurfacePathFromVertexPath(
		const FIcogrid& Grid,
		const TArray<int32>& VertexWaypoints,
		int32 MaxShortcutSamples = 24);

	/**
	 * Rasterize a surface polyline back into per-vertex scalar storage.
	 *
	 * RadiusRadians controls corridor thickness. Values <= 0 use a deterministic
	 * estimate based on subdivision level.
	 */
	static void RasterizeSurfacePathToVertices(
		const FIcogrid& Grid,
		const FIcogridSurfacePathResult& SurfacePath,
		TArray<float>& InOutVertexScalars,
		float Weight = 1.0f,
		float RadiusRadians = 0.0f);

	/**
	 * Bake 16-spoke horizon profiles for every vertex in the icogrid.
	 *
	 * For each vertex, 16 compass spokes are traced outward along the graph.
	 * Each spoke walks greedily toward its compass direction, sampling terrain
	 * height at each hop and checking whether the sightline from the observer
	 * (vertex height + ObserverHeight) to the target vertex is blocked by
	 * intervening terrain.
	 *
	 * Planet curvature is accounted for: a sightline chord between two points
	 * on a sphere dips below the surface at intermediate vertices.
	 *
	 * @param Grid            The unified icogrid (must have adjacency built).
	 * @param PlanetRadius    Mean planet radius (world units) for curvature calc.
	 * @param ObserverHeight  Height above terrain surface for the observer (world units).
	 * @param MaxHops         Maximum graph hops to trace per spoke.
	 * @return Complete horizon data for all vertices.
	 */
	static FIcogridHorizonData BakeHorizonProfiles(
		const FIcogrid& Grid,
		float PlanetRadius,
		float ObserverHeight,
		int32 MaxHops,
		float OcclusionGraceDegrees = 0.5f,
		float CoverageTransitionHalfWidthHops = 0.5f,
		int32 CoverageHoleFillMaxVertices = 4,
		int32 CoverageGapFillMaxThicknessHops = 0,
		int32 CoverageNeckCloseMaxThicknessHops = 0);

	/**
	 * Query whether two vertices have line-of-sight using pre-baked horizon data.
	 *
	 * Determines the compass direction from FromVertex to ToVertex, finds the
	 * nearest spoke, then checks whether the hop distance falls within any
	 * blocked range on that spoke. Targets beyond the baked MaxHops radius, or
	 * beyond the conservative smooth-sphere horizon implied by the endpoint
	 * radii, are treated as not visible.
	 *
	 * @param Grid        The unified icogrid.
	 * @param HorizonData Pre-baked horizon profiles (from BakeHorizonProfiles).
	 * @param FromVertex  Observer vertex index.
	 * @param ToVertex    Target vertex index.
	 * @return True if LoS is clear, false if terrain blocks the sightline.
	 */
	[[nodiscard]] static bool HasLineOfSight(
		const FIcogrid& Grid,
		const FIcogridHorizonData& HorizonData,
		int32 FromVertex,
		int32 ToVertex);
};
