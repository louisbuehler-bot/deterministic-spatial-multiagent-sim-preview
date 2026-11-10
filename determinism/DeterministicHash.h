// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// HASH primitives for per-shard determinism checksums (two modes).
//
// Backs IShardBridge::ComputeShardChecksum() -- the per-shard payload checksum the
// tape/deterministic-replay runner diffs to localize a desync to a specific shard. TWO modes,
// but only ONE of them has any callers:
//   - Bytes(): DERIVED CHECKSUM -- hash a shard's SerializeShardState payload, so
//     the checksum is a pure function of the saved/shipped bytes (the binding
//     contract). THIS IS THE ONLY MODE IN USE: all 10 shard classes in the roster
//     go through it.
//   - Quantize/Scalar/Vector(): CURRENTLY ZERO CALLERS -- reserved scaffolding for
//     the D5 cross-platform work, NOT a live path. It existed for authoritative
//     state a payload did not fully capture -- specifically PlanetShard's per-entity
//     ECS registry while that registry sat outside SerializeShardState. Schema v4
//     brought the registry INTO the payload, which deleted the last
//     caller and with it the only justification for the mode.
//     DO NOT reintroduce a caller that quantizes ONLY the checksum: a quantized
//     checksum is more permissive than the unquantized payload that actually
//     leaves the machine, which is the trusted-false-negative direction the
//     DERIVED CHECKSUM hard rule forbids. When D5 lands, quantization belongs
//     INSIDE serialization so the payload is quantized and the checksum inherits
//     it. Mechanics of the mode, for whenever that happens: floats are QUANTIZED
//     to a fixed quantum before hashing (invariant C6, Numeric Determinism), so
//     sub-quantum wobble (a 1-ulp formula change, a cross-platform rounding
//     difference) cannot flip the checksum -- it tracks authoritative OUTCOMES,
//     not float noise. After quantization everything is integer arithmetic, so the
//     hash is exactly reproducible under same-binary lockstep.
//
// Order-independence: combine per-entity hashes with XOR after salting each with
// its (stable) entity id, so the result does NOT depend on EnTT view iteration
// order (which the determinism rules forbid relying on). Two entities with
// identical payloads still differ because the id is mixed in, so XOR will not
// cancel them.
//
// Foundation module (GalaxyCore/Numeric): pure header-only determinism bedrock
// (inline constexpr + FORCEINLINE, CoreMinimal-only, no reflection, no mutable
// global), sibling of GalaxyDeterministicFP.h.
//
// EXPORTS: GalaxyDeterminism::Hash (Seed, Quantum*, Mix, Combine, Quantize,
//   Scalar, Vector, Int, BytesRaw, Bytes, Str)
// ============================================================================
#pragma once

#include "CoreMinimal.h"

namespace GalaxyDeterminism
{
	namespace Hash
	{
		// Golden seed (pi fractional bits) so a populated shard's checksum is
		// effectively never 0 -- BuildTapeCheckpointEntry drops a 0 checksum as the
		// "ComputeShardChecksum not implemented" sentinel, so an implemented shard
		// must avoid colliding with it.
		inline constexpr uint64 Seed = 0x243F6A8885A308D3ull;

		// Absolute quanta, chosen per field scale at the call site. The quantum is
		// the granularity below which a difference is treated as noise, so pick it a
		// few orders of magnitude above a float ulp at that field's magnitude but
		// well below a gameplay-meaningful delta.
		inline constexpr double QuantumUnit = 1.0e-5; // normalized / unit-vector components [-1,1]
		inline constexpr double QuantumPos  = 1.0e-2; // positions / heights / radii (UU)
		inline constexpr double QuantumStat = 1.0e-3; // magnitudes (strength, speed, supply, mass)

		// Stable non-finite sentinel: NaN/Inf hash to a fixed value (the NaN/Inf
		// sentinel catches them separately; here we only need reproducibility).
		inline constexpr int64 NonFiniteSentinel = 0x7FFFFFFFFFFFFFFFll;

		// 64-bit avalanche (MurmurHash3 finalizer constants).
		[[nodiscard]] FORCEINLINE uint64 Mix(uint64 H)
		{
			H ^= H >> 33;
			H *= 0xFF51AFD7ED558CCDull;
			H ^= H >> 33;
			H *= 0xC4CEB9FE1A85EC53ull;
			H ^= H >> 33;
			return H;
		}

