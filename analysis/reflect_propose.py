#!/usr/bin/env python3
"""Turn persistent ledger Findings into gated proposals.

Reads the Finding ledger, clusters the live findings by the surface they implicate, and emits one
proposal per cluster, tiered by surface risk and tagged with the gate that must sign it off. The
motivating ledger entries then move to `proposed`.

THE AUTONOMY CEILING: nothing auto-applies. This tool has no write path into balance JSON, AI weights
or the desired_outcome_matrix. It only produces a proposal and routes it; a person applies the change.
It is an offline, advisory, between-builds step, so determinism is unaffected.

Surface tiers, where risk and gate ascend together:
  a -- coverage / tooling / AI heuristic hints. Lowest risk; gate `review`. No balance number moves.
  b -- JSON balance / an attributed knob. Needs human judgement against authored intent.
  c -- the desired_outcome_matrix itself (the INTENT may be wrong, not the tuning). Human-only.

Proposals are idempotent: the proposal_id is a stable hash of (surface, subject), so re-running after
new findings refreshes the same proposal rather than duplicating it. A refresh keeps the findings
already on the proposal, so their configs and attributed knobs survive it.

After a person applies and commits a fix, `--committed <proposal_id>` moves that proposal's findings
to `committed`; the next ledger ingest over the same subject confirms the fix (resolved) or catches a
regression (reopened).

The last stdout line is a machine-readable `PROPOSE_RESULT:` summary.
"""

from __future__ import annotations

import argparse
import hashlib
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
from tools.reflect_ledger import (  # noqa: E402
    LEDGER_PATH,
    ST_OPEN,
    ST_REOPENED,
    ST_PROPOSED,
    ST_COMMITTED,
    load_ledger,
    save_ledger,
    set_status,
)

PROPOSALS_PATH = ROOT / ".github" / "reflect" / "proposals.json"

# rule_id -> proposal routing. Findings whose rule_id is absent here are NON-ACTIONABLE (INFO/context
# noise -- e.g. reflect-live-adapted, reflect-live-synthetic-context) and never raise a proposal.
# `tier`/`gate` ascend with risk; `surface` + `direction` are HINTS for the human, not edits.
_TIER_RANK = {"a": 0, "b": 1, "c": 2}
PROPOSAL_RULES: dict[str, dict] = {
    # tier b -- JSON balance / an attributed knob; a person judges vs authored intent.
    "reflect-metric-miss": {"tier": "b", "gate": "balance", "surface": "json-balance",
                            "direction": "tune the implicated stat/formula toward the authored target"},
    # Same surface as the metric-miss it explains, so a counterfactual attribution MERGES into the
    # balance proposal for that scenario and upgrades its direction from "tune something" to the
    # specific responsible knob (carried on the ledger entry's `knobs`).
    "reflect-cf-attributed": {"tier": "b", "gate": "balance", "surface": "json-balance",
                              "direction": "adjust the attributed knob that moved the metric over threshold"},
    "reflect-live-health-miss": {"tier": "b", "gate": "balance", "surface": "json-balance",
                                 "direction": "observed outcome left the healthy band -- investigate force/balance"},
    # NOTE: reflect-intent-verdict is deliberately NOT routed. It is a per-scenario headline emitted
    # for every graded scenario, so routing it would raise proposals for passing scenarios and
    # double-count the metric misses it summarises. The "intent may be wrong" escalation (tier c) is a
    # deliberate human call after a tier-b proposal is judged un-tunable. The verdict still flows into
    # the ledger.
    # tier c -- a novel recurring live matchup the library lacks; authoring its intent is a design act.
    "reflect-coverage-gap": {"tier": "c", "gate": "human", "surface": "scenario-library",
                             "direction": "promote this recurring live matchup into the authored library "
                                          "with an authored desired_outcome_matrix (a draft scenario exists "
                                          "if --write-candidates was used)"},
    # tier a -- AI heuristic (the safest surface: brain weights / IntentDoctrine, not
    # balance JSON). A recurring missed/regretted intent is a decision-quality pattern to tune.
    "reflect-ai-intent-missed": {"tier": "a", "gate": "review", "surface": "ai-heuristic",
                                 "direction": "the brain committed to an intent that keeps missing -- tune "
                                              "the IntentDoctrine weight / heuristic behind it"},
    "reflect-ai-divergence-regretted": {"tier": "a", "gate": "review", "surface": "ai-heuristic",
                                        "direction": "the brain keeps overriding the player and regretting "
                                                     "it -- lower its override confidence for this pattern"},
    # tier a -- coverage / tooling; no balance number moves; review gate.
    "reflect-metric-unresolved": {"tier": "a", "gate": "review", "surface": "report-coverage",
                                  "direction": "enrich the persisted report so this metric is gradable"},
    "reflect-cf-unattributed": {"tier": "a", "gate": "review", "surface": "counterfactual-sweep",
                                "direction": "no single swept knob brings this back -- widen the sweep or try combinations"},
    "reflect-live-source-divergence": {"tier": "a", "gate": "review", "surface": "metric-source",
                                       "direction": "reconcile the summary-mean vs trace-observation read"},
}


