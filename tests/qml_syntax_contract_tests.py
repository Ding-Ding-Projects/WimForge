#!/usr/bin/env python3
"""Guard the repaired sharp-radius bindings and parse the actual QML sources."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys


# Exact inventory of bindings malformed by the July 26 sharp-corner change.
EXPECTED_BINDINGS = {
    "qml/Main.qml": 21,
    "qml/components/ContextHistoryPanel.qml": 4,
    "qml/components/MetricCard.qml": 2,
    "qml/components/NotificationCenter.qml": 3,
    "qml/components/ProjectStartPage.qml": 5,
    "qml/components/SearchPalette.qml": 3,
    "qml/components/Snackbar.qml": 1,
    "qml/components/UpdateCatalogSheet.qml": 3,
    "qml/components/WfButton.qml": 2,
    "qml/components/WfCard.qml": 2,
    "qml/components/WfField.qml": 1,
    "qml/components/WfIconButton.qml": 2,
    "qml/components/WfStatusChip.qml": 2,
    "qml/components/WfTabBar.qml": 1,
    "qml/pages/CustomizePage.qml": 10,
    "qml/pages/DashboardPage.qml": 5,
    "qml/pages/GpoStudioPage.qml": 8,
    "qml/pages/HistoryPage.qml": 8,
    "qml/pages/PackageStudioPage.qml": 2,
    "qml/pages/PlanPage.qml": 2,
    "qml/pages/SettingsPage.qml": 3,
    "qml/pages/SourcePage.qml": 2,
    "qml/pages/TerminalPage.qml": 3,
    "qml/pages/VmLabPage.qml": 11,
    "qml/pages/WinForgeBridgePage.qml": 3,
}
RADIUS = re.compile(r"\bradius\s*:\s*([^;\r\n}]+)")
NON_CODE = re.compile(r"//[^\r\n]*|/\*[\s\S]*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'")


def code_only(text: str) -> str:
    return NON_CODE.sub(lambda match: re.sub(r"[^\r\n]", " ", match.group()), text)


def validate(sources: dict[str, str]) -> list[str]:
    errors = []
    total = 0
    for name, expected in EXPECTED_BINDINGS.items():
        values = RADIUS.findall(code_only(sources[name]))
        total += len(values)
        if len(values) != expected:
            errors.append(f"{name}: expected {expected} radius bindings, found {len(values)}")
        if any(value.strip() != "1" for value in values):
            errors.append(f"{name}: sharp-radius expressions must be exactly 1")
    if total != 109:
        errors.append(f"expected 109 inventoried radius bindings, found {total}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--require-parser", action="store_true")
    args = parser.parse_args()
    sources = {name: (args.repo_root / name).read_text(encoding="utf-8-sig")
               for name in EXPECTED_BINDINGS}
    errors = validate(sources)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Radius contract: 109 bindings in 25 files passed")

    if args.self_test:
        negative_checks = 0
        for name in EXPECTED_BINDINGS:
            first = RADIUS.search(code_only(sources[name]))
            assert first is not None
            for replacement in ("radius: 2", ""):
                mutated = dict(sources)
                mutated[name] = (sources[name][:first.start()] + replacement
                                 + sources[name][first.end():])
                if not validate(mutated):
                    print(f"Negative regression failed to reject {name}: {replacement!r}",
                          file=sys.stderr)
                    return 1
                negative_checks += 1
        if validate(sources):
            print("Restored source contract failed", file=sys.stderr)
            return 1
        print(f"Negative regressions: {negative_checks} mutations rejected; restored sources passed")

    qmlformat = shutil.which("qmlformat") or shutil.which("qmlformat6")
    if not qmlformat:
        print("QML parser: not run (qmlformat unavailable)")
        return 1 if args.require_parser else 0
    qml_files = sorted((args.repo_root / "qml").rglob("*.qml"))
    for path in qml_files:
        result = subprocess.run([qmlformat, str(path)], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        if result.returncode:
            print(f"QML parser failed for {path.relative_to(args.repo_root)}:\n{result.stderr}",
                  file=sys.stderr)
            return 1
    print(f"QML parser: {len(qml_files)} real source files passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
