// Copyright GALAXY_PROJECT. All Rights Reserved.
#pragma once

// ============================================================================
// Process-stable, content-derived name hashing for PERSISTED identity.
//
// WHY THIS EXISTS -- GetTypeHash(FName) is the wrong primitive for anything that
// reaches disk, and the reason is invisible at the call site: it resolves to
// GetTypeHash(FNameEntryId) -> FNameEntryHandle{Block, Offset}, the name pool's
// ALLOCATION SLOT, i.e. WHERE the string happened to land in this process's name
// table. The engine says so itself: FNameEntryId::ToUnstableInt() is commented
// "Get process specific integer", and operator<<(FArchive&, FNameEntryId&)
// serializes a value it names UnstableInt and remaps on load. An id minted from
// it is stable WITHIN a process and not guaranteed across runs -- which shipped
// here as authored-equipment instance ids that keyed PERSISTED ammo fills
// (save v85 is the flag day; see .github/assistant-context/history/save-system.history.md).
//
// StrCrc32 by contrast folds the CHARACTERS, so the value is a pure function of
// the text on every run and every platform this project targets.
//
// CASE IS NORMALIZED, deliberately: FName equality is case-insensitive, so two
// FNames that compare equal MUST hash equal here -- ToString() preserves the
// authored casing of the FIRST registration this process saw, which would
// otherwise smuggle the same process-order dependence back in through the door.
//
// THE CROSS-PROCESS CONTRACT IS PINNED BY GOLDEN SNAPSHOTS
// (StableNameHashTests.cpp): the stored constants were computed in one process
// and every later test run is a different process, so a primitive change breaks
// them loudly. No same-process round-trip can see this property -- do not
// weaken those tests into computed-vs-computed comparisons.
// ============================================================================

#include "CoreMinimal.h"

namespace GalaxyStableHash
{
	/**
	 * Content-derived, case-normalized, process-stable hash of a name's text.
	 * Use for any hash that is PERSISTED or otherwise compared across processes;
	 * keep GetTypeHash(FName) for in-process containers, where it is faster.
	 */
	[[nodiscard]] inline uint32 StableNameHash(FName Name)
	{
		return FCrc::StrCrc32(*Name.ToString().ToLower());
	}

	/**
	 * Slot-scoped card identity hash: folds the SLOT and the CARD together so two
	 * slots holding the SAME card stay distinguishable. The shared core of the two
	 * authored-equipment id mints (UnitLoadoutIngest::MakeAuthoredInstanceId and
	 * OutpostDerivedStateProjector::BuildEquippedSlotsFromLoadout), which apply
	 * their own sign/range treatment on top -- deliberately opposite treatments,
	 * documented at their sites; do not converge them here.
	 */
	[[nodiscard]] inline uint32 StableSlotScopedCardHash(FName SlotName, FName CardName)
	{
		return HashCombine(StableNameHash(SlotName), StableNameHash(CardName));
	}
}
