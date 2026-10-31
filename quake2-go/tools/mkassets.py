#!/usr/bin/env python3
"""Build the read-only Quake II model/skin cache embedded in quake2-go.bin.

The engine uses the MD2 models and the skins straight from this image, so they
take no PSRAM. Without it every model of the map is loaded into the engine
arena and large maps die with "Hunk_Begin: out of memory".

The default keeps every MD2 model (8.3 MB, the 14.6 MB models+skins image of
the upstream port does not fit the core pool). Add --skins to spend the rest of
the capacity on decoded skins as well.
"""

import argparse
import struct
from pathlib import Path

HEADER = struct.Struct("<4siiiIiii")
ENTRY = struct.Struct("<56sii")
ALIGN = 16
MAX_PAKS = 10


def align(value):
    return (value + ALIGN - 1) & ~(ALIGN - 1)


def read_pak(path):
    with path.open("rb") as f:
        head = f.read(12)
        if len(head) != 12 or head[:4] != b"PACK":
            raise ValueError(f"invalid PAK: {path}")
        directory_offset, directory_size = struct.unpack_from("<ii", head, 4)
        if directory_size < 0 or directory_size > 0x1000000 or directory_size % 64:
            raise ValueError(f"invalid PAK directory: {path}")
        f.seek(directory_offset)
        directory = f.read(directory_size)
        if len(directory) != directory_size:
            raise ValueError(f"truncated PAK directory: {path}")
        return directory


def source_assets(baseq2):
    selected = {}
    for index in range(MAX_PAKS - 1, -1, -1):
        path = baseq2 / f"pak{index}.pak"
        if not path.is_file():
            continue
        directory = read_pak(path)
        for pos in range(0, len(directory), 64):
            entry = directory[pos:pos + 64]
            name = entry[:56].split(b"\0", 1)[0].decode("ascii").lower()
            offset, size = struct.unpack_from("<ii", entry, 56)
            if name.endswith(".md2") or (name.startswith("models/") and name.endswith(".pcx")):
                selected.setdefault(name, (index, path, offset, size))
    if not selected:
        raise ValueError(f"no MD2 models or model PCX skins found in {baseq2}")
    return selected


def prepare(name, source):
    pak_index, path, offset, pak_size = source
    with path.open("rb") as f:
        f.seek(offset)
        raw = f.read(pak_size)
    if len(raw) != pak_size:
        raise ValueError(f"truncated asset {name} in {path.name}")

    if name.endswith(".pcx"):
        if len(raw) < 128 or raw[:4] != bytes((10, 5, 1, 8)):
            return None
        xmax, ymax = struct.unpack_from("<HH", raw, 8)
        if xmax >= 640 or ymax >= 480:
            return None
        width, height = xmax + 1, ymax + 1
        pixels = bytearray(width * height)
        src = 128
        x = y = 0
        while y < height and src < len(raw):
            value = raw[src]
            src += 1
            run = 1
            if value & 0xC0 == 0xC0:
                run = value & 0x3F
                value = raw[src] if src < len(raw) else 0
                src += src < len(raw)
            for _ in range(run):
                if y * width + x < len(pixels):
                    pixels[y * width + x] = value
                x += 1
                if x == width:
                    x = 0
                    y += 1
                    if y >= height:
                        break
        payload = struct.pack("<HHB3x", width, height, int(255 in pixels)) + pixels
    else:
        if len(raw) < 68:
            return None
        fields = struct.unpack_from("<17i", raw)
        framesize, num_skins, num_st = fields[4], fields[5], fields[7]
        num_tris, num_frames = fields[8], fields[10]
        ofs_skins, ofs_st, ofs_tris, ofs_frames, ofs_gl = fields[11:16]
        valid_end = (
            ofs_gl >= ofs_skins + num_skins * 64
            and ofs_gl >= ofs_st + num_st * 4
            and ofs_gl >= ofs_tris + num_tris * 12
            and ofs_gl >= ofs_frames + num_frames * framesize
            and 68 <= ofs_gl <= len(raw)
        )
        size = ofs_gl if valid_end else len(raw)
        payload = bytearray(raw[:size])
        if size != len(raw):
            payload[36:40] = b"\0" * 4
            struct.pack_into("<i", payload, 64, size)
        payload = bytes(payload)
    return {"name": name, "payload": payload, "pak_index": pak_index}


def build(baseq2, output, capacity, skins=False):
    sources = source_assets(baseq2)
    assets = []
    for name, source in sources.items():
        item = prepare(name, source)
        if item:
            assets.append(item)

    # Largest first: every byte kept here is a byte the engine does not have to
    # load into PSRAM. Skin images cost no model hunk, so they are opt-in.
    used = HEADER.size
    kept = []
    pools = [[a for a in assets if a["name"].endswith(".md2")]]
    if skins:
        pools.append([a for a in assets if a["name"].endswith(".pcx")])
    for pool in pools:
        for item in sorted(pool, key=lambda a: (-len(a["payload"]), a["name"])):
            need = ENTRY.size + align(len(item["payload"]))
            if used + need > capacity:
                continue
            kept.append(item)
            used += need
    kept.sort(key=lambda a: a["name"])
    if not kept:
        raise ValueError("cache capacity is too small for any assets")

    cursor = align(HEADER.size + len(kept) * ENTRY.size)
    offsets = []
    for item in kept:
        offsets.append(cursor)
        cursor += align(len(item["payload"]))
    blob = bytearray(cursor)
    HEADER.pack_into(blob, 0, b"Q2AS", 1, len(kept), HEADER.size, 0, 0, 0, 0)
    for i, (item, offset) in enumerate(zip(kept, offsets)):
        ENTRY.pack_into(blob, HEADER.size + i * ENTRY.size,
                        item["name"].encode("ascii")[:55], offset,
                        len(item["payload"]))
        blob[offset:offset + len(item["payload"])] = item["payload"]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(blob)
    models = sum(1 for a in kept if a["name"].endswith(".md2"))
    print(f"Cached {len(kept)} of {len(assets)} assets ({models} models): {len(blob):,} bytes")
    print(f"Output: {output}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseq2", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--capacity", type=int, default=9 * 1024 * 1024)
    parser.add_argument("--skins", action="store_true",
                        help="also cache decoded skins (6.3 MB, needs a bigger capacity)")
    args = parser.parse_args()
    build(args.baseq2, args.output, args.capacity, args.skins)


if __name__ == "__main__":
    main()
