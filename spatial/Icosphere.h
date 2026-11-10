// Copyright GALAXY_PROJECT. All Rights Reserved.
#pragma once

// ============================================================================
// Icosphere grid: geodesic subdivision of an icosahedron projected onto a
// unit sphere. Provides a near-uniform tessellation with no face seams,
// polar singularities, or axis-aligned distortion.
//
// WHEN TO USE:
//   - Planetary zone / cell grids that span the entire sphere
//   - Near-uniform spherical sampling (area variance < 1.6:1 at any level)
//   - Adjacency queries on a sphere (vertex and triangle neighbors)
//   - Replacing cube-face grids to eliminate seam artifacts
//
// WHEN NOT TO USE:
//   - High subdivision levels on frequently regenerated grids (precompute)
//   - Axis-aligned rectangular queries (use cube-face grids or S2 cells)
//
// VERTEX / FACE COUNTS BY SUBDIVISION LEVEL:
//
//   Level  Vertices   Triangles    Edges      Notes
//   -----  --------   ---------    -----      -----
//     0        12          20         30      Base icosahedron
//     1        42          80        120
//     2       162         320        480
//     3       642       1,280      1,920
//     4     2,562       5,120      7,680      ~2.5K cells
//     5    10,242      20,480     30,720      ~10K cells
//     6    40,962      81,920    122,880      ~41K cells
//     7   163,842     327,680    491,520      ~164K cells
//     8   655,362   1,310,720  1,966,080      ~655K cells
//
//   Formula: V = 10 * 4^n + 2,  F = 20 * 4^n,  E = 30 * 4^n
//
// ALGORITHM:
//   1. Build base icosahedron (12 vertices, 20 faces)
//   2. For each subdivision pass:
//      a. For each triangle, split every edge at its midpoint
//      b. Project new midpoints onto the unit sphere (normalize)
//      c. Replace parent triangle with 4 child triangles
//      d. Cache edge midpoints by sorted vertex-pair to avoid duplicates
//   3. Build adjacency (vertex->vertex neighbors, triangle->triangle neighbors)
//   4. Build spatial lookup for nearest-vertex queries
//
// THREAD SAFETY:
//   Generation is single-threaded. After generation, the grid is immutable
//   and safe to read from any thread.
//
// ============================================================================
// CLASS INDEX
// ============================================================================
//
// --- DATA STRUCTURES ---
//   FIcosphereVertex        - Vertex on the unit sphere (direction + neighbors)
//   FIcosphereTriangle      - Triangle face (3 vertex indices + centroid)
//   FIcosphereGrid          - Complete grid: vertices, triangles, adjacency
//
// --- GENERATION ---
//   FIcosphereGrid::Generate(Level)          - Build from scratch
//   FIcosphereGrid::GenerateBaseIcosahedron  - Level 0 only
//
// --- QUERIES ---
//   FIcosphereGrid::FindNearestVertex     - Closest vertex to a direction
//   FIcosphereGrid::FindNearestTriangle   - Closest triangle to a direction
//   FIcosphereGrid::GetVertexNeighbors    - Adjacent vertices
//   FIcosphereGrid::GetTriangleNeighbors  - Adjacent triangles (shared edge)
//   FIcosphereGrid::GetVertexTriangles    - All triangles sharing a vertex
//
#include "CoreMinimal.h"

// ============================================================================
// ICOSPHERE VERTEX
// ============================================================================

/**
 * A single vertex on the unit sphere.
 * Position is stored as a unit-length direction vector.
 * Neighbor indices refer to adjacent vertices (typically 5 or 6).
 */
struct FIcosphereVertex
{
	/** Unit direction from sphere center */
	FVector3f Direction;

	/** Indices of adjacent vertices (valence 5 at icosahedron poles, 6 elsewhere) */
	TArray<int32, TInlineAllocator<6>> Neighbors;

	/** Indices of all triangles sharing this vertex */
	TArray<int32, TInlineAllocator<6>> Triangles;

	FIcosphereVertex() = default;

	explicit FIcosphereVertex(const FVector3f& InDirection)
		: Direction(InDirection)
	{
	}
};

// ============================================================================
// ICOSPHERE TRIANGLE
// ============================================================================