def _proposal_id(surface: str, subject: str) -> str:
    return "RP-" + hashlib.sha1(f"{surface}|{subject}".encode("utf-8")).hexdigest()[:8]


def propose(entries: dict[str, dict], min_seen: int = 1, min_configs: int = 1,
            today: str | None = None) -> tuple[dict[str, dict], dict[str, dict], dict[str, int]]:
    """Cluster LIVE actionable findings into gated proposals. Pure (copies entries).

    Returns (proposals, updated_entries, summary). A proposal clusters by (surface, subject); its
    tier/gate is the MOST restrictive among its motivating findings. A cluster is raised only when it
    holds a finding no proposal has claimed yet; findings already `proposed` stay in it as evidence.

    `min_configs` is the cross-config breadth gate: a cluster is only proposed if its findings appeared
    under >= min_configs DISTINCT runs/labels. This is how the loop refines meaningfulness as the
    corpus grows -- a real balance bug recurs across many independent configs, a probe artifact stays
    concentrated in one ablation label. Findings that carry no config data count as breadth 1 (so the
    gate is a no-op until config-stamped findings accrue; raise min_configs to filter probe noise)."""
    stamp = today or date.today().isoformat()
    out_entries = {fp: dict(e) for fp, e in entries.items()}

    # Gather actionable, sufficiently-recurring, live findings.
    clusters: dict[tuple[str, str], dict] = {}
    for fp, e in sorted(out_entries.items()):
        status = e.get("status")
        if status not in (ST_OPEN, ST_REOPENED, ST_PROPOSED):
            continue
        # A finding already on a proposal cleared this bar when the proposal was raised.
        if status != ST_PROPOSED and int(e.get("seen_count", 0)) < min_seen:
            continue
        rule = PROPOSAL_RULES.get(e.get("rule_id", ""))
        if rule is None:
            continue
        key = (rule["surface"], e.get("subject", ""))
        c = clusters.setdefault(key, {
            "surface": rule["surface"], "subject": e.get("subject", ""),
            "tier": rule["tier"], "gate": rule["gate"], "direction": rule["direction"],
            "fingerprints": [], "unclaimed": [], "rule_ids": set(), "configs": set(), "knobs": set(),
        })
        c["fingerprints"].append(fp)
        if status != ST_PROPOSED:
            c["unclaimed"].append(fp)
        c["rule_ids"].add(e.get("rule_id", ""))
        c["configs"].update(e.get("configs", []) or [])
        c["knobs"].update(e.get("knobs", []) or [])
        # Escalate to the most restrictive tier/gate seen in the cluster.
        if _TIER_RANK[rule["tier"]] > _TIER_RANK[c["tier"]]:
            c["tier"], c["gate"], c["direction"] = rule["tier"], rule["gate"], rule["direction"]

    proposals: dict[str, dict] = {}
    for (surface, subject), c in clusters.items():
        if not c["unclaimed"]:
            continue  # nothing new since this proposal was raised
        # Breadth gate: a cluster with no config data is treated as breadth 1 (gate is a no-op until
        # config-stamped findings accrue). Below the threshold -> probably config-specific, skip.
        breadth = max(len(c["configs"]), 1)
        if breadth < min_configs:
            continue
        pid = _proposal_id(surface, subject)
        knobs = sorted(c["knobs"])
        # Attribution-backed: when a counterfactual has named the responsible knob(s), upgrade the
        # direction from the generic "tune something" to the specific knob -- what the reviewer needs.
        direction = c["direction"]
        if knobs:
            direction = f"adjust {', '.join(knobs)} (counterfactual-attributed cause); " + direction
        proposals[pid] = {
            "proposal_id": pid,
            "tier": c["tier"],
            "gate": c["gate"],
            "surface": surface,
            "subject": subject,
            "direction": direction,
            "knobs": knobs,
            "rule_ids": sorted(c["rule_ids"]),
            "motivated_by": sorted(c["fingerprints"]),
            "configs": sorted(c["configs"]),
            "config_breadth": breadth,
            "rationale": f"{len(c['fingerprints'])} live finding(s) on {subject} implicate {surface}, "
                         f"seen under {breadth} distinct config(s)"
                         + (f"; attributed knob(s): {', '.join(knobs)}" if knobs else "") + ".",
            "status": "raised",
            "created": stamp,
        }
        set_status(out_entries, c["unclaimed"], ST_PROPOSED, today=stamp, proposal_id=pid)

    summary = {
        "proposals": len(proposals),
        "tier_a": sum(1 for p in proposals.values() if p["tier"] == "a"),
        "tier_b": sum(1 for p in proposals.values() if p["tier"] == "b"),
        "tier_c": sum(1 for p in proposals.values() if p["tier"] == "c"),
    }
    return proposals, out_entries, summary


