#!/usr/bin/env python3
"""
make_zx_art.py - Generates authentic Sinclair ZX Spectrum art for Retro-Go:
  - art/background.png (320x240)
  - art/banner.png (272x24)
  - art/logo.png (46x50)
"""

import os
import struct
import zlib

def save_indexed_png(path, width, height, pixels, palette, trns_idx=None):
    raw = bytearray()
    for y in range(height):
        raw.append(0) # Filter: None
        for x in range(width):
            raw.append(pixels[y * width + x])
    compressed = zlib.compress(bytes(raw), 9)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    out = bytearray(b'\x89PNG\r\n\x1a\n')
    out.extend(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 3, 0, 0, 0)))
    
    plte = bytearray()
    for r, g, b in palette:
        plte.extend([r, g, b])
    out.extend(chunk(b'PLTE', bytes(plte)))

    if trns_idx is not None:
        trns = bytearray([255] * (trns_idx + 1))
        trns[trns_idx] = 0
        out.extend(chunk(b'tRNS', bytes(trns)))

    out.extend(chunk(b'IDAT', compressed))
    out.extend(chunk(b'IEND', b''))

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(out)
    print(f'Wrote {path} ({width}x{height}, {len(out)} bytes)')


# --------------------------------------------------------------------------
# 1. LOGO (46x50) - Iconic ZX Spectrum Rubber Key Microcomputer
# --------------------------------------------------------------------------
def generate_logo(out_path):
    W, H = 46, 50
    pixels = [0] * (W * H)

    # Palette
    PAL = [
        (255, 0, 255),    # 0: Chroma Key Transparent
        (14, 15, 18),     # 1: Dark Outer Border / Shadow
        (28, 29, 34),     # 2: Case Body Charcoal
        (48, 50, 58),     # 3: Case Highlight / Bevel
        (68, 70, 80),     # 4: Top Edge Highlight
        (38, 40, 48),     # 5: Keyboard Well
        (20, 20, 24),     # 6: Key Gap
        (54, 56, 64),     # 7: Rubber Key Cap
        (80, 84, 96),     # 8: Key Cap Top Highlight
        (230, 235, 245),  # 9: White "ZX" / "sinclair" text
        (224, 27, 36),    # 10: Rainbow Red
        (246, 211, 45),   # 11: Rainbow Yellow
        (51, 209, 122),   # 12: Rainbow Green
        (28, 113, 216),   # 13: Rainbow Blue
        (180, 20, 30),    # 14: Case Red power LED/dot
        (140, 145, 155),  # 15: Silver metal badge plate
    ]

    # Draw console case (angled wedge shape)
    # Box bounds: x: 3..42 (width 40), y: 9..43 (height 35)
    for y in range(9, 44):
        for x in range(3, 43):
            # Outer border
            if x in (3, 42) or y in (9, 43):
                pixels[y * W + x] = 1
            elif y == 10:
                pixels[y * W + x] = 4 # Top bevel
            elif x == 4 or y in (11, 12):
                pixels[y * W + x] = 3 # Bevel highlight
            elif y == 42 or x == 41:
                pixels[y * W + x] = 1 # Bottom shadow
            else:
                pixels[y * W + x] = 2 # Case body

    # Sinclair badge at top-left: x: 6..20, y: 12..14
    for y in range(12, 15):
        for x in range(6, 21):
            pixels[y * W + x] = 15
    # Red power badge dot
    pixels[13 * W + 7] = 14
    pixels[13 * W + 8] = 14

    # Keyboard well: x: 6..39, y: 17..34
    for y in range(17, 35):
        for x in range(6, 40):
            pixels[y * W + x] = 5

    # 4 rows of 10 rubber chiclet keys
    # Row y coordinates:
    rows_y = [18, 22, 26, 30]
    for ry in rows_y:
        for k in range(10):
            kx = 7 + k * 3
            if kx + 2 < 40:
                # Key body
                pixels[ry * W + kx] = 8
                pixels[ry * W + kx + 1] = 8
                pixels[(ry + 1) * W + kx] = 7
                pixels[(ry + 1) * W + kx + 1] = 7
                pixels[(ry + 2) * W + kx] = 6
                pixels[(ry + 2) * W + kx + 1] = 6

    # Space bar on bottom row (middle 4 keys merged into space bar)
    for sx in range(16, 30):
        pixels[31 * W + sx] = 8
        pixels[32 * W + sx] = 7

    # Authentic Sinclair 4-color diagonal rainbow stripe in lower right!
    # Diagonal angle 45 degrees: x + y
    # Placed across bottom right corner: x: 26..41, y: 35..42
    for y in range(35, 43):
        for x in range(25, 42):
            diag = (x - 25) - (y - 35)
            # 4 stripes, each 3 pixels wide
            if 3 <= diag < 6:
                pixels[y * W + x] = 10 # Red
            elif 6 <= diag < 9:
                pixels[y * W + x] = 11 # Yellow
            elif 9 <= diag < 12:
                pixels[y * W + x] = 12 # Green
            elif 12 <= diag < 15:
                pixels[y * W + x] = 13 # Blue

    # White "ZX" logo text on bottom left front edge: x: 7..21, y: 37..41
    # Letter 'Z' (x: 7..12, y: 37..41)
    for x in range(7, 13):
        pixels[37 * W + x] = 9
        pixels[41 * W + x] = 9
    pixels[38 * W + 11] = 9
    pixels[39 * W + 9] = 9
    pixels[39 * W + 10] = 9
    pixels[40 * W + 8] = 9

    # Letter 'X' (x: 14..19, y: 37..41)
    for i in range(5):
        pixels[(37 + i) * W + 14 + i] = 9
        pixels[(37 + i) * W + 18 - i] = 9

    save_indexed_png(out_path, W, H, pixels, PAL, trns_idx=0)