/**
 * A triangle face on the icosphere.
 * Vertices are wound counter-clockwise when viewed from outside the sphere.
 */
struct FIcosphereTriangle
{
	/** Vertex indices (CCW winding from outside) */
	int32 V[3];

	/** Pre-computed unit centroid direction */
	FVector3f Centroid;

	/** Indices of up to 3 adjacent triangles (sharing an edge). -1 if none. */
	int32 Adjacent[3];

	GALAXYCORE_API FIcosphereTriangle();

	GALAXYCORE_API FIcosphereTriangle(int32 A, int32 B, int32 C);

	/** Get the edge opposite to local vertex index (0,1,2) */
	FORCEINLINE TPair<int32, int32> GetEdge(int32 LocalEdgeIndex) const
	{
		// Edge 0 = V[0]-V[1], Edge 1 = V[1]-V[2], Edge 2 = V[2]-V[0]
		return TPair<int32, int32>(V[LocalEdgeIndex], V[(LocalEdgeIndex + 1) % 3]);
	}

	/** Check if this triangle contains a given vertex index */
	FORCEINLINE bool ContainsVertex(int32 VertexIndex) const
	{
		return V[0] == VertexIndex || V[1] == VertexIndex || V[2] == VertexIndex;
	}
};

// ============================================================================
// EDGE KEY HELPER
// ============================================================================

namespace IcosphereInternal
{
	/** Canonical edge key: always (min, max) so lookup is order-independent */
	FORCEINLINE uint64 MakeEdgeKey(int32 A, int32 B)
	{
		if (A > B) Swap(A, B);
		return (static_cast<uint64>(A) << 32) | static_cast<uint64>(B);
	}
}

// ============================================================================
// BASE ICOSAHEDRON CONSTANTS
// ============================================================================
//
// The 12 vertices and 20 triangles of a regular icosahedron on the unit sphere.
// These are the canonical seed data for all recursive icosphere subdivision.
// Extracted to namespace scope so hierarchical descent can reference them
// without generating a full FIcosphereGrid.
// ============================================================================

namespace IcosphereConstants
{
	/** Golden ratio. */
	inline const float Phi = (1.0f + FMath::Sqrt(5.0f)) * 0.5f;

	/** Normalization factor for icosahedron vertices. */
	inline const float InvLen = 1.0f / FMath::Sqrt(1.0f + Phi * Phi);

	/** 12 vertices of a regular icosahedron on the unit sphere. */
	inline const FVector3f BaseVerts[12] = {
		FVector3f(-1.0f,  Phi, 0.0f) * InvLen,  // 0
		FVector3f( 1.0f,  Phi, 0.0f) * InvLen,  // 1
		FVector3f(-1.0f, -Phi, 0.0f) * InvLen,  // 2
		FVector3f( 1.0f, -Phi, 0.0f) * InvLen,  // 3
		FVector3f(0.0f, -1.0f,  Phi) * InvLen,  // 4
		FVector3f(0.0f,  1.0f,  Phi) * InvLen,  // 5
		FVector3f(0.0f, -1.0f, -Phi) * InvLen,  // 6
		FVector3f(0.0f,  1.0f, -Phi) * InvLen,  // 7
		FVector3f( Phi, 0.0f, -1.0f) * InvLen,  // 8
		FVector3f( Phi, 0.0f,  1.0f) * InvLen,  // 9
		FVector3f(-Phi, 0.0f, -1.0f) * InvLen,  // 10
		FVector3f(-Phi, 0.0f,  1.0f) * InvLen,  // 11
	};

	/** 20 triangles of the icosahedron (CCW winding from outside). */
	inline const int32 BaseFaces[20][3] = {
		// 5 faces around vertex 0
		{0, 11,  5},
		{0,  5,  1},
		{0,  1,  7},
		{0,  7, 10},
		{0, 10, 11},
		// 5 adjacent faces
		{ 1,  5, 9},
		{ 5, 11, 4},
		{11, 10, 2},
		{10,  7, 6},
		{ 7,  1, 8},
		// 5 faces around vertex 3
		{ 3, 9, 4},
		{ 3, 4, 2},
		{ 3, 2, 6},
		{ 3, 6, 8},
		{ 3, 8, 9},
		// 5 adjacent faces
		{4, 9,  5},
		{2, 4, 11},
		{6, 2, 10},
		{8, 6,  7},
		{9, 8,  1},
	};

