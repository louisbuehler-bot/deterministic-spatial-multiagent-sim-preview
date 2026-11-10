#!/usr/bin/env python3
"""Reflect on a concluded combat: grade a persisted mission report against its authored intent.

A read-only, deterministic digest. It reads a mission-report JSON the simulation persisted and grades
the observed outcome against the scenario's authored `desired_outcome_matrix`, the balance ground
truth. It emits structured Findings, so a reflected match carries the same fingerprint identity as
every other finding in the loop.

It changes nothing and runs no simulation. A metric a persisted report cannot answer is flagged as a
coverage gap, never silently passed; a pass that rests on only some of the authored metrics is marked
PARTIAL; and a report with no authored intent is reported as ungraded.

The last stdout line is a machine-readable `REFLECT_RESULT:` summary.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_scripts_dir = str(Path(__file__).resolve().parent.parent)
if _scripts_dir not in sys.path:
    sys.path.insert(0, _scripts_dir)
import verdict  # noqa: E402
from audit_common import ROOT  # noqa: E402
from audit_finding import (  # noqa: E402
    SEV_RISK,
    SEV_REVIEW,
    SEV_INFO,
    SEV_BACKLOG,
    Finding,
    location_of,
)

FITNESS_REPORTS_DIR = ROOT / "Saved" / "CombatMissionReports" / "fitness_simulation"
SCENARIOS_DIR = ROOT / "Data" / "FitnessSimulationScenarios"

# Mapping from an authored metric name (desired_outcome_matrix metric_expectations / success_profile)
# to the persisted report `summary` field that carries its observed value. This MIRRORS the C++
# resolver FitnessSimulationEvaluator::TryGetMetricActualValue for the subset of metrics whose value
# survives into the persisted summary. Metrics not listed here are computed in C++ from the full
# FCombatTrace (per-round data), which the persisted report does NOT carry -- those are reported as
# unresolved-from-report (a corpus coverage gap), never silently treated as passing.
#   OutcomeStability -> mean_combat_outcome_score : the persisted mean of ComputeOutcomeScore(Outcome)
#     (resolved outcome = 1.0, stalemate = 0.4, mutual-destruction = 0.25, unresolved/in-progress = 0).
SUMMARY_METRIC_FIELDS: dict[str, str] = {
    "DefenderSurvivalRatio": "mean_defender_survival_ratio",
    "AttackerSurvivalRatio": "mean_attacker_survival_ratio",
    "OutcomeStability": "mean_combat_outcome_score",
    "SuccessProfileScore": "mean_success_profile_score",
    "CombinedSimulationScore": "mean_combined_simulation_score",
    "CombatOutcomeScore": "mean_combat_outcome_score",
    "AttackerWinRate": "attacker_win_rate",
    "DefenderWinRate": "defender_win_rate",
    "UnresolvedRate": "unresolved_rate",
}


def resolve_metric(summary: dict, metric_name: str,
                   observations: dict[str, float] | None = None) -> float | None:
    """Observed value for an authored metric, or None if the report cannot carry it.

    Precedence: the persisted summary FIRST (its fields are cross-iteration MEANS -- statistically
    better), then the embedded `metric_observations` map (first-iteration trace-derived
    values) for metrics the summary does not carry. None -> caller emits a coverage-gap finding."""
    field = SUMMARY_METRIC_FIELDS.get(metric_name)
    if field is not None and field in summary:
        value = summary[field]
        if isinstance(value, (int, float)):
            return float(value)
    if observations is not None and metric_name in observations:
        return observations[metric_name]
    return None


def observations_map(report: dict) -> dict[str, float]:
    """Name -> value for the SUPPORTED entries of the report's embedded metric_observations.
    Unsupported observations (the evaluator could not compute them) are skipped so they
    still surface as coverage gaps rather than grading on a meaningless 0."""
    result: dict[str, float] = {}
    for obs in report.get("metric_observations", []) or []:
        name = obs.get("name", "")
        if name and obs.get("supported", False) and isinstance(obs.get("value"), (int, float)):
            result[name] = float(obs["value"])
    return result


def expectation_met(observed: float, expectation: dict) -> bool:
    """True if `observed` satisfies a metric_expectation's target_min / target_max bounds."""
    if "target_min" in expectation and observed < float(expectation["target_min"]):
        return False
    if "target_max" in expectation and observed > float(expectation["target_max"]):
        return False
    return True


