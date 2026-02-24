#!/usr/bin/env python3
"""
fetch_wiki.py — Build-time data pipeline for The Emu Pages

Auto-discovers all EmuVR wiki pages via the MediaWiki API,
fetches content via Special:Export, resolves redirects,
converts wikitext to plain text, and generates src/wiki_data.h
as a C header with embedded content arrays.

Usage:
    python3 tools/fetch_wiki.py
"""

import json
import re
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

# ── Configuration ──────────────────────────────────────────────

WIKI_BASE = "https://www.emuvr.net/wiki"
API_URL = "https://www.emuvr.net/w/api.php"
EXPORT_URL = f"{WIKI_BASE}/Special:Export"
MW_NS = "http://www.mediawiki.org/xml/export-0.10/"
LINE_WIDTH = 74  # usable text columns (leave 2 for left margin)
OUTPUT_FILE = "src/wiki_data.h"

# Preferred TOC order (from wiki sidebar navigation).
# Pages not listed here get appended alphabetically at the end.
# Section-redirect pages are inserted after their parent page.
PREFERRED_ORDER = [
    "Updates",
    "Installation Guide",
    "How To Play",
    "Controls",
    "Customization",
    "Netplay",
    "Light Guns",
    "Room Saving",
    "Playing Videos and Music",
    "DOSBox Games",
    "Adding DOSBox Games",
    "Keyboard and Mouse Input For Games",
    "Settings",
    "FAQ",
    "Troubleshooting",
]

# Pages to exclude (landing page replaces Main Page)
EXCLUDE_PAGES = {"Main Page"}

# Line types for the C header
LINE_NORMAL = 0
LINE_H2 = 1
LINE_H3 = 2
LINE_H4 = 3
LINE_BLANK = 0  # blank lines use LINE_NORMAL

# ── API Discovery ─────────────────────────────────────────────

def api_query(params):
    """Make a MediaWiki API query and return the JSON response."""
    params["format"] = "json"
    query_string = "&".join(f"{k}={urllib.request.quote(str(v))}" for k, v in params.items())
    url = f"{API_URL}?{query_string}"
    req = urllib.request.Request(url, headers={"User-Agent": "EmuPages/1.0"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())


def discover_all_pages():
    """Discover all content pages in the wiki via the API.

    Returns a list of page titles (strings).
    """
    titles = []
    params = {"action": "query", "list": "allpages", "aplimit": "500", "apnamespace": "0"}

    while True:
        data = api_query(params)
        for page in data.get("query", {}).get("allpages", []):
            titles.append(page["title"])
        # Handle pagination
        if "continue" in data:
            params["apcontinue"] = data["continue"]["apcontinue"]
        else:
            break

    return titles


def resolve_redirects(titles):
    """Resolve redirect pages via the API.

    Returns a dict mapping redirect source title to (target_title, fragment_or_None).
    """
    redirects = {}
    # API accepts up to 50 titles per query
    batch_size = 50
    for i in range(0, len(titles), batch_size):
        batch = titles[i:i + batch_size]
        pipe_titles = "|".join(batch)
        data = api_query({
            "action": "query",
            "titles": pipe_titles,
            "redirects": "1",
        })
        for redir in data.get("query", {}).get("redirects", []):
            source = redir["from"]
            target = redir["to"]
            fragment = redir.get("tofragment")
            redirects[source] = (target, fragment)

    return redirects


# ── Fetch ──────────────────────────────────────────────────────

def fetch_page(title):
    """Fetch a single page from MediaWiki Special:Export."""
    url_title = title.replace(" ", "_")
    url = f"{EXPORT_URL}/{url_title}"
    print(f"  Fetching: {title} ...", end=" ", flush=True)
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "EmuPages/1.0"})
        with urllib.request.urlopen(req, timeout=30) as resp:
            xml_data = resp.read()
    except Exception as e:
        print(f"FAILED: {e}")
        return title, ""

    root = ET.fromstring(xml_data)
    ns = {"mw": MW_NS}

    page = root.find(".//mw:page", ns)
    if page is None:
        print("FAILED: no <page> element")
        return title, ""

    page_title = page.find("mw:title", ns)
    display_title = page_title.text if page_title is not None else title

    text_elem = page.find(".//mw:revision/mw:text", ns)
    wikitext = text_elem.text if (text_elem is not None and text_elem.text) else ""

    print(f"OK ({len(wikitext)} chars)")
    return display_title, wikitext


