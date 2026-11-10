// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Category: GALAXY_PROJECT.SpatialAlgorithms.LeafTopology.*
//
// These tests guard the EXACT leaf hierarchy the wildlife population fields move
// conserved mass across. The invariants below are not stylistic -- each one is a
// conservation bug if it breaks:
//   * every leaf has exactly 3 edge-neighbours   -> mass cannot leak off-graph
//   * adjacency is symmetric                     -> flux A->B is answerable as B->A
//   * areas sum to the whole sphere              -> no leaf lost or double-counted
//   * the descent matches the shipped one        -> the graph agrees with the
//                                                   function that locates positions
// ============================================================================

#include "Misc/AutomationTest.h"
#include "TestUtilities.h"

#include "Spatial/IcogridLeafTopology.h"
#include "IcogridTestFixtures.h" // GalaxyTestFixtures::MakeIcogrid
#include "ECS/Types/FaceIcogridData.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Depth used by the topology tests. 20 * 4^3 = 1280 leaves -- enough structure to
	 *  exercise pentagon neighbourhoods, small enough to check exhaustively. */
	constexpr int32 TestDepth = 3;

	/**
	 * Deterministic near-uniform sphere sampling (Fibonacci spiral). No RNG: the
	 * point set is a pure function of Count, so a failure reproduces exactly.
	 */
	FVector3f FibonacciDirection(int32 Index, int32 Count)
	{
		const float GoldenAngle = PI * (3.0f - FMath::Sqrt(5.0f));
		const float Z = 1.0f - 2.0f * (static_cast<float>(Index) + 0.5f) / static_cast<float>(Count);
		const float R = FMath::Sqrt(FMath::Max(0.0f, 1.0f - Z * Z));
		const float Theta = GoldenAngle * static_cast<float>(Index);
		return FVector3f(R * FMath::Cos(Theta), R * FMath::Sin(Theta), Z).GetSafeNormal();
	}

	/** Shared body in IcogridTestFixtures.h; zero heights -- this suite
		cross-checks LEAF TOPOLOGY, which is elevation-independent. */
	FIcogrid BuildGridForCrossCheck(int32 SubdivisionLevel)
	{
		return GalaxyTestFixtures::MakeIcogrid(SubdivisionLevel, /*UniformHeight*/ 0.0f);
	}
}

// ============================================================================
// LeafId arithmetic -- the merge/split walk
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeafIdArithmeticTest,
	"GALAXY_PROJECT.SpatialAlgorithms.LeafTopology.Unit.LeafIdArithmeticWalksTheHierarchy",
	GALAXY_TEST_FLAGS)

bool FLeafIdArithmeticTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const int32 NumLeaves = IcogridLeafTopology::LeavesAtDepth(TestDepth);
	TestEqual(TEXT("Leaf count is 20 * 4^depth"), NumLeaves, 20 * 64);

	int32 Checked = 0;
	for (int32 LeafId = 0; LeafId < NumLeaves; ++LeafId)
	{
		// Decompose and recompose.
		const int32 Base = IcogridLeafTopology::BaseTriangleOf(LeafId, TestDepth);
		const int32 Path = IcogridLeafTopology::ChildPathOf(LeafId, TestDepth);
		if (Base * IcogridLeafTopology::Pow4(TestDepth) + Path != LeafId)
		{
			AddError(FString::Printf(TEXT("Leaf %d failed decompose/recompose"), LeafId));
			return false;
		}

		// Walking all the way up lands on the base triangle; each step is a
		// well-formed leaf at its own depth.
		if (IcogridLeafTopology::AncestorAtDepth(LeafId, TestDepth, 0) != Base)
		{
			AddError(FString::Printf(TEXT("Leaf %d ancestor at depth 0 is not its base triangle"), LeafId));
			return false;
		}

		// Every ancestor claims this leaf as a descendant, and the descendant block
		// of that ancestor is contiguous and contains it.
		for (int32 Up = 0; Up <= TestDepth; ++Up)
		{
			const int32 Ancestor = IcogridLeafTopology::AncestorAtDepth(LeafId, TestDepth, Up);
			if (!IcogridLeafTopology::IsDescendantOf(LeafId, TestDepth, Ancestor, Up))
			{
				AddError(FString::Printf(TEXT("Leaf %d not a descendant of its own depth-%d ancestor"), LeafId, Up));
				return false;
			}

			const int32 First = IcogridLeafTopology::FirstDescendantAtDepth(Ancestor, Up, TestDepth);
			const int32 Count = IcogridLeafTopology::DescendantCount(Up, TestDepth);
			if (LeafId < First || LeafId >= First + Count)
			{
				AddError(FString::Printf(
					TEXT("Leaf %d outside its depth-%d ancestor's block [%d, %d)"),
					LeafId, Up, First, First + Count));
				return false;
			}
			++Checked;
		}
	}

	// Scope, not just success: a loop that silently ran zero times reads identical
	// to one that passed.
	TestEqual(TEXT("Ancestor relations checked"), Checked, NumLeaves * (TestDepth + 1));

	// Identity cases.
	TestEqual(TEXT("Ancestor at own depth is self"),
		IcogridLeafTopology::AncestorAtDepth(777, TestDepth, TestDepth), 777);
	TestEqual(TEXT("Descendant count at own depth is 1"),
		IcogridLeafTopology::DescendantCount(TestDepth, TestDepth), 1);

	return true;
}

