#!/usr/bin/env python3
"""Embed per-core launcher metadata and art in an ESP application image."""

import argparse
import json
import re
import struct
import zlib
from pathlib import Path


MAGIC = b"RGCOREA1"
VERSION = 2
HEADER = struct.Struct("<8sHHIIHHI")
MANIFEST_V1 = struct.Struct("<16s64s64s64sB7x")
MANIFEST = struct.Struct("<16s64s64s64sB7x32s")
ASSET_RECORD = struct.Struct("<BI")
MAX_BUNDLE_SIZE = 256 * 1024
ART_FILES = ((1, "background.png"), (2, "logo.png"), (3, "banner.png"))


def write_text(path, content):
    with path.open("w", encoding="utf-8", newline="\n") as fp:
        fp.write(content)


def fixed_text(value, size, label):
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    encoded = value.encode("utf-8")
    if len(encoded) >= size:
        raise ValueError(f"{label} is longer than {size - 1} UTF-8 bytes")
    return encoded + b"\0" * (size - len(encoded))


def load_metadata(project):
    metadata_path = project / "core.json"
    if metadata_path.exists():
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if not isinstance(metadata, dict):
            raise ValueError("core.json must contain a JSON object")
    else:
        metadata = {}

    core_id = metadata.get("id", project.name.lower())
    if not isinstance(core_id, str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,15}", core_id):
        raise ValueError("core id must use 1-15 letters, digits, '_' or '-'")
    core_id = core_id.lower()

    title = metadata.get("title", project.name)
    if not isinstance(title, str) or not title.strip():
        raise ValueError("title must be a non-empty string")
    mode = metadata.get("launch_mode", "direct" if (project / "start_game").is_file() else "rom")
    if (project / "start_game").is_file():
        mode = "direct"
    if mode not in ("direct", "rom"):
        raise ValueError("launch_mode must be 'direct' or 'rom'")

    extensions = metadata.get("rom_extensions", [])
    if isinstance(extensions, str):
        extensions = extensions.split()
    if not isinstance(extensions, list) or any(not isinstance(ext, str) for ext in extensions):
        raise ValueError("rom_extensions must be a string or a list of strings")
    extensions = sorted({ext.lower().lstrip(".") for ext in extensions})
    if any(not re.fullmatch(r"[a-z0-9]{1,8}", ext) for ext in extensions):
        raise ValueError("ROM extensions must contain 1-8 letters or digits, without dots")
    if mode == "rom" and not extensions:
        raise ValueError("ROM-launching cores must declare rom_extensions in core.json")

    rom_folder = metadata.get("rom_folder", core_id)
    if not isinstance(rom_folder, str) or not rom_folder or rom_folder.startswith(("/", "\\")):
        raise ValueError("rom_folder must be a relative folder under /sd/roms")
    parts = rom_folder.replace("\\", "/").split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError("rom_folder cannot contain empty, '.' or '..' path segments")

    version = metadata.get("version", "")
    if not isinstance(version, str):
        raise ValueError("version must be a string")
    version = version.strip()

    return core_id, title, " ".join(extensions), "/".join(parts), mode, version


def make_bundle(project):
    core_id, title, extensions, rom_folder, mode, version = load_metadata(project)
    manifest = MANIFEST.pack(
        fixed_text(core_id, 16, "id"),
        fixed_text(title, 64, "title"),
        fixed_text(extensions, 64, "rom_extensions"),
        fixed_text(rom_folder, 64, "rom_folder"),
        1 if mode == "direct" else 0,
        fixed_text(version, 32, "version"),
    )

    payload = bytearray(manifest)
    asset_count = 0
    art_dir = project / "art"
    for kind, filename in ART_FILES:
        path = art_dir / filename
        if not path.exists():
            continue
        data = path.read_bytes()
        if len(data) < 16 or not data.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError(f"{path} is not a PNG image")
        if len(data) > 128 * 1024:
            raise ValueError(f"{path} is larger than 128 KB")
        payload += ASSET_RECORD.pack(kind, len(data))
        payload += data
        asset_count += 1

    total_size = HEADER.size + len(payload)
    if total_size > MAX_BUNDLE_SIZE:
        raise ValueError(f"generated core bundle exceeds {MAX_BUNDLE_SIZE // 1024} KB")

    header = HEADER.pack(
        MAGIC, VERSION, HEADER.size, total_size, len(manifest), asset_count, 0,
        zlib.crc32(payload) & 0xFFFFFFFF,
    )
    generated_manifest = {
        "schema": VERSION,
        "id": core_id,
        "title": title,
        "version": version,
        "launch_mode": mode,
        "rom_extensions": extensions.split() if extensions else [],
        "rom_folder": rom_folder,
        "art": [filename for _, filename in ART_FILES if (project / "art" / filename).is_file()],
    }
    return header + payload, generated_manifest, asset_count