def extract_section(wikitext, fragment):
    """Extract a section from wikitext by heading fragment.

    Finds the heading matching `fragment` and returns everything from that
    heading down to the next heading of equal or higher level.
    """
    # Normalize fragment for matching: underscores to spaces
    target = fragment.replace("_", " ").strip()

    lines = wikitext.split("\n")
    result_lines = []
    capturing = False
    capture_level = 0

    for line in lines:
        # Check if this is a heading
        heading_match = re.match(r"^(={2,})\s*(.+?)\s*\1$", line)
        if heading_match:
            level = len(heading_match.group(1))
            heading_text = heading_match.group(2).strip()
            # Strip inline markup from heading for comparison
            clean_heading = re.sub(r"'{2,3}", "", heading_text)
            clean_heading = re.sub(r"\[\[[^|\]]*\|([^\]]+)\]\]", r"\1", clean_heading)
            clean_heading = re.sub(r"\[\[([^\]]+)\]\]", r"\1", clean_heading)
            clean_heading = clean_heading.strip()

            if capturing:
                # Stop if we hit a heading at same or higher level
                if level <= capture_level:
                    break
                result_lines.append(line)
            elif clean_heading.lower() == target.lower():
                capturing = True
                capture_level = level
                result_lines.append(line)
        elif capturing:
            result_lines.append(line)

    return "\n".join(result_lines) if result_lines else ""


# ── TOC Ordering ───────────────────────────────────────────────

def order_pages(content_titles, section_redirects):
    """Order pages for the TOC.

    content_titles: list of non-redirect page titles
    section_redirects: dict of {redirect_title: (target_title, fragment)}

    Returns ordered list of all titles to include.
    """
    ordered = []
    remaining = set(content_titles)

    # Build a map of target_title -> list of section redirect titles
    parent_children = {}
    for redir_title, (target, _frag) in section_redirects.items():
        parent_children.setdefault(target, []).append(redir_title)

    # First pass: add pages in preferred order
    for title in PREFERRED_ORDER:
        if title in remaining:
            ordered.append(title)
            remaining.discard(title)
            # Insert any section redirects that point to this page
            for child in sorted(parent_children.get(title, [])):
                ordered.append(child)

    # Second pass: add remaining pages alphabetically
    for title in sorted(remaining):
        ordered.append(title)
        for child in sorted(parent_children.get(title, [])):
            ordered.append(child)

    return ordered


# ── Landing Page ───────────────────────────────────────────────

def generate_landing_page(page_count, build_date):
    """Generate The Emu Pages landing page content."""
    lines = []
    lines.append(("Welcome to the EmuVR Wiki", LINE_H2))
    lines.append(("", LINE_BLANK))
    lines.append(("Webhead wanted the wiki to be more", LINE_NORMAL))
    lines.append(("accessible in-game. So naturally someone", LINE_NORMAL))
    lines.append(("built a Commodore 64 that reads it to you.", LINE_NORMAL))
    lines.append(("", LINE_BLANK))
    lines.append(("Every page from emuvr.net/wiki is baked", LINE_NORMAL))
    lines.append(("right into this core. No internet needed.", LINE_NORMAL))
    lines.append(("Just load it up and read.", LINE_NORMAL))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("Controls", LINE_H2))
    lines.append(("", LINE_BLANK))
    lines.append(("  D-Pad Up/Down    Move cursor / scroll", LINE_NORMAL))
    lines.append(("  A                Open page", LINE_NORMAL))
    lines.append(("  B / Start        Back to contents", LINE_NORMAL))
    lines.append(("  D-Pad Left/Right Previous / next page", LINE_NORMAL))
    lines.append(("  L / R Shoulder   Page up / page down", LINE_NORMAL))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append(("", LINE_BLANK))
    lines.append((f"The Emu Pages  -  {page_count} pages  -  {build_date}", LINE_NORMAL))
    return lines


# ── Wikitext → Plain Text ─────────────────────────────────────

