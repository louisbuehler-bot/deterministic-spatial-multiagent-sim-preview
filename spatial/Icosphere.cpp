// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Definitions for Icosphere.h.

#include "Spatial/Icosphere.h"

// ============================================================================
// FIcosphereTriangle
// ============================================================================

FIcosphereTriangle::FIcosphereTriangle()
{
	V[0] = V[1] = V[2] = -1;
	Adjacent[0] = Adjacent[1] = Adjacent[2] = -1;
	Centroid = FVector3f::ZeroVector;
}

FIcosphereTriangle::FIcosphereTriangle(int32 A, int32 B, int32 C)
{
	V[0] = A;
	V[1] = B;
	V[2] = C;
	Adjacent[0] = Adjacent[1] = Adjacent[2] = -1;
	Centroid = FVector3f::ZeroVector;
}


// ============================================================================
// FIcosphereGrid
// ============================================================================

void FIcosphereGrid::Generate(int32 Level)
{
	check(Level >= 0 && Level <= 10); // checkf-ok -- out-of-range level causes exponential subdivision crash regardless; diagnostic only
	SubdivisionLevel = Level;

	// Step 1: base icosahedron
	GenerateBaseIcosahedron();

	// Step 2: subdivide
	for (int32 Pass = 0; Pass < Level; ++Pass)
	{
		Subdivide();
	}

	// Step 3: compute centroids
	ComputeCentroids();

	// Step 4: build adjacency (vertex<->vertex, vertex<->triangle, triangle<->triangle)
	BuildAdjacency();
}

int32 FIcosphereGrid::FindNearestVertex(const FVector3f& Direction) const
{
	if (Vertices.Num() == 0) return -1;

	const FVector3f Dir = Direction.GetSafeNormal();
	int32 BestIndex = 0;
	float BestDot = FVector3f::DotProduct(Dir, Vertices[0].Direction);

	for (int32 I = 1; I < Vertices.Num(); ++I)
	{
		const float D = FVector3f::DotProduct(Dir, Vertices[I].Direction);
		if (D > BestDot)
		{
			BestDot = D;
			BestIndex = I;
		}
	}
	return BestIndex;
}

int32 FIcosphereGrid::FindNearestVertexWalk(const FVector3f& Direction, int32 SeedVertex) const
{
	if (Vertices.Num() == 0) return -1;
	check(SeedVertex >= 0 && SeedVertex < Vertices.Num()); // checkf-ok -- OOB vertex access crashes regardless; diagnostic only

	const FVector3f Dir = Direction.GetSafeNormal();
	int32 Current = SeedVertex;
	float CurrentDot = FVector3f::DotProduct(Dir, Vertices[Current].Direction);

	for (;;)
	{
		bool bImproved = false;
		for (int32 NeighborIdx : Vertices[Current].Neighbors)
		{
			const float D = FVector3f::DotProduct(Dir, Vertices[NeighborIdx].Direction);
			if (D > CurrentDot)
			{
				CurrentDot = D;
				Current = NeighborIdx;
				bImproved = true;
				break; // greedy: take first improvement
			}
		}
		if (!bImproved) break;
	}
	return Current;
}

int32 FIcosphereGrid::FindNearestTriangle(const FVector3f& Direction) const
{
	if (Triangles.Num() == 0) return -1;

	const FVector3f Dir = Direction.GetSafeNormal();
	int32 BestIndex = 0;
	float BestDot = FVector3f::DotProduct(Dir, Triangles[0].Centroid);

	for (int32 I = 1; I < Triangles.Num(); ++I)
	{
		const float D = FVector3f::DotProduct(Dir, Triangles[I].Centroid);
		if (D > BestDot)
		{
			BestDot = D;
			BestIndex = I;
		}
	}
	return BestIndex;
}