def expectation_target_str(expectation: dict) -> str:
    parts = []
    if "target_min" in expectation:
        parts.append(f">= {expectation['target_min']}")
    if "target_max" in expectation:
        parts.append(f"<= {expectation['target_max']}")
    return " and ".join(parts) if parts else "(no bound)"


def evaluate_tier(tier: dict, summary: dict, observations: dict[str, float] | None = None) -> dict:
    """Evaluate one desired_outcome_matrix tier against the observed summary.

    Returns {met, resolved, total, unresolved: [names], misses: [(name, observed, target)]}.
    `met` is True only when every expectation that COULD be resolved from the report passed AND at
    least one expectation was resolvable (an all-unresolved tier cannot be claimed as met)."""
    expectations = tier.get("metric_expectations", []) or []
    resolved = 0
    misses: list[tuple[str, float, str]] = []
    unresolved: list[str] = []
    for exp in expectations:
        name = exp.get("name", "")
        observed = resolve_metric(summary, name, observations)
        if observed is None:
            unresolved.append(name)
            continue
        resolved += 1
        if not expectation_met(observed, exp):
            misses.append((name, observed, expectation_target_str(exp)))
    met = resolved > 0 and not misses
    return {
        "tier": tier.get("tier", "?"),
        "met": met,
        "resolved": resolved,
        "total": len(expectations),
        "unresolved": unresolved,
        "misses": misses,
    }


# Tier ordering, best -> worst. "Regression" is the authored failure band (target_max bounds), not a
# passing rung; everything above minimum_passing_tier is a pass.
_TIER_RANK = {"Ideal": 3, "Acceptable": 2, "Marginal": 1, "Regression": 0}


