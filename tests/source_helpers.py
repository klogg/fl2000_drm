# SPDX-License-Identifier: GPL-2.0
"""Extract driver helpers for userspace tests and computation benchmarks."""

import re


def _code_mask(source):
    """Hide comments and literals while preserving source offsets and lines."""
    return re.sub(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: re.sub(r"[^\n]", " ", match.group()),
        source,
        flags=re.DOTALL,
    )


def function(source, name):
    code = _code_mask(source)
    match = re.search(
        r"^[ \t]*(?:static\s+)?(?:inline\s+)?[\w \t*]+(?:\n[ \t]*)?\b"
        + name
        + r"\([^;]*?\)\s*(?:/\*[^\n]*\*/\s*)?\{",
        code,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"Function not found: {name}")
    start = match.start()
    pos = code.index("{", match.start())
    depth = 1
    end = pos + 1
    while depth:
        depth += (code[end] == "{") - (code[end] == "}")
        end += 1
    return source[start:end]


def structure(source, name):
    match = re.search(
        r"struct " + re.escape(name) + r" \{.*?\n\};",
        _code_mask(source),
        re.DOTALL,
    )
    if not match:
        raise AssertionError(f"Structure not found: {name}")
    return source[match.start() : match.end()]


def register_header(source):
    """Use real register definitions with the harness's types and bit helpers."""
    return re.sub(
        r"^#include <linux/(?:bits|types)\.h>\n", "", source, flags=re.MULTILINE
    )
