#!/usr/bin/env python3
"""
Tie-Break Determinism Audit -- the "who wins a tie" hazard.

An argmax is only nondeterministic when its SOURCE is unordered. First-wins over a
sorted TArray is perfectly deterministic and is NOT flagged here. The defect is the
pair: an unordered iteration feeding a comparison that has no tie-break, so on equal
scores the winner is whatever the storage layout happened to yield first.

  A. ARGMAX OVER AN UNORDERED SOURCE
       auto View = Registry.view<CResourceZone>();
       for (const entt::entity E : View) {
           if (Zone.MeanRichness > BestRichness) { BestRichness = ...; BestZone = E; }
       }
     Equal-richness zones resolve by EnTT storage order.

  B. UNSTABLE SORT ON A SINGLE KEY
       Members.Sort([](const FMemberInfo& A, const FMemberInfo& B)
           { return A.EffectiveSpeed > B.EffectiveSpeed; });
     TArray::Sort is introsort, which is unstable. Same-type units have identical speeds,
     so ties are the norm, and the tied order falls through to however the array was
     collected. The comparator is present, so the site does not read as missing anything.

  C. A TIE-BREAK THAT IS PRESENT BUT UNREACHABLE. See the section below.

THE CORRECT IDIOM (compare exactly, then on a stable key):
       const bool bTake = (Best == None)
           || (Score > BestScore)
           || (!(Score < BestScore) && StableKey < BestStableKey);
  That is a total order, so the winner cannot depend on visit order, and it is the
  order GalaxyDeterminism::SortByScoreDescThenId uses. `!(Score < BestScore)` is an
  exact tie test with no float `==`.

  A TOLERANCE IS NOT A TIE-BREAK:
       (Score > BestScore + TOL) || (IsNearlyEqual(Score, BestScore, TOL) && Key < BestKey)
  "Within TOL" is not transitive. With TOL 1e-4, scores 1.0, 1.00006, 1.00012 and keys
  1, 2, 3, visiting them in that order picks the third and visiting them in reverse
  picks the first. The form is safe only over an ordered source, or when no three
  candidates chain within TOL. Shapes A and C accept it.

  Passing the tolerance to only one side is worse. IsNearlyEqual's third parameter
  defaults to UE_SMALL_NUMBER (1e-8), not KINDA_SMALL_NUMBER, so a strict test at 1e-4
  beside a defaulted IsNearlyEqual leaves a dead band: a pair separated by anything
  between 1e-8 and 1e-4 is too close to win outright and too far apart to count as
  tied, satisfies neither branch, and whoever arrived first survives.

Suppression: `// tiebreak-safe: <why>`. The reason is required.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

_scripts_dir = str(Path(__file__).resolve().parent.parent)
if _scripts_dir not in sys.path:
    sys.path.insert(0, _scripts_dir)
from audit_common import AuditResult, ROOT, SOURCE_ROOT
from audit_knobs import get_knob  # centralized knob registry
from typing import Any

from audit_registry import AuditDefinition, CADENCE_DEFAULT_HOURS

AUDIT = AuditDefinition("tiebreak-determinism", "Argmax over an unordered source, or an unstable sort on a measurement, with no tie-break", cadence_hours=CADENCE_DEFAULT_HOURS, cadence_reason="A tie-break finding is cheap to patch once seen, and the shipping-build pass runs --tier full, which bypasses the cadence and runs this live.")

# ============================================================================
# Constants
# ============================================================================

_SUPPRESSION_COMMENT = "tiebreak-safe"

# The "best so far" holder an argmax latches into. Deliberately a NAME convention:
# this tree writes BestScore/ClosestDist/HighestTier consistently, and a name-based
# match keeps the rule readable where a dataflow analysis would not be.
_BEST_HOLDER = r'(?:Best|Closest|Nearest|Highest|Lowest|Strongest|Weakest|Cheapest|Top|Max|Min)\w*'

# if (Something > BestScore)  /  if (Dist < ClosestDist)
_ARGMAX_RE = re.compile(
    r'\bif\s*\(\s*(?P<lhs>[A-Za-z_][\w\.\[\]]*(?:->\w+)*)\s*(?P<op>[<>])\s*(?P<best>' + _BEST_HOLDER + r')\s*\)'
)

# The latch body must record a WINNER, not just a running scalar.
#
#   if (Order->PriceLimit > Best) { Best = Order->PriceLimit; }      <- running max
#   if (Zone.Richness > BestRichness) { BestRichness = ...; Best = E; }  <- ARGMAX
#
# Only the second has a tie problem: the first returns the same number whatever order
# it visits, because it never records WHICH element won. So the test is whether the
# body assigns anything besides the holder named in the condition.
_ASSIGNMENT_RE = re.compile(r'\b(?P<name>[A-Za-z_]\w*)\s*=(?!=)')

# Unordered iteration sources.
#
# These are DERIVED FROM DECLARATIONS, never from a name. A first draft matched
# `\w*(?:View|Group)\b` and `\w*(?:Map|Set)\b` and promptly flagged
# `for (int32 Idx : Group)` over a TArray<int32> and
# `for (const FThreatSource& S : FaceMap.ThreatSources)` -- both perfectly ordered.
# A name heuristic cannot tell a TMap from a struct that happens to end in "Map",
# so the container has to be looked up in what the file actually declares.
_EACH_RE = re.compile(r'\.each\s*\(')
_ENTT_ENTITY_LOOP_RE = re.compile(r'for\s*\([^:]*\bentt::entity\b[^:]*:')
_RANGE_FOR_RE = re.compile(r'for\s*\([^:;]*:\s*(?P<container>[^)]+?)\s*\)')

# Declarations that make a name an unordered container, for THIS file only.
_UNORDERED_DECL_RES: list[tuple[re.Pattern, str]] = [
    (re.compile(r'\bT(?:Multi)?Map\s*<.*?>\s*&?\s*(?P<name>\w+)\s*[;={,)]'), "TMap iteration"),
    (re.compile(r'\bTSet\s*<.*?>\s*&?\s*(?P<name>\w+)\s*[;={,)]'), "TSet iteration"),
    (re.compile(r'\bauto\s*&?\s*(?P<name>\w+)\s*=\s*[\w>.:\-]*\.\s*(?:view|group)\s*<'),
     "EnTT view iteration"),
]


def _unordered_names(lines: list[str]) -> dict[str, str]:
    """Names this file declares as an unordered container -> what kind."""
    found: dict[str, str] = {}
    for line in lines:
        for pattern, label in _UNORDERED_DECL_RES:
            for match in pattern.finditer(line):
                found[match.group("name")] = label
    return found

# Anything here means the site already decides ties on purpose.
_TIEBREAK_TOKENS = (
    "IsNearlyEqual", "KINDA_SMALL_NUMBER", "SMALL_NUMBER", "UE_KINDA_SMALL_NUMBER",
    "to_integral", "StableId", "GetStableId", "TieBreak", "Tiebreak", "tie-break",
    "tiebreak", "TieBreaker", "DeterministicTie",
)

# A sort comparator that reads exactly one key and can therefore tie.
_SORT_CALL_RE = re.compile(r'\.\s*(?:Sort|StableSort|HeapSort)\s*\(')
_STABLE_SORT_RE = re.compile(r'\.\s*StableSort\s*\(')
_SINGLE_KEY_RETURN_RE = re.compile(
    r'return\s+(?P<a>[A-Za-z_][\w\.\[\]]*(?:->\w+)*)\s*[<>]\s*(?P<b>[A-Za-z_][\w\.\[\]]*(?:->\w+)*)\s*;'
)

# A sort on a UNIQUE key cannot tie, so it is already a total order -- and sorting by
# stable id is the very fix this audit exists to encourage. Flagging it would tell
# people their correct code is broken, so identifier-shaped keys are silent.
_UNIQUE_KEY_SUFFIXES = (
    "id", "ids", "key", "keys", "index", "indices", "idx", "ordinal", "handle",
    "name", "path", "guid", "uuid", "slot", "tick", "number", "seed", "hash",
)

# ...whereas a MEASUREMENT ties routinely. This is the CargoHold shape: same-type
# units share an EffectiveSpeed exactly, so the tie is the common case.
_MEASUREMENT_HINTS = (
    "score", "value", "dist", "distance", "speed", "weight", "priority", "richness",
    "freq", "frequency", "count", "mass", "damage", "threat", "urgency", "fill",
    "utilization", "cost", "ratio", "fraction", "percent", "quality", "strength",
    "range", "angle", "dot", "area", "volume", "density", "rate", "level", "power",
)


def _sort_key_is_tieable(expr_a: str, expr_b: str) -> bool:
    """True when the compared expression looks like a MEASUREMENT rather than an id.

    `return A.SquadId < B.SquadId`   -> unique key, silent
    `return A > B` (bare params)     -> the elements ARE the key, silent
    `return Freq[A] > Freq[B]`       -> a tally, tieable
    `return A.EffectiveSpeed > B...` -> a measurement, tieable
    """
    def leaf(expr: str) -> str:
        cleaned = re.sub(r'\[[^\]]*\]', '', expr)          # drop subscripts
        parts = re.split(r'\.|->', cleaned)
        return parts[-1].strip().lower() if parts else ""

    # Bare single-letter params compared directly: the values themselves are the key.
    if len(expr_a.strip()) <= 2 and len(expr_b.strip()) <= 2 and "[" not in expr_a:
        return False

    key_a, key_b = leaf(expr_a), leaf(expr_b)
    container_a = expr_a.split("[")[0].strip().lower() if "[" in expr_a else ""

    for key in (key_a, key_b):
        if key and any(key.endswith(suffix) for suffix in _UNIQUE_KEY_SUFFIXES):
            return False

    haystack = f"{key_a} {key_b} {container_a}"
    return any(hint in haystack for hint in _MEASUREMENT_HINTS)

# Diagnostics/UI/test surfaces: iteration order cannot reach authoritative state.
_DIAGNOSTIC_PATH_FRAGMENTS = [
    "Bridges", "DeveloperTools", "UIClasses", "Tests",
]


@dataclass
class Finding:
    file: str
    line: int
    code: str
    severity: str  # "RISK", "NEEDS_REVIEW", "INFO"
    reason: str

    def format(self) -> str:
        return f"[{self.severity}] {self.file}:{self.line}: {self.reason}"

    def to_dict(self) -> dict[str, Any]:
        return {
            "file": self.file,
            "line": self.line,
            "severity": self.severity,
            "reason": self.reason,
            "code": self.code.strip(),
        }


def _is_diagnostic_path(filepath: Path) -> bool:
    rel = str(filepath.relative_to(SOURCE_ROOT))
    return any(frag in rel for frag in _DIAGNOSTIC_PATH_FRAGMENTS)


def _braced_body(lines: list[str], idx: int, max_lines: int = 40) -> str:
    """The `if` body starting at idx, bounded by BRACES rather than a line count.

    A fixed window reads past the closing brace and picks up assignments that are not
    in the latch at all -- which is exactly how a running min
    (`if (Dist < NearestFriendly) { NearestFriendly = Dist; }` followed six lines later
    by `Context.DistanceToNearestFriendly = NearestFriendly;`) got mistaken for an
    argmax. Single-statement bodies with no braces fall back to the next line.
    """
    depth = 0
    seen_open = False
    collected: list[str] = []
    for probe in range(idx, min(len(lines), idx + max_lines)):
        line = lines[probe]
        collected.append(line)
        depth += line.count("{") - line.count("}")
        if "{" in line:
            seen_open = True
        if seen_open and depth <= 0:
            return "\n".join(collected)
        if not seen_open and probe > idx and line.strip().endswith(";"):
            return "\n".join(collected)   # braceless single statement
    return "\n".join(collected)


def _suppressed(lines: list[str], idx: int, window: int) -> bool:
    """True if a tiebreak-safe marker governs this site (own line or just above)."""
    for probe in range(max(0, idx - window), min(len(lines), idx + 2)):
        if _SUPPRESSION_COMMENT in lines[probe]:
            return True
    return False


def _enclosing_unordered_source(
    lines: list[str], idx: int, lookback: int, unordered: dict[str, str]
) -> str | None:
    """Name the unordered iteration this line sits inside, if any.

    Walks back to the NEAREST enclosing loop and classifies only that one -- the latch
    belongs to the innermost loop, so an ordered inner loop makes the site safe even
    when an unordered outer loop exists. Anything the file does not declare as a TMap,
    TSet or EnTT view is treated as ordered: silence is the conservative default.
    """
    # Walk back tracking brace depth so only loops that actually ENCLOSE the latch are
    # considered. Without this, a sibling loop that already closed counts as the
    # source -- which is how an argmax nested in two counted loops got attributed to a
    # `for (int32 VolId : AvoidSet)` several lines above that had long since ended
    # (PolymassRegistry::GenerateWaypoints).
    depth = 0
    skip_header = False
    for probe in range(idx, max(-1, idx - lookback), -1):
        line = lines[probe]
        if probe < idx:
            # Count closers FIRST: a line is in our enclosing scope only if every block
            # opened below it has already been closed above the latch. Its own '{' is
            # applied after the check, so the header of a sibling block that already
            # ended is correctly skipped rather than claimed as the source.
            depth += line.count("}")
            in_scope = depth == 0
            opened = line.count("{")
            depth = max(0, depth - opened)
            if not in_scope:
                # This tree is Allman-braced, so a closed block's HEADER sits on the
                # line above its lone '{' and would otherwise be read at depth 0 as if
                # it enclosed us (PolymassRegistry's `for (int32 VolId : AvoidSet)`).
                if opened and line.strip() == "{":
                    skip_header = True
                continue
            if skip_header:
                skip_header = False
                if line.strip():
                    continue

        if _EACH_RE.search(line):
            return "EnTT view .each()"
        if _ENTT_ENTITY_LOOP_RE.search(line):
            return "EnTT view iteration"

        match = _RANGE_FOR_RE.search(line)
        if match:
            container = match.group("container").strip().lstrip("*&")
            leaf = re.split(r'\.|->', container)[-1].strip()
            return unordered.get(leaf) or unordered.get(container.split(".")[0].strip())

        # A counted loop is ordered, and it is the innermost one -- stop here.
        if re.search(r'\bfor\s*\(.*;.*;.*\)', line):
            return None
    return None


def _classify_argmax(lines: list[str], idx: int, filepath: Path, lookback: int,
                     unordered: dict[str, str]) -> Finding | None:
    line = lines[idx]
    match = _ARGMAX_RE.search(line)
    if match is None:
        return None

    body = _braced_body(lines, idx)
    assigned = {m.group("name") for m in _ASSIGNMENT_RE.finditer(body)}
    assigned.discard(match.group("best"))
    if not assigned:
        return None  # a running max/min, not an argmax -- order-independent

    context = "\n".join(lines[max(0, idx - 4):min(len(lines), idx + 8)])
    if any(token in context for token in _TIEBREAK_TOKENS):
        return None  # already decides ties deliberately

    source = _enclosing_unordered_source(lines, idx, lookback, unordered)
    if source is None:
        return None  # ordered source -- first-wins is deterministic

    severity = "INFO" if _is_diagnostic_path(filepath) else "RISK"
    return Finding(
        file=str(filepath.relative_to(ROOT)), line=idx + 1, code=line,
        severity=severity,
        reason=(
            f"argmax on '{match.group('best')}' over {source} with no tie-break -- "
            f"equal scores resolve by storage order (break the tie on a stable key, "
            f"e.g. entt::to_integral, or add // tiebreak-safe: <why>)"
        ),
    )


# ============================================================================
# Shape C -- the tie-break that is present but UNREACHABLE
# ============================================================================
#
# Shapes A and B are about a MISSING tie-break. This one is worse, because the
# tie-break is right there in the source and still does not run:
#
#     if (Score > BestScore || (FMath::IsNearlyEqual(Score, BestScore) && Idx < BestIdx))
#
# The bare '>' short-circuits. Among candidates "tied" within IsNearlyEqual's
# tolerance (default UE_SMALL_NUMBER, 1e-8), the stable key is consulted ONLY when the candidate
# arrives an ulp LOW, and is skipped when it arrives an ulp HIGH. Which of the two
# rules applies is decided by the sign of a sub-tolerance difference, i.e. by float
# noise. The fix is the exact compare in this file's header. Carrying the SAME
# tolerance on both sides also makes the two branches mutually exclusive, but it is
# not transitive, so it stays order-dependent over three or more candidates:
#
#     if (Score > BestScore || (!(Score < BestScore) && Idx < BestIdx))
#
# KNOWN LIMIT: the pairing is WITHIN ONE STATEMENT. A condition split across
# separate `const bool` declarations (the MeteorHarvestSystem form) is not paired
# and stays silent. Widening that needs real dataflow; the in-statement form is
# where every instance in this tree lives.

_EXPR = r'[A-Za-z_]\w*(?:(?:\.|->)\w+|\[[^\[\]]*\])*'

_NEARLY_EQUAL_RE = re.compile(
    r'IsNearlyEqual\s*\(\s*(?P<a>' + _EXPR + r')\s*,\s*(?P<b>' + _EXPR + r')\s*[,)]'
)

# `A > B` / `A < B`, optionally with an additive tolerance riding on the right.
# The lookarounds keep '->', '<=', '>=', '<<' and '>>' out of the match.
_STRICT_CMP_RE = re.compile(
    r'(?P<lhs>' + _EXPR + r')\s*(?<![-<>=!])(?P<op>[<>])(?![=<>])\s*'
    r'(?P<rhs>' + _EXPR + r')(?P<tol>\s*[+-]\s*[A-Za-z_0-9][\w:.]*)?'
)


def _norm(expr: str) -> str:
    return re.sub(r'\s+', '', expr)


def _statement_span(lines: list[str], idx: int, max_lines: int) -> tuple[int, int]:
    """The line range of the statement containing idx.

    Backward until a line that clearly ends a statement (';', '{', '}'), forward
    until the parens opened by that statement close again.
    """
    start = idx
    for probe in range(idx - 1, max(-1, idx - max_lines), -1):
        stripped = lines[probe].strip()
        if not stripped or stripped.startswith("//") or stripped.endswith((";", "{", "}")):
            break
        start = probe

    depth = 0
    end = start
    for probe in range(start, min(len(lines), start + max_lines)):
        depth += lines[probe].count("(") - lines[probe].count(")")
        end = probe
        if probe >= idx and depth <= 0:
            break
    return start, end


def _classify_tolerance_gap(lines: list[str], idx: int, filepath: Path,
                            max_span: int) -> Finding | None:
    """Flag an IsNearlyEqual tie-break whose strict partner carries no tolerance."""
    if "IsNearlyEqual" not in lines[idx]:
        return None

    start, end = _statement_span(lines, idx, max_span)
    # One finding per statement: a later IsNearlyEqual in the same condition is
    # covered by the first one's pass over the joined text.
    if any("IsNearlyEqual" in lines[probe] for probe in range(start, idx)):
        return None

    text = "\n".join(lines[start:end + 1])
    strict = [
        (_norm(m.group("lhs")), _norm(m.group("rhs")), m.group("tol"))
        for m in _STRICT_CMP_RE.finditer(text)
    ]

    for match in _NEARLY_EQUAL_RE.finditer(text):
        a, b = _norm(match.group("a")), _norm(match.group("b"))
        partners = [s for s in strict if {s[0], s[1]} == {a, b}]
        if not partners:
            continue  # no strict comparison over this pair -- not this shape
        if any(s[2] for s in partners):
            continue  # the strict side already carries a tolerance

        anchor = start
        for probe in range(start, end + 1):
            hit = next(
                (s for s in _STRICT_CMP_RE.finditer(lines[probe])
                 if {_norm(s.group("lhs")), _norm(s.group("rhs"))} == {a, b}
                 and not s.group("tol")),
                None,
            )
            if hit is not None:
                anchor = probe
                break

        severity = "INFO" if _is_diagnostic_path(filepath) else "RISK"
        return Finding(
            file=str(filepath.relative_to(ROOT)), line=anchor + 1, code=lines[anchor],
            severity=severity,
            reason=(
                f"tie-break on '{match.group('a')}' vs '{match.group('b')}' is UNREACHABLE -- "
                f"the strict comparison carries no tolerance, so it short-circuits and the "
                f"IsNearlyEqual branch only runs when the candidate lands low "
                f"(drop the tolerance: compare exactly, then on the stable key, "
                f"`A > B || (!(A < B) && Key < BestKey)`. A tolerance on both sides "
                f"closes the gap but is not transitive, so it stays order-dependent "
                f"over three or more candidates)"
            ),
        )
    return None


def _classify_sort(lines: list[str], idx: int, filepath: Path) -> Finding | None:
    """Flag an UNSTABLE sort whose comparator reads exactly one key."""
    if not _SORT_CALL_RE.search(lines[idx]):
        return None
    # KNOWN LIMIT: silent on StableSort. It is safe over ordered input, but over unordered input
    # ties keep the collection order, and this scanner cannot tell the two apart.
    if _STABLE_SORT_RE.search(lines[idx]):
        return None

    # Bound the comparator by its BRACES, not by a line count. A fixed window is wrong in
    # both directions: it can stop short of a multi-key comparator's later returns (a false
    # positive), and it can read past the closing brace into the next statement, where an
    # unrelated `return` reads as a composite key (a false negative).
    body = _braced_body(lines, idx)
    if "{" not in body:
        return None  # comparator is not an inline lambda (functor, or a bare Sort())
    if body.count("{") != body.count("}"):
        return None  # body hit the line bound unclosed -- judge nothing we cannot see
    if any(token in body for token in _TIEBREAK_TOKENS):
        return None
    # More than one comparison in the comparator = a composite key, already total.
    matches = _SINGLE_KEY_RETURN_RE.findall(body)
    if len(matches) != 1 or body.count("return") != 1:
        return None
    if not _sort_key_is_tieable(matches[0][0], matches[0][1]):
        return None  # unique key -- cannot tie, so the order is already total

    # NEEDS_REVIEW, not RISK: a measurement CAN tie, but proving it does needs the
    # data. The argmax shape is provable from the code alone; this one is not.
    severity = "INFO" if _is_diagnostic_path(filepath) else "NEEDS_REVIEW"
    return Finding(
        file=str(filepath.relative_to(ROOT)), line=idx + 1, code=lines[idx],
        severity=severity,
        reason=(
            "unstable Sort keyed on a measurement with no tie-break -- equal values keep "
            "whatever order the collection pass produced (add a stable second key, e.g. "
            "GalaxyDeterminism::SortByScoreDescThenId; StableSort keeps that same order)"
        ),
    )


def run_audit(single_file: Path | None = None) -> AuditResult:
    findings: list[Finding] = []
    files_scanned = 0

    lookback = get_knob("TIEBREAK_SOURCE_LOOKBACK_LINES")
    suppression_window = get_knob("TIEBREAK_SUPPRESSION_WINDOW_LINES")
    statement_span = get_knob("TIEBREAK_STATEMENT_SPAN_LINES")

    source_files = list(SOURCE_ROOT.rglob("*.h")) + list(SOURCE_ROOT.rglob("*.cpp"))

    for src_file in source_files:
        if single_file and single_file != src_file and single_file not in src_file.parents:
            continue
        try:
            lines = src_file.read_text(encoding="utf-8", errors="ignore").splitlines()
        except Exception:
            continue

        files_scanned += 1
        unordered = _unordered_names(lines)
        for i in range(len(lines)):
            if lines[i].lstrip().startswith("//"):
                continue
            if _suppressed(lines, i, suppression_window):
                continue
            finding = _classify_argmax(lines, i, src_file, lookback, unordered)
            if finding is None:
                finding = _classify_tolerance_gap(lines, i, src_file, statement_span)
            if finding is None:
                finding = _classify_sort(lines, i, src_file)
            if finding is not None:
                findings.append(finding)

    risks = [f for f in findings if f.severity == "RISK"]
    reviews = [f for f in findings if f.severity == "NEEDS_REVIEW"]
    infos = [f for f in findings if f.severity == "INFO"]

    message = (
        f"Scanned {files_scanned} files; {len(risks)} RISK, {len(reviews)} NEEDS_REVIEW, "
        f"{len(infos)} INFO tie-break dependencies"
    )

    return AuditResult(
        passed=len(risks) == 0,
        errors=[f.format() for f in risks],
        warnings=[f.format() for f in reviews],
        info=[f.format() for f in infos],
        message=message,
        findings=findings,
        scanned=files_scanned,
        scanned_unit="source files",
    )


# ============================================================================
# CLI
# ============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(description="Tie-break determinism audit")
    parser.add_argument("--format", choices=["text", "json"], default="text")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--file", type=str, help="Scan a single file only")
    args = parser.parse_args()

    single = Path(args.file).resolve() if args.file else None
    result = run_audit(single)

    if args.format == "json":
        print(json.dumps(result.to_dict(), indent=2))
    else:
        status = "PASS" if result.passed else "FAIL"
        print(f"\n{'='*60}")
        print(f"Tie-Break Determinism Audit: {status}")
        print(f"{'='*60}")
        print(f"  {result.message}\n")
        if result.errors:
            print("RISK (tie decided by storage order in authoritative code):")
            for e in result.errors:
                print(f"  {e}")
            print()
        # NEEDS_REVIEW is the actionable channel and is always printed: a finding that cannot
        # be enumerated cannot be fixed or closed out.
        if result.warnings:
            print("NEEDS_REVIEW (measurement sorts with no tie-break -- review each):")
            for w in result.warnings:
                print(f"  {w}")
            print()
        if args.verbose and result.info:
            print("INFO (diagnostic/UI/test paths):")
            for i in result.info:
                print(f"  {i}")
            print()

    return 0 if result.passed else 1


if __name__ == "__main__":
    sys.exit(main())