// ============================================================================
// Exact adjacency -- 3 neighbours, symmetric, closed
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeafAdjacencyIsExactTest,
	"GALAXY_PROJECT.SpatialAlgorithms.LeafTopology.Unit.AdjacencyIsExactAndSymmetric",
	GALAXY_TEST_FLAGS)

bool FLeafAdjacencyIsExactTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	IcogridLeafTopology::FLeafAdjacency Graph;
	Graph.Build(TestDepth);

	if (!TestTrue(TEXT("Graph built"), Graph.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Graph covers every leaf"), Graph.NumLeaves, IcogridLeafTopology::LeavesAtDepth(TestDepth));

	// Every leaf: 3 in-range, distinct, non-self neighbours. A conforming
	// triangulation has no boundary, so there is no legitimate -1 anywhere.
	int32 DirectedEdges = 0;
	for (int32 LeafId = 0; LeafId < Graph.NumLeaves; ++LeafId)
	{
		const TConstArrayView<int32> Neighbors = Graph.GetNeighbors(LeafId);
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 N = Neighbors[K];
			if (N < 0 || N >= Graph.NumLeaves)
			{
				AddError(FString::Printf(TEXT("Leaf %d edge %d has out-of-range neighbour %d"), LeafId, K, N));
				return false;
			}
			if (N == LeafId)
			{
				AddError(FString::Printf(TEXT("Leaf %d edge %d is self-referential"), LeafId, K));
				return false;
			}
			++DirectedEdges;
		}

		if (Neighbors[0] == Neighbors[1] || Neighbors[1] == Neighbors[2] || Neighbors[0] == Neighbors[2])
		{
			AddError(FString::Printf(
				TEXT("Leaf %d has duplicate neighbours (%d, %d, %d)"),
				LeafId, Neighbors[0], Neighbors[1], Neighbors[2]));
			return false;
		}
	}
	TestEqual(TEXT("Directed edges = leaves * 3"), DirectedEdges, Graph.NumLeaves * 3);

	// Symmetry: if A reaches B, B reaches A. Without this a flux computed from one
	// side would not be answerable from the other, and mass would not conserve.
	int32 SymmetricPairs = 0;
	for (int32 LeafId = 0; LeafId < Graph.NumLeaves; ++LeafId)
	{
		for (const int32 N : Graph.GetNeighbors(LeafId))
		{
			const TConstArrayView<int32> Back = Graph.GetNeighbors(N);
			if (Back[0] != LeafId && Back[1] != LeafId && Back[2] != LeafId)
			{
				AddError(FString::Printf(TEXT("Adjacency not symmetric: %d -> %d, but not back"), LeafId, N));
				return false;
			}
			++SymmetricPairs;
		}
	}
	TestEqual(TEXT("Symmetric checks performed"), SymmetricPairs, Graph.NumLeaves * 3);

	// Handshake: 30 * 4^depth undirected edges.
	TestEqual(TEXT("Undirected edge count is 30 * 4^depth"),
		DirectedEdges / 2, 30 * IcogridLeafTopology::Pow4(TestDepth));

	// POSITIVE CONTROL -- prove the symmetry check above has teeth. Corrupt one
	// entry and confirm the same scan reports it; a symmetry test that cannot fail
	// would pass just as happily over a broken graph.
	{
		IcogridLeafTopology::FLeafAdjacency Corrupted = Graph;
		const int32 Victim = 0;
		const int32 Original = Corrupted.Neighbors[Victim * 3];
		// Point leaf 0's first edge at some leaf that does not point back.
		int32 Stranger = INDEX_NONE;
		for (int32 Candidate = 1; Candidate < Corrupted.NumLeaves; ++Candidate)
		{
			const TConstArrayView<int32> Back = Graph.GetNeighbors(Candidate);
			if (Candidate != Original && Back[0] != Victim && Back[1] != Victim && Back[2] != Victim)
			{
				Stranger = Candidate;
				break;
			}
		}
		if (!TestTrue(TEXT("Control found a non-adjacent leaf to corrupt with"), Stranger != INDEX_NONE))
		{
			return false;
		}
		Corrupted.Neighbors[Victim * 3] = Stranger;

		bool bDetected = false;
		for (const int32 N : Corrupted.GetNeighbors(Victim))
		{
			const TConstArrayView<int32> Back = Corrupted.GetNeighbors(N);
			if (Back[0] != Victim && Back[1] != Victim && Back[2] != Victim)
			{
				bDetected = true;
			}
		}
		TestTrue(TEXT("CONTROL: corrupting one edge is detected as asymmetry"), bDetected);
	}

	return true;
}

