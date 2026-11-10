# Excerpt from a larger file: a one-parameter-at-a-time sweep, and the attribution of a missed target
# to the parameter that brings it back.

# Default single-knob sweep: neutralise each accuracy ERROR source to 0 in turn. If zeroing one error
# axis lifts a below-intent hit metric back over its threshold, that axis alone can explain the miss. Mirrors
# the knobs ApplyAccuracyConfigOverridesFromParams accepts in FitnessSimulationHarnessCommandlet.cpp.
# (AccuracyEngagementRangeScale is a multiplier, not an error term -- excluded from the zero-sweep; pass
# it explicitly with --knob if you want it.)
DEFAULT_SWEEP_KNOBS: list[tuple[str, float]] = [
    ("AccuracyPositionErrorScale", 0.0),
    ("AccuracyRecoilErrorScale", 0.0),
    ("AccuracyLeadErrorSUSpeed", 0.0),
    ("AccuracyBaseConeError", 0.0),
    ("AccuracySlewErrorWeight", 0.0),
]

DEFAULT_EDITOR_CMD = engine_root_or_default() / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
DEFAULT_PROJECT_FILE = ROOT / "GALAXY_PROJECT.uproject"


def _passing_tier_dict(scenario: dict) -> tuple[dict | None, str]:
    """The desired_outcome_matrix tier that is the minimum passing bar, plus its name."""
    matrix = scenario.get("desired_outcome_matrix") or {}
    passing_tier = (matrix.get("good_result_definition", {}) or {}).get("minimum_passing_tier", "Acceptable")
    for tier in matrix.get("tiers", []) or []:
        if tier.get("tier") == passing_tier:
            return tier, passing_tier
    return None, passing_tier


def baseline_misses(scenario: dict, baseline_report: dict) -> list[dict]:
    """Authored metrics at the minimum passing tier that the baseline re-run FAILS (and can resolve).

    Returns [{name, observed, expectation, target}]. Unresolved metrics are skipped -- they are a
    coverage gap reflect_match already reports, not something a knob sweep can attribute."""
    tier, _name = _passing_tier_dict(scenario)
    if tier is None:
        return []
    summary = baseline_report.get("summary", {}) or {}
    obs = observations_map(baseline_report)
    misses: list[dict] = []
    for exp in tier.get("metric_expectations", []) or []:
        metric = exp.get("name", "")
        observed = resolve_metric(summary, metric, obs)
        if observed is None:
            continue
        if not expectation_met(observed, exp):
            misses.append({
                "name": metric,
                "observed": observed,
                "expectation": exp,
                "target": expectation_target_str(exp),
            })
    return misses


def attribute(
    scenario: dict,
    scenario_id: str,
    baseline_report: dict,
    variant_reports: dict[str, dict],
    variant_knob: dict[str, tuple[str, float]],
) -> list[Finding]:
    """Pure attribution: which swept knob moves each below-intent metric back over its threshold.

    No I/O, no sim -- feed it report dicts (the runner supplies real ones; tests supply fixtures)."""
    findings: list[Finding] = []
    _tier, passing_tier = _passing_tier_dict(scenario)
    misses = baseline_misses(scenario, baseline_report)

    if not misses:
        findings.append(Finding(
            rule_id="reflect-cf-baseline-meets",
            location=location_of(f"fitness_simulation/{scenario_id}"),
            identity=(scenario_id,),
            severity=SEV_INFO,
            message=f"[reflect-cf] {scenario_id}: baseline re-run meets the '{passing_tier}' tier -- "
                    f"nothing below intent to attribute (the original miss may be iteration variance).",
        ))
        return findings

    for miss in misses:
        metric = miss["name"]
        base = miss["observed"]
        exp = miss["expectation"]
        target = miss["target"]
        attributed: list[tuple[str, str, float, float, float]] = []  # label, knob_str, base, variant, delta

        for label, report in variant_reports.items():
            summary = report.get("summary", {}) or {}
            v_obs = resolve_metric(summary, metric, observations_map(report))
            if v_obs is None:
                continue
            if expectation_met(v_obs, exp):
                kname, kval = variant_knob[label]
                attributed.append((label, f"{kname}={kval:g}", base, v_obs, v_obs - base))

        loc = location_of(f"fitness_simulation/{scenario_id}", metric)
        if attributed:
            for label, knob_str, b, v, delta in attributed:
                findings.append(Finding(
                    rule_id="reflect-cf-attributed",
                    location=loc,
                    identity=(scenario_id, metric, knob_str),
                    severity=SEV_REVIEW,
                    message=f"[reflect-cf] {scenario_id}: {metric} = {b:.3f} (below intent, wants {target}); "
                            f"{knob_str} moves it to {v:.3f} ({delta:+.3f}), crossing the '{passing_tier}' "
                            f"threshold -- a counterfactual cause of the miss.",
                    evidence={
                        "metric": metric,
                        "baseline": b,
                        "variant": v,
                        "delta": delta,
                        "knob": knob_str,
                        "tier": passing_tier,
                        "target": target,
                    },
                ))
        else:
            swept = ", ".join(sorted(f"{k}={val:g}" for k, val in variant_knob.values()))
            findings.append(Finding(
                rule_id="reflect-cf-unattributed",
                location=loc,
                identity=(scenario_id, metric),
                severity=SEV_BACKLOG,
                message=f"[reflect-cf] {scenario_id}: {metric} = {base:.3f} stays below intent (wants {target}) "
                        f"under every swept knob -- no single change in {{{swept}}} brings it back, so the "
                        f"cause is a combination of them or outside them; widen the sweep.",
                evidence={"metric": metric, "baseline": base, "target": target, "swept": swept},
            ))

    return findings


# ---------------------------------------------------------------------------
# Editor runner (the side-effecting half; kept apart from the pure attribution above)
# ---------------------------------------------------------------------------

def _bundle_report_path(bundle_label: str, scenario_id: str) -> Path:
    """Where the harness writes the per-scenario artifact for a given Report= bundle label."""
    return FITNESS_REPORTS_DIR / bundle_label / f"{scenario_id}.json"


def run_harness_variant(
    scenario_id: str,
    knobs: dict[str, float],
    bundle_label: str,
    editor_cmd: Path,
    project_file: Path,
    log_dir: Path,
) -> Path | None:
    """Run one harness variant; return the graded per-scenario report path (or None on failure)."""
    report_arg = f"Saved/CombatMissionReports/{bundle_label}.json"
    # Flags come from the ONE launch primitive (headless_launch), pinned against the
    # PowerShell twin by tests/test_headless_launch_parity.py. The knobs go in as job args
    # rather than being appended afterwards, so the builder's 16384-char FCommandLine
    # assertion measures the WHOLE line -- a knob sweep is exactly the kind of caller that
    # grows an argument list without anyone watching, and UE truncates SILENTLY.
    job_args = [f"Id={scenario_id}", f"Report={report_arg}"]
    job_args.extend(f"{name}={value:g}" for name, value in knobs.items())
    command = build_commandlet_command(
        "FitnessSimulationHarness",
        job_args=job_args,
        editor_cmd=editor_cmd,
        project_file=project_file,
    )

    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{bundle_label}.log"
    with open(log_path, "w", encoding="utf-8", newline="\n") as log_file:
        log_file.write("Command:\n" + " ".join(command) + "\n\n")
        log_file.flush()
        # Marked so the build's memory reaper leaves it alone: a reaped run would otherwise read as a
        # knob that broke the simulation.
        with launch_marker(label=f"reflect-counterfactual {bundle_label}"):
            result = subprocess.run(command, cwd=ROOT, stdout=log_file, stderr=subprocess.STDOUT,
                                    text=True, check=False)
    if result.returncode != 0:
        print(f"[reflect-cf] harness run failed (exit {result.returncode}) for '{bundle_label}'; "
              f"see {log_path}", file=sys.stderr)
        return None

    bundle = _bundle_report_path(bundle_label, scenario_id)
    if not bundle.exists():
        print(f"[reflect-cf] harness produced no report for '{bundle_label}' at {bundle}", file=sys.stderr)
        return None
    return bundle