def parse_wiki_table(table_text):
    """Convert a wiki table to readable plain text lines.

    Returns a list of plain text lines representing the table.
    """
    lines = table_text.strip().split("\n")
    headers = []
    rows = []
    current_row = []
    in_header = False

    for line in lines:
        line = line.strip()

        # Skip table open/close and caption
        if line.startswith("{|") or line.startswith("|}"):
            continue
        if line.startswith("|+"):
            # Table caption
            caption = line[2:].strip()
            caption = strip_inline_markup(caption)
            if caption:
                rows.append(("caption", caption))
            continue

        # Row separator
        if line.startswith("|-"):
            if current_row:
                if in_header:
                    headers = current_row
                else:
                    rows.append(("row", current_row))
                current_row = []
            in_header = False
            continue

        # Header cell
        if line.startswith("!"):
            in_header = True
            cells = re.split(r"\|\|", line.lstrip("!"))
            for cell in cells:
                # Strip wiki links first (they contain | that isn't a style separator)
                cell = strip_inline_markup(cell)
                # Strip style attributes on what remains
                if "|" in cell:
                    cell = cell.split("|", 1)[-1]
                current_row.append(cell.strip())
            continue

        # Data cell
        if line.startswith("|"):
            cells = re.split(r"\|\|", line.lstrip("|"))
            for cell in cells:
                # Strip wiki links first (they contain | that isn't a style separator)
                cleaned = strip_inline_markup(cell)
                # Strip style attributes on what remains
                if "|" in cleaned:
                    parts = cleaned.split("|", 1)
                    if "=" in parts[0] or parts[0].strip().startswith("class"):
                        cleaned = parts[1]
                current_row.append(cleaned.strip())
            continue

    # Flush last row
    if current_row:
        if in_header:
            headers = current_row
        else:
            rows.append(("row", current_row))

    # Format output
    output = []

    for item in rows:
        if item[0] == "caption":
            output.append(f"  [{item[1]}]")
            output.append("")
            continue

        row_data = item[1]
        if not headers:
            # No headers — just list the cells
            output.append("  " + " | ".join(row_data))
            continue

        # First cell is typically the row label
        if len(row_data) > 0:
            label = row_data[0] if row_data[0] else "(unnamed)"
            output.append(f"  {label}:")
            for i, cell in enumerate(row_data[1:], 1):
                if cell and i < len(headers):
                    header = headers[i] if i < len(headers) else f"Col {i}"
                    output.append(f"    {header}: {cell}")
                elif cell:
                    output.append(f"    {cell}")
            output.append("")

    return output


def strip_inline_markup(text):
    """Remove inline wikitext markup, keeping readable text."""
    # Remove templates: {{...}} (may be nested, so do multiple passes)
    for _ in range(3):
        text = re.sub(r"\{\{[^{}]*\}\}", "", text)

    # HTML line breaks to spaces
    text = re.sub(r"<br\s*/?>", " / ", text, flags=re.IGNORECASE)

    # Remove all HTML tags (keep content). Broad pattern to catch center, ref, etc.
    text = re.sub(r"</?[a-zA-Z][^>]*>", "", text)

    # Bold/italic
    text = re.sub(r"'{2,3}", "", text)

    # Internal links: [[#anchor|Link]] → "" (strip wiki nav "Link" elements)
    text = re.sub(r"\[\[#[^|\]]*\|Link\]\]", "", text)
    # [[#anchor| *]] → "" (strip footnote asterisks)
    text = re.sub(r"\[\[#[^|\]]*\|\s*\*\s*\]\]", "", text)
    # [[#anchor|Display]] → Display
    text = re.sub(r"\[\[#[^|\]]*\|([^\]]+)\]\]", r"\1", text)
    # [[Page|Display]] → Display
    text = re.sub(r"\[\[[^|\]]*\|([^\]]+)\]\]", r"\1", text)
    # [[Page]] → Page
    text = re.sub(r"\[\[([^\]]+)\]\]", r"\1", text)

    # Clean up stray ]] from partially matched links
    text = text.replace("]]", "")

    # External links: [url text] → text
    text = re.sub(r"\[https?://[^\s\]]+ ([^\]]+)\]", r"\1", text)
    text = re.sub(r"\[https?://[^\]]+\]", "", text)
    # Bare URLs
    text = re.sub(r"https?://\S+", "", text)

    # Numeric HTML entities: &#42; → *, &#x2630; → #, etc.
    def decode_html_entity(m):
        try:
            if m.group(1).startswith("x"):
                return chr(int(m.group(1)[1:], 16))
            return chr(int(m.group(1)))
        except (ValueError, OverflowError):
            return "?"
    text = re.sub(r"&#(x?[0-9a-fA-F]+);", decode_html_entity, text)

    # Named HTML entities
    text = text.replace("&amp;", "&")
    text = text.replace("&lt;", "<")
    text = text.replace("&gt;", ">")
    text = text.replace("&quot;", '"')
    text = text.replace("&nbsp;", " ")

    # Collapse whitespace
    text = re.sub(r"  +", " ", text)

    return text.strip()


