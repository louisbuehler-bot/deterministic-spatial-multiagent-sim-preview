#!/usr/bin/env python3
"""Emit and parse machine-readable verdict lines that carry their denominator.

Every tool in the project prints a `*_RESULT:` or `*_VERDICT:` line, and the one question that
matters about any of them is what the verdict was out of:

    passed=True, errors=[]

is printed both by a tool that examined 2470 files and found nothing wrong and by one that
examined nothing at all. Those are opposite facts wearing the same output.

1. SCOPE IS A REQUIRED ARGUMENT. You cannot call `emit` without saying what you looked at.

2. "EXAMINED NOTHING" IS A THIRD STATE. Scope 0 is neither pass nor fail: it is INDETERMINATE
   and has its own exit code (2), so a caller checking `exit_code != 0` cannot read "I checked
   nothing" as success, and does not have to parse anything to get that right. A tool that
   legitimately ran over an empty selection says so with `zero_scope_reason=`, which keeps the
   verdict GREEN and records why in the line.

3. THE LINE IS PARSEABLE. `parse()` gives consumers the scope back, so a gate can assert on
   reach instead of trusting a summary sentence.
"""

from __future__ import annotations

import json
import re
import sys
import traceback
from dataclasses import dataclass, field
from typing import IO, Any, Callable

GREEN = "GREEN"
RED = "RED"
INDETERMINATE = "INDETERMINATE"

EXIT_OK = 0
EXIT_FAIL = 1
#: Distinct on purpose. A caller testing `exit_code != 0` must not read "examined
#: nothing" as success, and gets that for free without parsing the line.
EXIT_INDETERMINATE = 2

_LINE_RE = re.compile(r"^\s*(?P<name>[A-Z][A-Z0-9_]*(?:_RESULT|_VERDICT))\s*:\s*(?P<body>.*)$")
# Quoted values may contain escaped quotes/backslashes (see _render_value) -- the writer and
# this reader must share one grammar, or emit() produces lines its own parse() misreads.
_KV_RE = re.compile(r"(?P<key>[A-Za-z_][\w-]*)=(?P<value>\"(?:\\.|[^\"\\])*\"|\S+)")


@dataclass
class Verdict:
    """A parsed verdict line. `scope is None` means the line declared no denominator."""

    name: str
    status: str | None
    scope: int | None
    scope_unit: str | None
    fields: dict[str, str] = field(default_factory=dict)
    raw: str = ""

    @property
    def examined_nothing(self) -> bool:
        return self.scope == 0

    @property
    def declares_scope(self) -> bool:
        return self.scope is not None


def status_for(passed: bool, scope: int, zero_scope_reason: str | None = None) -> str:
    """GREEN / RED / INDETERMINATE.

    A zero scope outranks `passed`: a tool that examined nothing has no verdict to give,
    however cleanly it ran. Stating a reason is the author asserting that an empty run is
    legitimate here (no changed files, empty selector), which restores the normal reading.
    """
    if scope == 0 and not zero_scope_reason:
        return INDETERMINATE
    return GREEN if passed else RED


def exit_code_for(status: str) -> int:
    if status == GREEN:
        return EXIT_OK
    if status == RED:
        return EXIT_FAIL
    return EXIT_INDETERMINATE


def _render_value(value: Any) -> str:
    # Quote only when forced to (space, quote, empty) -- an UNQUOTED value is read verbatim
    # by the parser, so raw backslashes round-trip untouched and a Windows path renders as
    # `SIDECAR=D:\x\y.txt`, not `SIDECAR="D:\\x\\y.txt"` (which would break every consumer
    # that pastes or greps the raw token). Escaping exists only INSIDE quotes, where the
    # closing delimiter makes it necessary.
    text = str(value)
    if " " in text or '"' in text or not text:
        escaped = text.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'
    return text


def _unquote(value: str) -> str:
    if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
        return value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    return value


#: Keys this module writes itself. A caller-supplied field of the same name would appear a
#: second time in the line, and `parse()` builds its dict last-wins, so a tool's own `status`
#: would overwrite the computed GREEN/RED/INDETERMINATE. Rejected rather than silently renamed.
RESERVED_FIELDS = frozenset({"status", "scope", "unit", "reach", "zero_scope_reason"})


def verdict_line(
    name: str,
    *,
    passed: bool,
    scope: int,
    scope_unit: str,
    zero_scope_reason: str | None = None,
    fields: dict[str, Any] | None = None,
) -> str:
    """Compose the line. `scope` and `scope_unit` are REQUIRED -- that is the contract."""
    if not name.endswith(("_RESULT", "_VERDICT")):
        raise ValueError(f"verdict name must end in _RESULT or _VERDICT: {name!r}")
    if scope < 0:
        raise ValueError(f"scope must be >= 0, got {scope}")
    if not scope_unit:
        raise ValueError("scope_unit is required -- '40 of what?' is the question this answers")
    clashes = sorted(RESERVED_FIELDS.intersection(fields or {}))
    if clashes:
        raise ValueError(
            f"field name(s) {clashes} are RESERVED by the verdict line itself -- emitting one "
            f"would put the key in the line twice, and parse() takes the LAST occurrence, so "
            f"your value would silently replace the computed status/scope. Rename the field "
            f"(e.g. a tool's own ok/failed state -> 'run_status')."
        )

    status = status_for(passed, scope, zero_scope_reason)
    parts = [f"status={status}", f"scope={scope}", f"unit={scope_unit}"]
    if scope == 0:
        # Loud, greppable, and distinct from a low count.
        parts.append("reach=NONE")
        if zero_scope_reason:
            parts.append(f"zero_scope_reason={_render_value(zero_scope_reason)}")
    for key, value in (fields or {}).items():
        parts.append(f"{key}={_render_value(value)}")
    return f"{name}: " + " ".join(parts)


