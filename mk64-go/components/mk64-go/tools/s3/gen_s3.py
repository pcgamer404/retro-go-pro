#!/usr/bin/env python3
"""Generate the ESP32-S3 flash-asset layout for mk64-go.

Layout strategy
---------------
* Keep every ROM recipe except the audio block [AUDIO_START, AUDIO_END).
* All recipes before AUDIO_START keep their original offset.  Course segment
  tables, course relocation destinations and any code that does
  ``symbol + constant`` therefore remain valid without regeneration.
* The small tail after the audio block is moved down to AUDIO_START.  Only
  aliases and recipe destinations for that tail change.
* Relocation tables (RELOCS recipes) and fixups are pre-applied by the
  first-run extractor, so the flash copy is complete before it is mapped.

The generated files are:
  tools/s3/recipes_s3.json   compact recipe manifest
  recipe_index.c             regenerated recipe table (audio removed)
  asset_aliases.S            absolute flash aliases
  course_relocs.c            augmented with s3_reloc_tables/fixups
  s3_layout.h                compile-time layout constants
  course_layout.h            region size patched
"""

import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

from gen_recipe_index import correct_structured_recipes, KIND  # noqa: E402

BASE = 0x3D000000
HEADER_OFFSET = 0x1DF000
DATA_OFFSET = 0x1E0000
MMU_PAGE = 0x10000

AUDIO_START = 10_591_216
AUDIO_END = 13_224_672
OLD_REGION = 13_231_904
TAIL_SHIFT = AUDIO_END - AUDIO_START

SRC_JSON = TOOLS / "psp" / "recipes.json"
S3_DIR = ROOT / "tools" / "s3"
S3_JSON = S3_DIR / "recipes_s3.json"


def fail(msg):
    raise SystemExit("gen_s3: " + msg)


def normalize(path):
    data = path.read_bytes().replace(b"\r\n", b"\n")
    path.write_bytes(data)


def recipe_kind_containers(recipes, fixup_offsets):
    """Attribute every fixup to the preceding recipe, including its gap.

    Many menu textures are 5..157 bytes of ROM data followed by alignment
    padding; the reloc fixups intentionally build a larger structure in that
    padding.  The extractor therefore writes ``recipe data + gap`` as one
    extended buffer and patches fixups anywhere in it.
    """
    import bisect
    ordered = sorted(recipes, key=lambda r: r["off"])
    starts = [r["off"] for r in ordered]
    seen = {}
    for off in fixup_offsets:
        i = bisect.bisect_right(starts, off) - 1
        if i < 0:
            fail(f"fixup offset {off} has no preceding recipe")
        r = ordered[i]
        if r["off"] < AUDIO_START <= r["off"] + r["size"]:
            fail(f"fixup container {r['name']} straddles audio start")
        if AUDIO_START <= r["off"] < AUDIO_END:
            fail(f"fixup container {r['name']} is in the audio block")
        if r["off"] >= AUDIO_END:
            fail(f"fixup container {r['name']} is in the tail")
        seen.setdefault(r["name"], (r, []))[1].append(off)
    return seen


def parse_relocs(text):
    tables = re.findall(
        r"memcpy\(region \+ (\d+)u, (table_\d+), (\d+)u\);", text)
    m = re.search(
        r"static const struct \{ uint32_t offset; uintptr_t value; \} "
        r"fixups\[\] = \{(.*?)\n\};", text, re.S)
    if not m:
        fail("cannot find fixups table in course_relocs.c")
    body = m.group(1)
    offsets = [int(x) for x in re.findall(r"\{(\d+)u,", body)]
    return tables, body, offsets


