#!/usr/bin/env python3
"""Validate, compare, and publish docs/wiki to the GitHub Wiki repository."""

from __future__ import annotations

import argparse
import base64
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "docs" / "wiki"
SAFE_PAGE = re.compile(r"^[A-Za-z0-9_.-]+\.md$")


def run_git(arguments: list[str], cwd: Path | None = None, token: str = "") -> str:
    env = os.environ.copy()
    if token:
        credential = base64.b64encode(f"x-access-token:{token}".encode()).decode()
        env["GIT_CONFIG_COUNT"] = "1"
        env["GIT_CONFIG_KEY_0"] = "http.https://github.com/.extraheader"
        env["GIT_CONFIG_VALUE_0"] = f"AUTHORIZATION: basic {credential}"
    result = subprocess.run(
        ["git", *arguments],
        cwd=cwd,
        env=env,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"Git operation failed: {detail}")
    return result.stdout.strip()


def canonical_pages(source: Path) -> dict[str, str]:
    if not source.is_dir():
        raise ValueError(f"Wiki source directory does not exist: {source}")
    pages: dict[str, str] = {}
    for path in sorted(source.iterdir(), key=lambda item: item.name.casefold()):
        if path.is_dir():
            raise ValueError(f"Wiki source may not contain subdirectories: {path.name}")
        if not path.is_file() or not SAFE_PAGE.fullmatch(path.name):
            raise ValueError(f"Unexpected wiki source entry: {path.name}")
        pages[path.name] = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    required = {"Home.md", "_Sidebar.md", "_Footer.md"}
    missing = sorted(required - pages.keys())
    if missing:
        raise ValueError(f"Required wiki pages are missing: {', '.join(missing)}")
    if len(pages) < 20:
        raise ValueError(f"Expected a complete wiki, found only {len(pages)} pages")
    return pages


def repository_pages(clone: Path) -> dict[str, str]:
    return {
        path.name: path.read_text(encoding="utf-8").replace("\r\n", "\n")
        for path in clone.glob("*.md")
        if path.is_file()
    }


def describe_drift(source: dict[str, str], published: dict[str, str]) -> list[str]:
    source_names = set(source)
    published_names = set(published)
    lines = [f"add {name}" for name in sorted(source_names - published_names)]
    lines += [f"remove {name}" for name in sorted(published_names - source_names)]
    lines += [
        f"update {name}"
        for name in sorted(source_names & published_names)
        if source[name] != published[name]
    ]
    return lines


def synchronize(source: dict[str, str], clone: Path) -> None:
    for path in clone.glob("*.md"):
        if path.name not in source:
            path.unlink()
    for name, content in source.items():
        (clone / name).write_text(content, encoding="utf-8", newline="\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("validate", "check", "publish"), required=True)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument(
        "--repository",
        default=os.environ.get("GITHUB_REPOSITORY", "codingmachineedge/WimForge"),
    )
    parser.add_argument("--token-env", default="GITHUB_TOKEN")
    parser.add_argument(
        "--commit-message",
        default="docs: synchronize the WimForge wiki / 文件：同步 WimForge Wiki",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source = canonical_pages(args.source.resolve())
    print(f"Validated {len(source)} canonical wiki pages.")
    if args.mode == "validate":
        return 0

    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
        raise ValueError(f"Invalid GitHub repository name: {args.repository}")
    remote = f"https://github.com/{args.repository}.wiki.git"
    token = os.environ.get(args.token_env, "") if args.mode == "publish" else ""
    if args.mode == "publish" and not token:
        raise ValueError(f"Publish mode requires a token in {args.token_env}")

    with tempfile.TemporaryDirectory(prefix="wimforge-wiki-") as temporary:
        clone = Path(temporary) / "wiki"
        run_git(["clone", "--depth=1", remote, str(clone)], token=token)
        drift = describe_drift(source, repository_pages(clone))
        if args.mode == "check":
            if drift:
                print("Wiki drift detected:")
                print("\n".join(f"  - {line}" for line in drift))
                return 1
            print("The live GitHub Wiki matches docs/wiki.")
            return 0

        if not drift:
            print("The live GitHub Wiki is already current.")
            return 0

        synchronize(source, clone)
        run_git(["add", "--all", "--", "."], cwd=clone)
        staged = run_git(["diff", "--cached", "--name-only"], cwd=clone).splitlines()
        unexpected = [name for name in staged if not SAFE_PAGE.fullmatch(name)]
        if unexpected:
            raise RuntimeError(f"Refusing to publish unexpected files: {', '.join(unexpected)}")
        run_git(["config", "user.name", "github-actions[bot]"], cwd=clone)
        run_git(
            ["config", "user.email", "41898282+github-actions[bot]@users.noreply.github.com"],
            cwd=clone,
        )
        run_git(["commit", "-m", args.commit_message], cwd=clone)
        run_git(["push", "origin", "HEAD:master"], cwd=clone, token=token)
        print(f"Published {len(staged)} changed wiki pages in a separate Wiki commit.")
        return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
