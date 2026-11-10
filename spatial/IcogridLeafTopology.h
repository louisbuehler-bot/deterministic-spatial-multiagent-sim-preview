#pragma once

#include "CoreMinimal.h"

// ============================================================================
// IcogridLeafTopology -- EXACT topology of the icosphere descent-leaf hierarchy.
// ============================================================================
//
// A "descent leaf" is one spherical triangle of the canonical icosphere
// subdivision, descended `Depth` levels from the 20 base faces. The id encoding
// is the one FIcogridDescentAccelerator already ships:
//
//     LeafId = BaseTriangle * 4^Depth + ChildPath
//
// where ChildPath accumulates one base-4 digit per level, COARSEST digit in the
// most significant position. That layout is what makes the hierarchy walkable by
// arithmetic alone: an ancestor is a right-shift, a descendant range is a
// left-shift, and neither needs a stored parent array.
//
// WHY THIS EXISTS (and why it is not FIcogridDescentAccelerator::GetLeafNeighbors):
// that accelerator's LeafAdjacency is built by counting SHARED VERTICES between
// leaves and keeping the top 3 (FaceIcogridData.cpp, BuildLeafAdjacency). It is a
// fine heuristic for widening a nearest-vertex scan, which is all it was ever for.
// It is NOT an edge-adjacency graph: a leaf can touch more than three others, the
// top-3 truncation silently drops real edges, and a vertex-sharing "neighbour" need
// not share an edge at all. Anything that moves a CONSERVED QUANTITY across leaf
// boundaries needs the exact graph -- mass leaking through a dropped edge, or
// crossing a corner-only contact, is a conservation bug that no test of the
// accelerator would ever report.
//
// EXACTNESS, and how it is obtained: adjacency here is derived GEOMETRICALLY --
// step a hair across each edge midpoint and ask the descent which leaf you landed
// in. That is deliberately the same question the simulation asks when it locates a
// position, so the adjacency graph cannot disagree with the leaf assignment it
// serves. Every leaf gets exactly 3 neighbours, none of them -1: at uniform depth
// the subdivision is conforming, so every edge is shared with exactly one leaf.
//
// DUPLICATION NOTE: DescendToLeaf below re-implements the descent that
// FaceIcogridData.cpp keeps in an anonymous namespace, because that file sits in
// a higher module and cannot be
// called downward. The two are pinned together by a cross-validation test
// (IcogridLeafTopologyTests), which is what keeps the copy honest. Collapsing the
// accelerator onto this implementation is a later cleanup, not this file's job.
//
// Pure math: no grid, no allocation outside FLeafAdjacency::Build, no registry, no
// RNG, no clock. Safe to call from authoritative simulation on any thread.
// ============================================================================

namespace IcogridLeafTopology
{
	/**
	 * Deepest descent this module supports.
	 * At depth 8 a LeafId reaches 19 * 65536 + 65535 = 1,310,719 -- comfortably
	 * inside int32, and far past the depth 5 the runtime actually materializes.
	 */
	inline constexpr int32 MaxSupportedDepth = 8;

	/** 4^N. N must be >= 0. */
	[[nodiscard]] GALAXYCORE_API int32 Pow4(int32 N);

	/** Total leaves at a depth: 20 * 4^Depth. */
	[[nodiscard]] GALAXYCORE_API int32 LeavesAtDepth(int32 Depth);

	/** Nominal edge arc (radians) of a leaf at a depth: the base icosahedron edge halved per level. */
	[[nodiscard]] GALAXYCORE_API float EdgeArcAtDepth(int32 Depth);

	/** Which of the 20 base triangles a leaf descends from. */
	[[nodiscard]] GALAXYCORE_API int32 BaseTriangleOf(int32 LeafId, int32 Depth);

	/** The packed base-4 child path of a leaf, coarsest digit most significant. */
	[[nodiscard]] GALAXYCORE_API int32 ChildPathOf(int32 LeafId, int32 Depth);

	/**
	 * The ancestor of a leaf at a shallower depth -- the merge direction.
	 * ToDepth must be in [0, FromDepth]. ToDepth == FromDepth returns LeafId.
	 */
	[[nodiscard]] GALAXYCORE_API int32 AncestorAtDepth(int32 LeafId, int32 FromDepth, int32 ToDepth);