	/** Number of base icosahedron vertices. */
	inline constexpr int32 NumBaseVerts = 12;

	/** Number of base icosahedron triangles. */
	inline constexpr int32 NumBaseFaces = 20;

	/**
	 * THE canonical icosphere subdivision step: component-wise midpoint, then
	 * project onto the unit sphere. This is the single definition -- every
	 * generator that walks the lattice (FIcosphereGrid::GetOrCreateMidpoint,
	 * the tactical-map patch builder in
	 * UIClasses/TacticalMap/TacticalMapIcogridLattice.cpp) MUST call it rather
	 * than re-spell the expression, so their vertices stay BITWISE identical.
	 *
	 * Do NOT "clean this up" into GetSafeNormal()/a different op order: a
	 * mathematically-equivalent variant can land on the far side of a
	 * quantization step and silently split one lattice vertex into two keys,
	 * corrupting direction-keyed dedup (invisible missing grid lines).
	 * Pinned by GALAXY_PROJECT.TacticalView.Lattice tests.
	 */
	FORCEINLINE FVector3f CanonicalMidpoint(const FVector3f& A, const FVector3f& B)
	{
		FVector3f Mid = (A + B) * 0.5f;
		Mid.Normalize();
		return Mid;
	}
}

// ============================================================================
// ICOSPHERE GRID
// ============================================================================

/**
 * Complete icospheric geodesic grid.
 *
 * Call Generate(Level) once, then use query methods. The grid is immutable
 * after generation and safe to share across threads.
 *
 * Memory footprint (approximate, Level 4 / 2,562 verts):
 *   Vertices:   ~60 KB  (direction + 6 neighbors + 6 triangles each)
 *   Triangles:  ~80 KB  (3 verts + centroid + 3 adjacent each)
 *   Total:      ~140 KB
 */
struct FIcosphereGrid
{
	/** All vertices on the unit sphere */
	TArray<FIcosphereVertex> Vertices;

	/** All triangle faces */
	TArray<FIcosphereTriangle> Triangles;

	/** Subdivision level used to generate this grid */
	int32 SubdivisionLevel = -1;

	// ====================================================================
	// GENERATION
	// ====================================================================

	/**
	 * Generate icosphere at the given subdivision level.
	 * Clears any existing data. Level 0 = base icosahedron (12 verts, 20 tris).
	 * Practical range: 0-8. Level 8 = ~655K vertices.
	 *
	 * @param Level Subdivision depth (0 = base icosahedron)
	 */
	GALAXYCORE_API void Generate(int32 Level);

	// ====================================================================
	// QUERIES
	// ====================================================================

	/**
	 * Find the vertex nearest to a given direction (unit or non-unit).
	 * Brute-force O(V) scan. For hot-path use, cache results or build
	 * a spatial acceleration structure over vertex directions.
	 *
	 * @param Direction Direction to search for (will be normalized internally)
	 * @return Index of the nearest vertex, or -1 if grid is empty
	 */
	[[nodiscard]] GALAXYCORE_API int32 FindNearestVertex(const FVector3f& Direction) const;

	/**
	 * Find the vertex nearest to a given direction using greedy walk.
	 * Starts from a seed vertex and walks via neighbors toward the target.
	 * O(sqrt(V)) average for well-connected grids. Requires adjacency.
	 *
	 * @param Direction Direction to search for (will be normalized internally)
	 * @param SeedVertex Starting vertex (use 0 or a cached nearby vertex)
	 * @return Index of the nearest vertex
	 */
	[[nodiscard]] GALAXYCORE_API int32 FindNearestVertexWalk(const FVector3f& Direction, int32 SeedVertex = 0) const;

	/**
	 * Find the triangle whose centroid is nearest to a given direction.
	 * Brute-force O(F) scan.
	 *
	 * @param Direction Direction to search for
	 * @return Index of the nearest triangle, or -1 if empty
	 */
	[[nodiscard]] GALAXYCORE_API int32 FindNearestTriangle(const FVector3f& Direction) const;