def grade_report(report: dict, scenario: dict, scenario_id: str, config: str | None = None) -> list[Finding]:
    """Grade one report against its scenario's desired_outcome_matrix; return Findings.

    `config` = the run/label this report came from (the bundle dir, e.g. 'a4_base_off'). It is stamped
    into balance-finding evidence (NOT the fingerprint), so the same (scenario, metric) miss dedupes
    across runs while the ledger accumulates the SET of configs it appeared under. Cross-config breadth
    is what later distinguishes a real balance bug (appears under many independent configs) from a
    probe artifact (concentrated in one ablation label) -- the loop refines meaningfulness as the
    corpus grows, rather than gating on a static baseline."""
    findings: list[Finding] = []
    loc = location_of(f"fitness_simulation/{scenario_id}")
    summary = report.get("summary", {}) or {}
    observations = observations_map(report)
    matrix = scenario.get("desired_outcome_matrix")
    expanded_id = summary.get("expanded_scenario_id", scenario_id) or scenario_id
    is_variant = expanded_id != scenario_id

    if not matrix:
        findings.append(Finding(
            rule_id="reflect-no-intent",
            location=loc,
            identity=(scenario_id,),
            severity=SEV_BACKLOG,
            message=f"[reflect] {scenario_id}: scenario has no desired_outcome_matrix -- "
                    f"cannot grade against authored intent.",
        ))
        return findings

    tiers = matrix.get("tiers", []) or []
    passing_tier = (matrix.get("good_result_definition", {}) or {}).get("minimum_passing_tier", "Acceptable")
    passing_rank = _TIER_RANK.get(passing_tier, 2)

    # Evaluate each passing-rank tier; the achieved tier is the best one fully met.
    evals = [evaluate_tier(t, summary, observations) for t in tiers]
    achieved_rank = -1
    achieved_name = "None"
    achieved_eval: dict | None = None
    for ev in evals:
        rank = _TIER_RANK.get(ev["tier"], -1)
        if rank >= passing_rank and ev["met"] and rank > achieved_rank:
            achieved_rank = rank
            achieved_name = ev["tier"]
            achieved_eval = ev

    met_minimum = achieved_rank >= passing_rank
    # A tier is met on the metrics the report could answer, so a PASS states how many that was. A
    # partial PASS is never INFO: nothing has shown that the unanswered metrics passed.
    metrics_resolved = achieved_eval["resolved"] if achieved_eval else 0
    metrics_total = achieved_eval["total"] if achieved_eval else 0
    partial = metrics_resolved < metrics_total

    # The headline verdict finding.
    if met_minimum:
        is_ideal = achieved_rank >= _TIER_RANK.get("Ideal", 3)
        verdict_sev = SEV_INFO if is_ideal and not partial else SEV_REVIEW
        msg = (f"[reflect] {scenario_id}: PASS -- achieved tier '{achieved_name}' "
               f"(>= minimum '{passing_tier}')"
               + (f", PARTIAL: {metrics_resolved} of {metrics_total} authored metrics answered."
                  if partial else "."))
    else:
        verdict_sev = SEV_RISK
        msg = (f"[reflect] {scenario_id}: BELOW INTENT -- did not reach minimum tier "
               f"'{passing_tier}' (best met: '{achieved_name}').")
    findings.append(Finding(
        rule_id="reflect-intent-verdict",
        location=loc,
        identity=(scenario_id,),
        severity=verdict_sev,
        message=msg,
        evidence={
            "achieved_tier": achieved_name,
            "minimum_passing_tier": passing_tier,
            "scored_outcome": summary.get("scored_outcome", ""),
            "metrics_resolved": metrics_resolved,
            "metrics_total": metrics_total,
            "partial": partial,
        },
    ))

    # Per-metric misses against the minimum passing tier (the actionable balance signal).
    # A VARIANT report is graded against the BASE scenario's intent (variants author none), so its
    # misses may be EXPECTED (e.g. a stationary-control variant is supposed to die). Emit those under a
    # distinct rule_id at REVIEW, not the RISK `reflect-metric-miss` that drives balance proposals --
    # they stay visible in the corpus but are not routed as a tunable balance bug.
    min_tier_eval = next((ev for ev in evals if ev["tier"] == passing_tier), None)
    if min_tier_eval:
        miss_id = expanded_id if is_variant else scenario_id
        for name, observed, target in min_tier_eval["misses"]:
            findings.append(Finding(
                rule_id="reflect-variant-metric-miss" if is_variant else "reflect-metric-miss",
                location=location_of(f"fitness_simulation/{scenario_id}", name),
                identity=(miss_id, name),
                severity=SEV_REVIEW if is_variant else SEV_RISK,
                message=f"[reflect] {miss_id}: {name} = {observed:.3f}, authored '{passing_tier}' "
                        f"wants {target}{' (variant vs base intent -- may be expected)' if is_variant else ''}.",
                evidence={"observed": observed, "target": target, "tier": passing_tier,
                          "is_variant": is_variant, "config": config},
            ))

    # A variant report (expanded id != source id) is graded against the BASE scenario's intent,
    # because variants in the scenario JSON override forces/dimensions but do not author their own
    # desired_outcome_matrix. The base intent may not describe the variant (e.g. a stationary-control
    # variant is SUPPOSED to die), so flag it -- the verdict above is suspect for variants.
    if is_variant:
        findings.append(Finding(
            rule_id="reflect-variant-intent",
            location=location_of(f"fitness_simulation/{scenario_id}", expanded_id),
            identity=(expanded_id,),
            severity=SEV_BACKLOG,
            message=f"[reflect] {expanded_id}: graded against BASE intent of '{scenario_id}' -- "
                    f"this variant authors no desired_outcome_matrix of its own; verdict may not apply.",
            evidence={"expanded_scenario_id": expanded_id, "base_scenario_id": scenario_id},
        ))

    # Coverage gaps: authored metrics the persisted report cannot resolve. De-duplicate across tiers.
    unresolved_names: set[str] = set()
    for ev in evals:
        unresolved_names.update(ev["unresolved"])
    for name in sorted(unresolved_names):
        findings.append(Finding(
            rule_id="reflect-metric-unresolved",
            location=location_of(f"fitness_simulation/{scenario_id}", name),
            identity=(scenario_id, name),
            severity=SEV_BACKLOG,
            message=f"[reflect] {scenario_id}: metric '{name}' is authored but not carried by the "
                    f"persisted report (needs the per-round trace -- corpus coverage gap).",
        ))

    return findings


