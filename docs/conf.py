"""Sphinx configuration for the Pico-as-ROM documentation website.

Combines:
  * Doxygen (C firmware) via Breathe
  * Python host-tool API via autodoc
  * Prose written in MyST Markdown
"""

import os
import sys
from pathlib import Path

# Make the Python host tools importable for autodoc.
DOCS_DIR = Path(__file__).resolve().parent
REPO_ROOT = DOCS_DIR.parent
sys.path.insert(0, str(REPO_ROOT / "rom-builder"))

# -- Project information -----------------------------------------------------

project = "Pico-as-ROM"
author = "big-iron-cde"
copyright = "2026, big-iron-cde"
release = "0.1"

# -- General configuration ---------------------------------------------------

extensions = [
    "breathe",
    "myst_parser",
    "sphinx.ext.autodoc",
    "sphinx.ext.napoleon",
    "sphinx.ext.viewcode",
    "sphinxcontrib.mermaid",
]

myst_enable_extensions = [
    "colon_fence",
    "deflist",
    "fieldlist",
]
myst_heading_anchors = 3

templates_path = ["_templates"]
exclude_patterns = ["_build", "_doxygen", "Thumbs.db", ".DS_Store"]

# -- Breathe (Doxygen bridge) ------------------------------------------------

breathe_projects = {"pico-rom": str(DOCS_DIR / "_doxygen" / "xml")}
breathe_default_project = "pico-rom"
breathe_domain_by_extension = {"h": "c", "c": "c"}

# -- autodoc -----------------------------------------------------------------

# pyserial is an optional runtime import in hardware_api; mock it so docs build
# without the dependency present on the build machine.
autodoc_mock_imports = ["serial"]
autodoc_member_order = "bysource"

# -- HTML output -------------------------------------------------------------

html_theme = "furo"
html_title = "Pico-as-ROM"
html_static_path = ["_static"]