// ============================================================================
// Areas -- sum to the sphere, and are genuinely not uniform
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeafAreaTest,
	"GALAXY_PROJECT.SpatialAlgorithms.LeafTopology.Unit.AreasSumToSphereAndVary",
	GALAXY_TEST_FLAGS)

bool FLeafAreaTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	IcogridLeafTopology::FLeafAdjacency Graph;
	Graph.Build(TestDepth);
	if (!TestTrue(TEXT("Graph built"), Graph.IsValid()))
	{
		return false;
	}

	// Global closure: every leaf counted once, none lost or duplicated. This is the
	// strongest single check in the file -- a missing or repeated leaf shows up here
	// even when every local invariant passes.
	double Total = 0.0;
	float MinArea = TNumericLimits<float>::Max();
	float MaxArea = 0.0f;
	for (int32 LeafId = 0; LeafId < Graph.NumLeaves; ++LeafId)
	{
		const float Area = Graph.GetArea(LeafId);
		if (Area <= 0.0f)
		{
			AddError(FString::Printf(TEXT("Leaf %d has non-positive area %f"), LeafId, Area));
			return false;
		}
		Total += Area;
		MinArea = FMath::Min(MinArea, Area);
		MaxArea = FMath::Max(MaxArea, Area);
	}

	const double FullSphere = 4.0 * PI;
	TestTrue(
		FString::Printf(TEXT("Leaf areas sum to 4*PI (got %f, want %f)"), Total, FullSphere),
		FMath::Abs(Total - FullSphere) < 1.0e-3);

	// NON-VACUITY: the areas must actually vary, or the per-leaf array is dead
	// weight and callers should have used the mean. The 12 pentagonal corners of
	// the icosahedron are what make this true.
	const float MeanArea = static_cast<float>(FullSphere) / static_cast<float>(Graph.NumLeaves);
	TestTrue(
		FString::Printf(TEXT("Smallest leaf is meaningfully below the mean (min %f, mean %f)"), MinArea, MeanArea),
		MinArea < MeanArea * 0.99f);
	TestTrue(
		FString::Printf(TEXT("Largest leaf is meaningfully above the mean (max %f, mean %f)"), MaxArea, MeanArea),
		MaxArea > MeanArea * 1.01f);

	return true;
}

// ============================================================================
// Cross-validation -- this descent IS the shipped descent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeafDescentMatchesAcceleratorTest,
	"GALAXY_PROJECT.SpatialAlgorithms.LeafTopology.Unit.DescentMatchesShippedAccelerator",
	GALAXY_TEST_FLAGS)

bool FLeafDescentMatchesAcceleratorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Level 5 keeps DescentAccelerator::Build from clamping our test depth away:
	// it clamps to min(requested, SubdivisionLevel - 2).
	FIcogrid Grid = BuildGridForCrossCheck(5);
	if (!TestTrue(TEXT("Cross-check grid built"), Grid.VertexCount > 0))
	{
		return false;
	}

	Grid.DescentAccelerator.Build(Grid, TestDepth);
	if (!TestTrue(TEXT("Accelerator built"), Grid.DescentAccelerator.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Accelerator kept the requested depth (no clamp)"),
		Grid.DescentAccelerator.DescentDepth, TestDepth);

	constexpr int32 SampleCount = 4000;
	int32 Compared = 0;
	int32 Mismatches = 0;
	for (int32 I = 0; I < SampleCount; ++I)
	{
		const FVector3f Dir = FibonacciDirection(I, SampleCount);
		const int32 Mine = IcogridLeafTopology::DescendToLeaf(Dir, TestDepth);
		const int32 Theirs = Grid.DescentAccelerator.DescendToLeaf(Dir);
		if (Mine != Theirs)
		{
			if (Mismatches < 5)
			{
				AddError(FString::Printf(
					TEXT("Descent disagrees at sample %d: GalaxyCore %d vs accelerator %d"),
					I, Mine, Theirs));
			}
			++Mismatches;
		}
		++Compared;
	}

	TestEqual(TEXT("Every sampled direction was compared"), Compared, SampleCount);
	TestEqual(TEXT("Descent agrees with the shipped accelerator everywhere"), Mismatches, 0);

	// POSITIVE CONTROL -- a comparison that cannot fail proves nothing. Descending
	// to a DIFFERENT depth must disagree, which shows the equality above is a real
	// measurement rather than two functions both returning the same constant.
	int32 ControlMismatches = 0;
	for (int32 I = 0; I < SampleCount; ++I)
	{
		const FVector3f Dir = FibonacciDirection(I, SampleCount);
		if (IcogridLeafTopology::DescendToLeaf(Dir, TestDepth - 1) != Grid.DescentAccelerator.DescendToLeaf(Dir))
		{
			++ControlMismatches;
		}
	}
	TestTrue(TEXT("CONTROL: a wrong-depth descent does disagree"), ControlMismatches > 0);

	return true;
}

// ============================================================================
// Adjacency agrees with the descent it serves
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeafAdjacencyMatchesGeometryTest,
	"GALAXY_PROJECT.SpatialAlgorithms.LeafTopology.Unit.NeighboursShareTheCrossedEdge",
	GALAXY_TEST_FLAGS)

bool FLeafAdjacencyMatchesGeometryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	IcogridLeafTopology::FLeafAdjacency Graph;
	Graph.Build(TestDepth);
	if (!TestTrue(TEXT("Graph built"), Graph.IsValid()))
	{
		return false;
	}

	// The neighbour across edge k must physically share edge k's two endpoints.
	// This is the geometric statement of "edge-adjacent", independent of how the
	// graph was constructed -- so it catches a corner-only contact that a
	// shared-vertex heuristic would happily accept.
	constexpr float PositionTolerance = 1.0e-4f;
	int32 EdgesVerified = 0;

	for (int32 LeafId = 0; LeafId < Graph.NumLeaves; ++LeafId)
	{
		FVector3f Corner[3];
		IcogridLeafTopology::LeafCorners(LeafId, TestDepth, Corner[0], Corner[1], Corner[2]);

		const TConstArrayView<int32> Neighbors = Graph.GetNeighbors(LeafId);
		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			const FVector3f& E0 = Corner[Edge];
			const FVector3f& E1 = Corner[(Edge + 1) % 3];

			FVector3f NCorner[3];
			IcogridLeafTopology::LeafCorners(Neighbors[Edge], TestDepth, NCorner[0], NCorner[1], NCorner[2]);

			int32 Shared = 0;
			for (const FVector3f& Mine : { E0, E1 })
			{
				for (const FVector3f& Theirs : NCorner)
				{
					if (FVector3f::Distance(Mine, Theirs) < PositionTolerance)
					{
						++Shared;
						break;
					}
				}
			}

			if (Shared != 2)
			{
				AddError(FString::Printf(
					TEXT("Leaf %d edge %d: neighbour %d shares %d of 2 edge endpoints"),
					LeafId, Edge, Neighbors[Edge], Shared));
				return false;
			}
			++EdgesVerified;
		}
	}

	TestEqual(TEXT("Every leaf edge was geometrically verified"), EdgesVerified, Graph.NumLeaves * 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