	/**
	 * Find which triangle a direction actually falls inside (point-in-triangle test).
	 * More accurate than FindNearestTriangle for border cases.
	 * O(F) scan with early-out.
	 *
	 * @param Direction Direction to test (normalized internally)
	 * @return Index of containing triangle, or -1 if not found (shouldn't happen on valid grid)
	 */
	[[nodiscard]] GALAXYCORE_API int32 FindContainingTriangle(const FVector3f& Direction) const;

	/**
	 * Get the indices of vertices adjacent to a given vertex.
	 * @return Span of neighbor indices (typically 5 or 6 elements)
	 */
	[[nodiscard]] GALAXYCORE_API TConstArrayView<int32> GetVertexNeighbors(int32 VertexIndex) const;

	/**
	 * Get the indices of triangles sharing a given vertex.
	 */
	[[nodiscard]] GALAXYCORE_API TConstArrayView<int32> GetVertexTriangles(int32 VertexIndex) const;

	/**
	 * Get the 3 triangles adjacent to a given triangle (sharing an edge).
	 * Returns -1 for missing neighbors (shouldn't happen on a closed sphere).
	 */
	GALAXYCORE_API void GetTriangleNeighbors(int32 TriangleIndex, int32 OutNeighbors[3]) const;

	/**
	 * Compute the solid angle (spherical area) of a triangle on the unit sphere.
	 * Uses the spherical excess formula: Area = A + B + C - pi
	 * where A, B, C are the vertex angles of the spherical triangle.
	 *
	 * @return Solid angle in steradians
	 */
	[[nodiscard]] GALAXYCORE_API float GetTriangleArea(int32 TriangleIndex) const;

	/**
	 * Get total vertex count.
	 * Formula: V = 10 * 4^Level + 2
	 */
	[[nodiscard]] FORCEINLINE int32 GetVertexCount() const { return Vertices.Num(); }

	/**
	 * Get total triangle count.
	 * Formula: F = 20 * 4^Level
	 */
	[[nodiscard]] FORCEINLINE int32 GetTriangleCount() const { return Triangles.Num(); }

	/** Check if grid has been generated */
	[[nodiscard]] FORCEINLINE bool IsValid() const { return SubdivisionLevel >= 0 && Vertices.Num() > 0; }

	/**
	 * Compute expected vertex count for a given level without generating.
	 * @param Level Subdivision level (0+)
	 * @return Number of vertices: 10 * 4^Level + 2
	 */
	[[nodiscard]] GALAXYCORE_API static int32 ExpectedVertexCount(int32 Level);

	/**
	 * Compute expected triangle count for a given level without generating.
	 * @param Level Subdivision level (0+)
	 * @return Number of triangles: 20 * 4^Level
	 */
	[[nodiscard]] GALAXYCORE_API static int32 ExpectedTriangleCount(int32 Level);

private:

	// ====================================================================
	// BASE ICOSAHEDRON
	// ====================================================================

	/**
	 * Build the regular icosahedron (12 vertices, 20 triangles).
	 * Golden ratio coordinates, normalized to unit sphere.
	 */
	GALAXYCORE_API void GenerateBaseIcosahedron();

	// ====================================================================
	// SUBDIVISION
	// ====================================================================

	/**
	 * Subdivide every triangle into 4 child triangles.
	 *
	 *         V0
	 *        / \
	 *      M01---M02
	 *      / \ / \
	 *    V1---M12---V2
	 *
	 * Each edge midpoint is computed once (cached by edge key) and
	 * projected onto the unit sphere.
	 */
	GALAXYCORE_API void Subdivide();

	/**
	 * Get the midpoint vertex of an edge, creating it if it doesn't exist.
	 * New vertices are normalized to the unit sphere.
	 *
	 * @param A First vertex index
	 * @param B Second vertex index
	 * @param Cache Edge key -> midpoint vertex index
	 * @return Index of the midpoint vertex
	 */
	GALAXYCORE_API int32 GetOrCreateMidpoint(int32 A, int32 B, TMap<uint64, int32>& Cache);

	// ====================================================================
	// POST-GENERATION
	// ====================================================================