int32 FIcosphereGrid::FindContainingTriangle(const FVector3f& Direction) const
{
	if (Triangles.Num() == 0) return -1;

	const FVector3f Dir = Direction.GetSafeNormal();

	for (int32 I = 0; I < Triangles.Num(); ++I)
	{
		const FIcosphereTriangle& Tri = Triangles[I];
		const FVector3f& A = Vertices[Tri.V[0]].Direction;
		const FVector3f& B = Vertices[Tri.V[1]].Direction;
		const FVector3f& C = Vertices[Tri.V[2]].Direction;

		// Spherical point-in-triangle: Dir is inside if it's on the same
		// side of all three great-circle edges (AB, BC, CA)
		const FVector3f CrossAB = FVector3f::CrossProduct(A, B);
		const FVector3f CrossBC = FVector3f::CrossProduct(B, C);
		const FVector3f CrossCA = FVector3f::CrossProduct(C, A);

		if (FVector3f::DotProduct(Dir, CrossAB) >= 0.0f &&
			FVector3f::DotProduct(Dir, CrossBC) >= 0.0f &&
			FVector3f::DotProduct(Dir, CrossCA) >= 0.0f)
		{
			return I;
		}
	}

	// Fallback: numerical edge case -- return nearest centroid
	return FindNearestTriangle(Direction);
}

TConstArrayView<int32> FIcosphereGrid::GetVertexNeighbors(int32 VertexIndex) const
{
	check(VertexIndex >= 0 && VertexIndex < Vertices.Num()); // checkf-ok -- OOB vertex access crashes regardless; diagnostic only
	return TConstArrayView<int32>(
		Vertices[VertexIndex].Neighbors.GetData(),
		Vertices[VertexIndex].Neighbors.Num()
	);
}

TConstArrayView<int32> FIcosphereGrid::GetVertexTriangles(int32 VertexIndex) const
{
	check(VertexIndex >= 0 && VertexIndex < Vertices.Num()); // checkf-ok -- OOB vertex access crashes regardless; diagnostic only
	return TConstArrayView<int32>(
		Vertices[VertexIndex].Triangles.GetData(),
		Vertices[VertexIndex].Triangles.Num()
	);
}

void FIcosphereGrid::GetTriangleNeighbors(int32 TriangleIndex, int32 OutNeighbors[3]) const
{
	check(TriangleIndex >= 0 && TriangleIndex < Triangles.Num()); // checkf-ok -- OOB triangle access crashes regardless; diagnostic only
	OutNeighbors[0] = Triangles[TriangleIndex].Adjacent[0];
	OutNeighbors[1] = Triangles[TriangleIndex].Adjacent[1];
	OutNeighbors[2] = Triangles[TriangleIndex].Adjacent[2];
}

float FIcosphereGrid::GetTriangleArea(int32 TriangleIndex) const
{
	check(TriangleIndex >= 0 && TriangleIndex < Triangles.Num()); // checkf-ok -- OOB triangle access crashes regardless; diagnostic only
	const FIcosphereTriangle& Tri = Triangles[TriangleIndex];
	const FVector3f& A = Vertices[Tri.V[0]].Direction;
	const FVector3f& B = Vertices[Tri.V[1]].Direction;
	const FVector3f& C = Vertices[Tri.V[2]].Direction;

	// Spherical excess via arc tangent formula (Girard's theorem numerically stable form)
	const float AB = FMath::Acos(FMath::Clamp(FVector3f::DotProduct(A, B), -1.0f, 1.0f));
	const float BC = FMath::Acos(FMath::Clamp(FVector3f::DotProduct(B, C), -1.0f, 1.0f));
	const float CA = FMath::Acos(FMath::Clamp(FVector3f::DotProduct(C, A), -1.0f, 1.0f));

	const float S = (AB + BC + CA) * 0.5f; // semi-perimeter
	const float TanQuarterArea = FMath::Sqrt(
		FMath::Max(0.0f,
			FMath::Tan(S * 0.5f) *
			FMath::Tan((S - AB) * 0.5f) *
			FMath::Tan((S - BC) * 0.5f) *
			FMath::Tan((S - CA) * 0.5f)
		)
	);
	return 4.0f * FMath::Atan(TanQuarterArea);
}

