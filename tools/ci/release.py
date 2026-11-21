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

    tools/ci/release.py manifests --out DIR --version V [--check dist]
        Writes the ESP Web Tools manifests for the flasher page:
          DIR/manifests/<board>.json  one per board, pointing at
                                      ../firmware/ghostlight-node-<board>-V-full.bin
          DIR/boards.json             the board list the page shows
        The copies in docs/flasher/ are made with --version unreleased; the
        release workflow writes them again for the version it publishes.
        --check fails if a full image a manifest points at is not in that
        folder.

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


def cmd_manifests(args):
    os.makedirs(os.path.join(args.out, "manifests"), exist_ok=True)
    index = []
    for b in boards():
        full = file_name(b["board"], args.version, "full")
        if args.check and not os.path.isfile(os.path.join(args.check, full)):
            fail(f"{full} is missing from {args.check}")
        manifest = {
            "name": f"Ghostlight Node firmware ({b['board']})",
            "version": args.version,
            # A new install always erases the whole flash first (ESP Web
            # Tools' default): partition tables differ between boards and
            # releases, so old settings are not kept.
            "new_install_prompt_erase": False,
            "builds": [
                {
                    "chipFamily": b["chip_family"],
                    "parts": [
                        {
                            "path": "../firmware/" + full,
                            "offset": 0,
                        }
                    ],
                }
            ],
        }
        rel = f"manifests/{b['board']}.json"
        with open(os.path.join(args.out, rel), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        index.append({
            "board": b["board"],
            "chip_family": b["chip_family"],
            "description": b["description"],
            "manifest": rel,
        })
    with open(os.path.join(args.out, "boards.json"), "w", encoding="utf-8") as f:
        json.dump({"version": args.version, "boards": index}, f, indent=2)
        f.write("\n")
    print(f"wrote {len(index)} manifests to {args.out}")


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

    s = sub.add_parser("manifests")
    s.add_argument("--out", required=True)
    s.add_argument("--version", required=True)
    s.add_argument("--check", metavar="DIR", help="folder that must hold every full image")
    s.set_defaults(func=cmd_manifests)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