def main():
    data = json.loads(SRC_JSON.read_text(encoding="utf-8"))
    if data["region_size"] != OLD_REGION:
        fail(f"unexpected original region size {data['region_size']}")

    correct_structured_recipes(data, ROOT)
    recipes = sorted(data["recipes"], key=lambda r: (r["off"], r["name"]))

    # Boundary validation: the audio block must start and end on recipe edges.
    straddle = [r for r in recipes
                if r["off"] < AUDIO_START < r["off"] + r["size"] or
                r["off"] < AUDIO_END < r["off"] + r["size"]]
    if straddle:
        fail("recipe straddles an audio boundary: " +
             ", ".join(r["name"] for r in straddle[:4]))
    before = [r for r in recipes if r["off"] + r["size"] <= AUDIO_START]
    after = [r for r in recipes if r["off"] >= AUDIO_END]
    if max(r["off"] + r["size"] for r in before) != AUDIO_START:
        fail("main region does not end exactly at the audio start")
    if min(r["off"] for r in after) != AUDIO_END:
        fail("tail does not start exactly at the audio end")

    keep = [r for r in recipes
            if not (AUDIO_START <= r["off"] < AUDIO_END)]
    new_region = OLD_REGION - TAIL_SHIFT

    def new_off(r):
        return r["off"] - TAIL_SHIFT if r["off"] >= AUDIO_END else r["off"]

    packed = []
    for r in keep:
        r2 = dict(r)
        r2["off"] = new_off(r)
        if r2["off"] < 0 or r2["off"] + r2["size"] > new_region:
            fail(f"recipe {r['name']} exceeds new region")
        packed.append(r2)
    offsets = [r["off"] for r in packed]
    if len(set(offsets)) != len(offsets):
        fail("duplicate new recipe offsets")

    out = dict(data)
    out["region_size"] = new_region
    out["recipes"] = packed
    S3_DIR.mkdir(parents=True, exist_ok=True)
    S3_JSON.write_text(json.dumps(out, separators=(",", ":")),
                       encoding="utf-8")

    # --- recipe index -----------------------------------------------------
    subprocess.run([sys.executable, str(TOOLS / "gen_recipe_index.py"),
                    str(S3_JSON), str(ROOT / "recipe_index.c")],
                   cwd=str(ROOT), check=True)
    normalize(ROOT / "recipe_index.c")

    # --- absolute aliases -------------------------------------------------
    alias_lines = [
        "/* Generated by tools/s3/gen_s3.py; absolute flash aliases. */",
        ".text",
    ]
    seen = set()
    dummy = 0
    for r in recipes:
        name = r["name"]
        if name in seen:
            fail(f"duplicate symbol {name}")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
            fail(f"invalid symbol {name}")
        seen.add(name)
        if AUDIO_START <= r["off"] < AUDIO_END:
            off = 0
            dummy += 1
        else:
            off = new_off(r)
        alias_lines.append(f".globl {name}")
        alias_lines.append(f".set {name}, 0x{BASE:08X} + {off}")
    (ROOT / "asset_aliases.S").write_text("\n".join(alias_lines) + "\n",
                                          encoding="utf-8")

    # --- relocation data --------------------------------------------------
    relocs_path = ROOT / "course_relocs.c"
    relocs_text = relocs_path.read_text(encoding="utf-8")
    marker = "/* ---- S3 flash pre-apply data"
    idx = relocs_text.find(marker)
    if idx >= 0:
        relocs_text = relocs_text[:idx].rstrip() + "\n"
    tables, fix_body, fix_offsets = parse_relocs(relocs_text)

    containers = recipe_kind_containers(recipes, fix_offsets)
    packed_sorted = sorted(packed, key=lambda r: r["off"])
    next_by_name = {}
    for i, r in enumerate(packed_sorted):
        next_by_name[r["name"]] = (
            packed_sorted[i + 1]["off"] if i + 1 < len(packed_sorted)
            else new_region)
    max_patched = 0
    for name, (r, offs) in containers.items():
        if KIND[r["kind"]] not in (1, 2):
            fail(f"fixup container {name} has kind {r['kind']}, expected RAW/MIO0")
        extended = next_by_name.get(name, new_region) - new_off(r)
        if extended <= 0:
            fail(f"fixup container {name} has an empty extended range")
        max_patched = max(max_patched, extended)
    if max_patched > 4096:
        fail(f"extended fixup container is too large: {max_patched} bytes")
    if not tables:
        fail("no relocation tables found")
    max_table = 0
    for off, table, length in tables:
        off, length = int(off), int(length)
        max_table = max(max_table, length)
        # A table must start inside a RELOCS recipe.
        i = next((r for r in recipes if r["off"] <= off < r["off"] + r["size"]), None)
        if i is None or KIND[i["kind"]] != 4:
            fail(f"relocation table at {off} is not in a RELOCS recipe")
        if length > i["size"] + 1:
            fail(f"relocation table at {off} is longer than its recipe")

    aug = [
        "",
        "/* ---- S3 flash pre-apply data (generated by tools/s3/gen_s3.py) ---- */",
        "#include \"s3_reloc_data.h\"",
        "",
        "const s3_reloc_table_t s3_reloc_tables[] = {",
    ]
    for off, table, length in tables:
        aug.append(f"    {{ {off}u, {table}, {length}u }},")
    aug += [
        "};",
        "const size_t s3_reloc_table_count = "
        "sizeof(s3_reloc_tables) / sizeof(s3_reloc_tables[0]);",
        "",
        "const s3_reloc_fixup_t s3_reloc_fixups[] = {",
    ]
    aug.append(fix_body.rstrip())
    aug += [
        "};",
        "const size_t s3_reloc_fixup_count = "
        "sizeof(s3_reloc_fixups) / sizeof(s3_reloc_fixups[0]);",
        "",
    ]
    relocs_path.write_text(relocs_text.rstrip() + "\n" + "\n".join(aug),
                           encoding="utf-8")

    (ROOT / "s3_reloc_data.h").write_text(
        "/* Generated by tools/s3/gen_s3.py. */\n"
        "#pragma once\n"
        "#include <stdint.h>\n"
        "#include <stddef.h>\n"
        "\n"
        "typedef struct {\n"
        "    uint32_t offset;\n"
        "    const void *data;\n"
        "    uint32_t length;\n"
        "} s3_reloc_table_t;\n"
        "\n"
        "typedef struct {\n"
        "    uint32_t offset;\n"
        "    uintptr_t value;\n"
        "} s3_reloc_fixup_t;\n"
        "\n"
        "extern const s3_reloc_table_t s3_reloc_tables[];\n"
        "extern const size_t s3_reloc_table_count;\n"
        "extern const s3_reloc_fixup_t s3_reloc_fixups[];\n"
        "extern const size_t s3_reloc_fixup_count;\n",
        encoding="utf-8")

    # --- layout tag -------------------------------------------------------
    h = 2166136261

    def fnv(text):
        nonlocal h
        for b in text.encode("utf-8"):
            h = ((h ^ b) * 16777619) & 0xFFFFFFFF

    fnv(f"region={new_region};")
    for r in packed:
        fnv(f"{r['name']}|{r['off']}|{r['size']}|{r['kind']}|"
            f"{r['src']}|{r['extra']}|{r['xform']}\n")
    for c in data["courses"]:
        fnv(f"c{c['idx']}|{c['rom_off']}|{c['rom_len']}|{c['unpacked_len']}\n")
    for b in data["blocks"]:
        fnv(f"b{b['rom_off']}|{b['rom_len']}|{b['decomp_len']}\n")
    tag = h

    map_size = (new_region + MMU_PAGE - 1) & ~(MMU_PAGE - 1)
    required = DATA_OFFSET + map_size
    app_limit = HEADER_OFFSET

    (ROOT / "s3_layout.h").write_text(
        "/* Generated by tools/s3/gen_s3.py. */\n"
        "#pragma once\n"
        "#include <stdint.h>\n"
        "\n"
        f"#define MK64_S3_FLASH_BASE 0x{BASE:08X}u\n"
        f"#define MK64_S3_REGION_SIZE {new_region}u\n"
        f"#define MK64_S3_MAP_SIZE {map_size}u\n"
        f"#define MK64_S3_HEADER_OFFSET 0x{HEADER_OFFSET:06X}u\n"
        f"#define MK64_S3_DATA_OFFSET 0x{DATA_OFFSET:06X}u\n"
        f"#define MK64_S3_REQUIRED_PARTITION {required}u\n"
        f"#define MK64_S3_APP_IMAGE_LIMIT 0x{app_limit:06X}u\n"
        f"#define MK64_S3_LAYOUT_TAG 0x{tag:08X}u\n"
        f"#define MK64_S3_MAX_EXTENDED_BYTES {max_patched}u\n"
        f"#define MK64_S3_RELOC_TABLE_COUNT {len(tables)}u\n"
        f"#define MK64_S3_RELOC_FIXUP_COUNT {len(fix_offsets)}u\n"
        f"#define MK64_S3_MAX_TABLE_BYTES {max_table}u\n",
        encoding="utf-8")

    # --- course_layout.h --------------------------------------------------
    cl = ROOT / "course_layout.h"
    text = cl.read_text(encoding="utf-8")
    text = re.sub(r"#define MK64_ASSET_REGION_SIZE \d+u",
                  f"#define MK64_ASSET_REGION_SIZE {new_region}u", text, count=1)
    if "PSP-era course ranges" not in text:
        text = text.replace(
            "/* Generated from tools/psp/recipes.json; contains no game data. */",
            "/* Generated from tools/psp/recipes.json; contains no game data.\n"
            " * S3: the PSP-era course ranges below are informational and unused;\n"
            " * only MK64_ASSET_REGION_SIZE is consumed at runtime. */", 1)
    cl.write_text(text, encoding="utf-8")

    tail = [r for r in recipes if r["off"] >= AUDIO_END]
    print("gen_s3: region  %d -> %d bytes (%.3f MiB)" %
          (OLD_REGION, new_region, new_region / 1048576))
    print("gen_s3: recipes %d kept, %d audio excluded" %
          (len(keep), len(recipes) - len(keep)))
    print("gen_s3: tail    %d recipes, %d bytes, shifted -%d" %
          (len(tail), sum(r["size"] for r in tail), TAIL_SHIFT))
    print("gen_s3: map     %d bytes, %d pages, required partition %d" %
          (map_size, map_size // MMU_PAGE, required))
    print("gen_s3: app image must stay below 0x%X (%d bytes)" %
          (app_limit, app_limit))
    print("gen_s3: relocs  %d tables (%d max bytes), %d fixups, "
          "%d containers, max patched %d bytes" %
          (len(tables), max_table, len(fix_offsets), len(containers),
           max_patched))
    print("gen_s3: tag     0x%08X" % tag)


if __name__ == "__main__":
    main()