def include_generated_source(cmake_path):
    if not cmake_path.exists():
        raise ValueError(f"missing component CMakeLists.txt: {cmake_path}")
    source = cmake_path.read_text(encoding="utf-8")
    marker = "rg_core_assets_generated.c"
    if marker in source:
        return

    if "COMPONENT_SRCDIRS" in source and re.search(r"COMPONENT_SRCDIRS[^\n]*[\"']?\.([\"']|\s|$)", source):
        # Legacy ESP-IDF components compile every source file in their source dirs.
        return

    register = re.search(r"idf_component_register\s*\(", source, re.IGNORECASE)
    if register:
        close = source.find(")", register.end())
        if close < 0:
            raise ValueError(f"could not find end of idf_component_register in {cmake_path}")
        body = source[register.end():close]
        sources_match = re.search(r"\bSRCS\b", body, re.IGNORECASE)
        if sources_match:
            insert_at = register.end() + sources_match.end()
            source = source[:insert_at] + f' "{marker}"' + source[insert_at:]
        else:
            source = source[:register.end()] + f' SRCS "{marker}"' + source[register.end():]
    elif "register_component()" in source:
        source = source.replace(
            "register_component()",
            f'set(COMPONENT_SRCS ${{COMPONENT_SRCS}} "{marker}")\nregister_component()',
            1,
        )
    else:
        raise ValueError(f"unsupported ESP-IDF component CMake format: {cmake_path}")

    write_text(cmake_path, source)


def emit_c_source(project, bundle):
    main_dir = project / "main"
    if not main_dir.is_dir():
        raise ValueError(f"core project has no main directory: {main_dir}")
    cmake_path = main_dir / "CMakeLists.txt"
    include_generated_source(cmake_path)

    lines = []
    for offset in range(0, len(bundle), 16):
        lines.append("    " + ", ".join(f"0x{byte:02X}" for byte in bundle[offset:offset + 16]) + ",")
    content = (
        "/* Generated by tools/gen_core_assets.py. Do not edit. */\n"
        "#include <stdint.h>\n\n"
        "__attribute__((used, section(\".rodata.rgcore\")))\n"
        "const uint8_t rg_core_assets_bundle[] = {\n"
        + "\n".join(lines)
        + "\n};\n\n"
        "__attribute__((constructor)) static void rg_core_assets_keep(void)\n"
        "{\n    volatile uint8_t marker = rg_core_assets_bundle[0];\n    (void)marker;\n}\n"
    )
    write_text(main_dir / "rg_core_assets_generated.c", content)


def emit_manifest(project, manifest):
    main_dir = project / "main"
    write_text(main_dir / "core_assets_manifest.generated.json", json.dumps(manifest, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path, help="Core project directory")
    args = parser.parse_args()
    project = args.project.resolve()
    try:
        bundle, manifest, asset_count = make_bundle(project)
        emit_c_source(project, bundle)
        emit_manifest(project, manifest)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    print(f"Embedded core bundle for {manifest['id']}: launch={manifest['launch_mode']}, artwork={asset_count}, {len(bundle)} bytes")


if __name__ == "__main__":
    main()
