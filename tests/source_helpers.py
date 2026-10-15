# SPDX-License-Identifier: GPL-2.0
"""Extract driver helpers for userspace tests and computation benchmarks."""

import re


def function(source, name):
    match = re.search(
        r"(?:static\s+)?(?:inline\s+)?[\w *]+\b" + name + r"\([^;]*?\)\s*\{", source
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
