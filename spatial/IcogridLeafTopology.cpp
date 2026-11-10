
#include "Spatial/IcogridLeafTopology.h"

#include "Spatial/Icosphere.h"

namespace IcogridLeafTopology
{
	namespace
	{
		/**
		 * Spherical point-in-triangle test, matching the one FaceIcogridData.cpp
		 * uses for descent. The small NEGATIVE tolerance is load-bearing: a
		 * direction lying exactly on a great-circle edge (every base icosahedron
		 * vertex is shared by 5 faces) would otherwise fail all three tests and
		 * fall out of the descent entirely.
		 */
		FORCEINLINE bool IsInsideSphericalTriangle(
			const FVector3f& Dir,
			const FVector3f& A,
			const FVector3f& B,
			const FVector3f& C)
		{
			constexpr float Eps = -1.0e-6f;
			return FVector3f::DotProduct(Dir, FVector3f::CrossProduct(A, B)) >= Eps
				&& FVector3f::DotProduct(Dir, FVector3f::CrossProduct(B, C)) >= Eps
				&& FVector3f::DotProduct(Dir, FVector3f::CrossProduct(C, A)) >= Eps;
		}

		/** Arc length between two unit directions. The clamp is not optional -- a dot
		 *  of 1.0000001 from rounding makes Acos return NaN, which then poisons every
		 *  area and every nudge derived from it. */
		FORCEINLINE float ArcBetween(const FVector3f& A, const FVector3f& B)
		{
			return FMath::Acos(FMath::Clamp(FVector3f::DotProduct(A, B), -1.0f, 1.0f));
		}

		/** Girard's theorem in the numerically stable half-tangent form -- the same
		 *  formula FIcosphereGrid::GetTriangleArea uses, so areas agree across modules. */
		float SphericalTriangleArea(const FVector3f& A, const FVector3f& B, const FVector3f& C)
		{
			const float AB = ArcBetween(A, B);
			const float BC = ArcBetween(B, C);
			const float CA = ArcBetween(C, A);

			const float S = (AB + BC + CA) * 0.5f;
			const float TanQuarterArea = FMath::Sqrt(
				FMath::Max(0.0f,
					FMath::Tan(S * 0.5f) *
					FMath::Tan((S - AB) * 0.5f) *
					FMath::Tan((S - BC) * 0.5f) *
					FMath::Tan((S - CA) * 0.5f)));

			return 4.0f * FMath::Atan(TanQuarterArea);
		}

		[[nodiscard]] bool IsDepthSupported(int32 Depth)
		{
			return Depth >= 1 && Depth <= MaxSupportedDepth;
		}
	}

	int32 Pow4(int32 N)
	{
		int32 Result = 1;
		for (int32 I = 0; I < N; ++I)
		{
			Result *= 4;
		}
		return Result;
	}

	int32 LeavesAtDepth(int32 Depth)
	{
		return IcosphereConstants::NumBaseFaces * Pow4(Depth);
	}