	/**
	 * The FIRST (lowest-id) descendant of a leaf at a deeper depth -- the split
	 * direction. Descendants occupy the contiguous id range
	 * [FirstDescendantAtDepth(...), + DescendantCount(...)), which is what makes a
	 * split a range operation rather than a search.
	 */
	[[nodiscard]] GALAXYCORE_API int32 FirstDescendantAtDepth(int32 LeafId, int32 FromDepth, int32 ToDepth);

	/** How many leaves at ToDepth sit under one leaf at FromDepth: 4^(ToDepth - FromDepth). */
	[[nodiscard]] GALAXYCORE_API int32 DescendantCount(int32 FromDepth, int32 ToDepth);

	/** Whether LeafId (at LeafDepth) lies under AncestorId (at AncestorDepth). */
	[[nodiscard]] GALAXYCORE_API bool IsDescendantOf(
		int32 LeafId, int32 LeafDepth, int32 AncestorId, int32 AncestorDepth);

	/**
	 * The three spherical-triangle corners of a leaf, in the winding the descent
	 * produced them (CCW from outside).
	 */
	GALAXYCORE_API void LeafCorners(
		int32 LeafId, int32 Depth, FVector3f& OutA, FVector3f& OutB, FVector3f& OutC);

	/** Normalized centroid direction of a leaf. */
	[[nodiscard]] GALAXYCORE_API FVector3f LeafCentroid(int32 LeafId, int32 Depth);

	/**
	 * Exact solid angle of a leaf in steradians (Girard's theorem, numerically
	 * stable half-tangent form). Leaves are NOT equal-area: the 12 pentagonal
	 * corners of the icosahedron distort their neighbourhoods, so a density that
	 * divides mass by area must use this rather than the 4*PI/NumLeaves mean.
	 */
	[[nodiscard]] GALAXYCORE_API float LeafAreaSteradians(int32 LeafId, int32 Depth);

	/**
	 * Which leaf contains a direction. Pure math, no precomputed data.
	 * Mirrors FIcogridDescentAccelerator::DescendToLeaf for the same depth --
	 * pinned by cross-validation test, see the DUPLICATION NOTE above.
	 *
	 * @param Direction Unit direction (normalized by the caller).
	 * @param Depth     Descent depth, 1..MaxSupportedDepth.
	 * @return Leaf id in [0, LeavesAtDepth(Depth)), or INDEX_NONE on bad input.
	 */
	[[nodiscard]] GALAXYCORE_API int32 DescendToLeaf(const FVector3f& Direction, int32 Depth);

	/**
	 * The exact edge-adjacency graph and per-leaf areas at one depth.
	 *
	 * Depends on NOTHING but the depth -- the icosphere subdivision is canonical,
	 * so one built graph serves every planet at that depth. Build it once and share
	 * it; it is immutable afterwards.
	 */
	struct GALAXYCORE_API FLeafAdjacency
	{
		/** Depth this graph describes. 0 until built. */
		int32 Depth = 0;

		/** LeavesAtDepth(Depth). 0 until built. */
		int32 NumLeaves = 0;

		/**
		 * Flat NumLeaves*3 array of neighbour ids, leaf-major.
		 * Neighbour k of leaf L is the leaf across L's edge k, where edge 0 is
		 * (A,B), edge 1 is (B,C), edge 2 is (C,A) of LeafCorners. Never INDEX_NONE
		 * -- a conforming triangulation has no boundary.
		 */
		TArray<int32> Neighbors;

		/** Per-leaf solid angle in steradians. Length NumLeaves. */
		TArray<float> Areas;

		/** Build (or rebuild) for a depth in 1..MaxSupportedDepth. Clears on bad input. */
		void Build(int32 InDepth);

		void Clear();

		[[nodiscard]] bool IsValid() const;

		/** The 3 neighbours of a leaf. Caller checks IsValid() and the id range. */
		[[nodiscard]] TConstArrayView<int32> GetNeighbors(int32 LeafId) const
		{
			return TConstArrayView<int32>(Neighbors.GetData() + LeafId * 3, 3);
		}

		[[nodiscard]] float GetArea(int32 LeafId) const
		{
			return Areas[LeafId];
		}

		[[nodiscard]] int32 GetMemoryBytes() const
		{
			return Neighbors.Num() * sizeof(int32) + Areas.Num() * sizeof(float);
		}
	};
}
