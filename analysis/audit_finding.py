#!/usr/bin/env python3
"""Structured audit Finding + stable fingerprint identity.

A finding's stable IDENTITY (the `fingerprint`) is kept apart from its evidence and presentation, so
baselines survive cosmetic and count churn: a change to volatile evidence (counts, mtimes, line
numbers) or presentation (phrasing, fix-hints) is not a new finding. It is the SARIF
`partialFingerprints` pattern that ESLint / RuboCop / PHPStan / Psalm baselines all converged on.

Three separable layers of a finding:
  * IDENTITY   -> rule_id + location + identity tuple  (the ONLY thing keyed)
  * EVIDENCE   -> evidence dict + line                 (volatile detail, never keyed)
  * PRESENTATION -> message + severity                 (human text + gate class)

`fingerprint` is a readable, diffable composite, not an opaque hash, so a baseline file stays
reviewable.
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass, field

# Severity vocabulary -- the gate class a finding lands in. Mirrors the runner's
# four buckets (errors/warnings/backlog/info). Severity is PRESENTATION: it is
# deliberately NOT part of the fingerprint, so reclassifying a rule never churns
# the baseline (see the audit_severity.json registry).
SEV_RISK = "risk"
SEV_REVIEW = "review"
SEV_BACKLOG = "backlog"
SEV_INFO = "info"

_WS_RE = re.compile(r"\s+")


def content_anchor(text: str, length: int = 10) -> str:
    """Stable short hash of normalized matched content.

    The location fallback when a finding has no symbol/heading/key anchor: two
    findings of the same rule in the same file are distinguished by WHAT they
    matched, not by their line number (which shifts on unrelated edits). Whitespace
    is collapsed so reformatting does not change the anchor.
    """
    norm = _WS_RE.sub(" ", text or "").strip()
    return "h:" + hashlib.sha1(norm.encode("utf-8", "ignore")).hexdigest()[:length]


@dataclass
class Finding:
    """One audit finding with an identity separate from evidence and presentation.

    Construct with the stable parts (rule_id/location/identity) and the human
    message; push anything volatile (counts, mtimes, the matched line number) into
    `evidence`/`line`. Identity equality is via `.fingerprint`, never object hash,
    so the (unhashable) evidence dict is fine on a plain dataclass.

    IDENTITY DISCIPLINE (the dual of anchor drift, and the worse half
    because it UNDER-counts silently). A rule that can fire MORE THAN ONCE per
    `location` MUST put a per-occurrence, edit-stable token in `identity` -- the
    matched symbol/name, or `content_anchor(matched_region)` (same for one
    occurrence across line shifts, DIFFERENT between two occurrences). Otherwise two
    co-located hits of the same rule differ only by `line` (which is evidence, not
    keyed) -> identical fingerprint -> the engine would have to collision-fold them
    by ordinal, which is churn-prone. Empty `identity` is allowed ONLY for
    at-most-one-per-location rules (file-subject audits, singletons).

    RELATIONAL findings (about a relationship, not a file) build `location` with
    `relational_location(subjects)` so direction (A->B vs B->A) never flips the key.
    """

    rule_id: str                       # stable id owned by the audit: "CSA-AMBIG-OVERLAP", "bare-ensure"
    location: str                      # file rel-path, optionally + a stable anchor ("file::Symbol", "file::h:ab12")
    identity: tuple[str, ...] = ()     # minimal discriminator among same rule+location (alias, doc-pair, basename)
    message: str = ""                  # human rendering (may embed evidence) -- never a fingerprint input
    severity: str = SEV_REVIEW         # risk | review | backlog | info  (audit_severity.json may override)
    evidence: dict = field(default_factory=dict)   # volatile detail (counts/mtimes) -- never keyed
    line: int | None = None            # display convenience; evidence, not identity
    migrated_from: tuple[str, ...] = ()  # prior fingerprints this finding supersedes (auto-migrates the baseline)

    @property
    def fingerprint(self) -> str:
        """Readable, diffable identity key. Independent of message/severity/evidence/line."""
        ident = "|".join(self.identity)
        return f"{self.rule_id}::{self.location}::{ident}"

    def render(self) -> str:
        """Human-facing string for the runner's errors/warnings/info lists.

        Defaults to the message; audits that already build a `[TAG] file:line: ...`
        string can pass it as `message` unchanged so output is byte-identical during
        migration.
        """
        return self.message or self.fingerprint

    def to_dict(self) -> dict:
        return {
            "rule_id": self.rule_id,
            "location": self.location,
            "identity": list(self.identity),
            "fingerprint": self.fingerprint,
            "severity": self.severity,
            "message": self.message,
            "line": self.line,
            "evidence": self.evidence,
            "migrated_from": list(self.migrated_from),
        }

    @classmethod
    def from_dict(cls, data: dict) -> "Finding":
        """
        Rebuild a Finding from `to_dict` output.

        Exists for RESULT REPLAY (run_cache): an audit whose recorded inputs are
        byte-identical to its last green run is not re-run, and its banked findings
        are handed to the baseline engine in its place. That only stays honest if
        the round trip is exact on everything the engine KEYS on, so the reconstruction
        must reproduce `fingerprint` identically -- rule_id, location and identity.
        `fingerprint` itself is deliberately NOT read back: it is derived, and
        trusting a stored copy would let a hand-edited cache assert an identity its
        own fields do not support.
        """
        rule_id = data.get("rule_id", "")
        location = data.get("location", "")
        if not rule_id and not location:
            # FAIL CLOSED. With both empty the fingerprint is "::::" -- and so is
            # every OTHER identity-less finding's, so they all collide into one
            # key, fold to positional `::::#<ordinal>` slots, and `render()`
            # (message or fingerprint) degrades to the literal string "::::".
            # A legacy finding dict (file/line/code/reason/severity) replayed
            # through this schema looks EXACTLY like this: every shared
            # field missing, every default taken. Refusing here turns a corrupt
            # replay into a cache miss -- `audit_run_cache.replay_result` catches ValueError and
            # the audit simply runs for real.
            raise ValueError(
                "Finding.from_dict: refusing to build an identity-less finding "
                f"(keys={sorted(data)}). rule_id and location are both empty, which "
                "yields the degenerate fingerprint '::::' shared by every other "
                "identity-less finding. This is the signature of a LEGACY finding "
                "dict being rehydrated through the shared schema -- the caller "
                "banked the wrong shape."
            )
        return cls(
            rule_id=rule_id,
            location=location,
            identity=tuple(data.get("identity") or ()),
            message=data.get("message", ""),
            severity=data.get("severity", SEV_REVIEW),
            evidence=data.get("evidence") or {},
            line=data.get("line"),
            migrated_from=tuple(data.get("migrated_from") or ()),
        )


def location_of(rel_path: str, anchor: str | None = None) -> str:
    """Compose a location: file rel-path, optionally suffixed with a stable anchor.

    `anchor` should be a symbol/function name, markdown heading, or JSON key when
    available; use `content_anchor(...)` for the matched-text fallback. Paths are
    normalized to forward slashes so the key is OS-independent.
    """
    rel = (rel_path or "").replace("\\", "/")
    return f"{rel}::{anchor}" if anchor else rel


def relational_location(subjects) -> str:
    """Canonical `location` for a RELATIONSHIP finding (no single owning file).

    Audits whose finding is about a relationship between several subjects
    (orphan-refs, reciprocity checks, the color/fusion gate) have no single file
    `location`. Without a rule the fingerprint would flip
    when the audit reports A->B vs B->A. CONTRACT: the location is the SORTED tuple of the involved subjects joined
    canonically with `|`, never emission order -- so the fingerprint is identical
    regardless of which direction the relationship was reported.

    Paths are normalized to forward slashes and de-duplicated before sorting so the
    key is OS-independent and order-independent.
    """
    norm = {str(s).replace("\\", "/") for s in subjects if str(s)}
    return "|".join(sorted(norm))