def authored_expansion_ids(scenario: dict, scenario_id: str) -> set[str]:
    """Every expanded scenario id this scenario CURRENTLY authors: itself, plus one per variant suffix.

    Mirrors the compiler's expansion naming (`<base>_<id_suffix>`). Used to detect persisted reports
    whose variant id the scenario no longer authors -- see the orphan guard in `reflect`."""
    ids = {scenario_id}
    for variant in scenario.get("variants", []) or []:
        suffix = str(variant.get("id_suffix") or "").strip()
        if suffix:
            ids.add(f"{scenario_id}_{suffix}")
    return ids


def load_json(path: Path) -> dict | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[reflect] WARN: could not read {path}: {exc}", file=sys.stderr)
        return None


def scenario_id_of(report: dict) -> str:
    return (report.get("summary", {}) or {}).get("source_scenario_id", "")


def collect_report_paths(args: argparse.Namespace) -> list[Path]:
    if args.report:
        return [Path(args.report)]
    if args.label:
        label_dir = FITNESS_REPORTS_DIR / args.label
        return sorted(p for p in label_dir.glob("*.json")
                      if not p.name.endswith((".compact.json", ".markers.json")))
    if args.all:
        return sorted(p for p in FITNESS_REPORTS_DIR.glob("*/*.json")
                      if not p.name.endswith((".compact.json", ".markers.json")))
    return []


