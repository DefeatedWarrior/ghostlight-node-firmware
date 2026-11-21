#!/usr/bin/env python3
"""Helpers for .github/workflows/release.yml (and for trying it locally).

    tools/ci/release.py matrix
        Prints the boards under boards/ as a JSON list for a GitHub Actions
        matrix: [{"board": ..., "target": ..., "chip_family": ...}, ...].

    tools/ci/release.py images --board B --build-dir build-B --out dist --version V
        Makes the two release files for one built board:
          dist/ghostlight-node-B-V-full.bin  everything merged, flash at 0x0
          dist/ghostlight-node-B-V-ota.bin   the app alone, for the web UI's
                                             firmware upload
        Uses build-B/flasher_args.json and esptool's merge_bin.

    tools/ci/release.py flasher --dist dist --version V
        Writes dist/flasher.json, the index the browser flasher on
        https://defeatedwarrior.github.io/flasher.html reads to put this
        release on the page:
          {"version": V, "boards": [{"board", "chip_family", "description",
                                     "full", "ota"}, ...]}
        Fails if a file it lists is not in dist/.

The board list always comes from boards/*/sdkconfig.defaults, so a new board
folder is picked up without editing anything here.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PREFIX = "ghostlight-node"

# CONFIG_IDF_TARGET -> ESP Web Tools chipFamily
CHIP_FAMILIES = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c2": "ESP32-C2",
    "esp32c3": "ESP32-C3",
    "esp32c6": "ESP32-C6",
    "esp32h2": "ESP32-H2",
}


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def read_board(board):
    """Target, chip family and description of boards/<board>/sdkconfig.defaults.

    The description is the first comment line of that file, for example
    "Ghostlight Node rev1: ESP32-S3-WROOM-1-N8 (8 MB quad flash, no PSRAM)".
    """
    path = os.path.join(ROOT, "boards", board, "sdkconfig.defaults")
    target = None
    description = ""
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not description and line.startswith("#"):
                description = line.lstrip("#").strip()
            m = re.match(r'^CONFIG_IDF_TARGET="([^"]+)"', line)
            if m:
                target = m.group(1)
    if not target:
        fail(f"{path} does not set CONFIG_IDF_TARGET")
    if target not in CHIP_FAMILIES:
        fail(f"{path}: no ESP Web Tools chip family known for '{target}'")
    return {
        "board": board,
        "target": target,
        "chip_family": CHIP_FAMILIES[target],
        "description": description,
    }


def boards():
    base = os.path.join(ROOT, "boards")
    names = sorted(
        d for d in os.listdir(base)
        if os.path.isfile(os.path.join(base, d, "sdkconfig.defaults"))
    )
    if not names:
        fail("no boards found under boards/")
    return [read_board(b) for b in names]


def file_name(board, version, kind):
    return f"{PREFIX}-{board}-{version}-{kind}.bin"


def cmd_matrix(_args):
    rows = [{k: b[k] for k in ("board", "target", "chip_family")} for b in boards()]
    print(json.dumps(rows))


def esptool():
    for name in ("esptool.py", "esptool"):
        exe = shutil.which(name)
        if exe:
            return [exe]
    return [sys.executable, "-m", "esptool"]


def cmd_images(args):
    build = os.path.abspath(args.build_dir)
    with open(os.path.join(build, "flasher_args.json"), encoding="utf-8") as f:
        fa = json.load(f)

    chip = fa.get("extra_esptool_args", {}).get("chip") or read_board(args.board)["target"]
    files = sorted(fa["flash_files"].items(), key=lambda kv: int(kv[0], 16))
    os.makedirs(args.out, exist_ok=True)

    full = os.path.join(args.out, file_name(args.board, args.version, "full"))
    cmd = esptool() + ["--chip", chip, "merge_bin", "-o", os.path.abspath(full)]
    cmd += fa.get("write_flash_args", [])
    for offset, path in files:
        cmd += [offset, path]
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=build, check=True)

    ota = os.path.join(args.out, file_name(args.board, args.version, "ota"))
    shutil.copyfile(os.path.join(build, fa["app"]["file"]), ota)

    for p in (full, ota):
        print(f"{p}: {os.path.getsize(p)} bytes")


def cmd_flasher(args):
    index = []
    for b in boards():
        row = {
            "board": b["board"],
            "chip_family": b["chip_family"],
            "description": b["description"],
            "full": file_name(b["board"], args.version, "full"),
            "ota": file_name(b["board"], args.version, "ota"),
        }
        for kind in ("full", "ota"):
            if not os.path.isfile(os.path.join(args.dist, row[kind])):
                fail(f"{row[kind]} is missing from {args.dist}")
        index.append(row)
    out = os.path.join(args.dist, "flasher.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"version": args.version, "boards": index}, f, indent=2)
        f.write("\n")
    print(f"wrote {out} ({len(index)} boards)")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("matrix").set_defaults(func=cmd_matrix)

    s = sub.add_parser("images")
    s.add_argument("--board", required=True)
    s.add_argument("--build-dir", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--version", required=True)
    s.set_defaults(func=cmd_images)

    s = sub.add_parser("flasher")
    s.add_argument("--dist", required=True, help="folder with the release files")
    s.add_argument("--version", required=True)
    s.set_defaults(func=cmd_flasher)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
