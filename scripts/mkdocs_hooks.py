"""Build-time compatibility between GitHub Wiki links and MkDocs links."""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
WIKI_PAGES = {path.stem for path in (ROOT / "docs" / "wiki").glob("*.md")}
WIKI_LINK = re.compile(
    r"(?P<prefix>\]\()"
    r"(?P<page>[A-Za-z0-9_.-]+)"
    r"(?P<fragment>#[^)\s]+)?"
    r"(?P<suffix>\))"
)
WIKI_INDEX_MARKER = "<!-- WIMFORGE_WIKI_INDEX -->"


def wiki_page_index() -> str:
    """Build the Pages wiki directory from the canonical mirror automatically."""
    pages: list[tuple[str, str]] = []
    for path in sorted((ROOT / "docs" / "wiki").glob("*.md")):
        if path.stem.startswith("_"):
            continue
        content = path.read_text(encoding="utf-8")
        heading = re.search(r"^#\s+(.+?)\s*$", content, re.MULTILINE)
        title = heading.group(1) if heading else path.stem.replace("-", " ")
        pages.append((title, path.name))

    pages.sort(key=lambda page: (page[1] != "Home.md", page[0].casefold()))
    return "\n".join(f"- [{title}](wiki/{name})" for title, name in pages)


def on_page_markdown(markdown: str, page, **kwargs) -> str:
    """Append .md to Wiki-style page slugs only for the Material site build."""
    source_uri = page.file.src_uri.replace("\\", "/")
    if source_uri == "wiki.md":
        return markdown.replace(WIKI_INDEX_MARKER, wiki_page_index())
    if not source_uri.startswith("wiki/"):
        return markdown

    def replace(match: re.Match[str]) -> str:
        page_name = match.group("page")
        if page_name not in WIKI_PAGES:
            return match.group(0)
        fragment = match.group("fragment") or ""
        return f"]({page_name}.md{fragment})"

    return WIKI_LINK.sub(replace, markdown)

