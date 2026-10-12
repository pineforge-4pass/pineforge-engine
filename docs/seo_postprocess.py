#!/usr/bin/env python3
"""Add deterministic SEO metadata to Doxygen's top-level HTML pages."""
from __future__ import annotations

import argparse
from html import escape
from html.parser import HTMLParser
from pathlib import Path
import re
import sys
from urllib.parse import quote
import xml.etree.ElementTree as ET


SITE_URL = "https://cdocs.pineforge.dev/"
SITEMAP_NS = "http://www.sitemaps.org/schemas/sitemap/0.9"
# The opening layer description in docs/pages/index.md, condensed to one sentence.
LANDING_DESCRIPTION = (
    "PineForge's C++17 engine has two layers documented in this API reference: "
    "a kernel that knows nothing about PineScript or TradingView and a Pine "
    "adapter that reproduces TradingView's execution semantics."
)
DESCRIPTION_SUFFIX = " — PineForge C++17 engine API documentation."
PAGE_LINK = re.compile(r"(?P<page>(?:\./)?[^/\\:?#]+)\.html(?P<anchor>#.*)?\Z")
# Consume whole quoted attributes so href text inside another attribute's
# value (or a data-href attribute) cannot be mistaken for a link.
QUOTED_ATTRIBUTE = re.compile(
    r"(?P<prefix>\s+(?P<name>[^\s=/>]+)\s*=\s*)"
    r"(?P<quote>[\"'])(?P<value>.*?)(?P=quote)", re.DOTALL
)


def canonical_url(page: Path) -> str:
    return SITE_URL if page.name == "index.html" else SITE_URL + quote(page.stem, safe="")


def rewrite_href(attribute: re.Match) -> str:
    if attribute["name"].lower() != "href":
        return attribute[0]
    if "://" in attribute["value"] or "mailto:" in attribute["value"]:
        return attribute[0]
    link = PAGE_LINK.fullmatch(attribute["value"])
    if link is None:
        return attribute[0]
    target = link["page"]
    if target in ("index", "./index"):
        target = "./"
    return (attribute["prefix"] + attribute["quote"] + target
            + (link["anchor"] or "") + attribute["quote"])


class PageMarkup(HTMLParser):
    """Locate edits without reserializing Doxygen's markup or script contents."""

    def __init__(self, document: str) -> None:
        super().__init__(convert_charrefs=True)
        self.document = document
        self.line_offsets = [0] + [match.end() for match in re.finditer("\n", document)]
        self.edits: list[tuple[int, int, str]] = []
        self.descriptions: list[tuple[int, int]] = []
        self.head_ends: list[int] = []
        self.title_parts: list[str] = []
        self.in_title = False

    def source_offset(self) -> int:
        line, column = self.getpos()
        return self.line_offsets[line - 1] + column

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        raw = self.get_starttag_text()
        start = self.source_offset()
        end = start + len(raw)
        attributes = dict(attrs)
        if tag == "title":
            self.in_title = True
        if tag == "link" and "canonical" in (attributes.get("rel") or "").lower().split():
            # Remove the following newline too, keeping a second pass byte-identical.
            if self.document[end:end + 1] == "\n":
                end += 1
            self.edits.append((start, end, ""))
        elif tag == "meta" and (attributes.get("name") or "").lower() == "description":
            self.descriptions.append((start, end))
        else:
            rewritten = QUOTED_ATTRIBUTE.sub(rewrite_href, raw)
            if rewritten != raw:
                self.edits.append((start, end, rewritten))

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.handle_starttag(tag, attrs)

    def handle_endtag(self, tag: str) -> None:
        if tag == "title":
            self.in_title = False
        elif tag == "head":
            self.head_ends.append(self.source_offset())

    def handle_data(self, data: str) -> None:
        if self.in_title:
            self.title_parts.append(data)


def process_page(page: Path, document: str) -> str:
    if page.name == "404.html":
        # An error page is not a documentation page: no canonical, no sitemap entry.
        return document
    markup = PageMarkup(document)
    markup.feed(document)
    markup.close()
    if len(markup.head_ends) != 1:
        raise ValueError(f"{page.name}: expected exactly one </head>")
    title = "".join(markup.title_parts).strip()
    if not title:
        raise ValueError(f"{page.name}: missing page title")
    if title.startswith("PineForge: "):
        title = title[len("PineForge: "):]
    description = LANDING_DESCRIPTION if page.name == "index.html" else title + DESCRIPTION_SUFFIX
    meta = f'<meta name="description" content="{escape(description, quote=True)}"/>'
    addition = f'<link rel="canonical" href="{escape(canonical_url(page), quote=True)}"/>\n'
    if markup.descriptions:
        for number, (start, end) in enumerate(markup.descriptions):
            markup.edits.append((start, end, meta if number == 0 else ""))
    else:
        addition = meta + "\n" + addition
    head_end = markup.head_ends[0]
    markup.edits.append((head_end, head_end, addition))
    for start, end, replacement in sorted(markup.edits, reverse=True):
        document = document[:start] + replacement + document[end:]
    return document


def postprocess(site: Path) -> int:
    if not site.is_dir():
        raise ValueError(f"not a site directory: {site}")
    pages = sorted(page for page in site.glob("*.html") if page.is_file())
    if not any(page.name != "404.html" for page in pages):
        raise ValueError("site has no top-level documentation pages")
    # Validate all pages before writing any of them.
    processed = [(page, process_page(page, page.read_bytes().decode("utf-8"))) for page in pages]
    for page, document in processed:
        page.write_bytes(document.encode("utf-8"))

    ET.register_namespace("", SITEMAP_NS)
    urlset = ET.Element(f"{{{SITEMAP_NS}}}urlset")
    for url in sorted(canonical_url(page) for page in pages if page.name != "404.html"):
        entry = ET.SubElement(urlset, f"{{{SITEMAP_NS}}}url")
        ET.SubElement(entry, f"{{{SITEMAP_NS}}}loc").text = url
    (site / "sitemap.xml").write_bytes(ET.tostring(urlset, encoding="utf-8", xml_declaration=True) + b"\n")
    return len(pages)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("site", type=Path, help="Doxygen HTML output directory")
    args = parser.parse_args()
    try:
        count = postprocess(args.site)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"seo_postprocess: {error}", file=sys.stderr)
        return 1
    print(f"==> SEO post-processing: {count} pages and sitemap.xml")
    return 0


if __name__ == "__main__":
    sys.exit(main())