	float EdgeArcAtDepth(int32 Depth)
	{
		// Nominal only: real edges vary a few percent because normalizing a
		// midpoint does not bisect a spherical edge exactly. Used for sizing a
		// nudge, never for measuring.
		const float BaseEdgeArc = ArcBetween(
			IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[0][0]],
			IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[0][1]]);
		return BaseEdgeArc / static_cast<float>(1 << Depth);
	}

	int32 BaseTriangleOf(int32 LeafId, int32 Depth)
	{
		return LeafId / Pow4(Depth);
	}

	int32 ChildPathOf(int32 LeafId, int32 Depth)
	{
		return LeafId % Pow4(Depth);
	}

	int32 AncestorAtDepth(int32 LeafId, int32 FromDepth, int32 ToDepth)
	{
		if (ToDepth < 0 || ToDepth > FromDepth)
		{
			return INDEX_NONE;
		}

		const int32 Base = BaseTriangleOf(LeafId, FromDepth);
		const int32 Path = ChildPathOf(LeafId, FromDepth);

		// Coarsest digit is most significant, so dropping the deepest
		// (FromDepth - ToDepth) digits is a right shift of two bits each.
		const int32 AncestorPath = Path >> (2 * (FromDepth - ToDepth));
		return Base * Pow4(ToDepth) + AncestorPath;
	}

	int32 DescendantCount(int32 FromDepth, int32 ToDepth)
	{
		if (ToDepth < FromDepth)
		{
			return 0;
		}
		return Pow4(ToDepth - FromDepth);
	}

	int32 FirstDescendantAtDepth(int32 LeafId, int32 FromDepth, int32 ToDepth)
	{
		if (ToDepth < FromDepth)
		{
			return INDEX_NONE;
		}

		const int32 Base = BaseTriangleOf(LeafId, FromDepth);
		const int32 Path = ChildPathOf(LeafId, FromDepth);

		// Appending (ToDepth - FromDepth) zero digits gives the lowest-id
		// descendant; the rest of the block follows contiguously.
		const int32 DeepPath = Path << (2 * (ToDepth - FromDepth));
		return Base * Pow4(ToDepth) + DeepPath;
	}

	bool IsDescendantOf(int32 LeafId, int32 LeafDepth, int32 AncestorId, int32 AncestorDepth)
	{
		if (AncestorDepth > LeafDepth)
		{
			return false;
		}
		return AncestorAtDepth(LeafId, LeafDepth, AncestorDepth) == AncestorId;
	}

	void LeafCorners(int32 LeafId, int32 Depth, FVector3f& OutA, FVector3f& OutB, FVector3f& OutC)
	{
		const int32 Base = BaseTriangleOf(LeafId, Depth);
		const int32 Path = ChildPathOf(LeafId, Depth);

		OutA = IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[Base][0]];
		OutB = IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[Base][1]];
		OutC = IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[Base][2]];

		// Walk digits coarsest-first, applying exactly the corner assignments the
		// descent makes. Child layout is pinned to Subdivide() in Icosphere.h:
		//   0 = corner at A, 1 = corner at B, 2 = corner at C, 3 = centre.
		for (int32 Level = Depth - 1; Level >= 0; --Level)
		{
			const int32 Digit = (Path >> (2 * Level)) & 3;

			FVector3f Mab = (OutA + OutB) * 0.5f; Mab.Normalize();
			FVector3f Mbc = (OutB + OutC) * 0.5f; Mbc.Normalize();
			FVector3f Mca = (OutC + OutA) * 0.5f; Mca.Normalize();

			switch (Digit)
			{
			case 0:  OutB = Mab; OutC = Mca; break;
			case 1:  OutA = Mab; OutC = Mbc; break;
			case 2:  OutA = Mca; OutB = Mbc; break;
			default: OutA = Mab; OutB = Mbc; OutC = Mca; break;
			}
		}
	}

	FVector3f LeafCentroid(int32 LeafId, int32 Depth)
	{
		FVector3f A, B, C;
		LeafCorners(LeafId, Depth, A, B, C);
		FVector3f Centroid = A + B + C;
		Centroid.Normalize();
		return Centroid;
	}

	float LeafAreaSteradians(int32 LeafId, int32 Depth)
	{
		FVector3f A, B, C;
		LeafCorners(LeafId, Depth, A, B, C);
		return SphericalTriangleArea(A, B, C);
	}

	int32 DescendToLeaf(const FVector3f& Direction, int32 Depth)
	{
		if (!IsDepthSupported(Depth))
		{
			return INDEX_NONE;
		}

		// Phase A: which base triangle.
		int32 BaseTriangle = INDEX_NONE;
		FVector3f TriA, TriB, TriC;

		for (int32 T = 0; T < IcosphereConstants::NumBaseFaces; ++T)
		{
			TriA = IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[T][0]];
			TriB = IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[T][1]];
			TriC = IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[T][2]];

			if (IsInsideSphericalTriangle(Direction, TriA, TriB, TriC))
			{
				BaseTriangle = T;
				break;
			}
		}

		if (BaseTriangle < 0)
		{
			// Exactly on a great-circle edge: fall back to nearest centroid, the
			// same recovery the shipped descent makes.
			float BestDot = -2.0f;
			for (int32 T = 0; T < IcosphereConstants::NumBaseFaces; ++T)
			{
				const FVector3f& A = IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[T][0]];
				const FVector3f& B = IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[T][1]];
				const FVector3f& C = IcosphereConstants::BaseVerts[IcosphereConstants::BaseFaces[T][2]];
				FVector3f Centroid = A + B + C;
				Centroid.Normalize();
				const float Dot = FVector3f::DotProduct(Direction, Centroid);
				if (Dot > BestDot)
				{
					BestDot = Dot;
					BaseTriangle = T;
					TriA = A;
					TriB = B;
					TriC = C;
				}
			}
		}

		// Phase B: descend.
		int32 ChildPath = 0;
		for (int32 Level = 0; Level < Depth; ++Level)
		{
			FVector3f Mab = (TriA + TriB) * 0.5f; Mab.Normalize();
			FVector3f Mbc = (TriB + TriC) * 0.5f; Mbc.Normalize();
			FVector3f Mca = (TriC + TriA) * 0.5f; Mca.Normalize();

			if (IsInsideSphericalTriangle(Direction, TriA, Mab, Mca))
			{
				ChildPath = ChildPath * 4 + 0;
				TriB = Mab;
				TriC = Mca;
			}
			else if (IsInsideSphericalTriangle(Direction, Mab, TriB, Mbc))
			{
				ChildPath = ChildPath * 4 + 1;
				TriA = Mab;
				TriC = Mbc;
			}
			else if (IsInsideSphericalTriangle(Direction, Mca, Mbc, TriC))
			{
				ChildPath = ChildPath * 4 + 2;
				TriA = Mca;
				TriB = Mbc;
			}
			else
			{
				ChildPath = ChildPath * 4 + 3;
				TriA = Mab;
				TriB = Mbc;
				TriC = Mca;
			}
		}

		return BaseTriangle * Pow4(Depth) + ChildPath;
	}

	void FLeafAdjacency::Clear()
	{
		Depth = 0;
		NumLeaves = 0;
		Neighbors.Empty();
		Areas.Empty();
	}

	void FLeafAdjacency::Build(int32 InDepth)
	{
		Clear();

		if (!IsDepthSupported(InDepth))
		{
			return;
		}

		Depth = InDepth;
		NumLeaves = LeavesAtDepth(Depth);
		Neighbors.SetNumUninitialized(NumLeaves * 3);
		Areas.SetNumUninitialized(NumLeaves);

		// Step this far across an edge to land in the neighbour. A thousandth of a
		// leaf edge is deep inside it (the nearest OTHER edge of the neighbour is
		// roughly half an edge away) and far above float noise at these magnitudes.
		const float NudgeArc = EdgeArcAtDepth(Depth) * 0.001f;
		const float NudgeTan = FMath::Tan(NudgeArc);

		for (int32 LeafId = 0; LeafId < NumLeaves; ++LeafId)
		{
			FVector3f Corner[3];
			LeafCorners(LeafId, Depth, Corner[0], Corner[1], Corner[2]);

			Areas[LeafId] = SphericalTriangleArea(Corner[0], Corner[1], Corner[2]);

			FVector3f Centroid = Corner[0] + Corner[1] + Corner[2];
			Centroid.Normalize();

			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				// Edge k spans Corner[k] -> Corner[(k+1)%3].
				FVector3f Midpoint = Corner[Edge] + Corner[(Edge + 1) % 3];
				Midpoint.Normalize();

				// Outward tangent at the midpoint. The ray from the centroid through
				// an edge midpoint leaves the triangle through THAT edge and no
				// other, which is what makes this pick the true edge-neighbour
				// rather than a corner-only one.
				FVector3f Outward = Midpoint - Centroid;
				Outward -= Midpoint * FVector3f::DotProduct(Outward, Midpoint);

				if (!Outward.Normalize())
				{
					// Degenerate only if the midpoint coincides with the centroid,
					// which no real subdivision produces. Leave the slot self-
					// referential rather than inventing a neighbour; IsValid() still
					// passes but the cross-check test will catch it loudly.
					Neighbors[LeafId * 3 + Edge] = LeafId;
					continue;
				}

				FVector3f Probe = Midpoint + Outward * NudgeTan;
				Probe.Normalize();

				Neighbors[LeafId * 3 + Edge] = DescendToLeaf(Probe, Depth);
			}
		}
	}
}

// ============================================================================
// IcogridLeafTopology::FLeafAdjacency
// ============================================================================

namespace IcogridLeafTopology
{
	bool FLeafAdjacency::IsValid() const
	{
		return Depth > 0
			&& NumLeaves > 0
			&& Neighbors.Num() == NumLeaves * 3
			&& Areas.Num() == NumLeaves;
	}
}