def wiki_to_plain(wikitext):
    """Convert wikitext to a list of (text, line_type) tuples."""
    lines_out = []

    text = wikitext

    # Normalize Unicode early (smart quotes, emojis, etc.)
    text = normalize_unicode(text)

    # Remove magic words
    text = re.sub(r"__[A-Z]+__", "", text)

    # Remove HTML comments
    text = re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL)

    # Remove images/files
    text = re.sub(r"\[\[(File|Image):[^\]]*\]\]", "", text, flags=re.IGNORECASE)

    # Remove categories
    text = re.sub(r"\[\[Category:[^\]]*\]\]", "", text, flags=re.IGNORECASE)

    # Handle collapsible divs — extract content, mark with a header
    text = re.sub(
        r'<div[^>]*class="mw-collapsible[^"]*"[^>]*data-expandtext="([^"]*)"[^>]*>',
        r"\n=== \1 ===\n",
        text,
        flags=re.IGNORECASE,
    )
    # Remove all remaining div tags (including </div>, <div ...>)
    text = re.sub(r"</?div[^>]*>", "", text, flags=re.IGNORECASE)

    # Remove templates: {{...}} (do before table extraction)
    for _ in range(3):
        text = re.sub(r"\{\{[^{}]*\}\}", "", text)

    # Extract and process tables separately
    # We'll replace tables with a placeholder, process them, then splice back
    tables = []
    def table_replacer(match):
        tables.append(match.group(0))
        return f"\n__TABLE_{len(tables) - 1}__\n"

    text = re.sub(r"\{\|.*?\|\}", table_replacer, text, flags=re.DOTALL)

    # Process line by line
    raw_lines = text.split("\n")
    num_counter = 0

    for raw_line in raw_lines:
        line = raw_line.rstrip()

        # Table placeholder
        table_match = re.match(r"^__TABLE_(\d+)__$", line.strip())
        if table_match:
            idx = int(table_match.group(1))
            table_lines = parse_wiki_table(tables[idx])
            lines_out.append(("", LINE_BLANK))
            for tl in table_lines:
                for wrapped in word_wrap(tl, LINE_WIDTH):
                    lines_out.append((wrapped, LINE_NORMAL))
            lines_out.append(("", LINE_BLANK))
            continue

        # Headings (must check before inline markup strip)
        h4 = re.match(r"^====\s*(.+?)\s*====$", line)
        if h4:
            lines_out.append(("", LINE_BLANK))
            lines_out.append((strip_inline_markup(h4.group(1)), LINE_H4))
            lines_out.append(("", LINE_BLANK))
            num_counter = 0
            continue

        h3 = re.match(r"^===\s*(.+?)\s*===$", line)
        if h3:
            lines_out.append(("", LINE_BLANK))
            lines_out.append((strip_inline_markup(h3.group(1)), LINE_H3))
            lines_out.append(("", LINE_BLANK))
            num_counter = 0
            continue

        h2 = re.match(r"^==\s*(.+?)\s*==$", line)
        if h2:
            lines_out.append(("", LINE_BLANK))
            lines_out.append((strip_inline_markup(h2.group(1)), LINE_H2))
            lines_out.append(("", LINE_BLANK))
            num_counter = 0
            continue

        # Strip inline markup for everything else
        line = strip_inline_markup(line)

        # Blank line
        if not line.strip():
            lines_out.append(("", LINE_BLANK))
            num_counter = 0
            continue

        # Nested bullets
        bullet3 = re.match(r"^\*\*\*\s*(.*)", line)
        if bullet3:
            for wrapped in word_wrap(f"      - {bullet3.group(1)}", LINE_WIDTH):
                lines_out.append((wrapped, LINE_NORMAL))
            continue

        bullet2 = re.match(r"^\*\*\s*(.*)", line)
        if bullet2:
            for wrapped in word_wrap(f"    - {bullet2.group(1)}", LINE_WIDTH):
                lines_out.append((wrapped, LINE_NORMAL))
            continue

        bullet1 = re.match(r"^\*\s*(.*)", line)
        if bullet1:
            for wrapped in word_wrap(f"  - {bullet1.group(1)}", LINE_WIDTH):
                lines_out.append((wrapped, LINE_NORMAL))
            continue

        # Numbered lists (with nesting)
        num2 = re.match(r"^##\*?\s*(.*)", line)
        if num2:
            for wrapped in word_wrap(f"    - {num2.group(1)}", LINE_WIDTH):
                lines_out.append((wrapped, LINE_NORMAL))
            continue

        num1 = re.match(r"^#\s*(.*)", line)
        if num1:
            num_counter += 1
            for wrapped in word_wrap(f"  {num_counter}. {num1.group(1)}", LINE_WIDTH):
                lines_out.append((wrapped, LINE_NORMAL))
            continue

        # Definition lists
        dl = re.match(r"^;(.+?):\s*(.*)", line)
        if dl:
            for wrapped in word_wrap(f"{dl.group(1)}: {dl.group(2)}", LINE_WIDTH):
                lines_out.append((wrapped, LINE_NORMAL))
            continue

        # Regular text
        for wrapped in word_wrap(line, LINE_WIDTH):
            lines_out.append((wrapped, LINE_NORMAL))

    # Clean up: collapse 3+ consecutive blank lines to 2
    cleaned = []
    blank_count = 0
    for text_val, line_type in lines_out:
        if text_val == "" and line_type == LINE_BLANK:
            blank_count += 1
            if blank_count <= 2:
                cleaned.append((text_val, line_type))
        else:
            blank_count = 0
            cleaned.append((text_val, line_type))

    # Strip leading/trailing blank lines
    while cleaned and cleaned[0][0] == "":
        cleaned.pop(0)
    while cleaned and cleaned[-1][0] == "":
        cleaned.pop()

    return cleaned