int32 FIcosphereGrid::ExpectedVertexCount(int32 Level)
{
	int32 Power = 1;
	for (int32 I = 0; I < Level; ++I) Power *= 4;
	return 10 * Power + 2;
}

int32 FIcosphereGrid::ExpectedTriangleCount(int32 Level)
{
	int32 Power = 1;
	for (int32 I = 0; I < Level; ++I) Power *= 4;
	return 20 * Power;
}

void FIcosphereGrid::GenerateBaseIcosahedron()
{
	Vertices.Reset();
	Triangles.Reset();

	Vertices.Reserve(IcosphereConstants::NumBaseVerts);
	for (int32 I = 0; I < IcosphereConstants::NumBaseVerts; ++I)
	{
		Vertices.Emplace(IcosphereConstants::BaseVerts[I]);
	}

	Triangles.Reserve(IcosphereConstants::NumBaseFaces);
	for (int32 I = 0; I < IcosphereConstants::NumBaseFaces; ++I)
	{
		Triangles.Emplace(
			IcosphereConstants::BaseFaces[I][0],
			IcosphereConstants::BaseFaces[I][1],
			IcosphereConstants::BaseFaces[I][2]);
	}
}

void FIcosphereGrid::Subdivide()
{
	TMap<uint64, int32> EdgeMidpointCache;
	EdgeMidpointCache.Reserve(Triangles.Num() * 3); // upper bound on unique edges

	TArray<FIcosphereTriangle> NewTriangles;
	NewTriangles.Reserve(Triangles.Num() * 4);

	for (const FIcosphereTriangle& Tri : Triangles)
	{
		const int32 V0 = Tri.V[0];
		const int32 V1 = Tri.V[1];
		const int32 V2 = Tri.V[2];

		// Get or create midpoints for each edge
		const int32 M01 = GetOrCreateMidpoint(V0, V1, EdgeMidpointCache);
		const int32 M12 = GetOrCreateMidpoint(V1, V2, EdgeMidpointCache);
		const int32 M20 = GetOrCreateMidpoint(V2, V0, EdgeMidpointCache);

		// 4 child triangles (preserve CCW winding)
		NewTriangles.Emplace(V0, M01, M20);
		NewTriangles.Emplace(M01, V1, M12);
		NewTriangles.Emplace(M20, M12, V2);
		NewTriangles.Emplace(M01, M12, M20); // center triangle
	}

	Triangles = MoveTemp(NewTriangles);
}

int32 FIcosphereGrid::GetOrCreateMidpoint(int32 A, int32 B, TMap<uint64, int32>& Cache)
{
	const uint64 Key = IcosphereInternal::MakeEdgeKey(A, B);

	if (const int32* Existing = Cache.Find(Key))
	{
		return *Existing;
	}

	// Compute midpoint and project onto unit sphere (the ONE canonical
	// subdivision step -- shared with every other lattice walker).
	const FVector3f Mid = IcosphereConstants::CanonicalMidpoint(
		Vertices[A].Direction, Vertices[B].Direction);

	const int32 NewIndex = Vertices.Num();
	Vertices.Emplace(Mid);
	Cache.Add(Key, NewIndex);

	return NewIndex;
}

void FIcosphereGrid::ComputeCentroids()
{
	for (FIcosphereTriangle& Tri : Triangles)
	{
		FVector3f C = Vertices[Tri.V[0]].Direction
					+ Vertices[Tri.V[1]].Direction
					+ Vertices[Tri.V[2]].Direction;
		C.Normalize();
		Tri.Centroid = C;
	}
}