# --------------------------------------------------------------------------
# 2. BANNER (272x24) - Sinclair ZX Spectrum Launcher Header
# --------------------------------------------------------------------------
def generate_banner(out_path):
    W, H = 272, 24
    pixels = [0] * (W * H)

    PAL = [
        (255, 0, 255),    # 0: Transparent Chroma Key
        (10, 10, 14),     # 1: Shadow / Dark Accent
        (255, 255, 255),  # 2: Crisp Pure White Text
        (200, 205, 215),  # 3: Light Gray Anti-aliasing
        (224, 27, 36),    # 4: Rainbow Red
        (246, 211, 45),   # 5: Rainbow Yellow
        (51, 209, 122),   # 6: Rainbow Green
        (28, 113, 216),   # 7: Rainbow Blue
        (180, 20, 25),    # 8: Badge Red Background
    ]

    # Helper font renderer for bitmap fonts
    def draw_glyph(gx, gy, glyph_lines, col_fg=2, col_sh=1):
        for row_idx, line in enumerate(glyph_lines):
            for col_idx, ch in enumerate(line):
                if ch == '#':
                    px = gx + col_idx
                    py = gy + row_idx
                    if 0 <= px < W and 0 <= py < H:
                        if col_sh is not None and py + 1 < H and px + 1 < W:
                            if pixels[(py + 1) * W + px + 1] == 0:
                                pixels[(py + 1) * W + px + 1] = col_sh
                        pixels[py * W + px] = col_fg

    # Left: Authentic Sinclair Rainbow Emblem (x: 8..20, y: 4..19)
    # A crisp 4-stripe diagonal emblem with a thin white frame
    for y in range(4, 20):
        for x in range(8, 22):
            if x in (8, 21) or y in (4, 19):
                pixels[y * W + x] = 2 # White border
            else:
                diag = (x - 9) + (y - 5)
                if diag < 6:
                    pixels[y * W + x] = 4 # Red
                elif diag < 12:
                    pixels[y * W + x] = 5 # Yellow
                elif diag < 18:
                    pixels[y * W + x] = 6 # Green
                else:
                    pixels[y * W + x] = 7 # Blue

    # Wordmark 1: "sinclair" in lowercase italic style (x: 28..88, y: 7..17)
    FONT_SINCLAIR = {
        's': [" ### ",
              "#   #",
              " #   ",
              "  #  ",
              "#   #",
              " ### "],
        'i': [" # ",
              "   ",
              "## ",
              " # ",
              " # ",
              "###"],
        'n': ["## # ",
              "# # #",
              "# # #",
              "# # #",
              "#   #",
              "#   #"],
        'c': [" ### ",
              "#   #",
              "#    ",
              "#    ",
              "#   #",
              " ### "],
        'l': ["## ",
              " # ",
              " # ",
              " # ",
              " # ",
              "###"],
        'a': [" ### ",
              "    #",
              " ####",
              "#   #",
              "#  ##",
              " ## #"],
        'r': ["## ##",
              "# # #",
              "# #  ",
              "#    ",
              "#    ",
              "#    "]
    }

    cur_x = 26
    for ch in "sinclair":
        g = FONT_SINCLAIR.get(ch)
        if g:
            draw_glyph(cur_x, 9, g, col_fg=3, col_sh=1)
            cur_x += len(g[0]) + 2

    # Vertical subtle separator bar at x: 81
    for y in range(5, 19):
        pixels[y * W + 81] = 3

    # Wordmark 2: "ZX SPECTRUM" in bold uppercase pixel typography
    FONT_ZX_SPECTRUM = {
        'Z': ["#####",
              "   # ",
              "  #  ",
              " #   ",
              "#####"],
        'X': ["#   #",
              " # # ",
              "  #  ",
              " # # ",
              "#   #"],
        ' ': ["  ",
              "  ",
              "  ",
              "  ",
              "  "],
        'S': [" ####",
              "#    ",
              " ### ",
              "    #",
              "#### "],
        'P': ["#### ",
              "#   #",
              "#### ",
              "#    ",
              "#    "],
        'E': ["#####",
              "#    ",
              "#### ",
              "#    ",
              "#####"],
        'C': [" ####",
              "#    ",
              "#    ",
              "#    ",
              " ####"],
        'T': ["#####",
              "  #  ",
              "  #  ",
              "  #  ",
              "  #  "],
        'R': ["#### ",
              "#   #",
              "#### ",
              "#  # ",
              "#   #"],
        'U': ["#   #",
              "#   #",
              "#   #",
              "#   #",
              " ### "],
        'M': ["#   #",
              "## ##",
              "# # #",
              "#   #",
              "#   #"]
    }

    # Render "ZX SPECTRUM" in bold 2px thick starting at x: 88 (ends at x: 222)
    cur_x = 88
    for ch in "ZX SPECTRUM":
        g = FONT_ZX_SPECTRUM.get(ch)
        if g:
            # Draw standard 5x7 scaled
            for ry, line in enumerate(g):
                for cx, c in enumerate(line):
                    if c == '#':
                        # Draw 2x2 block for bold presence
                        for dy in (0, 1):
                            for dx in (0, 1):
                                px = cur_x + cx * 2 + dx
                                py = 7 + ry * 2 + dy
                                if 0 <= px < W and 0 <= py < H:
                                    if py + 1 < H and px + 1 < W and pixels[(py + 1) * W + px + 1] == 0:
                                        pixels[(py + 1) * W + px + 1] = 1
                                    pixels[py * W + px] = 2
            cur_x += len(g[0]) * 2 + 3

    # Right side: Sinclair 4-color diagonal rainbow slash (x: 234..266)
    # Clear 12px margin after 'M' (which ends at x: 222), no 128K badge
    for y in range(4, 20):
        for x in range(230, 270):
            d = (x - 234) + (y - 4)
            col = None
            if 16 <= d < 20:
                col = 4 # Red
            elif 20 <= d < 24:
                col = 5 # Yellow
            elif 24 <= d < 28:
                col = 6 # Green
            elif 28 <= d < 32:
                col = 7 # Blue
            if col is not None and 0 <= x < W and 0 <= y < H:
                if y + 1 < H and x + 1 < W and pixels[(y + 1) * W + x + 1] == 0:
                    pixels[(y + 1) * W + x + 1] = 1
                pixels[y * W + x] = col

    save_indexed_png(out_path, W, H, pixels, PAL, trns_idx=0)


# --------------------------------------------------------------------------
# 3. BACKGROUND (320x240) - Authentic Sinclair Slate & Rainbow Theme
# --------------------------------------------------------------------------
def generate_background(out_path):
    W, H = 320, 240
    pixels = [0] * (W * H)

    PAL = [
        (16, 18, 22),     # 0: Base deep charcoal slate
        (22, 25, 30),     # 1: Subtle grid lattice
        (28, 32, 40),     # 2: Subtle card bevel / separator
        (180, 24, 38),    # 3: Signature Rainbow Red
        (215, 175, 30),   # 4: Signature Rainbow Yellow
        (38, 160, 88),    # 5: Signature Rainbow Green
        (24, 98, 190),    # 6: Signature Rainbow Blue
        (12, 13, 16),     # 7: Dark vignette border
    ]

    for y in range(H):
        for x in range(W):
            # Subtle tech grid pattern every 16 pixels
            if (x % 16 == 0) or (y % 16 == 0):
                pixels[y * W + x] = 1
            else:
                pixels[y * W + x] = 0

            # Subtle top & bottom launcher guide rules
            if y == 28 or y == 212:
                pixels[y * W + x] = 2

            # Vignette frame edges
            if x < 2 or x >= W - 2 or y < 2 or y >= H - 2:
                pixels[y * W + x] = 7

            # The Signature Sinclair 4-color diagonal rainbow stripe in lower-right corner!
            # Diagonal: d = (x - 200) + (y - 120)
            d = (x - 200) + (y - 120)
            if 140 <= d < 152:
                pixels[y * W + x] = 3 # Red
            elif 152 <= d < 164:
                pixels[y * W + x] = 4 # Yellow
            elif 164 <= d < 176:
                pixels[y * W + x] = 5 # Green
            elif 176 <= d < 188:
                pixels[y * W + x] = 6 # Blue

    save_indexed_png(out_path, W, H, pixels, PAL, trns_idx=None)


if __name__ == '__main__':
    art_dir = 'D:/Games/E/retro-go-pro/cores/apps/zx-go/art'
    generate_logo(os.path.join(art_dir, 'logo.png'))
    generate_banner(os.path.join(art_dir, 'banner.png'))
    generate_background(os.path.join(art_dir, 'background.png'))
    print('All Sinclair ZX Spectrum artwork generated successfully!')