	/** Compute unit-length centroid for every triangle */
	GALAXYCORE_API void ComputeCentroids();

	/**
	 * Build all adjacency data:
	 *   - Vertex -> neighbor vertices
	 *   - Vertex -> incident triangles
	 *   - Triangle -> adjacent triangles (shared edge)
	 */
	GALAXYCORE_API void BuildAdjacency();
};

// ============================================================================
// FACE PARTITION
// ============================================================================

/**
 * Partition of an icosphere grid by cube face.
 *
 * Maps each icosphere vertex/triangle to one of 6 cube faces (+/-X, +/-Y, +/-Z)
 * based on dominant axis. Vertices near face edges are tagged as shared
 * (belonging to 2+ faces) so cross-face cities can reference them from
 * either side.
 *
 * Face indices match EPlanetFace:
 *   0 = +X,  1 = -X,  2 = +Y,  3 = -Y,  4 = +Z,  5 = -Z
 *
 * Usage:
 *   FIcosphereGrid Grid;
 *   Grid.Generate(6);
 *   FIcosphereFacePartition Partition;
 *   Partition.Build(Grid);
 *   // Get all vertices belonging to face 0 (+X):
 *   const TArray<int32>& PlusXVerts = Partition.FaceVertices[0];
 *   // Check if a vertex is on a face boundary:
 *   bool bShared = Partition.IsSharedVertex(SomeVertexIndex);
 */
struct FIcosphereFacePartition
{
	/** Vertex indices belonging to each of the 6 cube faces */
	TArray<int32> FaceVertices[6];

	/** Triangle indices belonging to each of the 6 cube faces */
	TArray<int32> FaceTriangles[6];

	/** Primary face assignment per vertex (index into FaceVertices/0-5) */
	TArray<uint8> VertexPrimaryFace;

	/** Bitmask of faces each vertex touches (bit N = face N). >1 bit set = shared vertex */
	TArray<uint8> VertexFaceMask;

	/** Primary face assignment per triangle */
	TArray<uint8> TrianglePrimaryFace;

	/**
	 * Build the partition from a generated icosphere grid.
	 *
	 * @param Grid       The icosphere to partition (must be generated)
	 * @param EdgeMargin Cosine threshold for marking edge vertices as shared.
	 *                   If the two largest axis-aligned dot products are within
	 *                   this ratio, the vertex is considered on the face boundary.
	 *                   Default 0.05 marks vertices within ~3deg of a face edge.
	 */
	GALAXYCORE_API void Build(const FIcosphereGrid& Grid, float EdgeMargin = 0.05f);

	/** Check if a vertex is shared between 2+ faces */
	[[nodiscard]] FORCEINLINE bool IsSharedVertex(int32 VertexIndex) const
	{
		return FMath::CountBits(VertexFaceMask[VertexIndex]) > 1;
	}

	/** Get the number of faces a vertex belongs to (1-3) */
	[[nodiscard]] FORCEINLINE int32 GetVertexFaceCount(int32 VertexIndex) const
	{
		return FMath::CountBits(VertexFaceMask[VertexIndex]);
	}

	/** Get all face indices a vertex belongs to */
	[[nodiscard]] GALAXYCORE_API TArray<uint8, TInlineAllocator<3>> GetVertexFaces(int32 VertexIndex) const;

	/** Get total shared vertex count across all faces */
	[[nodiscard]] GALAXYCORE_API int32 CountSharedVertices() const;

	/** Get vertex count for a specific face (including shared vertices) */
	[[nodiscard]] FORCEINLINE int32 GetFaceVertexCount(int32 FaceIndex) const
	{
		check(FaceIndex >= 0 && FaceIndex < 6); // checkf-ok -- OOB face access crashes regardless; diagnostic only
		return FaceVertices[FaceIndex].Num();
	}

	/** Get triangle count for a specific face */
	[[nodiscard]] FORCEINLINE int32 GetFaceTriangleCount(int32 FaceIndex) const
	{
		check(FaceIndex >= 0 && FaceIndex < 6); // checkf-ok -- OOB face access crashes regardless; diagnostic only
		return FaceTriangles[FaceIndex].Num();
	}
};