def _metric_of(location: str) -> str:
    """The metric a finding location names (the `::<metric>` anchor), else ''."""
    return location.split("::", 1)[1] if "::" in location else ""


def _systemic_id(metric: str) -> str:
    return "RS-" + hashlib.sha1(f"systemic|{metric}".encode("utf-8")).hexdigest()[:8]


def detect_systemic(entries: dict[str, dict], min_scenarios: int = 3,
                    today: str | None = None) -> list[dict]:
    """Cross-scenario root-cause pass: a metric that misses across MANY UNRELATED scenarios is likely
    ONE systemic fitness-calibration/formula issue, not N per-scenario bugs. Returns systemic proposals (RS-*).

    This is an AGGREGATE overlay -- it does NOT transition finding status (the per-subject proposals own
    that); it is the headline that says 'before tuning these N scenarios individually, note metric M is
    systematically off'. Built only from RISK reflect-metric-miss findings (variant/probe misses are
    already excluded upstream)."""
    stamp = today or date.today().isoformat()
    by_metric: dict[str, dict] = {}
    for fp, e in sorted(entries.items()):
        if e.get("status") not in (ST_OPEN, ST_PROPOSED, ST_REOPENED):
            continue
        if e.get("rule_id") != "reflect-metric-miss":
            continue
        metric = _metric_of(e.get("location", ""))
        if not metric:
            continue
        m = by_metric.setdefault(metric, {"subjects": set(), "fingerprints": [], "configs": set()})
        m["subjects"].add(e.get("subject", ""))
        m["fingerprints"].append(fp)
        m["configs"].update(e.get("configs", []) or [])

    systemic: list[dict] = []
    for metric, m in by_metric.items():
        if len(m["subjects"]) < min_scenarios:
            continue
        pid = _systemic_id(metric)
        subjects = sorted(s.replace("fitness_simulation/", "") for s in m["subjects"])
        systemic.append({
            "proposal_id": pid,
            "tier": "b",
            "gate": "balance",
            "surface": "systemic-metric",
            "metric": metric,
            "scenario_count": len(m["subjects"]),
            "config_breadth": len(m["configs"]),
            "affected_scenarios": subjects,
            "motivated_by": sorted(m["fingerprints"]),
            "direction": f"{metric} misses across {len(m['subjects'])} scenarios -- likely a SYSTEMIC "
                         f"fitness-calibration/formula issue; investigate {metric} globally before per-scenario "
                         f"tuning",
            "rationale": f"{metric} below intent in {len(m['subjects'])} distinct scenarios "
                         f"({len(m['configs'])} config(s)).",
            "status": "raised",
            "created": stamp,
        })
    systemic.sort(key=lambda x: -x["scenario_count"])
    return systemic


def mark_committed(entries: dict[str, dict], proposals: dict[str, dict], proposal_id: str,
                   today: str | None = None) -> int:
    """Transition a proposal's motivating findings to `committed` (a human applied + committed the fix)."""
    p = proposals.get(proposal_id)
    if p is None:
        return 0
    n = set_status(entries, p.get("motivated_by", []), ST_COMMITTED, today=today)
    p["status"] = "committed"
    return n


# ---------------------------------------------------------------------------
# IO
# ---------------------------------------------------------------------------

def load_proposals(path: Path) -> dict[str, dict]:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return {str(k): v for k, v in (data.get("proposals", {}) or {}).items()}
    except (OSError, json.JSONDecodeError):
        return {}


