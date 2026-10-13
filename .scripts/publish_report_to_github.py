#!/usr/bin/env python3
"""Publish QA/compliance reports through GitHub-native surfaces.

The audit scripts already produce a machine-readable JSON payload and a
Markdown report. This publisher keeps that contract and replaces the old
external-document/table notification layer with:

- GitHub Actions step summaries
- GitHub issue registry entries keyed by finding fingerprint
- an optional report-feed issue that receives one comment per run
- an optional PR comment that is updated in place with the full Markdown report

It intentionally uses only the Python standard library so it can run on the
stock GitHub-hosted runner after actions/setup-python.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent

FINDING_MARKER = "harken_quality_finding"
FEED_MARKER = "harken_quality_report_feed"
PR_REPORT_MARKER = "harken_quality_pr_report"
FINDING_RE = re.compile(r"<!--\s*harken_quality_finding\s*(\{.*?\})\s*-->", re.S)
PR_REPORT_RE = re.compile(r"<!--\s*harken_quality_pr_report\s*(\{.*?\})\s*-->", re.S)
GITHUB_COMMENT_LIMIT = 65_536

SEVERITY_ORDER = {
    "critical": 0,
    "high": 1,
    "medium": 2,
    "low": 3,
    "info": 4,
}
SEVERITY_LABELS = {
    "critical": "P0 critical",
    "high": "P1 high",
    "medium": "P2 medium",
    "low": "P3 low",
    "info": "info",
}
SEVERITY_ISSUE_LABELS = {
    key: f"severity/{key}" for key in SEVERITY_ORDER
}
STATUS_LABELS = {
    "new": "qa/status/new",
    "persistent": "qa/status/persistent",
    "not_reproduced": "qa/status/not-reproduced",
    "recheck_passed": "qa/status/recheck-passed",
    "recheck_failed": "qa/status/recheck-failed",
}
MANUAL_READY_LABEL = "qa/manual/ready-for-recheck"
BASE_LABEL = "qa"
REPORT_FEED_LABEL = "qa/report-feed"

LABEL_DEFS = {
    BASE_LABEL: ("0E8A16", "Quality automation finding or report."),
    REPORT_FEED_LABEL: ("5319E7", "Issue that receives automated QA report comments."),
    MANUAL_READY_LABEL: ("FBCA04", "Human owner says the finding is fixed and ready for QA recheck."),
    STATUS_LABELS["new"]: ("D73A4A", "First seen by the automated QA flow."),
    STATUS_LABELS["persistent"]: ("B60205", "Still reproduced by the automated QA flow."),
    STATUS_LABELS["not_reproduced"]: ("C2E0C6", "Not reproduced in the latest automated QA run."),
    STATUS_LABELS["recheck_passed"]: ("0E8A16", "Automated recheck passed; waiting for human confirmation."),
    STATUS_LABELS["recheck_failed"]: ("D93F0B", "Automated recheck failed; the finding still reproduces."),
    SEVERITY_ISSUE_LABELS["critical"]: ("B60205", "Critical QA finding."),
    SEVERITY_ISSUE_LABELS["high"]: ("D93F0B", "High-severity QA finding."),
    SEVERITY_ISSUE_LABELS["medium"]: ("FBCA04", "Medium-severity QA finding."),
    SEVERITY_ISSUE_LABELS["low"]: ("C2E0C6", "Low-severity QA finding."),
    SEVERITY_ISSUE_LABELS["info"]: ("D0D7DE", "Informational QA finding."),
}


class PublishError(RuntimeError):
    pass


class GitHubClient:
    def __init__(self, *, repo: str, token: str, api_url: str = "https://api.github.com") -> None:
        if "/" not in repo:
            raise PublishError(f"Invalid GitHub repository identifier: {repo!r}")
        self.repo = repo
        self.token = token
        self.api_url = api_url.rstrip("/")

    def request(
        self,
        method: str,
        path_or_url: str,
        *,
        payload: dict[str, Any] | list[Any] | None = None,
        ok: tuple[int, ...] = (200, 201, 204),
    ) -> tuple[Any, dict[str, str]]:
        url = path_or_url if path_or_url.startswith("http") else f"{self.api_url}{path_or_url}"
        data = None
        headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self.token}",
            "X-GitHub-Api-Version": "2022-11-28",
        }
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json; charset=utf-8"
        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=30) as response:
                body = response.read()
                response_headers = {k.lower(): v for k, v in response.headers.items()}
                if response.status not in ok:
                    raise PublishError(f"{method} {redact_url(url)} returned HTTP {response.status}")
                if response.status == 204 or not body:
                    return None, response_headers
                return json.loads(body.decode("utf-8")), response_headers
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise PublishError(
                f"{method} {redact_url(url)} returned HTTP {exc.code}: {detail[:1000]}"
            ) from exc
        except urllib.error.URLError as exc:
            raise PublishError(f"{method} {redact_url(url)} failed: {exc}") from exc

    def paged(self, path: str) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        next_url: str | None = path
        while next_url:
            data, headers = self.request("GET", next_url)
            if isinstance(data, list):
                out.extend(item for item in data if isinstance(item, dict))
            next_url = next_link(headers.get("link", ""))
        return out

    def ensure_label(self, name: str) -> None:
        color, description = LABEL_DEFS.get(name, ("D0D7DE", "Managed by QA automation."))
        payload = {"name": name, "color": color, "description": description}
        try:
            self.request("POST", f"/repos/{self.repo}/labels", payload=payload, ok=(201,))
        except PublishError as exc:
            if "HTTP 422" not in str(exc):
                raise

    def list_issues(self, *, labels: list[str], state: str = "all") -> list[dict[str, Any]]:
        query = urllib.parse.urlencode(
            {
                "state": state,
                "labels": ",".join(labels),
                "per_page": "100",
            }
        )
        return [
            issue
            for issue in self.paged(f"/repos/{self.repo}/issues?{query}")
            if "pull_request" not in issue
        ]

    def create_issue(self, title: str, body: str, labels: list[str]) -> dict[str, Any]:
        data, _ = self.request(
            "POST",
            f"/repos/{self.repo}/issues",
            payload={"title": title, "body": body, "labels": labels},
            ok=(201,),
        )
        return data if isinstance(data, dict) else {}

    def update_issue(
        self,
        number: int,
        *,
        title: str | None = None,
        body: str | None = None,
        state: str | None = None,
        labels: list[str] | None = None,
    ) -> dict[str, Any]:
        payload: dict[str, Any] = {}
        if title is not None:
            payload["title"] = title
        if body is not None:
            payload["body"] = body
        if state is not None:
            payload["state"] = state
        if labels is not None:
            payload["labels"] = labels
        data, _ = self.request("PATCH", f"/repos/{self.repo}/issues/{number}", payload=payload)
        return data if isinstance(data, dict) else {}

    def create_comment(self, number: int, body: str) -> dict[str, Any]:
        data, _ = self.request(
            "POST",
            f"/repos/{self.repo}/issues/{number}/comments",
            payload={"body": body},
            ok=(201,),
        )
        return data if isinstance(data, dict) else {}

    def list_comments(self, number: int) -> list[dict[str, Any]]:
        return self.paged(f"/repos/{self.repo}/issues/{number}/comments?per_page=100")

    def update_comment(self, comment_id: int, body: str) -> dict[str, Any]:
        data, _ = self.request(
            "PATCH",
            f"/repos/{self.repo}/issues/comments/{comment_id}",
            payload={"body": body},
        )
        return data if isinstance(data, dict) else {}


def redact_url(url: str) -> str:
    parsed = urllib.parse.urlsplit(url)
    query = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    redacted = []
    for key, value in query:
        if key.lower() in {"access_token", "token", "client_secret"}:
            redacted.append((key, "<redacted>"))
        else:
            redacted.append((key, value))
    return urllib.parse.urlunsplit(
        (parsed.scheme, parsed.netloc, parsed.path, urllib.parse.urlencode(redacted), parsed.fragment)
    )


def next_link(header: str) -> str | None:
    if not header:
        return None
    for part in header.split(","):
        sections = part.split(";")
        if len(sections) < 2:
            continue
        url = sections[0].strip()
        rel = ";".join(sections[1:])
        if 'rel="next"' in rel and url.startswith("<") and url.endswith(">"):
            return url[1:-1]
    return None


def load_payload(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise PublishError(f"Report JSON does not exist: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def infer_report_type(payload: dict[str, Any], explicit: str, report_json: Path) -> str:
    if explicit:
        return explicit
    report_type = str(payload.get("report_type") or "").strip()
    if report_type:
        return report_type
    return "compliance"


def report_label(report_type: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "-", report_type.strip().lower()).strip("-")
    return f"qa/report/{safe or 'unknown'}"


def report_display_name(report_type: str) -> str:
    return {
        "compliance": "Compliance Audit",
    }.get(report_type, report_type.replace("_", " ").title())


def artifact_name(report_type: str, explicit: str) -> str:
    if explicit:
        return explicit
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "-", report_type.strip().lower()).strip("-")
    return f"{safe or 'report'}-audit"


def run_url() -> str:
    explicit = os.environ.get("GITHUB_RUN_URL", "").strip()
    if explicit:
        return explicit
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com").rstrip("/")
    repo = os.environ.get("GITHUB_REPOSITORY", "").strip()
    run_id = os.environ.get("GITHUB_RUN_ID", "").strip()
    if repo and run_id:
        return f"{server}/{repo}/actions/runs/{run_id}"
    return ""


def finding_fingerprint(item: dict[str, Any]) -> str:
    fingerprint = str(item.get("fingerprint") or "").strip()
    if fingerprint:
        return fingerprint
    parts = [
        str(item.get("source") or item.get("rule_id") or ""),
        str(item.get("file") or ""),
        str(item.get("evidence") or item.get("line") or ""),
    ]
    return "|".join(parts)


def fingerprint_hash(fingerprint: str) -> str:
    return hashlib.sha1(fingerprint.encode("utf-8")).hexdigest()[:12]


def severity(item: dict[str, Any]) -> str:
    value = str(item.get("severity") or "info").strip().lower()
    return value if value in SEVERITY_ORDER else "info"


def summary_counts(payload: dict[str, Any]) -> dict[str, int]:
    raw = payload.get("summary")
    counts = {key: 0 for key in SEVERITY_ORDER}
    if isinstance(raw, dict):
        for key in counts:
            try:
                counts[key] = int(raw.get(key, 0))
            except (TypeError, ValueError):
                counts[key] = 0
        return counts
    for item in payload.get("findings", []):
        if isinstance(item, dict):
            counts[severity(item)] += 1
    return counts


def high_risk_count(payload: dict[str, Any]) -> int:
    counts = summary_counts(payload)
    return counts.get("critical", 0) + counts.get("high", 0)


def markdown_summary_line(payload: dict[str, Any]) -> str:
    counts = summary_counts(payload)
    return (
        f"critical {counts['critical']} | high {counts['high']} | "
        f"medium {counts['medium']} | low {counts['low']} | info {counts['info']}"
    )


def compact(value: Any, limit: int = 160) -> str:
    text = " ".join(str(value or "").split())
    if len(text) <= limit:
        return text
    return text[: max(0, limit - 1)] + "..."


def table_value(value: Any) -> str:
    text = str(value if value is not None else "").replace("\r", " ").replace("\n", "<br>")
    return text.replace("|", "\\|") or "-"


def plain_value(value: Any) -> str:
    text = str(value if value is not None else "").strip()
    return text or "-"


def location_text(item: dict[str, Any]) -> str:
    file_path = str(item.get("file") or "").strip()
    if not file_path:
        return "repository"
    if item.get("line") is not None:
        return f"{file_path}:{item.get('line')}"
    return file_path


def marker_for(item: dict[str, Any], report_type: str) -> str:
    state = {
        "fingerprint": finding_fingerprint(item),
        "fingerprint_hash": fingerprint_hash(finding_fingerprint(item)),
        "report_type": report_type,
        "source": str(item.get("source") or item.get("rule_id") or ""),
    }
    return f"<!-- {FINDING_MARKER}\n{json.dumps(state, ensure_ascii=False, separators=(',', ':'))}\n-->"


def parse_finding_marker(issue: dict[str, Any]) -> dict[str, Any] | None:
    body = str(issue.get("body") or "")
    match = FINDING_RE.search(body)
    if not match:
        return None
    try:
        data = json.loads(match.group(1))
    except json.JSONDecodeError:
        return None
    return data if isinstance(data, dict) else None


def issue_title(item: dict[str, Any], report_type: str) -> str:
    sev = SEVERITY_LABELS[severity(item)]
    title = compact(item.get("title") or finding_fingerprint(item), 120)
    prefix = report_display_name(report_type)
    return f"[{prefix}][{sev}] {title}"


def issue_body(item: dict[str, Any], report_type: str, run_link: str, artifact: str) -> str:
    generated_at = plain_value(item.get("generated_at") or "")
    if not generated_at:
        generated_at = plain_value(os.environ.get("GITHUB_RUN_STARTED_AT") or "")
    lines = [
        marker_for(item, report_type),
        "",
        "## Finding",
        "",
        "| Field | Value |",
        "| --- | --- |",
        f"| Report type | `{table_value(report_type)}` |",
        f"| Severity | `{table_value(severity(item))}` |",
        f"| Source | `{table_value(item.get('source') or item.get('rule_id'))}` |",
        f"| Fingerprint | `{table_value(finding_fingerprint(item))}` |",
        f"| Location | `{table_value(location_text(item))}` |",
        f"| Commit | `{table_value(item.get('commit_sha') or os.environ.get('GITHUB_SHA'))}` |",
        f"| Generated at | {table_value(generated_at)} |",
    ]
    if run_link:
        lines.append(f"| Latest run | [Open run]({run_link}) |")
    if artifact:
        lines.append(f"| Artifact | `{table_value(artifact)}` in the latest run |")
    lines.extend(["", "## Evidence", "", plain_value(item.get("evidence"))])
    if item.get("detail"):
        lines.extend(["", "## Detail", "", plain_value(item.get("detail"))])
    if item.get("recommendation"):
        lines.extend(["", "## Recommendation", "", plain_value(item.get("recommendation"))])
    lines.extend(
        [
            "",
            "## Automation Notes",
            "",
            (
                "This issue body is managed by the GitHub QA publisher. "
                f"Use labels such as `{MANUAL_READY_LABEL}` and issue comments for human workflow state."
            ),
            "",
        ]
    )
    return "\n".join(lines)


def issue_labels(item: dict[str, Any], report_type: str, status_key: str, existing: list[str] | None = None) -> list[str]:
    labels = set(existing or [])
    labels.add(BASE_LABEL)
    labels.add(report_label(report_type))
    labels.add(SEVERITY_ISSUE_LABELS[severity(item)])
    labels.add(STATUS_LABELS[status_key])
    labels -= set(STATUS_LABELS.values()) - {STATUS_LABELS[status_key]}
    labels -= set(SEVERITY_ISSUE_LABELS.values()) - {SEVERITY_ISSUE_LABELS[severity(item)]}
    return sorted(labels)


def label_names(issue: dict[str, Any]) -> list[str]:
    labels = issue.get("labels")
    if not isinstance(labels, list):
        return []
    out = []
    for label in labels:
        if isinstance(label, dict) and isinstance(label.get("name"), str):
            out.append(label["name"])
    return out


def current_status(labels: list[str]) -> str:
    for key, label in STATUS_LABELS.items():
        if label in labels:
            return key
    return ""


def ensure_labels(client: GitHubClient, names: list[str], *, dry_run: bool) -> None:
    if dry_run:
        return
    for name in sorted(set(names)):
        client.ensure_label(name)


def build_issue_index(issues: list[dict[str, Any]], report_type: str) -> dict[str, dict[str, Any]]:
    out: dict[str, dict[str, Any]] = {}
    for issue in issues:
        marker = parse_finding_marker(issue)
        if not marker:
            continue
        if marker.get("report_type") != report_type:
            continue
        fingerprint = str(marker.get("fingerprint") or "").strip()
        if fingerprint and fingerprint not in out:
            out[fingerprint] = issue
    return out


def transition_comment(
    *,
    status_key: str,
    item: dict[str, Any] | None,
    report_type: str,
    run_link: str,
) -> str:
    status_text = {
        "persistent": "Still reproduced by the latest automated run.",
        "not_reproduced": "Not reproduced by the latest automated run.",
        "recheck_passed": "Automated recheck passed; waiting for human confirmation.",
        "recheck_failed": "Automated recheck failed; the finding still reproduces.",
    }.get(status_key, "Updated by the latest automated run.")
    lines = [
        f"Automated {report_display_name(report_type)} update: **{status_text}**",
        "",
    ]
    if item:
        lines.extend(
            [
                f"- Severity: `{severity(item)}`",
                f"- Location: `{location_text(item)}`",
                f"- Commit: `{plain_value(item.get('commit_sha') or os.environ.get('GITHUB_SHA'))}`",
            ]
        )
    if run_link:
        lines.append(f"- Run: {run_link}")
    return "\n".join(lines)


def sync_issues(
    *,
    client: GitHubClient,
    payload: dict[str, Any],
    report_type: str,
    run_link: str,
    artifact: str,
    dry_run: bool,
) -> dict[str, Any]:
    rep_label = report_label(report_type)
    common_labels = [BASE_LABEL, rep_label, *SEVERITY_ISSUE_LABELS.values(), *STATUS_LABELS.values(), MANUAL_READY_LABEL]
    ensure_labels(client, common_labels, dry_run=dry_run)

    issues = [] if dry_run else client.list_issues(labels=[BASE_LABEL, rep_label], state="all")
    existing_by_fp = build_issue_index(issues, report_type)
    current_findings = [
        item for item in payload.get("findings", []) if isinstance(item, dict) and finding_fingerprint(item)
    ]
    current_by_fp = {finding_fingerprint(item): item for item in current_findings}
    result = {
        "dry_run": dry_run,
        "report_type": report_type,
        "current_findings": len(current_findings),
        "created": 0,
        "updated": 0,
        "reopened": 0,
        "not_reproduced": 0,
        "recheck_passed": 0,
        "recheck_failed": 0,
        "comments": 0,
        "skipped": 0,
    }

    for fingerprint, item in current_by_fp.items():
        issue = existing_by_fp.get(fingerprint)
        if issue is None:
            labels = issue_labels(item, report_type, "new")
            if dry_run:
                result["created"] += 1
                continue
            issue = client.create_issue(
                issue_title(item, report_type),
                issue_body(item, report_type, run_link, artifact),
                labels,
            )
            result["created"] += 1
            continue

        labels_before = label_names(issue)
        manual_ready = MANUAL_READY_LABEL in labels_before
        status_key = "recheck_failed" if manual_ready else "persistent"
        labels = issue_labels(item, report_type, status_key, labels_before)
        previous_status = current_status(labels_before)
        state = str(issue.get("state") or "open")
        if dry_run:
            result["updated"] += 1
            if state == "closed":
                result["reopened"] += 1
            if status_key == "recheck_failed":
                result["recheck_failed"] += 1
            continue
        client.update_issue(
            int(issue["number"]),
            title=issue_title(item, report_type),
            body=issue_body(item, report_type, run_link, artifact),
            state="open" if state == "closed" else None,
            labels=labels,
        )
        result["updated"] += 1
        if state == "closed":
            result["reopened"] += 1
        if status_key == "recheck_failed":
            result["recheck_failed"] += 1
        if previous_status != status_key or state == "closed":
            client.create_comment(
                int(issue["number"]),
                transition_comment(status_key=status_key, item=item, report_type=report_type, run_link=run_link),
            )
            result["comments"] += 1

    for fingerprint, issue in existing_by_fp.items():
        if fingerprint in current_by_fp:
            continue
        if str(issue.get("state") or "open") != "open":
            continue
        labels_before = label_names(issue)
        manual_ready = MANUAL_READY_LABEL in labels_before
        status_key = "recheck_passed" if manual_ready else "not_reproduced"
        previous_status = current_status(labels_before)
        labels = set(labels_before)
        labels.add(BASE_LABEL)
        labels.add(rep_label)
        labels.add(STATUS_LABELS[status_key])
        labels -= set(STATUS_LABELS.values()) - {STATUS_LABELS[status_key]}
        if dry_run:
            if status_key == "recheck_passed":
                result["recheck_passed"] += 1
            else:
                result["not_reproduced"] += 1
            continue
        client.update_issue(int(issue["number"]), labels=sorted(labels))
        if status_key == "recheck_passed":
            result["recheck_passed"] += 1
        else:
            result["not_reproduced"] += 1
        if previous_status != status_key:
            client.create_comment(
                int(issue["number"]),
                transition_comment(status_key=status_key, item=None, report_type=report_type, run_link=run_link),
            )
            result["comments"] += 1

    return result


def top_findings(payload: dict[str, Any], limit: int | None = 10) -> list[dict[str, Any]]:
    findings = [item for item in payload.get("findings", []) if isinstance(item, dict)]
    sorted_findings = sorted(
        findings,
        key=lambda item: (
            SEVERITY_ORDER.get(severity(item), 99),
            str(item.get("source") or item.get("rule_id") or ""),
            str(item.get("file") or ""),
        ),
    )
    return sorted_findings if limit is None else sorted_findings[:limit]


def summary_markdown(
    *,
    payload: dict[str, Any],
    report_type: str,
    run_link: str,
    artifact: str,
    sync_result: dict[str, Any] | None,
) -> str:
    lines = [
        f"## {report_display_name(report_type)}",
        "",
        f"- Result: {markdown_summary_line(payload)}",
        f"- Generated at: {plain_value(payload.get('generated_at'))}",
        f"- Commit: `{plain_value(payload.get('commit_sha') or os.environ.get('GITHUB_SHA'))}`",
    ]
    if run_link:
        lines.append(f"- Run: {run_link}")
    if artifact:
        lines.append(f"- Artifact: `{artifact}`")
    if sync_result:
        lines.append(
            "- Issue sync: "
            f"created {sync_result.get('created', 0)}, "
            f"updated {sync_result.get('updated', 0)}, "
            f"not reproduced {sync_result.get('not_reproduced', 0)}, "
            f"recheck passed {sync_result.get('recheck_passed', 0)}, "
            f"recheck failed {sync_result.get('recheck_failed', 0)}"
        )
    usage_line = ai_usage_line(payload)
    if usage_line:
        lines.append(usage_line)
    lines.append("")
    findings = top_findings(payload, limit=None)
    if findings:
        lines.extend(["### All Findings", "", "| Severity | Source | Title | Location |", "| --- | --- | --- | --- |"])
        for item in findings:
            lines.append(
                f"| `{table_value(severity(item))}` | `{table_value(item.get('source') or item.get('rule_id'))}` | "
                f"{table_value(compact(item.get('title'), 100))} | `{table_value(location_text(item))}` |"
            )
        lines.append("")
    else:
        lines.extend(["No confirmed findings in this run.", ""])
    return "\n".join(lines)


def append_step_summary(markdown: str) -> bool:
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY", "").strip()
    if not summary_path:
        return False
    path = Path(summary_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as out:
        out.write(markdown)
        out.write("\n")
    return True


def feed_issue_title(report_type: str) -> str:
    return f"[QA] {report_display_name(report_type)} report feed"


def feed_issue_body(report_type: str) -> str:
    state = json.dumps({"report_type": report_type}, ensure_ascii=False, separators=(",", ":"))
    return "\n".join(
        [
            f"<!-- {FEED_MARKER} {state} -->",
            "",
            f"# {report_display_name(report_type)} Report Feed",
            "",
            "This issue is managed by the GitHub QA publisher.",
            "Each automated run adds a summary comment here so GitHub notifications stay inside the repository.",
            "",
        ]
    )


def should_mention(mode: str, payload: dict[str, Any]) -> bool:
    if mode == "always":
        return True
    if mode == "high-risk":
        return high_risk_count(payload) > 0
    return False


def ai_usage_line(payload: dict[str, Any]) -> str:
    usage = payload.get("ai_usage")
    if not isinstance(usage, dict):
        return ""
    line = (
        f"- AI spend: ${float(usage.get('cost') or 0.0):.4f} this run "
        f"({int(usage.get('calls') or 0)} calls, "
        f"{int(usage.get('input_tokens') or 0):,} in / {int(usage.get('output_tokens') or 0):,} out tokens)"
    )
    key_spend = usage.get("key_spend")
    if key_spend is not None:
        line += f"; key total ${float(key_spend):.2f}"
        max_budget = usage.get("max_budget")
        if max_budget is not None:
            line += f" / budget ${float(max_budget):.2f} (remaining ${float(max_budget) - float(key_spend):.2f})"
        else:
            line += " (no key-level budget cap)"
    return line


def feed_comment(
    *,
    payload: dict[str, Any],
    report_type: str,
    run_link: str,
    artifact: str,
    sync_result: dict[str, Any] | None,
    mentions: str,
) -> str:
    lines = []
    if mentions:
        lines.extend([mentions, ""])
    lines.extend(
        [
            f"Automated {report_display_name(report_type)} run completed.",
            "",
            f"- Result: {markdown_summary_line(payload)}",
            f"- Generated at: {plain_value(payload.get('generated_at'))}",
            f"- Commit: `{plain_value(payload.get('commit_sha') or os.environ.get('GITHUB_SHA'))}`",
        ]
    )
    if run_link:
        lines.append(f"- Run: {run_link}")
    if artifact:
        lines.append(f"- Artifact: `{artifact}`")
    if sync_result:
        lines.append(
            "- Issue sync: "
            f"created {sync_result.get('created', 0)}, "
            f"updated {sync_result.get('updated', 0)}, "
            f"not reproduced {sync_result.get('not_reproduced', 0)}, "
            f"recheck passed {sync_result.get('recheck_passed', 0)}, "
            f"recheck failed {sync_result.get('recheck_failed', 0)}"
        )
    usage_line = ai_usage_line(payload)
    if usage_line:
        lines.append(usage_line)
    top = top_findings(payload, limit=5)
    if top:
        lines.extend(["", "Top findings:"])
        for item in top:
            lines.append(
                f"- `{severity(item)}` {compact(item.get('title'), 100)} "
                f"({location_text(item)})"
            )
    return "\n".join(lines)


def comment_report_feed(
    *,
    client: GitHubClient,
    payload: dict[str, Any],
    report_type: str,
    run_link: str,
    artifact: str,
    sync_result: dict[str, Any] | None,
    mention_mode: str,
    dry_run: bool,
) -> dict[str, Any]:
    rep_label = report_label(report_type)
    ensure_labels(client, [BASE_LABEL, REPORT_FEED_LABEL, rep_label], dry_run=dry_run)
    mentions = os.environ.get("QA_NOTIFY_MENTIONS", "").strip()
    if not should_mention(mention_mode, payload):
        mentions = ""
    result = {"dry_run": dry_run, "commented": False, "issue_number": None}
    if dry_run:
        result["commented"] = True
        return result

    title = feed_issue_title(report_type)
    issues = client.list_issues(labels=[REPORT_FEED_LABEL, rep_label], state="all")
    feed = next((issue for issue in issues if str(issue.get("title") or "") == title), None)
    if feed is None:
        feed = client.create_issue(title, feed_issue_body(report_type), [BASE_LABEL, REPORT_FEED_LABEL, rep_label])
    else:
        state = str(feed.get("state") or "open")
        client.update_issue(
            int(feed["number"]),
            title=title,
            body=feed_issue_body(report_type),
            state="open" if state == "closed" else None,
            labels=sorted(set(label_names(feed)) | {BASE_LABEL, REPORT_FEED_LABEL, rep_label}),
        )
    client.create_comment(
        int(feed["number"]),
        feed_comment(
            payload=payload,
            report_type=report_type,
            run_link=run_link,
            artifact=artifact,
            sync_result=sync_result,
            mentions=mentions,
        ),
    )
    result["commented"] = True
    result["issue_number"] = int(feed["number"])
    return result


def pr_report_marker(report_type: str) -> str:
    state = json.dumps({"report_type": report_type}, ensure_ascii=False, separators=(",", ":"))
    return f"<!-- {PR_REPORT_MARKER} {state} -->"


def parse_pr_report_marker(comment: dict[str, Any]) -> dict[str, Any] | None:
    match = PR_REPORT_RE.search(str(comment.get("body") or ""))
    if not match:
        return None
    try:
        state = json.loads(match.group(1))
    except json.JSONDecodeError:
        return None
    return state if isinstance(state, dict) else None


def pr_comment_body(
    *,
    payload: dict[str, Any],
    report_type: str,
    report_markdown: str,
    run_link: str,
    artifact: str,
) -> str:
    lines = [
        pr_report_marker(report_type),
        "",
        f"## {report_display_name(report_type)} report",
        "",
        "This comment is updated automatically for the latest PR commit.",
        "",
        f"- Result: {markdown_summary_line(payload)}",
        f"- Commit: `{plain_value(payload.get('commit_sha') or os.environ.get('GITHUB_SHA'))}`",
    ]
    if run_link:
        lines.append(f"- Run: {run_link}")
    if artifact:
        lines.append(f"- Artifact: `{artifact}`")
    lines.extend(["", report_markdown.strip(), ""])
    rendered = "\n".join(lines)
    if len(rendered) <= GITHUB_COMMENT_LIMIT:
        return rendered

    footer = "\n\n> The report exceeded GitHub's comment size limit and was truncated."
    if run_link:
        footer += f" The complete artifact is available from {run_link}."
    available = GITHUB_COMMENT_LIMIT - len(footer)
    return rendered[:available].rstrip() + footer


def comment_pull_request(
    *,
    client: GitHubClient,
    pr_number: int,
    payload: dict[str, Any],
    report_type: str,
    report_markdown: str,
    run_link: str,
    artifact: str,
    dry_run: bool,
) -> dict[str, Any]:
    if pr_number <= 0:
        raise PublishError(f"Invalid pull request number: {pr_number}")
    body = pr_comment_body(
        payload=payload,
        report_type=report_type,
        report_markdown=report_markdown,
        run_link=run_link,
        artifact=artifact,
    )
    result = {
        "dry_run": dry_run,
        "commented": False,
        "pr_number": pr_number,
        "action": "create",
        "comment_id": None,
        "body_length": len(body),
    }
    if dry_run:
        result["commented"] = True
        return result

    comments = client.list_comments(pr_number)
    existing = next(
        (
            comment
            for comment in comments
            if (parse_pr_report_marker(comment) or {}).get("report_type") == report_type
        ),
        None,
    )
    if existing is None:
        published = client.create_comment(pr_number, body)
    else:
        result["action"] = "update"
        published = client.update_comment(int(existing["id"]), body)
    result["commented"] = True
    if published.get("id") is not None:
        result["comment_id"] = int(published["id"])
    return result


def write_result(path: Path, result: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report-json", required=True)
    parser.add_argument("--report-markdown", required=True)
    parser.add_argument("--result-out", default="")
    parser.add_argument("--report-type", default="")
    parser.add_argument("--artifact-name", default="")
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", ""))
    parser.add_argument("--api-url", default=os.environ.get("GITHUB_API_URL", "https://api.github.com"))
    parser.add_argument("--sync-issues", action="store_true")
    parser.add_argument("--comment-report-feed", action="store_true")
    parser.add_argument("--comment-pr", type=int, default=0)
    parser.add_argument("--mention-on", choices=("always", "high-risk", "never"), default="high-risk")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    report_json = REPO_ROOT / args.report_json
    report_markdown = REPO_ROOT / args.report_markdown
    payload = load_payload(report_json)
    if not report_markdown.exists():
        raise PublishError(f"Report Markdown does not exist: {report_markdown}")
    report_markdown_text = report_markdown.read_text(encoding="utf-8")
    report_type = infer_report_type(payload, args.report_type, report_json)
    artifact = artifact_name(report_type, args.artifact_name)
    link = run_url()
    result_path = REPO_ROOT / (args.result_out or f".temp/{report_type}/github-publish-result.json")

    client: GitHubClient | None = None
    if args.sync_issues or args.comment_report_feed or args.comment_pr:
        if not args.dry_run and (not args.repo or not args.token):
            raise PublishError("GITHUB_REPOSITORY and GITHUB_TOKEN are required for GitHub issue publishing.")
        client = GitHubClient(repo=args.repo or "dry-run/repo", token=args.token or "dry-run", api_url=args.api_url)

    sync_result: dict[str, Any] | None = None
    if args.sync_issues:
        assert client is not None
        sync_result = sync_issues(
            client=client,
            payload=payload,
            report_type=report_type,
            run_link=link,
            artifact=artifact,
            dry_run=args.dry_run,
        )

    summary = summary_markdown(
        payload=payload,
        report_type=report_type,
        run_link=link,
        artifact=artifact,
        sync_result=sync_result,
    )
    summary_written = append_step_summary(summary)

    feed_result: dict[str, Any] | None = None
    if args.comment_report_feed:
        assert client is not None
        feed_result = comment_report_feed(
            client=client,
            payload=payload,
            report_type=report_type,
            run_link=link,
            artifact=artifact,
            sync_result=sync_result,
            mention_mode=args.mention_on,
            dry_run=args.dry_run,
        )

    pr_comment_result: dict[str, Any] | None = None
    if args.comment_pr:
        assert client is not None
        pr_comment_result = comment_pull_request(
            client=client,
            pr_number=args.comment_pr,
            payload=payload,
            report_type=report_type,
            report_markdown=report_markdown_text,
            run_link=link,
            artifact=artifact,
            dry_run=args.dry_run,
        )

    result = {
        "status": "dry-run" if args.dry_run else "published",
        "report_type": report_type,
        "report_json": str(report_json.relative_to(REPO_ROOT)),
        "report_markdown": str(report_markdown.relative_to(REPO_ROOT)),
        "run_url": link,
        "artifact": artifact,
        "step_summary_written": summary_written,
        "issue_sync": sync_result,
        "report_feed": feed_result,
        "pr_comment": pr_comment_result,
    }
    write_result(result_path, result)
    print(f"GitHub report publish status: {result['status']}")
    print(f"report_type: {report_type}")
    if sync_result:
        print(f"issue sync: {json.dumps(sync_result, ensure_ascii=False)}")
    if feed_result:
        print(f"report feed: {json.dumps(feed_result, ensure_ascii=False)}")
    if pr_comment_result:
        print(f"PR comment: {json.dumps(pr_comment_result, ensure_ascii=False)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except PublishError as exc:
        print(f"github publish error: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc
