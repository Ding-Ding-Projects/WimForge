#!/usr/bin/env python3
"""Dependency-free structural and local-link checks for the WimForge Pages website."""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit


EXPECTED_GENERATED_ASSETS = (
    "assets/brand/wimforge-logo.png",
    "assets/site/hero-forge.webp",
    "assets/site/vm-lab.webp",
    "assets/site/image-servicing.webp",
    "assets/site/unattended.webp",
    "assets/site/package-studio.webp",
    "assets/site/gpo-studio.webp",
    "assets/site/history-time-machine.webp",
    "assets/site/safety-guardrails.webp",
    "assets/site/automation-cli.webp",
    "assets/site/workflow-overview.webp",
)


class SiteParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.ids: list[str] = []
        self.references: list[tuple[str, str, int]] = []
        self.images: list[tuple[dict[str, str | None], int]] = []
        self.copy_targets: list[tuple[str, int]] = []
        self.title_count = 0
        self.description_count = 0
        self.lang: str | None = None

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if tag == "html":
            self.lang = values.get("lang")
        if tag == "title":
            self.title_count += 1
        if tag == "meta" and values.get("name", "").lower() == "description":
            self.description_count += 1
        if values.get("id"):
            self.ids.append(values["id"] or "")
        if tag in {"a", "link"} and values.get("href"):
            self.references.append((tag, values["href"] or "", self.getpos()[0]))
        if tag in {"img", "script", "source"} and values.get("src"):
            self.references.append((tag, values["src"] or "", self.getpos()[0]))
        if tag == "img":
            self.images.append((values, self.getpos()[0]))
        if tag == "button" and values.get("data-copy-target"):
            self.copy_targets.append((values["data-copy-target"] or "", self.getpos()[0]))


def is_external(value: str) -> bool:
    scheme = urlsplit(value).scheme.lower()
    return bool(scheme) or value.startswith("//")


def resolve_local(source_file: Path, repo_root: Path, value: str) -> Path | None:
    path = unquote(urlsplit(value).path)
    if not path or path.startswith("/"):
        return None
    if path.startswith("assets/"):
        return repo_root / path
    return source_file.parent / path


def check_html(html_file: Path, repo_root: Path) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    parser = SiteParser()
    parser.feed(html_file.read_text(encoding="utf-8"))

    if parser.lang != "en":
        errors.append(f"{html_file}: expected html lang='en'")
    if parser.title_count != 1:
        errors.append(f"{html_file}: expected exactly one title element")
    if parser.description_count != 1:
        errors.append(f"{html_file}: expected exactly one meta description")

    duplicates = sorted({value for value in parser.ids if parser.ids.count(value) > 1})
    if duplicates:
        errors.append(f"{html_file}: duplicate ids: {', '.join(duplicates)}")

    known_ids = set(parser.ids)
    for target, line in parser.copy_targets:
        if target not in known_ids:
            errors.append(f"{html_file}:{line}: copy control targets missing id #{target}")

    for tag, value, line in parser.references:
        if value.startswith("#"):
            if value[1:] and value[1:] not in known_ids:
                errors.append(f"{html_file}:{line}: unresolved fragment {value}")
            continue
        if is_external(value) or value.startswith(("mailto:", "tel:", "data:")):
            continue
        local = resolve_local(html_file, repo_root, value)
        if local is not None and not local.exists():
            if local.as_posix().endswith(tuple(EXPECTED_GENERATED_ASSETS)):
                warnings.append(f"planned generated asset missing: {local.relative_to(repo_root)}")
            else:
                errors.append(f"{html_file}:{line}: missing local {tag} target {value}")

    for values, line in parser.images:
        if "alt" not in values:
            errors.append(f"{html_file}:{line}: image is missing alt text")
        if not values.get("width") and "data-lightbox-image" not in values:
            warnings.append(f"{html_file}:{line}: image has no intrinsic width")
        if not values.get("height") and "data-lightbox-image" not in values:
            warnings.append(f"{html_file}:{line}: image has no intrinsic height")

    return errors, warnings


def check_css(css_file: Path, repo_root: Path) -> list[str]:
    errors: list[str] = []
    text = css_file.read_text(encoding="utf-8")
    for value in re.findall(r"url\(\s*['\"]?([^)'\"]+)", text):
        if is_external(value) or value.startswith("data:"):
            continue
        local = resolve_local(css_file, repo_root, value)
        if local is not None and not local.exists():
            errors.append(f"{css_file}: missing CSS asset {value}")
    return errors


def read_png_dimensions(path: Path) -> tuple[int, int] | None:
    try:
        header = path.read_bytes()[:24]
    except OSError:
        return None
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", header[16:24])


def check_manifest(manifest_file: Path, repo_root: Path) -> list[str]:
    errors: list[str] = []
    try:
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"manifest.webmanifest: {exc}"]

    icons = manifest.get("icons") if isinstance(manifest, dict) else None
    if not isinstance(icons, list) or not icons:
        return ["manifest.webmanifest: expected at least one icon"]

    for index, icon in enumerate(icons):
        if not isinstance(icon, dict):
            errors.append(f"manifest.webmanifest: icon {index} must be an object")
            continue
        source = icon.get("src")
        declared_sizes = icon.get("sizes")
        if not isinstance(source, str) or not source:
            errors.append(f"manifest.webmanifest: icon {index} is missing src")
            continue
        local = resolve_local(manifest_file, repo_root, source)
        if local is None or not local.is_file():
            errors.append(f"manifest.webmanifest: icon {index} target is missing: {source}")
            continue
        if local.suffix.lower() != ".png" or not isinstance(declared_sizes, str):
            continue
        actual_size = read_png_dimensions(local)
        if actual_size is None:
            errors.append(f"manifest.webmanifest: icon {index} is not a valid PNG: {source}")
            continue
        expected = f"{actual_size[0]}x{actual_size[1]}"
        if expected not in declared_sizes.split():
            errors.append(
                f"manifest.webmanifest: icon {index} declares {declared_sizes!r}, "
                f"but {source} is {expected}"
            )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--require-assets", action="store_true", help="Fail when planned generated images are missing")
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    site_root = repo_root / "website"

    errors: list[str] = []
    warnings: list[str] = []
    for html_file in sorted(site_root.glob("*.html")):
        html_errors, html_warnings = check_html(html_file, repo_root)
        errors.extend(html_errors)
        warnings.extend(html_warnings)
    for css_file in sorted(site_root.glob("*.css")):
        errors.extend(check_css(css_file, repo_root))

    errors.extend(check_manifest(site_root / "manifest.webmanifest", repo_root))

    missing_assets = [path for path in EXPECTED_GENERATED_ASSETS if not (repo_root / path).is_file()]
    if args.require_assets and missing_assets:
        errors.extend(f"required generated asset missing: {path}" for path in missing_assets)

    for warning in sorted(set(warnings)):
        print(f"warning: {warning}")
    for error in errors:
        print(f"error: {error}", file=sys.stderr)

    if errors:
        print(f"site check failed with {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"site check passed ({len(set(warnings))} warning(s), {len(EXPECTED_GENERATED_ASSETS) - len(missing_assets)} generated assets present)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
