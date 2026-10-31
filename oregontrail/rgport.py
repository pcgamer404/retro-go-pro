#!/usr/bin/env python3
"""rgport.py - build a Retro-Go port and pack it into a .rgport package.

Run it from INSIDE a port folder (the folder that contains rgport.json):

    cd D:\\Games\\E\\retro-go-pro\\oregontrail
    python ..\\rgport.py                  build app.bin + pack
    python ..\\rgport.py --no-build       pack the existing build\\<app>.bin
    python ..\\rgport.py verify dist\\OregonTrail.rgport

A .rgport is a plain ZIP:
    manifest.json      generated (id, name, launch file, bin info, sizes)
    app.bin            the ESP-IDF app image, flashed into the "ports" slot
    files/...          assets, extracted to SD:/roms/ports/<id>/

Needs: ESP-IDF environment active (idf.py on PATH or IDF_PATH set) for the
build step. Packing/verify need only Python 3.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

DEFAULT_SLOT = 0x400000          # 4 MB "ports" slot
FIXED_TIME = (2020, 1, 1, 0, 0, 0)   # deterministic zip: same content -> same CRC
ESP_IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 0x0009
APP_DESC_MAGIC = 0xABCD5432
SKIP_NAMES = {".git", "__pycache__", "Thumbs.db", ".DS_Store", "desktop.ini"}


def die(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def parse_int(s):
    return int(str(s), 0)


# --------------------------------------------------------------------------
# App image inspection
# --------------------------------------------------------------------------
def inspect_image(data, slot_size):
    """Validate an ESP32-S3 app image. Returns info dict or raises ValueError."""
    if len(data) < 32 + 256:
        raise ValueError("file is too small to be an app image")
    if data[0] != ESP_IMAGE_MAGIC:
        raise ValueError("bad image magic - not an ESP app image "
                         "(did you pick bootloader.bin or a .img?)")
    chip_id = struct.unpack_from("<H", data, 12)[0]
    if chip_id != ESP32S3_CHIP_ID:
        raise ValueError("image is for chip id 0x%04x, expected ESP32-S3" % chip_id)
    if struct.unpack_from("<I", data, 32)[0] != APP_DESC_MAGIC:
        raise ValueError("app descriptor not found (not an application image)")
    if data[23]:  # hash_appended flag -> last 32 bytes are SHA-256 of the rest
        if hashlib.sha256(data[:-32]).digest() != data[-32:]:
            raise ValueError("image SHA-256 mismatch - file is corrupt")
    if len(data) > slot_size:
        raise ValueError("app.bin is %d bytes, slot is only %d bytes"
                         % (len(data), slot_size))
    return {
        "project": cstr(data[32 + 48:32 + 80]),
        "app_version": cstr(data[32 + 16:32 + 48]),
        "idf": cstr(data[32 + 112:32 + 144]),
        "target": "esp32s3",
    }


# --------------------------------------------------------------------------
# Manifest + paths
# --------------------------------------------------------------------------
def load_port_manifest(port_dir):
    path = os.path.join(port_dir, "rgport.json")
    if not os.path.isfile(path):
        die("rgport.json not found in %s\n       run this script from inside the "
            "port folder." % port_dir)
    with open(path, "r", encoding="utf-8") as f:
        m = json.load(f)
    for key in ("id", "name", "app", "launch_file", "assets"):
        if key not in m:
            die("rgport.json is missing required key '%s'" % key)
    return m


def find_root(start):
    d = os.path.abspath(start)
    while True:
        if os.path.isfile(os.path.join(d, "rg_tool.py")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def git_version(root):
    try:
        out = subprocess.run(["git", "describe", "--always", "--dirty"],
                             cwd=root, capture_output=True, text=True, timeout=10)
        v = out.stdout.strip()
        return v or "dev"
    except Exception:
        return "dev"


def collect_assets(port_dir, entries):
    """Return sorted list of (abs_path, relative_posix_path)."""
    found = {}
    for entry in entries:
        p = os.path.join(port_dir, entry)
        if os.path.isfile(p):
            found[entry.replace("\\", "/")] = p
        elif os.path.isdir(p):
            for base, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if d not in SKIP_NAMES]
                for fn in files:
                    if fn in SKIP_NAMES:
                        continue
                    ap = os.path.join(base, fn)
                    rel = os.path.relpath(ap, port_dir).replace("\\", "/")
                    found[rel] = ap
        else:
            die("asset '%s' listed in rgport.json does not exist" % entry)
    return sorted((p, rel) for rel, p in found.items())


# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------
def find_idf_py():
    p = shutil.which("idf.py")
    if p:
        return p
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        cand = os.path.join(idf_path, "tools", "idf.py")
        if os.path.isfile(cand):
            return cand
    return None


def build_app(port_dir, root, m, extra_defs):
    idf = find_idf_py()
    if not idf:
        die("idf.py not found. Open the ESP-IDF 5.3 PowerShell (or run export.ps1) "
            "first, or use --no-build.")
    defs = {
        "RG_PROJECT_APP": m["app"],
        "RG_PROJECT_VER": git_version(root or port_dir),
        "RG_BUILD_TARGET": "RG_TARGET_MY_HANDHELD",
        "RG_BUILD_RELEASE": "0",
        "RG_ENABLE_PROFILING": "0",
        "RG_ENABLE_NETWORKING": "1",
    }
    defs.update(m.get("build_defs", {}))
    defs.update(extra_defs)
    cmd = [sys.executable, idf, "app"] + ["-D%s=%s" % kv for kv in defs.items()]
    print("> " + " ".join(cmd))
    rc = subprocess.run(cmd, cwd=port_dir).returncode
    if rc != 0:
        die("build failed (exit code %d)" % rc)


def find_bin(port_dir, m, explicit=None):
    if explicit:
        if not os.path.isfile(explicit):
            die("bin not found: %s" % explicit)
        return explicit
    build = os.path.join(port_dir, "build")
    exact = os.path.join(build, m["app"] + ".bin")
    if os.path.isfile(exact):
        return exact
    cands = []
    if os.path.isdir(build):
        for fn in os.listdir(build):
            if fn.endswith(".bin") and "bootloader" not in fn and "partition" not in fn:
                cands.append(os.path.join(build, fn))
    if not cands:
        die("no app .bin found in %s - build first (omit --no-build)" % build)
    return max(cands, key=os.path.getmtime)


# --------------------------------------------------------------------------
# Pack / verify
# --------------------------------------------------------------------------
def zadd(zf, arcname, data):
    zi = zipfile.ZipInfo(arcname, FIXED_TIME)
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = 0o644 << 16
    zf.writestr(zi, data, compresslevel=9)


def pack(port_dir, m, bin_path, out_path, slot_size):
    with open(bin_path, "rb") as f:
        bin_data = f.read()
    try:
        info = inspect_image(bin_data, slot_size)
    except ValueError as e:
        die(str(e))

    assets = collect_assets(port_dir, m["assets"])
    rels = [rel for _, rel in assets]
    if m["launch_file"] not in rels:
        die("launch_file '%s' is not among the packed assets" % m["launch_file"])

    total = sum(os.path.getsize(p) for p, _ in assets) + len(bin_data)
    manifest = {
        "format": 1,
        "id": m["id"],
        "name": m["name"],
        "runtime": m.get("runtime", "native-slot"),
        "slot": m.get("slot", "ports"),
        "entry": "app.bin",
        "launch_file": m["launch_file"],
        "install_dir": m.get("install_dir", m["id"]),
        "save_dir": m.get("save_dir", m["id"]),
        "bin": {
            "file": "app.bin",
            "size": len(bin_data),
            "sha256": hashlib.sha256(bin_data).hexdigest(),
            "crc32": "%08x" % (zlib.crc32(bin_data) & 0xFFFFFFFF),
            **info,
        },
        "files": len(assets),
        "installed_size": total,
    }
    for key in ("description", "author", "config"):
        if key in m:
            manifest[key] = m[key]

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with zipfile.ZipFile(out_path, "w") as zf:
        zadd(zf, "manifest.json", json.dumps(manifest, indent=2).encode("utf-8"))
        zadd(zf, "app.bin", bin_data)
        for ap, rel in assets:
            with open(ap, "rb") as f:
                zadd(zf, "files/" + rel, f.read())

    pkg = os.path.getsize(out_path)
    print("\nPacked %s" % out_path)
    print("  app.bin   %8.1f KB  (%.0f%% of %d KB slot)  %s %s"
          % (len(bin_data) / 1024, 100.0 * len(bin_data) / slot_size,
             slot_size // 1024, info["project"], info["app_version"]))
    print("  assets    %d files" % len(assets))
    print("  package   %8.1f KB   installed ~%.1f KB" % (pkg / 1024, total / 1024))
    print("  copy to   SD:/roms/ports/%s" % os.path.basename(out_path))
    return manifest


def verify(path, slot_size):
    if not zipfile.is_zipfile(path):
        die("%s is not a zip/.rgport file" % path)
    with zipfile.ZipFile(path) as zf:
        bad = zf.testzip()
        if bad:
            die("zip entry corrupt: %s" % bad)
        names = set(zf.namelist())
        for need in ("manifest.json", "app.bin"):
            if need not in names:
                die("package is missing %s" % need)
        m = json.loads(zf.read("manifest.json"))
        data = zf.read("app.bin")
        b = m["bin"]
        if len(data) != b["size"]:
            die("app.bin size does not match manifest")
        if hashlib.sha256(data).hexdigest() != b["sha256"]:
            die("app.bin SHA-256 does not match manifest")
        try:
            inspect_image(data, slot_size)
        except ValueError as e:
            die(str(e))
        launch = "files/" + m["launch_file"]
        if launch not in names:
            die("launch file %s missing from package" % m["launch_file"])
        nfiles = sum(1 for n in names if n.startswith("files/") and not n.endswith("/"))
        if nfiles != m["files"]:
            die("manifest says %d files, package has %d" % (m["files"], nfiles))
    print("OK  %s  (%s, %d files, app.bin %d KB)"
          % (os.path.basename(path), m["name"], m["files"], b["size"] // 1024))


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Build and pack a Retro-Go port (.rgport)")
    ap.add_argument("command", nargs="?", default="pack", choices=["pack", "verify"])
    ap.add_argument("target", nargs="?", help="verify: path to a .rgport")
    ap.add_argument("--no-build", action="store_true", help="skip idf.py, pack existing bin")
    ap.add_argument("--bin", help="use this app .bin instead of build\\<app>.bin")
    ap.add_argument("--out", help="output .rgport path (default dist\\<file>.rgport)")
    ap.add_argument("--slot-size", default=hex(DEFAULT_SLOT),
                    help="ports slot size in bytes (default 0x400000)")
    ap.add_argument("-D", dest="defs", action="append", default=[],
                    metavar="KEY=VALUE", help="extra -D define for the build")
    args = ap.parse_args()
    slot = parse_int(args.slot_size)

    if args.command == "verify":
        if not args.target:
            die("usage: rgport.py verify <file.rgport>")
        verify(args.target, slot)
        return

    port_dir = os.getcwd()
    m = load_port_manifest(port_dir)
    root = find_root(port_dir)
    extra = dict(d.split("=", 1) for d in args.defs if "=" in d)

    if not args.no_build and not args.bin:
        build_app(port_dir, root, m, extra)
    bin_path = find_bin(port_dir, m, args.bin)

    out = args.out or os.path.join(port_dir, "dist", m.get("file", m["id"] + ".rgport"))
    pack(port_dir, m, bin_path, out, slot)
    verify(out, slot)


if __name__ == "__main__":
    main()
