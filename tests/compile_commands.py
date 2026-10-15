#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Merge backend builds, keeping the first configuration of each source file."""

import argparse
import json
from pathlib import Path


def merge_databases(paths):
    entries = {}
    for path in paths:
        database = json.loads(path.read_text())
        if not isinstance(database, list) or not database:
            raise ValueError(f"Compilation database is empty or invalid: {path}")
        for entry in database:
            if not ("command" in entry or "arguments" in entry):
                raise ValueError(f"Missing compiler command in {path}")
            directory = Path(entry["directory"])
            if not directory.is_absolute():
                directory = path.parent / directory
            directory = directory.resolve()
            source = (directory / entry["file"]).resolve()
            entries.setdefault(
                str(source), {**entry, "directory": str(directory), "file": str(source)}
            )
    return list(entries.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("databases", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    entries = merge_databases(args.databases)
    args.output.write_text(json.dumps(entries, indent=2) + "\n")
    print(f"Merged {len(entries)} compilation units into {args.output}")


if __name__ == "__main__":
    main()