def word_wrap(text, width):
    """Word-wrap a single line of text to the given width.

    Returns a list of lines. Preserves leading indentation.
    """
    if len(text) <= width:
        return [text]

    # Detect leading whitespace
    stripped = text.lstrip()
    indent = text[: len(text) - len(stripped)]

    # For continuation lines, add extra indent
    cont_indent = indent + "  " if indent else "  "

    lines = []
    remaining = text

    first = True
    while len(remaining) > width:
        # Find last space before width limit
        break_at = remaining.rfind(" ", 0, width + 1)
        if break_at <= len(indent if first else cont_indent):
            # No good break point — hard break
            break_at = width
        lines.append(remaining[:break_at].rstrip())
        remaining = (cont_indent if not first else indent) + remaining[break_at:].lstrip()
        first = False

    if remaining.strip():
        lines.append(remaining)

    return lines


# ── C Header Generation ───────────────────────────────────────

def normalize_unicode(text):
    """Replace common Unicode characters with ASCII equivalents.

    Handles emojis, smart quotes, special symbols, etc. so the
    8x8 bitmap font (ASCII 32-127 only) can display them.
    """
    replacements = {
        # Smart quotes and apostrophes
        "\u2018": "'",   # '
        "\u2019": "'",   # '
        "\u201C": '"',   # "
        "\u201D": '"',   # "
        "\u00B4": "'",   # ´
        "\u0060": "'",   # `
        # Dashes
        "\u2013": "-",   # en dash
        "\u2014": "--",  # em dash
        "\u2012": "-",   # figure dash
        # Spaces
        "\u00A0": " ",   # non-breaking space
        "\u2003": " ",   # em space
        "\u2002": " ",   # en space
        # Arrows
        "\u2190": "<-",  # ←
        "\u2192": "->",  # →
        "\u2191": "^",   # ↑
        "\u2193": "v",   # ↓
        "\u21D2": "=>",  # ⇒
        # PlayStation symbols
        "\u2716": "X",      # ✖ (Cross)
        "\u2B24": "O",      # ⬤ (Circle)
        "\u25FC": "[]",     # ◼ (Square)
        "\u25B2": "/\\",    # ▲ (Triangle)
        # Other common symbols
        "\u2022": "-",   # • bullet
        "\u2026": "...", # … ellipsis
        "\u00D7": "x",   # × multiplication
        "\u2714": "[x]", # ✔ check mark
        "\u2718": "[ ]", # ✘ cross mark
        "\u2605": "*",   # ★
        "\u2606": "*",   # ☆
        "\u00A9": "(c)", # ©
        "\u00AE": "(R)", # ®
        "\u2122": "(TM)", # ™
        "\u00BD": "1/2", # ½
        "\u00BC": "1/4", # ¼
        "\u00BE": "3/4", # ¾
        "\u2630": "#",   # ☰ hamburger menu
    }

    # Emoji ranges to strip (replace with nothing or a symbol)
    # Common emoji codepoint ranges
    import unicodedata

    for old, new in replacements.items():
        text = text.replace(old, new)

    # Strip remaining non-ASCII: replace emojis/symbols with nothing
    result = []
    for ch in text:
        if ord(ch) < 128:
            result.append(ch)
        else:
            # Try to get a useful name-based replacement
            try:
                name = unicodedata.name(ch, "").lower()
            except ValueError:
                name = ""

            if not name:
                continue  # silently drop
            elif "arrow" in name:
                result.append(">")
            elif "bullet" in name or "dot" in name:
                result.append("-")
            elif "star" in name:
                result.append("*")
            elif "check" in name or "ballot" in name:
                result.append("x")
            elif "cross" in name:
                result.append("X")
            elif "dash" in name or "hyphen" in name:
                result.append("-")
            elif "space" in name:
                result.append(" ")
            elif "quotation" in name or "apostrophe" in name:
                result.append("'")
            # Silently drop all other non-ASCII (emojis, decorative icons)

    return "".join(result)