def save_proposals(path: Path, proposals: dict[str, dict], today: str | None = None) -> None:
    stamp = today or date.today().isoformat()
    payload = {
        "_comment": "Reflective-loop proposals (reflect_propose.py). GATED suggestions; NOTHING "
                    "auto-applies. Each rides a gate (review/balance/human) a human clears by hand.",
        "generated": stamp,
        "count": len(proposals),
        "proposals": {pid: proposals[pid] for pid in sorted(proposals)},
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    old = path.read_text(encoding="utf-8") if path.exists() else ""
    safe_write_text(path, old, json.dumps(payload, indent=2) + "\n", write=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ledger", default=str(LEDGER_PATH))
    parser.add_argument("--proposals", default=str(PROPOSALS_PATH))
    parser.add_argument("--min-seen", type=int, default=1,
                        help="only propose for findings seen >= N times (ignore one-off variance)")
    parser.add_argument("--min-configs", type=int, default=1,
                        help="cross-config breadth gate: only propose clusters seen under >= N distinct "
                             "runs/labels (filters probe-label artifacts; raise as the corpus grows)")
    parser.add_argument("--systemic-min", type=int, default=3,
                        help="systemic-pattern threshold: flag a metric that misses across >= N distinct "
                             "scenarios as one root-cause finding (0 disables the cross-scenario pass)")
    parser.add_argument("--committed", metavar="PROPOSAL_ID",
                        help="mark a proposal's findings committed (a fix was applied + committed)")
    parser.add_argument("--dry-run", action="store_true", help="preview; do not write ledger/proposals")
    parser.add_argument("--json", help="write the proposals digest to this path")
    args = parser.parse_args(argv)

    ledger_path = Path(args.ledger)
    proposals_path = Path(args.proposals)
    entries = load_ledger(ledger_path)

    if args.committed:
        proposals = load_proposals(proposals_path)
        n = mark_committed(entries, proposals, args.committed)
        if n == 0:
            print(f"[reflect-propose] no proposal '{args.committed}' (or it has no findings).",
                  file=sys.stderr)
        if not args.dry_run and n > 0:
            save_ledger(ledger_path, entries)
            save_proposals(proposals_path, proposals)
        # scope = the ledger this pass SEARCHED, not the matches it found: n==0 means "no such
        # proposal" (RED, exit 1) or "the ledger was empty or unreadable" (INDETERMINATE, exit 2).
        return verdict.emit(
            "PROPOSE_RESULT",
            passed=n > 0,
            scope=len(entries),
            scope_unit="ledger-findings",
            fields={"committed_proposal": args.committed, "findings_marked": n},
        )

    proposals, updated, summary = propose(entries, min_seen=args.min_seen, min_configs=args.min_configs)
    systemic = detect_systemic(entries, min_scenarios=args.systemic_min) if args.systemic_min > 0 else []

    # Merge with any prior proposals (idempotent on proposal_id) so committed/older ones persist.
    merged = load_proposals(proposals_path)
    merged.update(proposals)

    gate_order = {"human": 0, "balance": 1, "review": 2}
    # SYSTEMIC root-cause headlines first -- they reframe many per-scenario proposals as one issue.
    if systemic:
        print("== SYSTEMIC (root-cause across scenarios) ==")
        for s in systemic:
            print(f"  [{s['gate']:7} tier-{s['tier']}] {s['proposal_id']} systemic:{s['metric']} "
                  f"-- {s['direction']} ({s['scenario_count']} scenarios, {s['config_breadth']} config(s))")
        print("== PER-SCENARIO ==")
    # Rank by config-breadth within each gate -- the wider the breadth, the likelier a real bug.
    for p in sorted(proposals.values(),
                    key=lambda x: (gate_order.get(x["gate"], 9), -x.get("config_breadth", 1), x["proposal_id"])):
        print(f"  [{p['gate']:7} tier-{p['tier']}] {p['proposal_id']} {p['surface']} @ {p['subject']} "
              f"-- {p['direction']} ({len(p['motivated_by'])} finding(s), {p.get('config_breadth', 1)} config(s))")

    if not args.dry_run:
        save_ledger(ledger_path, updated)
        save_proposals(proposals_path, merged)
        print(f"[reflect-propose] {len(proposals)} proposal(s) written to {proposals_path}")
    else:
        print("[reflect-propose] dry-run -- nothing written")

    if args.json:
        out = Path(args.json)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps({"summary": summary, "proposals": list(proposals.values()),
                                   "systemic": systemic}, indent=2), encoding="utf-8", newline="\n")
        print(f"[reflect-propose] digest written to {out}")

    # scope = the ledger findings proposals were derived FROM. "2 proposals" reads
    # completely differently out of 4 findings than out of 4000, and an empty ledger
    # (INDETERMINATE) must not present as "nothing worth proposing".
    return verdict.emit(
        "PROPOSE_RESULT",
        passed=True,
        scope=len(entries),
        scope_unit="ledger-findings",
        fields={
            "proposals": summary["proposals"],
            "tier_a": summary["tier_a"],
            "tier_b": summary["tier_b"],
            "tier_c": summary["tier_c"],
            "systemic": len(systemic),
        },
    )


if __name__ == "__main__":
    raise SystemExit(verdict.run_main(main))