		// Fold one value into a running hash (FNV prime multiply, then avalanche).
		[[nodiscard]] FORCEINLINE uint64 Combine(uint64 H, uint64 Value)
		{
			return Mix((H ^ Value) * 0x100000001B3ull);
		}

		// Round a value to the nearest integer multiple of Quantum. Non-finite ->
		// a fixed sentinel so the hash stays reproducible.
		[[nodiscard]] FORCEINLINE int64 Quantize(double Value, double Quantum)
		{
			if (!FMath::IsFinite(Value))
			{
				return NonFiniteSentinel;
			}
			return static_cast<int64>(FMath::RoundToDouble(Value / Quantum));
		}

		[[nodiscard]] FORCEINLINE uint64 Scalar(uint64 H, double Value, double Quantum)
		{
			return Combine(H, static_cast<uint64>(Quantize(Value, Quantum)));
		}

		[[nodiscard]] FORCEINLINE uint64 Vector(uint64 H, const FVector& V, double Quantum)
		{
			H = Scalar(H, V.X, Quantum);
			H = Scalar(H, V.Y, Quantum);
			H = Scalar(H, V.Z, Quantum);
			return H;
		}

		[[nodiscard]] FORCEINLINE uint64 Int(uint64 H, int64 Value)
		{
			return Combine(H, static_cast<uint64>(Value));
		}

		// DERIVED-CHECKSUM primitive: fold a byte buffer (a shard's
		// SerializeShardState payload) into a running hash. Length-prefixed so []
		// and [0] differ; bytes are consumed in array order = serialization order.
		// This is how a per-shard checksum becomes a pure function of the
		// saved/shipped payload -- one state enumeration, so the checksum cannot
		// diverge from the bytes that feed save / replay / resync (see the
		// serialization DERIVED CHECKSUM invariant). Raw
		// bytes are bit-stable under same-binary lockstep and intentionally SKIP C6
		// quantization; the Quantize/Scalar/Vector path above is for the residual
		// case of hashing authoritative state that a payload does NOT fully capture
		// (e.g. PlanetShard's per-entity ECS registry, not yet in its payload).
		[[nodiscard]] FORCEINLINE uint64 BytesRaw(uint64 H, const uint8* Data, int32 N)
		{
			H = Combine(H, static_cast<uint64>(N));
			int32 i = 0;
			for (; i + 8 <= N; i += 8)
			{
				uint64 Word = 0;
				for (int32 k = 0; k < 8; ++k)
				{
					Word |= static_cast<uint64>(Data[i + k]) << (k * 8);
				}
				H = Combine(H, Word);
			}
			if (i < N)
			{
				uint64 Word = 0;
				for (int32 k = 0; i + k < N; ++k)
				{
					Word |= static_cast<uint64>(Data[i + k]) << (k * 8);
				}
				H = Combine(H, Word);
			}
			return H;
		}

		[[nodiscard]] FORCEINLINE uint64 Bytes(uint64 H, const TArray<uint8>& Data)
		{
			return BytesRaw(H, Data.GetData(), Data.Num());
		}

		// Fold a STRING deterministically. For test/tooling digests that are built as
		// text (sorted, fixed-precision rows) and need pinning as a uint64 golden --
		// CompareGoldenChecksum takes a uint64, and a run-vs-run FString comparison
		// cannot be promoted to a golden without this step.
		//
		// Converted to UTF-8 FIRST, deliberately: FString stores TCHAR, which is
		// UTF-16 on Windows and can be UTF-32 elsewhere, so folding the code units
		// directly would give the SAME text different digests per platform -- the one
		// property a checked-in golden must never have. Length-prefixed via BytesRaw
		// (shared with Bytes, so the framing cannot drift between the two).
		//
		// NOT for authoritative shard payloads: those go through Bytes() on the real
		// SerializeShardState output, per the DERIVED CHECKSUM contract above. Hashing
		// a formatted string would hash a LOSSY rendering of state (%.4f discards
		// precision), which is the trusted-false-negative direction that contract
		// forbids.
		[[nodiscard]] FORCEINLINE uint64 Str(uint64 H, const FString& Value)
		{
			FTCHARToUTF8 Utf8(*Value);
			return BytesRaw(H, reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		}
	}
}