def reflect(report_paths: list[Path], scenarios_dir: Path) -> tuple[list[dict], list[Finding]]:
    """Grade each report; return (per-report digests, all findings). Pure over the filesystem."""
    digests: list[dict] = []
    all_findings: list[Finding] = []
    scenario_cache: dict[str, dict | None] = {}

    for report_path in report_paths:
        report = load_json(report_path)
        if report is None:
            continue
        scenario_id = scenario_id_of(report)
        source = (report.get("source") or report.get("provenance", {}).get("source") or "").lower()

        if source == "live_combat" or report_path.match("*/live_combat/*"):
            all_findings.append(Finding(
                rule_id="reflect-live-ungraded",
                location=location_of(str(report_path.relative_to(ROOT)) if report_path.is_absolute()
                                     and ROOT in report_path.parents else report_path.name),
                identity=(scenario_id or report_path.parent.name,),
                severity=SEV_BACKLOG,
                message=f"[reflect] live report {report_path.parent.name}: no authored intent to grade "
                        f"against.",
            ))
            continue

        if not scenario_id:
            all_findings.append(Finding(
                rule_id="reflect-no-scenario-id",
                location=location_of(report_path.name),
                severity=SEV_BACKLOG,
                message=f"[reflect] {report_path}: report carries no source_scenario_id.",
            ))
            continue

        if scenario_id not in scenario_cache:
            scenario_cache[scenario_id] = load_json(scenarios_dir / f"{scenario_id}.json")
        scenario = scenario_cache[scenario_id]
        if scenario is None:
            all_findings.append(Finding(
                rule_id="reflect-scenario-missing",
                location=location_of(f"fitness_simulation/{scenario_id}"),
                identity=(scenario_id,),
                severity=SEV_BACKLOG,
                message=f"[reflect] {scenario_id}: no scenario file at "
                        f"{scenarios_dir.name}/{scenario_id}.json -- cannot grade.",
            ))
            continue

        # ORPHAN GUARD -- a persisted report whose expanded id the scenario no longer authors is stale
        # output, not evidence. The report archive is append-only, so a renamed variant leaves its old
        # report on disk, and grading it would re-derive findings for variant ids that no longer exist.
        # Like the hygiene findings above, this rides the finding list and not a per-report digest, so
        # it stays visible in output without entering the ledger.
        expanded_id = (report.get("summary", {}) or {}).get("expanded_scenario_id") or scenario_id
        if expanded_id not in authored_expansion_ids(scenario, scenario_id):
            all_findings.append(Finding(
                rule_id="reflect-report-orphaned",
                location=location_of(f"fitness_simulation/{scenario_id}", expanded_id),
                identity=(expanded_id, report_path.parent.name),
                severity=SEV_BACKLOG,
                message=f"[reflect] {expanded_id}: persisted report in label "
                        f"'{report_path.parent.name}' names a variant '{scenario_id}' no longer "
                        f"authors -- stale output, not graded (regenerate the label or delete it).",
                evidence={"expanded_scenario_id": expanded_id, "base_scenario_id": scenario_id,
                          "label": report_path.parent.name},
            ))
            continue

        # The config/label is the report's bundle dir (Saved/.../fitness_simulation/<label>/<scn>.json).
        findings = grade_report(report, scenario, scenario_id, config=report_path.parent.name)
        all_findings.extend(findings)
        digests.append({
            "report": str(report_path),
            "scenario_id": scenario_id,
            "expanded_scenario_id": expanded_id,  # the ledger's coverage: this expansion, no other
            "graded": bool(scenario.get("desired_outcome_matrix")),
            "findings": [f.to_dict() for f in findings],
        })

    return digests, all_findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--report", help="path to a single mission_report.json")
    src.add_argument("--label", help="fitness label dir under Saved/CombatMissionReports/fitness_simulation/")
    src.add_argument("--all", action="store_true", help="sweep every fitness report on disk")
    parser.add_argument("--scenarios-dir", default=str(SCENARIOS_DIR),
                        help="directory of fitness scenario JSON (default: Data/FitnessSimulationScenarios)")
    parser.add_argument("--json", help="write the full digest JSON to this path")
    args = parser.parse_args(argv)

    report_paths = collect_report_paths(args)
    if not report_paths:
        print("[reflect] no reports found for the given selector.", file=sys.stderr)
        # No zero_scope_reason on purpose. A selector that matched nothing is the exact
        # ambiguity a scope exists to expose: "no combats fell below intent" and "no combats
        # were read" are opposite facts, and this branch is the second one.
        return verdict.emit(
            "REFLECT_RESULT",
            passed=False,
            scope=0,
            scope_unit="graded-reports",
            fields={"reports": 0, "graded": 0, "risk": 0, "review": 0, "info": 0,
                    "unresolved": 0, "partial_passes": 0},
        )

    digests, findings = reflect(report_paths, Path(args.scenarios_dir))
    graded = sum(1 for d in digests if d["graded"])
    partial_passes = sum(1 for f in findings
                         if f.rule_id == "reflect-intent-verdict" and f.evidence.get("partial"))

    # Human-facing rendering, grouped by severity (risk first).
    order = {SEV_RISK: 0, SEV_REVIEW: 1, SEV_BACKLOG: 2, SEV_INFO: 3}
    for f in sorted(findings, key=lambda x: order.get(x.severity, 9)):
        print(f"  [{f.severity.upper():6}] {f.render()}")

    counts = {SEV_RISK: 0, SEV_REVIEW: 0, SEV_INFO: 0, SEV_BACKLOG: 0}
    for f in findings:
        counts[f.severity] = counts.get(f.severity, 0) + 1

    if args.json:
        out_path = Path(args.json)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({
            "reports_examined": len(report_paths),
            "reports_graded": graded,
            "counts": counts,
            "digests": digests,
        }, indent=2), encoding="utf-8", newline="\n")
        print(f"[reflect] digest written to {out_path}")

    # scope = reports GRADED, so a run that graded nothing is INDETERMINATE however many files it
    # opened. A report that is unreadable or has no authored intent counts in `reports` only.
    return verdict.emit(
        "REFLECT_RESULT",
        passed=counts[SEV_RISK] == 0,
        scope=graded,
        scope_unit="graded-reports",
        fields={
            "reports": len(report_paths),
            "graded": graded,
            "risk": counts[SEV_RISK],
            "review": counts[SEV_REVIEW],
            "info": counts[SEV_INFO],
            "unresolved": counts[SEV_BACKLOG],
            "partial_passes": partial_passes,
        },
    )


if __name__ == "__main__":
    raise SystemExit(verdict.run_main(main))