void FIcosphereGrid::BuildAdjacency()
{
	// Clear existing adjacency
	for (FIcosphereVertex& Vert : Vertices)
	{
		Vert.Neighbors.Reset();
		Vert.Triangles.Reset();
	}
	for (FIcosphereTriangle& Tri : Triangles)
	{
		Tri.Adjacent[0] = Tri.Adjacent[1] = Tri.Adjacent[2] = -1;
	}

	// --- Vertex -> Triangles ---
	for (int32 TI = 0; TI < Triangles.Num(); ++TI)
	{
		const FIcosphereTriangle& Tri = Triangles[TI];
		Vertices[Tri.V[0]].Triangles.Add(TI);
		Vertices[Tri.V[1]].Triangles.Add(TI);
		Vertices[Tri.V[2]].Triangles.Add(TI);
	}

	// --- Vertex -> Vertex neighbors (from edges of triangles) ---
	// Use a set per vertex to avoid duplicates
	TArray<TSet<int32>> VertexNeighborSets;
	VertexNeighborSets.SetNum(Vertices.Num());

	for (const FIcosphereTriangle& Tri : Triangles)
	{
		for (int32 E = 0; E < 3; ++E)
		{
			const int32 VA = Tri.V[E];
			const int32 VB = Tri.V[(E + 1) % 3];
			VertexNeighborSets[VA].Add(VB);
			VertexNeighborSets[VB].Add(VA);
		}
	}

	for (int32 VI = 0; VI < Vertices.Num(); ++VI)
	{
		Vertices[VI].Neighbors.Reserve(VertexNeighborSets[VI].Num());
		for (int32 Neighbor : VertexNeighborSets[VI])
		{
			Vertices[VI].Neighbors.Add(Neighbor);
		}
	}

	// --- Triangle -> Triangle adjacency (shared edge) ---
	// Map each edge -> list of triangle indices
	TMap<uint64, TArray<int32, TInlineAllocator<2>>> EdgeToTriangles;
	EdgeToTriangles.Reserve(Triangles.Num() * 2); // ~1.5 edges per triangle on average

	for (int32 TI = 0; TI < Triangles.Num(); ++TI)
	{
		const FIcosphereTriangle& Tri = Triangles[TI];
		for (int32 E = 0; E < 3; ++E)
		{
			const uint64 Key = IcosphereInternal::MakeEdgeKey(Tri.V[E], Tri.V[(E + 1) % 3]);
			EdgeToTriangles.FindOrAdd(Key).Add(TI);
		}
	}

	for (int32 TI = 0; TI < Triangles.Num(); ++TI)
	{
		FIcosphereTriangle& Tri = Triangles[TI];
		for (int32 E = 0; E < 3; ++E)
		{
			const uint64 Key = IcosphereInternal::MakeEdgeKey(Tri.V[E], Tri.V[(E + 1) % 3]);
			const TArray<int32, TInlineAllocator<2>>& SharedTris = EdgeToTriangles[Key];
			for (int32 OtherTI : SharedTris)
			{
				if (OtherTI != TI)
				{
					Tri.Adjacent[E] = OtherTI;
					break;
				}
			}
		}
	}
}


// ============================================================================
// FIcosphereFacePartition
// ============================================================================

namespace IcospherePartitionDetail
{
	/** The dominant axis of a direction's magnitudes (0 = X, 1 = Y, 2 = Z); a tie goes to the lower axis. */
	int32 DominantAxis(const float (&Abs)[3])
	{
		if (Abs[0] >= Abs[1] && Abs[0] >= Abs[2])
		{
			return 0;
		}
		if (Abs[1] >= Abs[0] && Abs[1] >= Abs[2])
		{
			return 1;
		}
		return 2;
	}

	/** The cube face on one axis: 2 * Axis on its + side, 2 * Axis + 1 on its - side (the EPlanetFace order). */
	uint8 AxisFace(const FVector3f& Dir, int32 Axis)
	{
		return static_cast<uint8>(Axis * 2 + (Dir[Axis] > 0.0f ? 0 : 1));
	}
}

void FIcosphereFacePartition::Build(const FIcosphereGrid& Grid, float EdgeMargin)
{
	using namespace IcospherePartitionDetail;

	check(Grid.IsValid()); // checkf-ok -- invalid grid would crash on first access regardless; diagnostic only

	const int32 NumVerts = Grid.Vertices.Num();
	const int32 NumTris = Grid.Triangles.Num();

	VertexPrimaryFace.SetNumUninitialized(NumVerts);
	VertexFaceMask.SetNumZeroed(NumVerts);

	for (int32 FI = 0; FI < 6; ++FI)
	{
		FaceVertices[FI].Reset();
		FaceTriangles[FI].Reset();
	}

	auto AddSecondaryFace = [this](int32 VI, uint8 SecFace)
	{
		VertexFaceMask[VI] |= (1 << SecFace);
		FaceVertices[SecFace].Add(VI);
	};

	// --- Assign vertices to faces ---
	for (int32 VI = 0; VI < NumVerts; ++VI)
	{
		const FVector3f& Dir = Grid.Vertices[VI].Direction;
		const float Abs[3] = { FMath::Abs(Dir.X), FMath::Abs(Dir.Y), FMath::Abs(Dir.Z) };

		// Primary face: dominant axis. AxisA and AxisB are the other two, in ascending order.
		const int32 Dominant = DominantAxis(Abs);
		const int32 AxisA = Dominant == 0 ? 1 : 0;
		const int32 AxisB = Dominant == 2 ? 1 : 2;
		const float DominantMag = Abs[Dominant];
		const float SecondMag = FMath::Max(Abs[AxisA], Abs[AxisB]);
		const uint8 PrimaryFace = AxisFace(Dir, Dominant);

		VertexPrimaryFace[VI] = PrimaryFace;
		VertexFaceMask[VI] = (1 << PrimaryFace);
		FaceVertices[PrimaryFace].Add(VI);

		// Check if vertex is near a face edge -- add to secondary faces
		if (DominantMag > 0.0f && (DominantMag - SecondMag) / DominantMag < EdgeMargin)
		{
			if (Abs[AxisA] > Abs[AxisB] - EdgeMargin * DominantMag)
			{
				AddSecondaryFace(VI, AxisFace(Dir, AxisA));
			}
			if (Abs[AxisB] > Abs[AxisA] - EdgeMargin * DominantMag)
			{
				AddSecondaryFace(VI, AxisFace(Dir, AxisB));
			}
		}
	}

	// --- Assign triangles to faces ---
	TrianglePrimaryFace.SetNumUninitialized(NumTris);

	for (int32 TI = 0; TI < NumTris; ++TI)
	{
		// Use centroid direction for triangle assignment (more stable than vote)
		const FVector3f& C = Grid.Triangles[TI].Centroid;
		const float CAbs[3] = { FMath::Abs(C.X), FMath::Abs(C.Y), FMath::Abs(C.Z) };
		const uint8 Face = AxisFace(C, DominantAxis(CAbs));

		TrianglePrimaryFace[TI] = Face;
		FaceTriangles[Face].Add(TI);
	}
}

TArray<uint8, TInlineAllocator<3>> FIcosphereFacePartition::GetVertexFaces(int32 VertexIndex) const
{
	TArray<uint8, TInlineAllocator<3>> Result;
	const uint8 Mask = VertexFaceMask[VertexIndex];
	for (uint8 F = 0; F < 6; ++F)
	{
		if (Mask & (1 << F))
		{
			Result.Add(F);
		}
	}
	return Result;
}

int32 FIcosphereFacePartition::CountSharedVertices() const
{
	int32 Count = 0;
	for (int32 VI = 0; VI < VertexFaceMask.Num(); ++VI)
	{
		if (FMath::CountBits(VertexFaceMask[VI]) > 1)
		{
			++Count;
		}
	}
	return Count;
}
