#!/usr/bin/env python3
"""Repository-wide and pull-request compliance audit.

The audit scans the complete tracked tree for repository findings. On pull
requests it also validates the pull request description against the base
revision's template.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import html
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parent.parent

SEVERITY_ORDER = {
    "critical": 0,
    "high": 1,
    "medium": 2,
    "low": 3,
    "info": 4,
}

SEVERITY_LABELS = {
    "critical": "严重",
    "high": "高",
    "medium": "中",
    "low": "低",
    "info": "信息",
}

RULE_STANDARDS = {
    "LIC-001": "根目录必须存在项目自身许可证文件，例如 LICENSE、LICENSE.md 或 COPYING，否则项目自身授权边界不明确。",
    "LIC-002": "每个已跟踪的 vendored 第三方依赖必须在 NOTICE 中登记名称、版本、许可证、用途和获取方式。",
    "LIC-003": "每个已跟踪的 vendored 第三方依赖必须保留许可证/NOTICE 文本，或在根目录 NOTICE 中明确记录许可证全文嵌入的文件。",
    "LIC-004": "运行时/SDK 依赖闭包只允许宽松许可证；Studio 的弱 copyleft 依赖必须记录链接方式和发布义务；GPL/AGPL 禁止进入任何产品闭包。",
    "TEL-001": "产品或演示可执行程序不得默认链接会采集或上报身份信息的遥测模块，除非有明确告知和显式同意。",
    "TEL-002": "程序启动阶段不得默认触发遥测入口，除非有明确告知和显式同意。",
    "TEL-003": "遥测不得默认采集稳定设备或账号标识，例如 MachineGuid、Windows SID、本地 IP、第三方账号标识、用户名或主机名。",
    "TEL-004": "遥测记录不得默认写入隐藏或系统属性文件。",
    "TEL-005": "遥测记录不得默认写入 NTFS 备用数据流等不透明位置。",
    "TEL-006": "遥测记录不得默认写入注册表缓存等不透明位置。",
    "PRIV-001": "面向发布的构建产物不得嵌入本地主机名、用户名等个人环境信息。",
    "DEP-001": "FetchContent 下载归档必须使用 URL_HASH SHA256 或等价内容完整性校验。",
    "DEP-002": "vendored 依赖的编译能力范围应与项目声明一致；若未裁剪，需在发布声明中说明。",
    "DEP-003": "文件加载器实际支持的格式必须与公开声明一致，或限制为声明范围内的格式。",
    "DEP-005": "通过 CMake 获取 Git 仓库时必须把 GIT_TAG 固定到完整 40 位 commit SHA，不得只使用分支或可变标签。",
    "SEC-002": "API key、token、密码等凭据不得明文或仅 Base64 写入 QSettings、INI 等普通配置存储。",
    "GOV-001": "PR 描述必须保留目标分支 PR 模板的章节与全部条目条数；条目可只保留单一语言，但不得删减条数，以便评审者看到完整待确认项。",
    "GOV-002": "PR 描述的摘要、验证信息、各章节勾选结果以及已勾选项的说明内容必须填写，不得留空。",
    "GOV-003": "PR 描述条目文案应与模板语义一致；该项仅提示人工核对，不作为阻断条件。",
    "GOV-004": "PR 引入的每个非 merge commit 必须带有合法行首 Signed-off-by: Name <email>（trailer email 像邮箱即可，不比 author/committer 身份）；模板勾选不能替代 trailer。",
    "LOCAL-001": "本地 gitignored 配置存在时只作为提醒；不得提交、打包或发布。",
    "LOCAL-002": "本地未跟踪自动化目录存在时只作为提醒；不得默认进入发布范围。",
}

CATEGORY_LABELS = {
    "license": "许可证",
    "supply-chain": "供应链",
    "privacy": "隐私",
    "release-boundary": "发布边界",
    "repository-governance": "仓库治理",
    "secret": "密钥",
    "local-secret": "本地密钥",
    "local-working-tree": "本地工作区",
}

THEME_GROUPS = [
    (
        "授权与来源",
        "直接影响开源商用授权边界，包括项目许可证、第三方/历史代码来源、资产来源记录。",
        {"license"},
    ),
    (
        "隐私与数据处理",
        "涉及遥测、设备/账号标识、隐藏持久化、构建身份、对话记录等个人信息或敏感数据处理。",
        {"privacy"},
    ),
    (
        "发布边界与私有资料",
        "涉及个人自动化、私有工作流、本地状态目录等是否应随项目公开发布的问题。",
        {"release-boundary", "local-working-tree"},
    ),
    (
        "供应链与第三方依赖",
        "涉及依赖固定、下载完整性、第三方能力范围与声明一致性。",
        {"supply-chain"},
    ),
    (
        "仓库治理",
        "涉及 PR 描述、commit DCO 签名、受保护文件与仓库结构边界，防止审查约束在提交中被削弱或绕过。",
        {"repository-governance"},
    ),
    (
        "密钥与本地配置",
        "涉及产品凭据存储行为和本地配置状态。",
        {"secret", "local-secret"},
    ),
]

TEXT_SUFFIXES = {
    ".bat",
    ".c",
    ".cc",
    ".cmake",
    ".cmd",
    ".cpp",
    ".cs",
    ".css",
    ".filters",
    ".h",
    ".htm",
    ".html",
    ".hpp",
    ".in",
    ".ini",
    ".js",
    ".json",
    ".m",
    ".md",
    ".mm",
    ".props",
    ".ps1",
    ".py",
    ".qrc",
    ".rc",
    ".sh",
    ".sql",
    ".svg",
    ".targets",
    ".toml",
    ".txt",
    ".ts",
    ".ui",
    ".vcproj",
    ".vcxproj",
    ".xml",
    ".yml",
    ".yaml",
}

MAX_TEXT_BYTES = 10_000_000

VENDORED_DIRECTORY_NAMES = {"thirdparty", "third_party", "vendor", "vendored"}
LICENSE_FILENAMES = re.compile(
    r"(?i)^(license|licence|copying|notice|copyright)(?:\.[^/]+)?$"
)
PULL_REQUEST_TEMPLATE_PATH = ".github/PULL_REQUEST_TEMPLATE.md"
CONTRIBUTING_DCO_PATH = ".github/CONTRIBUTING.md"
SHA40_RE = re.compile(r"[0-9a-fA-F]{40}")
SIGNED_OFF_BY_LINE_RE = re.compile(
    r"^Signed-off-by: ([^<\n]{1,256}) <([^>\n]{1,254})>\s*$",
    re.IGNORECASE,
)
DCO_GIT_LOG_FIELD_SEP = "\x1f"
DCO_GIT_LOG_RECORD_SEP = "\x1e"
DCO_MAX_SIGN_OFF_LINE = 512
DCO_PARSE_ERROR = "dco-parse-error"
DCO_GIT_LOG_FORMAT = DCO_GIT_LOG_FIELD_SEP.join(
    ("%H", "%P", "%an", "%ae", "%cn", "%ce", "%B")
) + DCO_GIT_LOG_RECORD_SEP
DCO_EMAIL_ATEXT = r"[A-Za-z0-9!#$%&'*+/=?^_`{|}~-]"
DCO_EMAIL_DOMAIN_LABEL = r"[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?"
DCO_EMAIL_LOCAL_PART_RE = re.compile(rf"^{DCO_EMAIL_ATEXT}+(?:\.{DCO_EMAIL_ATEXT}+)*$")
DCO_EMAIL_DOMAIN_RE = re.compile(
    rf"^{DCO_EMAIL_DOMAIN_LABEL}(?:\.{DCO_EMAIL_DOMAIN_LABEL})*\.[A-Za-z]{{2,}}$"
)
DCO_EMAIL_MAX_TOTAL = 254
DCO_EMAIL_MAX_LOCAL_PART = 64
DCO_EMAIL_MAX_DOMAIN = 254
DCO_EMAIL_MAX_DOMAIN_LABEL = 63
DCO_FINDING_EVIDENCE_LIMIT = 8

PR_HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*$")
PR_ITEM_RE = re.compile(r"^[-*]\s+\[([ xX])\]\s*(.*)$")
PR_BULLET_RE = re.compile(r"^[-*]\s+(.*)$")
PR_QUOTE_RE = re.compile(r"^>\s*(.*)$")
PR_PUNCTUATION_MAP = str.maketrans(
    {
        "“": '"',
        "”": '"',
        "‘": "'",
        "’": "'",
        "：": ":",
        "（": "(",
        "）": ")",
    }
)
ALL_CHECKED_SECTION_HINTS = ("提交前检查", "pre-submission")

PRODUCT_SOURCE_PREFIXES = (
    "HarkenEngine/",
    "HarkenIntegration/",
    "HarkenStudio/",
)

PRODUCT_SOURCE_EXCLUDED_PARTS = (
    "/ThirdParty/",
    "/Test/",
)


@dataclasses.dataclass
class Finding:
    rule_id: str
    severity: str
    category: str
    title: str
    detail: str
    recommendation: str
    file: str | None = None
    line: int | None = None
    evidence: str | None = None

    def as_dict(self) -> dict:
        data = dataclasses.asdict(self)
        data["fingerprint"] = finding_fingerprint(data)
        return data


@dataclasses.dataclass(frozen=True)
class PullRequestContext:
    base_sha: str
    body: str
    head_sha: str | None


@dataclasses.dataclass(frozen=True)
class DcoSignOff:
    name: str
    email: str


@dataclasses.dataclass(frozen=True)
class DcoCommit:
    sha: str
    parents: tuple[str, ...]
    author_name: str
    author_email: str
    committer_name: str
    committer_email: str
    message: str


@dataclasses.dataclass
class MarkdownSection:
    level: int
    title: str
    lines: list[str]


@dataclasses.dataclass(frozen=True)
class SectionSpec:
    title: str
    aliases: tuple[str, ...]
    items: int
    fields: int
    require_all_checked: bool
    item_aliases: tuple[tuple[str, ...], ...]
    item_detail_prompts: tuple[tuple[tuple[str, ...], ...], ...]


@dataclasses.dataclass(frozen=True)
class BodyItem:
    checked: bool
    text: str
    own_content: str
    has_colon: bool
    details: list[str]


def git_executable() -> str:
    harken_git_dir = os.environ.get("HARKEN_GIT_DIR")
    if harken_git_dir:
        candidate = Path(harken_git_dir) / "git"
        if candidate.exists():
            return str(candidate)
    return "git"


def run_git(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [git_executable(), *args],
        cwd=REPO_ROOT,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def tracked_files() -> list[str]:
    result = run_git(["ls-files"])
    if result.returncode != 0:
        raise RuntimeError(f"git ls-files failed: {result.stderr.strip()}")
    return [line.strip().replace("\\", "/") for line in result.stdout.splitlines() if line.strip()]


def load_pull_request_context() -> PullRequestContext | None:
    event_path_value = os.environ.get("GITHUB_EVENT_PATH")
    if not event_path_value:
        return None

    event_path = Path(event_path_value)
    try:
        event = json.loads(event_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"failed to read GitHub event payload: {event_path}") from exc

    pull_request = event.get("pull_request")
    if pull_request is None:
        return None
    if not isinstance(pull_request, dict):
        raise RuntimeError("GitHub pull_request payload must be an object")

    base = pull_request.get("base")
    base_sha = base.get("sha") if isinstance(base, dict) else None
    head = pull_request.get("head")
    head_sha = head.get("sha") if isinstance(head, dict) else None
    body = pull_request.get("body")
    if not isinstance(base_sha, str) or not SHA40_RE.fullmatch(base_sha):
        raise RuntimeError("GitHub pull_request payload has an invalid base.sha")
    if body is not None and not isinstance(body, str):
        raise RuntimeError("GitHub pull_request payload body must be a string or null")
    if not isinstance(head_sha, str) or not SHA40_RE.fullmatch(head_sha):
        head_sha = None
    return PullRequestContext(base_sha=base_sha, body=body or "", head_sha=head_sha)


def read_git_text(revision: str, rel: str) -> str:
    result = run_git(["show", f"{revision}:{rel}"])
    if result.returncode != 0:
        raise RuntimeError(
            f"required file is unavailable at {revision}: {rel}: {result.stderr.strip()}"
        )
    return result.stdout


def git_status_short() -> list[str]:
    result = run_git(["status", "--short"])
    if result.returncode != 0:
        return []
    return [line.rstrip() for line in result.stdout.splitlines()]


def read_text(rel: str, max_bytes: int = MAX_TEXT_BYTES) -> str:
    path = REPO_ROOT / rel
    try:
        if path.stat().st_size > max_bytes:
            return ""
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def is_text_file(rel: str) -> bool:
    path = Path(rel)
    if path.suffix.lower() in TEXT_SUFFIXES:
        return True
    return path.name in {"CMakeLists.txt", ".gitignore", ".gitattributes", "VERSION"}


def exists(rel: str) -> bool:
    return (REPO_ROOT / rel).exists()


def find_line(rel: str, pattern: str) -> tuple[int | None, str | None]:
    text = read_text(rel)
    for index, line in enumerate(text.splitlines(), start=1):
        if pattern in line:
            return index, line.strip()
    return None, None


def add(
    findings: list[Finding],
    rule_id: str,
    severity: str,
    category: str,
    title: str,
    detail: str,
    recommendation: str,
    file: str | None = None,
    line: int | None = None,
    evidence: str | None = None,
) -> None:
    findings.append(
        Finding(
            rule_id=rule_id,
            severity=severity,
            category=category,
            title=title,
            detail=detail,
            recommendation=recommendation,
            file=file,
            line=line,
            evidence=evidence,
        )
    )


def normalize_pr_text(value: str) -> str:
    return re.sub(r"\s+", " ", value.translate(PR_PUNCTUATION_MAP)).strip()


def strip_html_comments(text: str) -> str:
    return re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL)


def split_markdown_sections(text: str) -> list[MarkdownSection]:
    sections: list[MarkdownSection] = []
    for raw in strip_html_comments(text).splitlines():
        heading = PR_HEADING_RE.match(raw.strip())
        if heading:
            sections.append(
                MarkdownSection(len(heading.group(1)), normalize_pr_text(heading.group(2)), [])
            )
        elif sections:
            sections[-1].lines.append(raw)
    return sections


def list_entry_indent(raw: str) -> int | None:
    stripped = raw.lstrip()
    if not PR_ITEM_RE.match(stripped) and not PR_BULLET_RE.match(stripped):
        return None
    expanded = raw.expandtabs(4)
    return len(expanded) - len(expanded.lstrip())


def top_level_list_indent(lines: list[str]) -> int | None:
    indents = [
        indent
        for raw in lines
        if (indent := list_entry_indent(raw)) is not None
    ]
    return min(indents, default=None)


def count_section_structure(lines: list[str]) -> tuple[int, int]:
    items = fields = 0
    top_level_indent = top_level_list_indent(lines)
    for raw in lines:
        if list_entry_indent(raw) != top_level_indent:
            continue
        stripped = raw.strip()
        if PR_ITEM_RE.match(stripped):
            items += 1
        elif PR_BULLET_RE.match(stripped):
            fields += 1
    return items, fields


def heading_aliases(title: str) -> tuple[str, ...]:
    parts = tuple(part for part in (p.strip() for p in title.split(" / ")) if part)
    return parts or (title,)


def alias_hit(title: str, aliases: Iterable[str]) -> bool:
    lowered = title.lower()
    return any(alias.lower() in lowered for alias in aliases)


def pull_request_section_specs(template: str) -> list[SectionSpec]:
    specs: list[SectionSpec] = []
    for section in split_markdown_sections(template):
        if section.level < 2:
            continue
        items, fields = count_section_structure(section.lines)
        aliases = heading_aliases(section.title)
        specs.append(
            SectionSpec(
                title=section.title,
                aliases=aliases,
                items=items,
                fields=fields,
                require_all_checked=any(
                    hint in alias.lower()
                    for alias in aliases
                    for hint in ALL_CHECKED_SECTION_HINTS
                ),
                item_aliases=tuple(template_item_aliases(section.lines)),
                item_detail_prompts=tuple(item_detail_prompts(section.lines)),
            )
        )
    return specs


def template_item_aliases(lines: list[str]) -> list[tuple[str, ...]]:
    entries: list[list[str]] = []
    for raw in lines:
        stripped = raw.strip()
        item = PR_ITEM_RE.match(stripped)
        if item:
            entries.append([item.group(2), ""])
        elif entries and stripped.startswith("/ "):
            entries[-1][1] = stripped[2:]
    return [item_aliases(text, tail) for text, tail in entries]


def item_detail_prompts(lines: list[str]) -> list[tuple[tuple[str, ...], ...]]:
    prompts: list[list[tuple[str, ...]]] = []
    for raw in lines:
        stripped = raw.strip()
        if PR_ITEM_RE.match(stripped):
            prompts.append([])
            continue
        quote = PR_QUOTE_RE.match(stripped) if prompts else None
        if quote:
            prompts[-1].append(item_aliases(quote.group(1), ""))
    return [tuple(entry) for entry in prompts]


def item_aliases(text: str, tail: str) -> tuple[str, ...]:
    normalized = normalize_pr_text(text)
    aliases = [normalized]
    for separator in re.finditer(r" / ", normalized):
        remainder = normalized[separator.end():]
        if remainder and sum(char.isascii() for char in remainder) / len(remainder) > 0.8:
            aliases.append(normalized[: separator.start()])
            aliases.append(remainder)
            break
    if tail:
        aliases.append(normalize_pr_text(tail))
    return tuple(dict.fromkeys(alias for alias in aliases if alias))


def label_content(label: str, following: list[str], parent_indent: int) -> str:
    normalized = normalize_pr_text(label)
    _, separator, after = normalized.rpartition(":")
    parts = [after] if separator else []
    for raw in following:
        stripped = raw.strip()
        if not stripped:
            break
        if PR_HEADING_RE.match(stripped):
            break
        item = PR_ITEM_RE.match(stripped)
        bullet = PR_BULLET_RE.match(stripped)
        if item or bullet:
            indent = list_entry_indent(raw)
            if indent is None or indent <= parent_indent:
                break
            parts.append(item.group(2) if item else bullet.group(1))
            continue
        if PR_QUOTE_RE.match(stripped):
            break
        parts.append(stripped)
    return normalize_pr_text(" ".join(parts))


def quote_block_text(label: str, following: list[str]) -> str:
    parts = [label]
    for raw in following:
        stripped = raw.strip()
        if not stripped:
            break
        if PR_HEADING_RE.match(stripped) or PR_ITEM_RE.match(stripped):
            break
        if PR_BULLET_RE.match(stripped):
            break
        quote = PR_QUOTE_RE.match(stripped)
        parts.append(quote.group(1) if quote else stripped)
    return normalize_pr_text(" ".join(parts))


def item_detail_contents(following: list[str]) -> list[str]:
    contents: list[str] = []
    for index, raw in enumerate(following):
        stripped = raw.strip()
        if not stripped:
            continue
        if PR_HEADING_RE.match(stripped) or PR_ITEM_RE.match(stripped):
            break
        quote = PR_QUOTE_RE.match(stripped)
        if quote:
            contents.append(quote_block_text(quote.group(1), following[index + 1:]))
    return contents


def detail_tokens(text: str) -> tuple[str, ...]:
    return tuple(re.findall(r"[^\W_]+", text.casefold()))


def detail_answered(content: str, prompts: tuple[str, ...]) -> bool:
    content_tokens = detail_tokens(content)
    for prompt in prompts:
        prompt_tokens = detail_tokens(prompt)
        if content_tokens[: len(prompt_tokens)] == prompt_tokens:
            return len(content_tokens) > len(prompt_tokens)
    return bool(content_tokens)


def section_entries(lines: list[str]) -> tuple[list[BodyItem], list[str]]:
    items: list[BodyItem] = []
    fields: list[str] = []
    top_level_indent = top_level_list_indent(lines)
    for index, raw in enumerate(lines):
        indent = list_entry_indent(raw)
        if indent is None or indent != top_level_indent:
            continue
        stripped = raw.strip()
        following = lines[index + 1:]
        item = PR_ITEM_RE.match(stripped)
        if item:
            text = normalize_pr_text(item.group(2))
            items.append(
                BodyItem(
                    checked=item.group(1).lower() == "x",
                    text=text,
                    own_content=label_content(item.group(2), following, indent),
                    has_colon=":" in text,
                    details=item_detail_contents(following),
                )
            )
        else:
            bullet = PR_BULLET_RE.match(stripped)
            if bullet:
                fields.append(label_content(bullet.group(1), following, indent))
    return items, fields


def section_prose(lines: list[str]) -> str:
    kept = [
        raw.strip()
        for raw in lines
        if raw.strip() and not PR_QUOTE_RE.match(raw.strip())
    ]
    return normalize_pr_text(" ".join(kept))


def match_template_items(
    items: list[BodyItem],
    aliases_by_item: tuple[tuple[str, ...], ...],
) -> tuple[list[int | None], list[int]]:
    matches: list[int | None] = [None] * len(items)
    claimed: set[int] = set()
    pending: list[int] = []
    duplicates: set[int] = set()
    for body_index, entry in enumerate(items):
        candidates = [
            template_index
            for template_index, aliases in enumerate(aliases_by_item)
            if alias_hit(entry.text, aliases)
        ]
        if len(candidates) != 1:
            pending.append(body_index)
            continue
        template_index = candidates[0]
        matches[body_index] = template_index
        if template_index in claimed:
            duplicates.add(template_index)
        else:
            claimed.add(template_index)

    remaining = [index for index in range(len(aliases_by_item)) if index not in claimed]
    for body_index, template_index in zip(pending, remaining):
        matches[body_index] = template_index
    return matches, sorted(duplicates)


def rule_pull_request_governance(
    context: PullRequestContext | None,
    findings: list[Finding],
) -> None:
    if context is None:
        return

    template = read_git_text(context.base_sha, PULL_REQUEST_TEMPLATE_PATH)
    specs = pull_request_section_specs(template)
    body_sections = split_markdown_sections(context.body)

    matched: dict[str, MarkdownSection] = {}
    used: set[int] = set()
    for spec in specs:
        for index, section in enumerate(body_sections):
            if index not in used and alias_hit(section.title, spec.aliases):
                matched[spec.title] = section
                used.add(index)
                break

    skeleton: list[str] = [
        f"缺少章节「{spec.title}」" for spec in specs if spec.title not in matched
    ]
    unanswered: list[str] = []
    rewritten: list[str] = []

    for spec in specs:
        section = matched.get(spec.title)
        if section is None:
            continue
        items, fields = section_entries(section.lines)

        item_matches, duplicate_items = match_template_items(items, spec.item_aliases)
        if len(items) < spec.items:
            skeleton.append(f"「{spec.title}」勾选项 {len(items)}/{spec.items} 条")
        for template_index in duplicate_items:
            skeleton.append(
                f"「{spec.title}」重复条目：{spec.item_aliases[template_index][0][:32]}"
            )

        if spec.items:
            checked = [entry for entry in items if entry.checked]
            if spec.require_all_checked:
                if len(checked) < spec.items:
                    unanswered.append(
                        f"「{spec.title}」需逐条确认后全部勾选（{len(checked)}/{spec.items}）"
                    )
            elif not checked:
                unanswered.append(f"「{spec.title}」未勾选任何一项")
            for index, entry in enumerate(items):
                if not entry.checked:
                    continue
                label = entry.text[:24]
                if entry.has_colon and not entry.own_content:
                    unanswered.append(f"「{spec.title}」已勾选项冒号后为空：{label}")
                template_index = item_matches[index]
                required_details = (
                    spec.item_detail_prompts[template_index]
                    if template_index is not None
                    and template_index < len(spec.item_detail_prompts)
                    else ()
                )
                if required_details and (
                    len(entry.details) < len(required_details)
                    or any(
                        not detail_answered(entry.details[position], prompts)
                        for position, prompts in enumerate(required_details)
                    )
                ):
                    unanswered.append(f"「{spec.title}」已勾选项未填写说明：{label}")

        filled = [content for content in fields if content]
        if len(filled) < spec.fields:
            unanswered.append(f"「{spec.title}」填写项已填 {len(filled)}/{spec.fields} 条")

        if not spec.items and not spec.fields and not section_prose(section.lines):
            unanswered.append(f"「{spec.title}」正文为空")

        for aliases in spec.item_aliases:
            if not any(alias_hit(entry.text, aliases) for entry in items):
                rewritten.append(f"「{spec.title}」{aliases[0][:32]}")

    if skeleton:
        add(
            findings,
            "GOV-001",
            "high",
            "repository-governance",
            "PR 描述未保留模板骨架",
            f"PR 描述相对目标分支模板缺少 {len(skeleton)} 处骨架结构，评审者无法据此看到完整的待确认项。",
            "按目标分支 PR 模板补齐章节与全部条目；条目可只保留中文或只保留英文，但不得删减条数。",
            PULL_REQUEST_TEMPLATE_PATH,
            evidence=" | ".join(skeleton[:5]),
        )

    if unanswered:
        add(
            findings,
            "GOV-002",
            "high",
            "repository-governance",
            "PR 描述必答项未完成",
            f"PR 描述有 {len(unanswered)} 处必答内容为空：章节正文、勾选结果、验证信息或已勾选项的说明未填写。",
            "填写摘要与验证信息；每个多选章节至少勾选一项，确认类章节逐条勾选；已勾选项须补齐冒号后的说明内容。",
            PULL_REQUEST_TEMPLATE_PATH,
            evidence=" | ".join(unanswered[:5]),
        )

    if rewritten:
        add(
            findings,
            "GOV-003",
            "low",
            "repository-governance",
            "PR 描述条目文案疑似改写",
            f"有 {len(rewritten)} 个模板条目在 PR 描述中未能按文案匹配到；条数校验已通过，此项仅提示评审者人工核对。",
            "确认这些条目只是措辞或语言差异；若条目含义被改写，请恢复模板原意。",
            PULL_REQUEST_TEMPLATE_PATH,
            evidence=" | ".join(rewritten[:5]),
        )


def looks_like_email(email: str | None) -> bool:
    value = str(email or "").strip()
    if len(value) > DCO_EMAIL_MAX_TOTAL:
        return False
    at = value.rfind("@")
    if at <= 0 or at == len(value) - 1:
        return False
    local_part = value[:at]
    domain = value[at + 1 :]
    if len(local_part) > DCO_EMAIL_MAX_LOCAL_PART or len(domain) > DCO_EMAIL_MAX_DOMAIN:
        return False
    if any(len(label) > DCO_EMAIL_MAX_DOMAIN_LABEL for label in domain.split(".")):
        return False
    return bool(DCO_EMAIL_LOCAL_PART_RE.fullmatch(local_part) and DCO_EMAIL_DOMAIN_RE.fullmatch(domain))


def parse_sign_offs(message: str) -> list[DcoSignOff]:
    sign_offs: list[DcoSignOff] = []
    for line in str(message or "").splitlines():
        if len(line) > DCO_MAX_SIGN_OFF_LINE:
            continue
        match = SIGNED_OFF_BY_LINE_RE.fullmatch(line)
        if match:
            sign_offs.append(DcoSignOff(name=match.group(1), email=match.group(2)))
    return sign_offs


def parse_git_log_commits(stdout: str) -> list[DcoCommit]:
    commits: list[DcoCommit] = []
    for record in str(stdout or "").split(DCO_GIT_LOG_RECORD_SEP):
        record = record.lstrip("\r\n")
        if not record.strip():
            continue
        fields = record.split(DCO_GIT_LOG_FIELD_SEP)
        if len(fields) != 7:
            raise RuntimeError(
                f"{DCO_PARSE_ERROR}: git log record field count {len(fields)} != 7"
            )
        sha, parents, author_name, author_email, committer_name, committer_email, message = fields
        sha = sha.strip()
        if not SHA40_RE.fullmatch(sha):
            raise RuntimeError(f"{DCO_PARSE_ERROR}: git log record has invalid sha")
        parent_text = parents.strip()
        commits.append(
            DcoCommit(
                sha=sha,
                parents=tuple(parent_text.split()) if parent_text else (),
                author_name=author_name,
                author_email=author_email,
                committer_name=committer_name,
                committer_email=committer_email,
                message=message,
            )
        )
    return commits


def dco_exempt_reason(commit: DcoCommit) -> str | None:
    if len(commit.parents) > 1:
        return "merge commit"
    return None


def validate_dco_commit(commit: DcoCommit) -> list[str]:
    sign_offs = parse_sign_offs(commit.message)
    if not sign_offs:
        return ["No Signed-off-by trailer."]

    if any(looks_like_email(sign_off.email) for sign_off in sign_offs):
        return []

    got = ", ".join(f'"{item.name} <{item.email}>"' for item in sign_offs)
    return [
        f"Signed-off-by trailer email is not a valid email address, got {got}."
    ]


def load_pull_request_commits(base_sha: str, head_sha: str) -> tuple[list[DcoCommit] | None, str | None]:
    result = run_git(
        ["log", f"--format={DCO_GIT_LOG_FORMAT}", f"{base_sha}..{head_sha}"]
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        return None, detail
    try:
        return parse_git_log_commits(result.stdout), None
    except RuntimeError as exc:
        return None, str(exc)


def rule_pull_request_dco(
    context: PullRequestContext | None,
    findings: list[Finding],
) -> None:
    if context is None:
        return

    if context.head_sha is None:
        add(
            findings,
            "GOV-004",
            "high",
            "repository-governance",
            "PR 缺少可校验的 head.sha",
            "GitHub pull_request 事件有合法 base.sha，但 head.sha 缺失或不是 40 位 hex，无法读取 PR 引入的 commit 做 DCO 校验。",
            "确认 workflow 使用完整 pull_request payload；不要用 checkout HEAD 代替 event 的 head.sha。漏签请用 git commit --amend -s 或 git rebase --signoff 补签。",
            CONTRIBUTING_DCO_PATH,
            evidence="head.sha missing or invalid",
        )
        return

    commits, git_error = load_pull_request_commits(context.base_sha, context.head_sha)
    if commits is None:
        add(
            findings,
            "GOV-004",
            "high",
            "repository-governance",
            "无法读取 PR commit 范围做 DCO 校验",
            f"git log {context.base_sha}..{context.head_sha} 失败，不能证明范围内提交已签署 DCO。",
            "确认 checkout fetch-depth: 0 且 PR head 对象存在；fork PR 缺对象时先 fetch 再重跑。漏签请用 git commit --amend -s 或 git rebase --signoff 补签。",
            CONTRIBUTING_DCO_PATH,
            evidence=git_error[:300] if git_error else "git log failed",
        )
        return

    if not commits:
        add(
            findings,
            "GOV-004",
            "high",
            "repository-governance",
            "PR commit 范围不可用",
            f"git log {context.base_sha}..{context.head_sha} 没有返回任何 commit，不能把空范围当成全部已签。",
            "确认 event 的 base.sha 与 head.sha 指向本次 PR 引入的提交范围。漏签请用 git commit --amend -s 或 git rebase --signoff 补签。",
            CONTRIBUTING_DCO_PATH,
            evidence=f"{context.base_sha[:8]}..{context.head_sha[:8]} empty",
        )
        return

    failures: list[str] = []
    for commit in commits:
        if dco_exempt_reason(commit):
            continue
        errors = validate_dco_commit(commit)
        if errors:
            failures.append(f"{commit.sha[:8]}: {errors[0]}")

    if not failures:
        return

    add(
        findings,
        "GOV-004",
        "high",
        "repository-governance",
        "PR commit 缺少有效 DCO 签名",
        f"范围内有 {len(failures)} 个非 merge commit 没有合法行首 Signed-off-by: Name <email>（trailer email 须像邮箱）。模板勾选不能替代 trailer。",
        "每个 commit 使用 git commit -s。漏签用 git commit --amend -s --no-edit，或多提交用 git rebase --signoff，再 git push --force-with-lease。不要写 remediation commit。",
        CONTRIBUTING_DCO_PATH,
        evidence=" | ".join(failures[:DCO_FINDING_EVIDENCE_LIMIT]),
    )


def rule_license(files: list[str], findings: list[Finding]) -> None:
    if not any(name in files for name in ("LICENSE", "LICENSE.md", "COPYING")):
        add(
            findings,
            "LIC-001",
            "high",
            "license",
            "缺少项目根许可证",
            "仓库已有 NOTICE，但没有根目录 LICENSE/COPYING 文件，因此项目自身尚未明确授权下游复制、开源发布或商用复用。",
            "补充根目录 LICENSE，明确项目采用的许可证；NOTICE 继续作为第三方依赖声明保留。",
        )


def vendored_dependency_roots(files: Iterable[str]) -> list[str]:
    roots: set[str] = set()
    for rel in files:
        parts = rel.split("/")
        for index, part in enumerate(parts[:-1]):
            if part.lower() not in VENDORED_DIRECTORY_NAMES:
                continue
            if index + 1 < len(parts):
                roots.add("/".join(parts[: index + 2]))
            break
    return sorted(roots)


def dependency_has_license_evidence(root: str, files: Iterable[str], notices: str) -> bool:
    prefix = root.rstrip("/") + "/"
    for rel in files:
        if rel.startswith(prefix) and LICENSE_FILENAMES.match(Path(rel).name):
            return True

    dependency_name = root.rsplit("/", 1)[-1].lower()
    for paragraph in re.split(r"\n\s*\n", notices):
        lowered = paragraph.lower()
        if dependency_name in lowered and "license" in lowered and "embedded" in lowered:
            return True
    return False


def rule_third_party_notices(files: list[str], findings: list[Finding]) -> None:
    roots = vendored_dependency_roots(files)
    if not roots:
        return

    notices = read_text("NOTICE") if "NOTICE" in files else ""
    if not notices:
        add(
            findings,
            "LIC-002",
            "high",
            "license",
            "存在 vendored 依赖但缺少 NOTICE",
            f"完整已跟踪树中发现 {len(roots)} 个 vendored 依赖根目录，但没有可发布的第三方声明清单。",
            "新增 NOTICE，逐项记录名称、版本、许可证、用途、获取方式和本地路径。",
            roots[0],
            evidence=", ".join(roots[:5]),
        )
        return

    lowered_notices = notices.lower()
    for root in roots:
        dependency_name = root.rsplit("/", 1)[-1]
        if root.lower() not in lowered_notices and dependency_name.lower() not in lowered_notices:
            add(
                findings,
                "LIC-002",
                "high",
                "license",
                "vendored 依赖未登记到 NOTICE",
                "已跟踪的第三方源码会随源代码发布，但声明清单中没有对应条目。",
                "补充名称、精确版本/commit、许可证、用途、获取方式和 vendored 路径。",
                root,
                evidence=dependency_name,
            )
        if not dependency_has_license_evidence(root, files, notices):
            add(
                findings,
                "LIC-003",
                "high",
                "license",
                "vendored 依赖缺少许可证文本证据",
                "依赖目录中没有 LICENSE/COPYING/NOTICE 等文件，根目录 NOTICE 也未明确说明许可证全文嵌入在哪个文件。",
                "保留上游许可证/NOTICE 文件；若许可证嵌入单文件源码，在根目录 NOTICE 中明确记录文件路径。",
                root,
                evidence=dependency_name,
            )


def rule_license_policy(files: list[str], findings: list[Finding]) -> None:
    if "NOTICE" not in files:
        return
    notices = read_text("NOTICE")
    if not notices:
        return

    strong = re.search(
        r"(?im)^\|[^\n]*\b(?:AGPL|GPL)(?:-[0-9.]*)?\b[^\n]*\|\s*$", notices
    )
    if strong:
        line = notices[: strong.start()].count("\n") + 1
        add(
            findings,
            "LIC-004",
            "critical",
            "license",
            "第三方声明包含禁止的强 copyleft 许可证",
            "仓库政策禁止 GPL/AGPL 进入运行时、SDK 或 Studio 产品闭包。",
            "移除该依赖或替换为许可证兼容的实现；如仅是文档提及，应在声明中明确它不进入任何产品闭包。",
            "NOTICE",
            line,
            strong.group(0).strip(),
        )

    runtime_match = re.search(
        r"(?is)##\s+Runtime[^\n]*\n(.*?)(?=\n##\s+Studio|\Z)", notices
    )
    if runtime_match:
        weak = re.search(
            r"(?im)^\|[^\n]*\b(?:LGPL|MPL|EPL|CDDL)(?:-[0-9.]*)?\b[^\n]*\|\s*$",
            runtime_match.group(1),
        )
        if weak:
            line = notices[: runtime_match.start(1) + weak.start()].count("\n") + 1
            add(
                findings,
                "LIC-004",
                "high",
                "license",
                "运行时/SDK 闭包包含非宽松许可证",
                "运行时、SDK 和随游戏发行的依赖闭包只允许 MIT/BSD/Apache-2.0/zlib/公有领域等宽松许可证。",
                "替换该依赖，或在进入发布分支前提供经法律审查批准的政策例外。",
                "NOTICE",
                line,
                weak.group(0).strip(),
            )

    studio_match = re.search(r"(?is)##\s+Studio[^\n]*\n(.*)\Z", notices)
    if studio_match:
        studio = studio_match.group(1)
        static_weak = re.search(r"(?is)LGPL.{0,160}static|static.{0,160}LGPL", studio)
        obligations_recorded = bool(
            re.search(r"(?i)relinkable|LGPL\s*§\s*6|dynamic linking|动态链接", studio)
        )
        if static_weak and not obligations_recorded:
            line = notices[: studio_match.start(1) + static_weak.start()].count("\n") + 1
            add(
                findings,
                "LIC-004",
                "high",
                "license",
                "Studio 静态弱 copyleft 依赖缺少发布义务记录",
                "静态链接 LGPL 等弱 copyleft 组件可能要求可重链接材料或改为动态链接。",
                "在 NOTICE 的发布检查项中记录动态链接方案，或需要提供的可重链接材料和许可证文本。",
                "NOTICE",
                line,
                static_weak.group(0).strip(),
            )


def project_cmake_files(files: Iterable[str]) -> list[str]:
    return [
        rel
        for rel in files
        if (Path(rel).name == "CMakeLists.txt" or Path(rel).suffix.lower() == ".cmake")
        and "/thirdparty/" not in f"/{rel.lower()}/"
        and exists(rel)
    ]


def rule_fetchcontent_hashes(files: list[str], findings: list[Finding]) -> None:
    for rel in project_cmake_files(files):
        text = read_text(rel)
        for match in re.finditer(r"FetchContent_Declare\s*\(([^)]*)\)", text, flags=re.S):
            block = match.group(0)
            if re.search(r"(?m)^\s*URL\s+", block) and "URL_HASH" not in block:
                line = text[: match.start()].count("\n") + 1
                tokens = match.group(1).strip().split()
                dependency = tokens[0] if tokens else "unknown"
                url_match = re.search(r"\bURL\s+(\S+)", block)
                evidence = f"{dependency}: {url_match.group(1)}" if url_match else dependency
                add(
                    findings,
                    "DEP-001",
                    "medium",
                    "supply-chain",
                    "FetchContent 下载缺少 URL_HASH",
                    "依赖归档包只按 URL/标签固定，没有按内容哈希固定；上游归档漂移或网络替换更难被发现。",
                    "为每个 FetchContent 归档补充 URL_HASH SHA256，或改为 vendored 依赖并记录声明。",
                    rel,
                    line,
                    evidence,
                )


def rule_dependency_git_pins(files: list[str], findings: list[Finding]) -> None:
    for rel in project_cmake_files(files):
        text = read_text(rel)
        for match in re.finditer(r"FetchContent_Declare\s*\(([^)]*)\)", text, flags=re.S):
            block = match.group(0)
            if "GIT_REPOSITORY" not in block:
                continue
            tag_match = re.search(r"\bGIT_TAG\s+([^\s)]+)", block)
            tag = tag_match.group(1).strip('"\'') if tag_match else ""
            if re.fullmatch(r"[0-9a-fA-F]{40}", tag):
                continue
            line = text[: match.start()].count("\n") + 1
            tokens = match.group(1).strip().split()
            dependency = tokens[0] if tokens else "unknown"
            add(
                findings,
                "DEP-005",
                "medium",
                "supply-chain",
                "FetchContent Git 依赖未固定到完整 commit SHA",
                "Git 分支、短 SHA 或可变标签不能提供不可变、可复现的依赖身份。",
                "将 GIT_TAG 改为上游完整 40 位 commit SHA，并在 NOTICE 中记录可读版本。",
                rel,
                line,
                f"{dependency}: GIT_TAG {tag or '<missing>'}",
            )


def product_source_files(files: Iterable[str]) -> list[str]:
    return [
        rel
        for rel in files
        if rel.startswith(PRODUCT_SOURCE_PREFIXES)
        and is_text_file(rel)
        and not any(part in f"/{rel}" for part in PRODUCT_SOURCE_EXCLUDED_PARTS)
        and exists(rel)
    ]


def first_product_match(
    files: Iterable[str], needles: Iterable[str]
) -> tuple[str, str] | None:
    for rel in files:
        text = read_text(rel)
        for needle in needles:
            if needle in text:
                return rel, needle
    return None


def rule_telemetry(files: list[str], findings: list[Finding]) -> None:
    product_sources = product_source_files(files)

    reporting_match = first_product_match(
        product_sources,
        (
            "Telemetry::",
            "harken_telemetry",
            "feishu.cn",
            "Webhook",
            "webhook",
        ),
    )
    if reporting_match:
        rel, needle = reporting_match
        line, evidence = find_line(rel, needle)
        add(
            findings,
            "TEL-001",
            "high",
            "privacy",
            "product runtime reporting code is present",
            "Product executables must not collect or report runtime identity data; only non-personal build identity may be embedded at configure time.",
            "Remove the runtime reporter and keep build identity in harken_attach_version_resource().",
            rel,
            line,
            evidence,
        )

    startup_match = first_product_match(
        product_sources,
        ("--enable-telemetry",),
    )
    if startup_match:
        rel, needle = startup_match
        line, evidence = find_line(rel, needle)
        add(
            findings,
            "TEL-002",
            "high",
            "privacy",
            "runtime reporting starts from a product entry point",
            "A product executable starts runtime reporting without a user-visible product action.",
            "Remove the runtime reporting startup path from product executables.",
            rel,
            line,
            evidence,
        )

    identity_match = first_product_match(
        product_sources,
        (
            "MachineGuid",
            "HARKEN_USER_EMAIL",
            "QNetworkInterface::allAddresses",
        ),
    )
    if identity_match:
        rel, needle = identity_match
        line, evidence = find_line(rel, needle)
        add(
            findings,
            "TEL-003",
            "high",
            "privacy",
            "product runtime collects stable device or account identifiers",
            "Runtime identity probes can collect device, account, network, host, or user identifiers.",
            "Remove stable personal and device identity probes from product executables.",
            rel,
            line,
            evidence,
        )

    persistence_checks = (
        ("FILE_ATTRIBUTE_HIDDEN", "TEL-004"),
        (":$DATA", "TEL-005"),
        ("\\\\Telemetry\\\\", "TEL-006"),
    )
    if reporting_match:
        for needle, rule_id in persistence_checks:
            persistence_match = first_product_match(product_sources, (needle,))
            if not persistence_match:
                continue
            rel, _ = persistence_match
            line, evidence = find_line(rel, needle)
            add(
                findings,
                rule_id,
                "high",
                "privacy",
                "runtime telemetry uses opaque local persistence",
                "The runtime reporting pipeline stores records in a location users do not naturally inspect.",
                "Remove opaque runtime telemetry persistence from product executables.",
                rel,
                line,
                evidence,
            )

    cmake = read_text("CMakeLists.txt")
    for needle in ("telemetry", "Telemetry", "TELEMETRY"):
        if needle in cmake:
            line, evidence = find_line("CMakeLists.txt", needle)
            add(
                findings,
                "TEL-001",
                "high",
                "privacy",
                "runtime telemetry wiring is present in the build system",
                "Runtime telemetry is not an allowed product behavior; builds may only embed non-personal build identity into binary file properties.",
                "Remove runtime telemetry options, targets, and links. Use harken_attach_version_resource() for build identity.",
                "CMakeLists.txt",
                line,
                evidence,
            )
            break

    telemetry_tree = [rel for rel in product_sources if "/Telemetry/" in rel]
    if telemetry_tree:
        add(
            findings,
            "TEL-001",
            "high",
            "privacy",
            "runtime telemetry source tree is present",
            "Runtime telemetry collection and notification code must not be carried in the product source tree.",
            "Remove the runtime telemetry source tree; keep build identity in CMake version resources only.",
            telemetry_tree[0],
        )


def rule_credential_storage(files: list[str], findings: list[Finding]) -> None:
    credential_literal = re.compile(
        r"(?i)setValue\s*\(\s*(?:QStringLiteral\s*\()?\s*[\"'](?:api[_-]?key|token|secret|password)[\"']"
    )
    for rel in product_source_files(files):
        text = read_text(rel)
        if "QSettings" not in text:
            continue

        direct_plaintext = credential_literal.search(text)
        base64_fallback = (
            "plaintext.toBase64()" in text
            and "#else" in text
            and "setValue" in text
        )
        if not direct_plaintext and not base64_fallback:
            continue

        needle = direct_plaintext.group(0) if direct_plaintext else "plaintext.toBase64()"
        line, evidence = find_line(rel, needle)
        if line is None and direct_plaintext:
            line = text[: direct_plaintext.start()].count("\n") + 1
            evidence = text.splitlines()[line - 1].strip()
        add(
            findings,
            "SEC-002",
            "high",
            "secret",
            "产品凭据写入普通配置存储",
            "API key/token 被写入 QSettings，或在某些平台仅做 Base64 编码；Base64 不是加密，普通配置文件不能作为凭据保险库。",
            "改用操作系统凭据保险库/用户绑定加密；不支持安全存储的平台应只接受进程环境或会话内输入，并明确不落盘。",
            rel,
            line,
            redact(evidence or needle),
        )


def rule_build_identity(files: list[str], findings: list[Finding]) -> None:
    rel = "cmake/BuildInfo.h.in"
    if rel in files:
        text = read_text(rel)
        cmake = read_text("cmake/BuildInfo.cmake")
        private_identity_gated = (
            "HARKEN_BUILD_PRIVATE_IDENTITY" in cmake
            and 'set(HARKEN_BI_HOSTNAME "")' in cmake
            and 'set(HARKEN_BI_USERNAME "")' in cmake
        )
        if "Hostname" in text and "Username" in text and not private_identity_gated:
            line, evidence = find_line(rel, "Username")
            add(
                findings,
                "PRIV-001",
                "medium",
                "privacy",
                "构建身份包含主机和用户字段",
                "生成的构建信息包含本地主机名和用户名，可能被编译进二进制或进入报告。",
                "公开构建使用适合 CI 的标识，避免把个人机器名或用户名嵌入可分发产物。",
                rel,
                line,
                evidence,
            )


def rule_miniaudio_scope(files: list[str], findings: list[Finding]) -> None:
    impl = "HarkenEngine/Source/Runtime/api/src/MiniaudioImpl.cpp"
    loader = "HarkenEngine/Source/Tooling/Common/WavLoader.h"
    documented = any(
        "miniaudio" in line and "Compiled without feature trimming" in line
        for line in read_text("NOTICE").splitlines()
    )
    if (
        impl in files
        and "No compile-time feature trimming" in read_text(impl)
        and not documented
    ):
        line, evidence = find_line(impl, "No compile-time feature trimming")
        add(
            findings,
            "DEP-002",
            "low",
            "supply-chain",
            "miniaudio 未做特性裁剪",
            "vendored 单头实现未做编译期特性裁剪，且 NOTICE 未记录实际编译进来的能力范围。",
            "商用发布前，要么在 NOTICE 记录实际编译进来的更广能力范围，要么在所有翻译单元中统一定义 miniaudio 裁剪宏。",
            impl,
            line,
            evidence,
        )
    if (
        loader in files
        and "encodingFormat = ma_encoding_format_wav" not in read_text(loader)
    ):
        line, evidence = find_line(loader, "ma_decoder_config_init")
        add(
            findings,
            "DEP-003",
            "low",
            "supply-chain",
            "Host 加载器接受任意 miniaudio 可解码格式",
            "公开依赖声明写的是 WAV 解码，但加载器未把解码器固定到 wav 后端，任何 miniaudio 支持的格式都能通过。",
            "在解码器配置中把 encodingFormat 固定为 wav，或让声明和文档与实际可接受格式一致。",
            loader,
            line,
            evidence,
        )


def rule_local_files(findings: list[Finding]) -> None:
    telemetry_config = REPO_ROOT / "harken_telemetry.json"
    if telemetry_config.exists():
        add(
            findings,
            "LOCAL-001",
            "info",
            "local-secret",
            "obsolete local telemetry config exists",
            "harken_telemetry.json is no longer read by the product, but an old local copy still exists and may contain secrets.",
            "Delete the local file when it is no longer needed. It remains gitignored only to prevent accidental secret commits.",
            "harken_telemetry.json",
        )
    for line in git_status_short():
        if line.startswith("?? .agents/"):
            add(
                findings,
            "LOCAL-002",
            "info",
            "local-working-tree",
            "本地存在未跟踪的 .agents 目录",
            ".agents 在本地存在，但未被 git 跟踪。",
            "除非明确授权并计划发布，否则保持未跟踪和私有。",
                ".agents",
            )


def redact(value: str) -> str:
    if len(value) <= 24:
        return "<redacted>"
    return value[:12] + "...<redacted>..." + value[-6:]


def finding_fingerprint(item: dict) -> str:
    parts = [
        str(item.get("rule_id") or ""),
        str(item.get("file") or ""),
        str(item.get("evidence") or item.get("line") or ""),
    ]
    return "|".join(parts)


def sort_findings(findings: list[Finding]) -> list[Finding]:
    return sorted(
        findings,
        key=lambda f: (SEVERITY_ORDER.get(f.severity, 99), f.category, f.rule_id, f.file or ""),
    )


def summarize(findings: list[Finding]) -> dict[str, int]:
    counts = {key: 0 for key in SEVERITY_ORDER}
    for finding in findings:
        counts[finding.severity] = counts.get(finding.severity, 0) + 1
    return counts


def highest_severity(findings: list[Finding]) -> str:
    if not findings:
        return "none"
    return min(findings, key=lambda f: SEVERITY_ORDER.get(f.severity, 99)).severity


def render_markdown(payload: dict) -> str:
    summary = payload["summary"]
    findings = payload["findings"]
    lines = [
        "# 全量审查问题明细",
        "",
        f"- 当前结论：**{audit_decision(summary, payload.get('scope') == 'full_tracked_tree_and_pull_request')}**",
        f"- 风险统计：严重 {summary.get('critical', 0)}，高 {summary.get('high', 0)}，中 {summary.get('medium', 0)}，低 {summary.get('low', 0)}，信息 {summary.get('info', 0)}",
        f"- 生成时间：{payload['generated_at']}",
        "- 检查范围：完整已跟踪代码树（`git ls-files`），并包含部分本地安全检查",
    ]
    if payload.get("scope") == "full_tracked_tree_and_pull_request":
        lines.append("- PR 检查：按目标分支模板校验 PR 描述")
    history_summary = render_inline_history_summary(payload)
    if history_summary:
        lines.append(f"- 历史对比：{history_summary}")
    lines.append("")

    if not findings:
        lines.extend(["未发现问题。", ""])
        lines.extend(render_machine_state(payload))
        return "\n".join(lines)

    status_by_fingerprint = history_status_by_fingerprint(payload)
    lines.extend(render_status_table(findings, status_by_fingerprint))

    severity_sections = [
        ("critical", "严重"),
        ("high", "高"),
        ("medium", "中"),
        ("low", "低"),
        ("info", "信息"),
    ]
    for severity, label in severity_sections:
        items = [item for item in findings if item.get("severity") == severity]
        lines.extend(render_severity_detail_section(label, items, status_by_fingerprint))

    lines.extend(render_machine_state(payload))
    return "\n".join(lines)


def audit_decision(summary: dict[str, int], pull_request: bool) -> str:
    if summary.get("critical", 0) > 0:
        return "存在严重阻断项，不建议合并 PR" if pull_request else "存在严重阻断项"
    if summary.get("high", 0) > 0:
        return "存在高风险项，不建议合并 PR" if pull_request else "存在高风险项"
    if summary.get("medium", 0) > 0:
        return "存在中风险项，建议评估整改后再合并 PR" if pull_request else "存在中风险项，建议评估整改"
    return "未发现阻断性合规风险"


def render_inline_history_summary(payload: dict) -> str:
    history = payload.get("history")
    if not history:
        return ""
    previous = history.get("previous_document") or {}
    previous_title = previous.get("title") or "未找到上一份报告"
    return (
        f"上一份报告：{previous_title}；"
        f"新增 {len(history.get('new', []))}；"
        f"持续 {len(history.get('persistent', []))}；"
        f"已解决 {len(history.get('resolved', []))}。"
    )


def render_severity_detail_section(
    label: str,
    items: list[dict],
    status_by_fingerprint: dict[str, str],
) -> list[str]:
    lines = [
        f"## {label}",
        "",
    ]
    if not items:
        lines.extend(["本级别无问题。", ""])
        return lines

    for item in items:
        status = status_by_fingerprint.get(str(item.get("fingerprint") or finding_fingerprint(item)), "新增")
        lines.extend(render_finding_record(item, status))
    return lines


def render_status_table(findings: list[dict], status_by_fingerprint: dict[str, str]) -> list[str]:
    rows = []
    for item in findings:
        fingerprint = str(item.get("fingerprint") or finding_fingerprint(item))
        status = status_by_fingerprint.get(fingerprint, "新增")
        severity = SEVERITY_LABELS.get(item.get("severity"), item.get("severity", "未知"))
        title = str(item.get("title") or "未命名问题")
        rows.append((status, severity, title))

    status_width = max([display_width("状态"), *(display_width(status) for status, _, _ in rows)])
    severity_width = max([display_width("风险等级"), *(display_width(severity) for _, severity, _ in rows)])
    title_width = max([display_width("标题"), *(display_width(title) for _, _, title in rows)])
    lines = [
        "## 问题状态统计",
        "",
        "```text",
        f"{pad_display('状态', status_width)}  {pad_display('风险等级', severity_width)}  {pad_display('标题', title_width)}",
        f"{'-' * status_width}  {'-' * severity_width}  {'-' * title_width}",
    ]
    for status, severity, title in rows:
        lines.append(
            f"{pad_display(status, status_width)}  {pad_display(severity, severity_width)}  {pad_display(title, title_width)}"
        )
    lines.extend(["```", ""])
    return lines


def display_width(value: str) -> int:
    width = 0
    for char in value:
        width += 2 if ord(char) > 127 else 1
    return width


def pad_display(value: str, width: int) -> str:
    return value + " " * max(0, width - display_width(value))


def render_finding_record(item: dict, status: str) -> list[str]:
    title = html.escape(str(item.get("title") or "未命名问题"))
    lines = [
        "<details>",
        f"<summary><strong>{title}</strong></summary>",
        "",
        f"**编号：** `{item.get('rule_id', '?')}`",
        "",
        f"**状态：** {status}",
        "",
        f"**问题详情：** {item.get('detail', '')}",
        "",
        f"**判定依据：** {judgement_basis(item)}",
        "",
        f"**证据来源：** {evidence_source(item)}",
        "",
    ]
    lines.extend([f"**影响说明：** {impact_description(item)}", "", "</details>", ""])
    return lines


def judgement_basis(item: dict) -> str:
    rule_id = str(item.get("rule_id") or "")
    standard = RULE_STANDARDS.get(rule_id)
    if standard:
        return f"命中 `{rule_id}` 规则。{standard}"
    return f"命中 `{rule_id}` 规则；该规则属于 {CATEGORY_LABELS.get(item.get('category'), item.get('category', '未分类'))} 检查项。"


def evidence_source(item: dict) -> str:
    location = format_location(item)
    evidence = item.get("evidence")
    if evidence:
        return f"{location}；命中内容：{evidence}"
    return location


def impact_description(item: dict) -> str:
    rule_id = str(item.get("rule_id") or "")
    category = str(item.get("category") or "")
    if rule_id.startswith("GOV-"):
        return "该项影响 PR 审查信息的完整性；补齐模板要求后，维护者才能准确判断变更范围、验证情况和待确认事项。"
    if rule_id.startswith("LIC-"):
        return "该项表示许可证或第三方授权材料尚不足以自动确认发布边界；需要补齐登记、许可证文本或依赖闭包核对。"
    if rule_id == "PRIV-001":
        return "该项表示构建产物可能包含本地主机名、用户名等个人环境信息；应清理相关标识并复核发布内容。"
    if rule_id.startswith("TEL-") or category == "privacy":
        return "该项表示数据处理行为需要进一步确认告知、同意、最小化和留存边界；应结合真实数据流和发布配置复核。"
    if rule_id.startswith("REL-"):
        return "该项表示发布范围可能包含非产品内容；应核对文件用途、公开必要性以及其中的个人或内部信息。"
    if rule_id.startswith("DEP-"):
        return "该项影响依赖的可复现性、完整性或声明准确性；应补充版本固定、哈希校验或第三方依赖说明。"
    if rule_id.startswith("SEC-"):
        return "若命中内容为真实凭据，应立即轮换并核查使用记录；自动扫描结果仍需人工排除示例值和误报。"
    if rule_id.startswith("LOCAL-"):
        return "该项通常不阻断发布；应确认本地配置或私有目录不会进入提交、构建产物和发布包。"
    return "该项需要按对应规则补充确认或整改；自动化结果仅作为工程审查输入。"


def render_history_section(payload: dict) -> list[str]:
    history = payload.get("history")
    if not history:
        return []

    previous = history.get("previous_document") or {}
    previous_title = previous.get("title") or "未找到"
    previous_url = previous.get("url")
    if previous_url and previous_title != "未找到":
        previous_title = f"[{previous_title}]({previous_url})"

    lines = [
        "## 历史变化",
        "",
        f"- 上一份报告：{previous_title}",
        f"- 新增问题：{len(history.get('new', []))}",
        f"- 持续问题：{len(history.get('persistent', []))}",
        f"- 已解决问题：{len(history.get('resolved', []))}",
        "",
    ]
    for label, key in (("新增问题", "new"), ("已解决问题", "resolved")):
        items = history.get(key, [])
        if not items:
            continue
        lines.append(f"### {label}")
        lines.append("")
        for item in items[:10]:
            lines.append(format_compact_finding(item))
        if len(items) > 10:
            lines.append(f"- 另有 {len(items) - 10} 项")
        lines.append("")
    if history.get("persistent"):
        lines.extend(
            [
                "持续问题不在本节逐项展开，请在下面的“问题明细”中按主题查看。",
                "",
            ]
        )
    return lines


def render_priority_section(findings: list[dict]) -> list[str]:
    priority_items = [
        item
        for item in findings
        if SEVERITY_ORDER.get(item.get("severity", "info"), 99) <= SEVERITY_ORDER["high"]
    ]
    if not priority_items:
        return []

    lines = [
        "## 优先处理清单",
        "",
        "以下仅列严重和高风险项，用于安排整改顺序；完整说明见后文问题明细。",
        "",
    ]
    for index, item in enumerate(priority_items, start=1):
        category = CATEGORY_LABELS.get(item.get("category"), item.get("category", "未分类"))
        lines.append(
            f"{index}. **{SEVERITY_LABELS.get(item.get('severity'), item.get('severity'))}｜{category}｜`{item.get('rule_id')}` {item.get('title')}**"
        )
        lines.append(f"   位置：{format_location(item)}")
        lines.append(f"   处理方向：{item.get('recommendation')}")
    lines.append("")
    return lines


def render_theme_sections(payload: dict) -> list[str]:
    findings = payload["findings"]
    status_by_fingerprint = history_status_by_fingerprint(payload)
    lines = [
        "## 问题明细",
        "",
        "问题按合规主题分组；每个问题只在这里展开一次，避免重复阅读。",
        "",
    ]
    used: set[str] = set()
    for theme, description, categories in THEME_GROUPS:
        items = [item for item in findings if item.get("category") in categories]
        if not items:
            continue
        used.update(str(item.get("fingerprint") or finding_fingerprint(item)) for item in items)
        lines.extend(render_theme_group(theme, description, items, status_by_fingerprint))

    remaining = [
        item
        for item in findings
        if str(item.get("fingerprint") or finding_fingerprint(item)) not in used
    ]
    if remaining:
        lines.extend(render_theme_group("其他", "未归入固定主题的发现项。", remaining, status_by_fingerprint))
    return lines


def render_theme_group(
    theme: str,
    description: str,
    items: list[dict],
    status_by_fingerprint: dict[str, str],
) -> list[str]:
    counts = summarize_dict_findings(items)
    lines = [
        f"### {theme}",
        "",
        description,
        "",
        f"本组统计：严重 {counts.get('critical', 0)}，高 {counts.get('high', 0)}，中 {counts.get('medium', 0)}，低 {counts.get('low', 0)}，信息 {counts.get('info', 0)}。",
        "",
    ]
    for item in items:
        status = status_by_fingerprint.get(str(item.get("fingerprint") or finding_fingerprint(item)), "本次发现")
        severity = SEVERITY_LABELS.get(item.get("severity"), item.get("severity", "未知"))
        lines.append(f"#### {severity}｜{status}｜`{item.get('rule_id')}` {item.get('title')}")
        lines.append("")
        lines.append(f"- 位置：{format_location(item)}")
        lines.append(f"- 说明：{item.get('detail')}")
        lines.append(f"- 建议：{item.get('recommendation')}")
        if item.get("evidence"):
            lines.append(f"- 证据：`{item['evidence']}`")
        lines.append("")
    return lines


def summarize_dict_findings(findings: list[dict]) -> dict[str, int]:
    counts = {key: 0 for key in SEVERITY_ORDER}
    for item in findings:
        severity = item.get("severity", "info")
        counts[severity] = counts.get(severity, 0) + 1
    return counts


def history_status_by_fingerprint(payload: dict) -> dict[str, str]:
    history = payload.get("history") or {}
    out: dict[str, str] = {}
    for key, label in (("new", "新增"), ("persistent", "持续"), ("resolved", "已解决")):
        for item in history.get(key, []):
            out[str(item.get("fingerprint") or finding_fingerprint(item))] = label
    return out


def format_compact_finding(item: dict) -> str:
    severity = SEVERITY_LABELS.get(item.get("severity"), item.get("severity", "未知"))
    return f"- {severity}｜`{item.get('rule_id', '?')}` {item.get('title', '未命名问题')}（{format_location(item)}）"


def format_location(item: dict) -> str:
    location = item.get("file") or "仓库整体"
    if item.get("line"):
        location += f":{item['line']}"
    return f"`{location}`"


def render_machine_state(payload: dict) -> list[str]:
    return [
        "**自动化状态（供脚本读取，请勿编辑）：**",
        "",
        "以下 JSON 区块供下一次巡检自动读取，用于判断问题是否新增、持续或已解决；人工阅读时可忽略。",
        "",
        "```json",
        json.dumps({"harken_compliance_state": compact_machine_state(payload)}, ensure_ascii=False, separators=(",", ":")),
        "```",
        "",
    ]


def compact_machine_state(payload: dict) -> dict:
    keys = (
        "rule_id",
        "severity",
        "category",
        "title",
        "file",
        "line",
        "evidence",
        "fingerprint",
    )
    findings = []
    for item in payload.get("findings", []):
        findings.append({key: item.get(key) for key in keys if item.get(key) is not None})
    return {
        "schema_version": payload.get("schema_version"),
        "generated_at": payload.get("generated_at"),
        "project": payload.get("project"),
        "scope": payload.get("scope"),
        "highest_severity": payload.get("highest_severity"),
        "summary": payload.get("summary"),
        "findings": findings,
    }


def build_payload(
    findings: list[Finding],
    pull_request: PullRequestContext | None,
) -> dict:
    sorted_findings = sort_findings(findings)
    payload = {
        "schema_version": 1,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "project": "harken",
        "scope": "full_tracked_tree_and_pull_request" if pull_request else "full_tracked_tree",
        "highest_severity": highest_severity(sorted_findings),
        "summary": summarize(sorted_findings),
        "findings": [finding.as_dict() for finding in sorted_findings],
    }
    return payload


def run_audit() -> dict:
    files = tracked_files()
    findings: list[Finding] = []
    pull_request = load_pull_request_context()
    rule_pull_request_governance(pull_request, findings)
    rule_pull_request_dco(pull_request, findings)
    rule_license(files, findings)
    rule_third_party_notices(files, findings)
    rule_license_policy(files, findings)
    rule_fetchcontent_hashes(files, findings)
    rule_dependency_git_pins(files, findings)
    rule_telemetry(files, findings)
    rule_credential_storage(files, findings)
    rule_build_identity(files, findings)
    rule_miniaudio_scope(files, findings)
    rule_local_files(findings)
    return build_payload(findings, pull_request)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-out", default=".temp/compliance/compliance-report.json")
    parser.add_argument("--markdown-out", default=".temp/compliance/compliance-report.md")
    parser.add_argument(
        "--fail-on",
        choices=tuple(SEVERITY_ORDER),
        help="Return exit code 1 when this severity or a higher severity is present.",
    )
    parser.add_argument(
        "--fail-category",
        choices=tuple(CATEGORY_LABELS),
        help="Limit --fail-on to findings in this category.",
    )
    args = parser.parse_args(argv)

    payload = run_audit()
    json_path = REPO_ROOT / args.json_out
    md_path = REPO_ROOT / args.markdown_out
    json_path.parent.mkdir(parents=True, exist_ok=True)
    md_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    md_path.write_text(render_markdown(payload), encoding="utf-8")

    summary = payload["summary"]
    print(
        "compliance audit complete: "
        f"highest={payload['highest_severity']} "
        f"critical={summary.get('critical', 0)} high={summary.get('high', 0)} "
        f"medium={summary.get('medium', 0)} low={summary.get('low', 0)} "
        f"info={summary.get('info', 0)}"
    )
    print(f"json: {json_path}")
    print(f"markdown: {md_path}")
    if args.fail_on:
        threshold = SEVERITY_ORDER[args.fail_on]
        if any(
            SEVERITY_ORDER[item["severity"]] <= threshold
            and (not args.fail_category or item["category"] == args.fail_category)
            for item in payload["findings"]
        ):
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