def emit(
    name: str,
    *,
    passed: bool,
    scope: int,
    scope_unit: str,
    zero_scope_reason: str | None = None,
    fields: dict[str, Any] | None = None,
    stream: IO[str] | None = None,
) -> int:
    """Print the verdict line; return the SUGGESTED exit code.

    Deliberately does not call sys.exit: several tools encode domain meaning in their
    codes, and silently overriding that would trade one silent wrongness for another.
    """
    line = verdict_line(
        name,
        passed=passed,
        scope=scope,
        scope_unit=scope_unit,
        zero_scope_reason=zero_scope_reason,
        fields=fields,
    )
    print(line, file=stream or sys.stdout)
    return exit_code_for(status_for(passed, scope, zero_scope_reason))


def run_main(main: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
    """Call a script's `main()` so that a CRASH exits 2, never 1:

        if __name__ == "__main__":
            raise SystemExit(run_main(main))

    Python exits 1 on an uncaught exception, which is the FINDINGS code, so a caller that
    branches on 1 reads a dead tool as a red one. A crash examined an unknown part of its
    scope, which is what INDETERMINATE means, so it takes that code.

    Only an `Exception` changes, after its traceback reaches stderr. The return value
    passes through (a tool's own domain codes stay its own), and so do `SystemExit`
    (argparse's usage 2, an explicit `sys.exit`) and `KeyboardInterrupt`.
    """
    try:
        return main(*args, **kwargs)
    except Exception:  # noqa: BLE001 -- the contract: a crash is 2, never the findings code
        traceback.print_exc()
        return EXIT_INDETERMINATE


def parse(line: str) -> Verdict | None:
    """Parse one verdict line. Tolerant by design -- it must read legacy shapes too.

    Returns None if the line is not a verdict at all. A legacy line parses fine and simply
    reports `scope is None`, which is exactly the state a consumer needs to distinguish.
    """
    match = _LINE_RE.match(line)
    if not match:
        return None
    body = match.group("body")

    # JSON-body verdicts: some tools emit `NAME: {json}`, and scope/scope_unit/status are
    # ordinary JSON keys there.
    if body.lstrip().startswith("{"):
        try:
            payload = json.loads(body)
        except ValueError:
            payload = None
        if isinstance(payload, dict):
            fields = {str(k): ("" if v is None else str(v)) for k, v in payload.items()}
            scope: int | None = None
            raw_scope = payload.get("scope")
            if isinstance(raw_scope, bool):
                raw_scope = None  # bool is an int subclass; True is not a denominator
            if isinstance(raw_scope, (int, float)):
                scope = int(raw_scope)
            elif isinstance(raw_scope, str):
                try:
                    scope = int(raw_scope)
                except ValueError:
                    scope = None
            return Verdict(
                name=match.group("name"),
                status=fields.get("status"),
                scope=scope,
                scope_unit=fields.get("scope_unit") or fields.get("unit"),
                fields=fields,
                raw=line.rstrip("\n"),
            )
        # Unparseable brace-body: fall through to the KV read, which yields the honest
        # "declares no scope" rather than pretending the line is not a verdict.

    fields = {m.group("key"): _unquote(m.group("value")) for m in _KV_RE.finditer(body)}

    scope = None
    raw_scope = fields.get("scope")
    if raw_scope is not None:
        try:
            scope = int(raw_scope)
        except ValueError:
            scope = None  # e.g. the legacy `scope=--only selection`, a selector not a count

    return Verdict(
        name=match.group("name"),
        status=fields.get("status"),
        scope=scope,
        scope_unit=fields.get("unit"),
        fields=fields,
        raw=line.rstrip("\n"),
    )


def assert_reach(line: str) -> list[str]:
    """Consumer-side check: complain about a verdict that examined nothing, or cannot say.

    A static audit can prove a tool mentions a scope; only this can see that the scope was zero.
    """
    verdict = parse(line)
    if verdict is None:
        return [f"not a verdict line: {line.strip()!r}"]
    issues: list[str] = []
    if not verdict.declares_scope:
        issues.append(
            f"{verdict.name}: declares no numeric scope -- cannot tell 'examined everything "
            f"and found nothing' from 'examined nothing'"
        )
    elif verdict.examined_nothing and "zero_scope_reason" not in verdict.fields:
        issues.append(
            f"{verdict.name}: reach=NONE (scope=0) with no zero_scope_reason -- this verdict "
            f"covers nothing and must not be read as a pass"
        )
    return issues


def _main(argv: list[str] | None = None) -> int:
    """Tiny CLI so non-Python consumers (the bash pre-push hook) can run the reach check.

    `--assert-reach -` reads the line from stdin -- the hook pipes it, because a verdict
    line routinely contains `|` segments and quotes that survive a pipe intact but not a
    shell-argument round trip. Exit 0 = the line declares a scope and examined something
    (or explained why not); exit 1 = a reach issue, printed one per line.
    """
    import argparse

    ap = argparse.ArgumentParser(description="verdict line utilities (see module docstring)")
    ap.add_argument(
        "--assert-reach", metavar="LINE",
        help="check one verdict line for reach; pass '-' to read the line from stdin",
    )
    args = ap.parse_args(argv)
    if args.assert_reach is None:
        ap.print_help()
        return 2
    line = sys.stdin.readline() if args.assert_reach == "-" else args.assert_reach
    issues = assert_reach(line)
    for issue in issues:
        print(f"[verdict] {issue}")
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(_main())
