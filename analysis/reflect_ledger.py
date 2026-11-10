#!/usr/bin/env python3
"""Finding ledger: accumulate and age analysis findings across builds.

Each analysis tool grades one run and emits its findings. This ledger is the memory between runs, so
the loop can tell a new miss from one that has persisted for ten builds, and can confirm that a
committed fix actually closed a miss. It is a fingerprint-keyed JSON store with a lifecycle: a finding
accumulates first_seen / last_seen / seen_count and moves through
open -> proposed -> committed -> resolved (-> reopened) across builds.

Reconciliation is scope-aware: a prior entry resolves only when the run actually covered its subject.
Absence under a partial run means "not checked", never "fixed".

Scope of a run = the set of SUBJECTS it graded (a subject = the part of a Finding location before
`::`, e.g. `fitness_simulation/<scenario>` or `live_combat/<combat_id>`). A digest names the subjects
it covered even when a subject graded CLEAN, so a subject that went from failing to clean still
resolves. Subjects outside the run are left untouched.

A run also vouches only for what its own tool examined. Each tool emits its own rules, so a
counterfactual digest naming a scenario says nothing about that scenario's `reflect-metric-miss`
entries, and a report of one variant says nothing about the base scenario or its other variants.
Absence closes an entry only when the same tool re-graded the same expansion (`rule_family`,
`coverage_key`). One exception: a counterfactual attribution explains a miss, so a grading run that
no longer reports the miss has answered the attribution too.

Lifecycle (status): open (active, no proposal) | proposed (raised by reflect_propose) |
committed (a fix commit referenced it) | resolved (gone on a run that covered its subject) |
reopened (resolved, retired or committed, then came back) | retired (the subject itself left the corpus).

RETIRED is not `resolved`. When a scenario is renamed or deleted, no future run can cover it, so
`--retire-missing` closes its findings explicitly: nothing was verified fixed, the question simply
stopped existing.

The last stdout line is a machine-readable `LEDGER_RESULT:` summary.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import date
from pathlib import Path

_scripts_dir = str(Path(__file__).resolve().parent.parent)
if _scripts_dir not in sys.path:
    sys.path.insert(0, _scripts_dir)

import verdict  # noqa: E402
from audit_common import ROOT  # noqa: E402
from mutation_safety import safe_write_text  # noqa: E402

LEDGER_PATH = ROOT / ".github" / "reflect" / "ledger.json"
SCENARIOS_DIR = ROOT / "Data" / "FitnessSimulationScenarios"

# Subject namespace prefixes. Only the fitness namespace is backed by an on-disk corpus file, so it is
# the only one whose subjects can be checked for existence (a live_combat subject is a concluded
# combat id -- it has no scenario file and its absence from the corpus means nothing).
FITNESS_SUBJECT_PREFIX = "fitness_simulation/"

# Lifecycle vocabulary.
ST_OPEN = "open"
ST_PROPOSED = "proposed"
ST_COMMITTED = "committed"
ST_RESOLVED = "resolved"
ST_REOPENED = "reopened"
ST_RETIRED = "retired"
# Statuses that still count as "live" (an active obligation the loop should act on).
LIVE_STATUSES = frozenset({ST_OPEN, ST_PROPOSED, ST_COMMITTED, ST_REOPENED})
# Terminal statuses a returning finding reopens FROM (it was closed, then came back).
CLOSED_STATUSES = frozenset({ST_RESOLVED, ST_RETIRED})
# A returning finding reopens from these: it was closed, or a fix was committed for it and did not hold.
REOPENABLE_STATUSES = CLOSED_STATUSES | {ST_COMMITTED}

# The tool whose runs re-examine a rule. Only that tool's digest can report the rule gone. A rule
# with no listed prefix is a reflect_match grade.
FAMILY_GRADE = "grade"
FAMILY_COUNTERFACTUAL = "counterfactual"
FAMILY_LIVE = "live"
_RULE_FAMILY_PREFIXES = (
    ("reflect-cf-", FAMILY_COUNTERFACTUAL),
    ("reflect-live-", FAMILY_LIVE),
    ("reflect-coverage-", "coverage"),
    ("reflect-ai-", "ai"),
)
# reflect_match rules keyed to one variant of a scenario: the first identity field is its expanded id.
VARIANT_RULES = frozenset({"reflect-variant-metric-miss", "reflect-variant-intent"})
VARIANT_SCOPE_SEP = "@"
# Counterfactual rules that explain the reflect-metric-miss at the same location.
MISS_EXPLANATION_RULES = frozenset({"reflect-cf-attributed", "reflect-cf-unattributed"})


def subject_of(location: str) -> str:
    """The graded subject a Finding location belongs to (location before the `::` anchor)."""
    return (location or "").split("::", 1)[0]


def rule_family(rule_id: str) -> str:
    """The tool whose runs can re-examine this rule."""
    for prefix, family in _RULE_FAMILY_PREFIXES:
        if rule_id.startswith(prefix):
            return family
    return FAMILY_GRADE


def coverage_key(entry: dict) -> str:
    """What a run must have graded before this entry's absence means anything: its subject, or, for
    a variant-scoped rule, that one variant of its subject."""
    subject = entry.get("subject", "")
    if entry.get("rule_id") not in VARIANT_RULES:
        return subject
    identity = entry.get("fingerprint", "").rsplit("::", 1)[-1]
    return f"{subject}{VARIANT_SCOPE_SEP}{identity.split('|', 1)[0]}"


def _graded_key(scenario_id: str, expanded_id: str | None) -> str:
    """The coverage a graded report provides: the base subject, or one variant of it."""
    subject = f"{FITNESS_SUBJECT_PREFIX}{scenario_id}"
    if expanded_id and expanded_id != scenario_id:
        return f"{subject}{VARIANT_SCOPE_SEP}{expanded_id}"
    return subject


# ---------------------------------------------------------------------------
# Digest -> (findings, covered subjects). Handles the three reflect digest shapes; the covered set
# includes subjects that graded CLEAN (so failing->clean resolves), not just subjects with findings.
# ---------------------------------------------------------------------------

def findings_from_digest(digest: dict) -> tuple[list[dict], set[str]]:
    findings: list[dict] = []
    covered: set[str] = set()

    # reflect_match: top-level "digests": [{scenario_id, expanded_scenario_id, findings:[...]}]. A
    # report covers only its own expansion, and its findings carry the base subject whichever that
    # was, so this shape takes no coverage from the findings themselves.
    if isinstance(digest.get("digests"), list):
        for d in digest["digests"]:
            sid = d.get("scenario_id")
            if sid:
                covered.add(_graded_key(sid, d.get("expanded_scenario_id")))
            findings.extend(d.get("findings", []) or [])
        return findings, covered
    # reflect_counterfactual: flat {scenario_id, findings:[...]}.
    if digest.get("scenario_id") and "combat_id" not in digest:
        covered.add(f"fitness_simulation/{digest['scenario_id']}")
        findings.extend(digest.get("findings", []) or [])
    # reflect_live: {combat_id, findings:[...]}.
    elif digest.get("combat_id"):
        covered.add(f"live_combat/{digest['combat_id']}")
        findings.extend(digest.get("findings", []) or [])
    else:
        findings.extend(digest.get("findings", []) or [])

    # Always include the subject of every finding present (covers tools whose top-level shape we did
    # not special-case, and is a no-op for the ones above).
    for f in findings:
        covered.add(subject_of(f.get("location", "")))
    covered.discard("")
    return findings, covered


def digest_families(digest: dict, findings: list[dict]) -> frozenset[str]:
    """The tools whose findings this digest can vouch for. Pairs with `findings_from_digest`."""
    if isinstance(digest.get("digests"), list):
        return frozenset({FAMILY_GRADE})
    if digest.get("scenario_id") and "combat_id" not in digest:
        return frozenset({FAMILY_COUNTERFACTUAL})
    if digest.get("combat_id"):
        return frozenset({FAMILY_LIVE})
    return frozenset(rule_family(f.get("rule_id", "")) for f in findings)


# ---------------------------------------------------------------------------
# Pure reconciliation (unit-tested; no I/O)
# ---------------------------------------------------------------------------

def ingest(entries: dict[str, dict], findings: list[dict], covered_subjects: set[str],
           today: str | None = None,
           families: frozenset[str] = frozenset({FAMILY_GRADE})) -> tuple[dict[str, dict], dict[str, int]]:
    """Fold a run's findings into the ledger. Returns (new_entries, summary counts).

    `families` names the tools this run speaks for (`digest_families`); the default is a grading run.

    - present finding, new fingerprint -> open (first_seen=today).
    - present finding, known fingerprint -> bump last_seen/seen_count; if it was resolved, retired or
      committed -> reopened (a committed one means the fix did not hold).
    - known entry ABSENT this run whose coverage key IS in covered_subjects and whose rule this run's
      tool examines -> resolved (the run checked it and it is gone -- this is the 'verified' closure
      for proposed/committed too).
    - any other known entry -> untouched (not checked)."""
    stamp = today or date.today().isoformat()
    out = {fp: dict(e) for fp, e in entries.items()}  # copy -- pure
    current: dict[str, dict] = {f["fingerprint"]: f for f in findings if f.get("fingerprint")}
    summary = {"new": 0, "reopened": 0, "resolved": 0, "still_live": 0, "total": 0}

    for fp, f in current.items():
        loc = f.get("location", "")
        ev = f.get("evidence") or {}
        cfg = ev.get("config")
        knob = ev.get("knob")  # counterfactual attribution (reflect-cf-attributed) -> the responsible knob
        if fp in out:
            e = out[fp]
            e["last_seen"] = stamp
            e["seen_count"] = int(e.get("seen_count", 0)) + 1
            e["severity"] = f.get("severity", e.get("severity", "review"))
            e["sample"] = f.get("message", e.get("sample", ""))
            # Accumulate the SET of configs this finding has appeared under -- cross-config breadth is
            # the meaningfulness signal that refines as the corpus grows (real bug = many configs).
            if cfg:
                cfgs = set(e.get("configs", []) or [])
                cfgs.add(cfg)
                e["configs"] = sorted(cfgs)
            if knob:
                ks = set(e.get("knobs", []) or [])
                ks.add(knob)
                e["knobs"] = sorted(ks)
            if e.get("status") in REOPENABLE_STATUSES:
                e["status"] = ST_REOPENED
                e.setdefault("history", []).append({"date": stamp, "event": ST_REOPENED})
                summary["reopened"] += 1
        else:
            out[fp] = {
                "fingerprint": fp,
                "rule_id": f.get("rule_id", ""),
                "location": loc,
                "subject": subject_of(loc),
                "severity": f.get("severity", "review"),
                "status": ST_OPEN,
                "first_seen": stamp,
                "last_seen": stamp,
                "seen_count": 1,
                "configs": [cfg] if cfg else [],
                "knobs": [knob] if knob else [],
                "sample": f.get("message", ""),
                "proposal_id": None,
                "history": [{"date": stamp, "event": "first_seen"}],
            }
            summary["new"] += 1

    # Resolve entries the run re-examined and no longer reports.
    still_missed = {f.get("location") for f in current.values() if f.get("rule_id") == "reflect-metric-miss"}
    for fp, e in out.items():
        if fp in current:
            continue
        if e.get("status") in CLOSED_STATUSES:
            continue
        if coverage_key(e) not in covered_subjects:
            continue
        rule_id = e.get("rule_id", "")
        explains_gone_miss = (FAMILY_GRADE in families and rule_id in MISS_EXPLANATION_RULES
                              and e.get("location") not in still_missed)
        if rule_family(rule_id) in families or explains_gone_miss:
            e["status"] = ST_RESOLVED
            e.setdefault("history", []).append({"date": stamp, "event": ST_RESOLVED})
            summary["resolved"] += 1

    summary["still_live"] = sum(1 for e in out.values() if e.get("status") in LIVE_STATUSES)
    summary["total"] = len(out)
    return out, summary


def retire_missing_subjects(entries: dict[str, dict], live_subjects: set[str],
                            today: str | None = None) -> tuple[dict[str, dict], list[dict]]:
    """Retire live entries whose graded SUBJECT no longer exists in the corpus. Pure.

    This is the only closure path for a RENAMED or DELETED scenario. `ingest` is scope-aware by
    design -- it resolves an entry only when the run actually covered its subject -- so a finding
    whose subject left the corpus can never be reached again and would stay open forever.

    NOT the same as `resolved`: resolved means a run re-graded the subject and the finding was gone
    (a fix verified). Retired means the question stopped existing and was never answered. Keeping the
    two distinct is the point -- collapsing them would let a rename masquerade as a fix.

    Only `fitness_simulation/*` subjects are judged. A `live_combat/*` subject is a concluded combat
    id with no corpus file, so its absence from `live_subjects` carries no information.

    Returns (new_entries, retired) where `retired` lists the affected entries for reporting."""
    stamp = today or date.today().isoformat()
    out = {fp: dict(e) for fp, e in entries.items()}  # copy -- pure
    retired: list[dict] = []
    for e in out.values():
        if e.get("status") not in LIVE_STATUSES:
            continue
        subject = e.get("subject", "")
        if not subject.startswith(FITNESS_SUBJECT_PREFIX) or subject in live_subjects:
            continue
        e["status"] = ST_RETIRED
        e.setdefault("history", []).append({"date": stamp, "event": ST_RETIRED})
        retired.append(e)
    return out, retired


def live_fitness_subjects(scenarios_dir: Path) -> set[str]:
    """The set of fitness subjects that still have an authored scenario file on disk."""
    return {f"{FITNESS_SUBJECT_PREFIX}{p.stem}" for p in sorted(scenarios_dir.glob("*.json"))}


def health_summary(entries: dict[str, dict], proposals: dict[str, dict] | None = None) -> dict:
    """Lifecycle health of the loop -- pure. Surfaces status counts, REGRESSIONS (reopened), the
    oldest still-open findings, and (if given) outstanding proposals by gate."""
    from collections import Counter
    statuses = Counter(e.get("status", "?") for e in entries.values())
    reopened = sorted(
        ({"fingerprint": fp, "rule_id": e.get("rule_id", ""), "subject": e.get("subject", ""),
          "seen_count": e.get("seen_count", 0), "sample": e.get("sample", "")}
         for fp, e in entries.items() if e.get("status") == ST_REOPENED),
        key=lambda x: -int(x["seen_count"]))
    open_live = [e for e in entries.values() if e.get("status") in (ST_OPEN, ST_REOPENED)]
    oldest_open = sorted(
        ({"subject": e.get("subject", ""), "rule_id": e.get("rule_id", ""),
          "first_seen": e.get("first_seen", ""), "seen_count": e.get("seen_count", 0)}
         for e in open_live if e.get("rule_id") == "reflect-metric-miss"),
        key=lambda x: (x["first_seen"], -int(x["seen_count"])))[:5]
    out: dict = {
        "total": len(entries),
        "statuses": dict(statuses),
        "regressions": reopened,
        "oldest_open_balance": oldest_open,
    }
    if proposals is not None:
        out["proposals_by_gate"] = dict(Counter(
            p.get("gate", "?") for p in proposals.values() if p.get("status") == "raised"))
    return out


def set_status(entries: dict[str, dict], fingerprints: list[str], status: str,
               today: str | None = None, proposal_id: str | None = None) -> int:
    """Transition the given fingerprints to `status` (used by reflect_propose). Returns count."""
    stamp = today or date.today().isoformat()
    n = 0
    for fp in fingerprints:
        e = entries.get(fp)
        if e is None:
            continue
        e["status"] = status
        if proposal_id is not None:
            e["proposal_id"] = proposal_id
        e.setdefault("history", []).append({"date": stamp, "event": status})
        n += 1
    return n


# ---------------------------------------------------------------------------
# IO
# ---------------------------------------------------------------------------

def load_ledger(path: Path) -> dict[str, dict]:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return {str(k): v for k, v in (data.get("entries", {}) or {}).items()}
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[reflect-ledger] WARN: could not read {path}: {exc}", file=sys.stderr)
        return {}


def save_ledger(path: Path, entries: dict[str, dict], today: str | None = None) -> None:
    stamp = today or date.today().isoformat()
    payload = {
        "_comment": "Reflective-loop Finding ledger (reflect_ledger.py). Fingerprint-keyed lifecycle "
                    "store; entries persist across builds. NOT an audit baseline -- reflect findings "
                    "are tool-scoped. Sorted by fingerprint for a stable diff.",
        "generated": stamp,
        "count": len(entries),
        "entries": {fp: entries[fp] for fp in sorted(entries)},
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    old = path.read_text(encoding="utf-8") if path.exists() else ""
    safe_write_text(path, old, json.dumps(payload, indent=2) + "\n", write=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ingest", help="path to a reflect digest JSON (--json output) to fold in")
    parser.add_argument("--list", nargs="?", const="all", default=None,
                        help="list the ledger (optionally filter to a status: open/proposed/resolved/...)")
    parser.add_argument("--health", action="store_true",
                        help="print loop-health: status counts, regressions (reopened), oldest-open")
    parser.add_argument("--retire-missing", action="store_true",
                        help="retire live findings whose scenario no longer exists (renamed/deleted "
                             "subject -- no future run can ever cover it). Not the same as resolved.")
    parser.add_argument("--scenarios-dir", default=str(SCENARIOS_DIR),
                        help="scenario corpus consulted by --retire-missing")
    parser.add_argument("--ledger", default=str(LEDGER_PATH))
    parser.add_argument("--dry-run", action="store_true", help="preview the ingest; do not write the ledger")
    parser.add_argument("--json", help="write the resulting ledger (or list) digest to this path")
    args = parser.parse_args(argv)

    ledger_path = Path(args.ledger)
    entries = load_ledger(ledger_path)

    if args.health:
        prop_path = ledger_path.parent / "proposals.json"
        proposals = None
        if prop_path.exists():
            try:
                proposals = {str(k): v for k, v in
                             (json.loads(prop_path.read_text(encoding="utf-8")).get("proposals", {}) or {}).items()}
            except (OSError, json.JSONDecodeError):
                proposals = None
        h = health_summary(entries, proposals)
        print(f"[reflect-ledger] LOOP HEALTH -- {h['total']} findings: " +
              ", ".join(f"{k}={v}" for k, v in sorted(h["statuses"].items())))
        if h["regressions"]:
            print(f"  REGRESSIONS (reopened -- a resolved finding came back): {len(h['regressions'])}")
            for r in h["regressions"][:10]:
                print(f"    x{r['seen_count']:<3} {r['rule_id']} {r['subject']} :: {r['sample']}")
        else:
            print("  regressions: none")
        if h["oldest_open_balance"]:
            print("  oldest open balance misses (first_seen):")
            for o in h["oldest_open_balance"]:
                print(f"    {o['first_seen']}  x{o['seen_count']:<3} {o['subject']}")
        if h.get("proposals_by_gate"):
            print("  outstanding proposals by gate: " +
                  ", ".join(f"{k}={v}" for k, v in sorted(h["proposals_by_gate"].items())))
        if args.json:
            Path(args.json).write_text(json.dumps(h, indent=2), encoding="utf-8", newline="\n")
        # Every mode of this tool shares one denominator -- the ledger it read. An empty
        # or unreadable ledger otherwise reports "regressions: none" in perfect health.
        return verdict.emit(
            "LEDGER_RESULT",
            passed=not h["regressions"],
            scope=h["total"],
            scope_unit="ledger-findings",
            fields={"mode": "health", "total": h["total"],
                    "regressions": len(h["regressions"])},
        )

    if args.retire_missing:
        live = live_fitness_subjects(Path(args.scenarios_dir))
        if not live:
            print(f"[reflect-ledger] REFUSING: no scenario files under {args.scenarios_dir} -- an empty "
                  f"corpus would retire the whole ledger. Check the path.", file=sys.stderr)
            # The zero here is the SCENARIO corpus, not the ledger -- so that is the unit
            # reported: retiring against nothing would silently empty the ledger.
            return verdict.emit(
                "LEDGER_RESULT",
                passed=False, scope=0, scope_unit="scenario-subjects",
                fields={"mode": "retire-missing", "retired": 0, "live": 0,
                        "total": len(entries), "note": "empty corpus, refused"},
            )
        scanned = len(entries)
        entries, retired = retire_missing_subjects(entries, live)
        by_subject: dict[str, int] = {}
        for e in retired:
            by_subject[e.get("subject", "")] = by_subject.get(e.get("subject", ""), 0) + 1
        for subject in sorted(by_subject):
            print(f"  retired x{by_subject[subject]:<3} {subject} (no scenario file -- renamed or deleted)")
        if not retired:
            print("[reflect-ledger] nothing to retire -- every live finding's subject still exists.")
        elif not args.dry_run:
            save_ledger(ledger_path, entries)
            print(f"[reflect-ledger] retired {len(retired)} finding(s) across {len(by_subject)} "
                  f"subject(s); ledger written to {ledger_path}")
        else:
            print("[reflect-ledger] dry-run -- ledger NOT written")
        still_live = sum(1 for e in entries.values() if e.get("status") in LIVE_STATUSES)
        return verdict.emit(
            "LEDGER_RESULT",
            passed=True,
            scope=scanned,
            scope_unit="ledger-findings",
            fields={"mode": "retire-missing", "retired": len(retired),
                    "subjects": len(by_subject), "live": still_live,
                    "total": len(entries), "scenario_subjects": len(live)},
        )

    if args.ingest:
        digest = None
        try:
            digest = json.loads(Path(args.ingest).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            print(f"[reflect-ledger] could not read digest {args.ingest}: {exc}", file=sys.stderr)
            return verdict.emit(
                "LEDGER_RESULT",
                passed=False, scope=0, scope_unit="digest-findings",
                fields={"mode": "ingest", "new": 0, "reopened": 0, "resolved": 0,
                        "live": 0, "total": 0, "note": "digest unreadable"},
            )
        findings, covered = findings_from_digest(digest)
        entries, summary = ingest(entries, findings, covered, families=digest_families(digest, findings))
        if not args.dry_run:
            save_ledger(ledger_path, entries)
            print(f"[reflect-ledger] ledger written to {ledger_path}")
        else:
            print("[reflect-ledger] dry-run -- ledger NOT written")
        # scope = findings READ OUT of the digest, not the ledger written to. A digest
        # that parses but yields nothing (a schema drift, an empty grade run) produced
        # `new=0 reopened=0 resolved=0` and exit 0 -- indistinguishable from a tick where
        # genuinely nothing changed. It is INDETERMINATE and exits 2.
        return verdict.emit(
            "LEDGER_RESULT",
            passed=True,
            scope=len(findings),
            scope_unit="digest-findings",
            fields={"mode": "ingest", "new": summary["new"],
                    "reopened": summary["reopened"], "resolved": summary["resolved"],
                    "live": summary["still_live"], "total": summary["total"]},
        )

    # --list (default action when no --ingest)
    flt = args.list or "all"
    rows = [e for e in entries.values() if flt == "all" or e.get("status") == flt]
    rows.sort(key=lambda e: (e.get("status", ""), -int(e.get("seen_count", 0)), e.get("fingerprint", "")))
    for e in rows:
        print(f"  [{e.get('status','?'):9}] x{e.get('seen_count',0):<3} "
              f"c{len(e.get('configs', []) or []):<2} {e.get('rule_id','')} "
              f"{e.get('subject','')} :: {e.get('sample','')}")
    if args.json:
        out = Path(args.json)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps({"count": len(rows), "entries": rows}, indent=2), encoding="utf-8", newline="\n")
        print(f"[reflect-ledger] list written to {out}")
    live = sum(1 for e in entries.values() if e.get("status") in LIVE_STATUSES)
    # scope = the whole ledger; `listed` is the filtered subset. Reporting the filter's
    # own output as the denominator would make every --list self-confirming.
    return verdict.emit(
        "LEDGER_RESULT",
        passed=True,
        scope=len(entries),
        scope_unit="ledger-findings",
        fields={"mode": "list", "filter": flt, "listed": len(rows), "live": live,
                "total": len(entries)},
    )


if __name__ == "__main__":
    raise SystemExit(verdict.run_main(main))