def escape_c_string(s):
    """Escape a string for use in a C string literal."""
    s = normalize_unicode(s)
    s = s.replace("\\", "\\\\")
    s = s.replace('"', '\\"')
    s = s.replace("\t", "\\t")
    # Any remaining non-ASCII (shouldn't happen after normalize)
    result = []
    for ch in s:
        if 32 <= ord(ch) <= 126:
            result.append(ch)
        elif ch == "\n":
            result.append("\\n")
        else:
            pass  # silently drop
    return "".join(result)


def generate_header(pages_data):
    """Generate the wiki_data.h C header content."""
    build_date = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    page_count = len(pages_data)

    out = []
    out.append("/* AUTO-GENERATED by tools/fetch_wiki.py -- DO NOT EDIT */")
    out.append(f"/* Built: {build_date} */")
    out.append("#ifndef WIKI_DATA_H")
    out.append("#define WIKI_DATA_H")
    out.append("")
    out.append(f"#define WIKI_PAGE_COUNT {page_count}")
    out.append(f'#define WIKI_BUILD_DATE "{build_date}"')
    out.append("")
    out.append("/* Line types */")
    out.append(f"#define LINE_NORMAL {LINE_NORMAL}")
    out.append(f"#define LINE_H2     {LINE_H2}")
    out.append(f"#define LINE_H3     {LINE_H3}")
    out.append(f"#define LINE_H4     {LINE_H4}")
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *text;")
    out.append("    int type;")
    out.append("} wiki_line_t;")
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *title;")
    out.append("    const wiki_line_t *lines;")
    out.append("    int line_count;")
    out.append("} wiki_page_t;")
    out.append("")

    # Generate per-page line arrays
    for i, (title, page_lines) in enumerate(pages_data):
        out.append(f"/* Page {i}: {title} */")
        out.append(f"static const wiki_line_t page_{i}_lines[] = {{")
        for text_val, line_type in page_lines:
            escaped = escape_c_string(text_val)
            out.append(f'    {{"{escaped}", {line_type}}},')
        out.append("};")
        out.append("")

    # Generate master array
    out.append("static const wiki_page_t wiki_pages[WIKI_PAGE_COUNT] = {")
    for i, (title, page_lines) in enumerate(pages_data):
        escaped_title = escape_c_string(title)
        count = len(page_lines)
        out.append(f'    {{"{escaped_title}", page_{i}_lines, {count}}},')
    out.append("};")
    out.append("")
    out.append("#endif /* WIKI_DATA_H */")

    return "\n".join(out) + "\n"


