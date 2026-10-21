#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Builds the documentation site with MkDocs.

The repository keeps its top-level documents (README, ARCHITECTURE, ROADMAP, …) next to the code,
where GitHub shows them, and the rest under docs/. MkDocs needs one source folder, so this script
stages both into build/docs-site/src — top-level documents at the site root with their "docs/"
links rewritten — and runs MkDocs with mkdocs.yml.

    python3 docs/tools/build_site.py            # build into build/docs-site/html (strict)
    python3 docs/tools/build_site.py --serve    # live preview
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STAGE = ROOT / "build" / "docs-site" / "src"
TOP_LEVEL = ["README.md", "ARCHITECTURE.md", "IMPLEMENTATION_STATUS.md", "ROADMAP.md", "DEVELOPMENT.md",
             "CONTRIBUTING.md", "SECURITY.md", "CODE_OF_CONDUCT.md", "THIRD_PARTY_NOTICES.md"]
STATIC = ["LICENSE", "NOTICE"]
LINK = re.compile(r"(\]\()([^)#\s]+)")


def rewrite_top_level(text: str) -> str:
    """Links of a top-level document, as seen from the site root."""
    def fix(m):
        target = m.group(2)
        if "://" in target or target.startswith("mailto:"):
            return m.group(0)
        if target.startswith("docs/"):
            target = target[len("docs/"):]
        if target.endswith("/"):           # a folder: link its index page if it has one
            target += "index.md"
        return m.group(1) + target
    return LINK.sub(fix, text)


def stage() -> None:
    if STAGE.exists():
        shutil.rmtree(STAGE)
    shutil.copytree(ROOT / "docs", STAGE, ignore=shutil.ignore_patterns("tools", "__pycache__", "*.log"))
    for name in TOP_LEVEL:
        text = (ROOT / name).read_text(encoding="utf-8")
        (STAGE / ("index.md" if name == "README.md" else name)).write_text(rewrite_top_level(text), encoding="utf-8")
    for name in STATIC:
        shutil.copy(ROOT / name, STAGE / name)
    # Pages under docs/ reach a top-level document one level up (../ROADMAP.md from docs/x.md);
    # on the site those documents sit beside docs' own root, one level closer. The README is the
    # site's index.
    names = "|".join(re.escape(n) for n in TOP_LEVEL + STATIC)
    for page in STAGE.rglob("*.md"):
        rel = page.relative_to(STAGE)
        if rel.parts[0] in TOP_LEVEL or str(rel) == "index.md":
            continue
        depth = len(rel.parts) - 1
        text = page.read_text(encoding="utf-8")
        fixed = re.sub(r"\]\(" + re.escape("../" * (depth + 1)) + r"(" + names + r")", lambda m: "](" + "../" * depth + m.group(1), text)
        fixed = re.sub(r"\]\(((?:\.\./)*)README\.md", r"](\1index.md", fixed)
        if fixed != text:
            page.write_text(fixed, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serve", action="store_true", help="serve with live reload instead of building")
    args = ap.parse_args()
    stage()
    cmd = [sys.executable, "-m", "mkdocs", "serve" if args.serve else "build", "-f", str(ROOT / "mkdocs.yml")]
    if not args.serve:
        cmd.append("--strict")
    return subprocess.call(cmd, cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
