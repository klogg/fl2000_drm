# SPDX-License-Identifier: GPL-2.0
"""Extract driver helpers for userspace tests and computation benchmarks."""

import re


def function(source, name):
    match = re.search(
        r"^[ \t]*(?:static\s+)?(?:inline\s+)?[\w \t*]+(?:\n[ \t]*)?\b"
        + name
        + r"\([^;]*?\)\s*\{",
        source,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"Function not found: {name}")
    start = match.start()
    pos = source.index("{", match.start())
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def structure(source, name):
    return re.search(r"struct " + name + r" \{.*?\n\};", source, re.DOTALL)[0]


def register_header(source):
    """Use real register definitions with the harness's types and bit helpers."""
    return re.sub(
        r"^#include <linux/(?:bits|types)\.h>\n", "", source, flags=re.MULTILINE
    )