# ── Main ───────────────────────────────────────────────────────

def main():
    build_date = datetime.now(timezone.utc).strftime("%Y-%m-%d")

    print("The Emu Pages Wiki Fetcher")
    print("======================")
    print()

    # Step 1: Discover all pages
    print("Discovering pages via API ...")
    all_titles = discover_all_pages()
    print(f"  Found {len(all_titles)} pages")
    print()

    # Step 2: Resolve redirects
    print("Resolving redirects ...")
    redirects = resolve_redirects(all_titles)
    redirect_titles = set(redirects.keys())
    content_titles = [t for t in all_titles if t not in redirect_titles and t not in EXCLUDE_PAGES]

    if EXCLUDE_PAGES & set(all_titles):
        print(f"  Excluding: {', '.join(EXCLUDE_PAGES & set(all_titles))}")

    # Classify redirects
    section_redirects = {}  # has fragment — include with extracted section
    alias_redirects = {}    # no fragment — skip (target already in content)
    for source, (target, fragment) in redirects.items():
        if fragment:
            section_redirects[source] = (target, fragment)
            print(f"  {source} -> {target}#{fragment} (section redirect, will extract)")
        else:
            alias_redirects[source] = target
            print(f"  {source} -> {target} (alias, skipping)")

    print(f"  {len(content_titles)} content pages, "
          f"{len(section_redirects)} section redirects, "
          f"{len(alias_redirects)} aliases skipped")
    print()

    # Step 3: Determine TOC order
    ordered_titles = order_pages(content_titles, section_redirects)

    # Step 4: Fetch all content pages
    print(f"Fetching {len(content_titles)} content pages from {WIKI_BASE}")
    print()

    # Cache wikitext by title (needed for section extraction)
    wikitext_cache = {}
    pages_data = {}

    for title in content_titles:
        display_title, wikitext = fetch_page(title)
        wikitext_cache[title] = wikitext

        if not wikitext:
            print(f"  WARNING: Empty content for {title}")
            pages_data[title] = (display_title, [("(Page content unavailable)", LINE_NORMAL)])
        else:
            plain_lines = wiki_to_plain(wikitext)
            pages_data[title] = (display_title, plain_lines)
            print(f"    -> {len(plain_lines)} lines")

        time.sleep(0.5)

    # Step 5: Process section redirects
    if section_redirects:
        print()
        print("Extracting sections for redirect pages ...")
        for redir_title, (target, fragment) in section_redirects.items():
            target_wikitext = wikitext_cache.get(target, "")
            if not target_wikitext:
                print(f"  WARNING: No wikitext for target {target}, skipping {redir_title}")
                pages_data[redir_title] = (redir_title, [("(Section content unavailable)", LINE_NORMAL)])
                continue

            section_text = extract_section(target_wikitext, fragment)
            if not section_text:
                print(f"  WARNING: Section '{fragment}' not found in {target}")
                pages_data[redir_title] = (redir_title, [("(Section not found)", LINE_NORMAL)])
                continue

            plain_lines = wiki_to_plain(section_text)
            pages_data[redir_title] = (redir_title, plain_lines)
            print(f"  {redir_title} <- {target}#{fragment} -> {len(plain_lines)} lines")

    # Step 6: Build final ordered list with landing page
    content_page_count = len(content_titles) + len(section_redirects)
    landing = generate_landing_page(content_page_count, build_date)

    final_pages = [("The Emu Pages", landing)]
    for title in ordered_titles:
        if title in pages_data:
            final_pages.append(pages_data[title])

    # Step 7: Generate header
    print()
    print(f"Generating {OUTPUT_FILE} ...")

    header_content = generate_header(final_pages)
    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        f.write(header_content)

    total_lines = sum(len(lines) for _, lines in final_pages)
    print(f"Done! {len(final_pages)} pages (1 landing + {content_page_count} wiki), "
          f"{total_lines} total lines")
    print(f"Output: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
