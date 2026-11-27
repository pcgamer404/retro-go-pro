#!/usr/bin/env python3
"""
make_fallout_art.py - Generates authentic Fallout artwork for Retro-Go:
  - apps/fallout-go/art/background.png (320x240)
  - apps/fallout-go/art/banner.png (272x24)
  - apps/fallout-go/art/logo.png (46x50)
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
# 1. LOGO (46x50) - Iconic Vault Boy Thumbs-Up Portrait & Vault-Tec Cog
# --------------------------------------------------------------------------
def generate_logo(out_path):
    W, H = 46, 50
    pixels = [0] * (W * H)

    PAL = [
        (255, 0, 255),    # 0: Transparent chroma key
        (12, 14, 18),     # 1: Deep black/dark outline
        (28, 32, 40),     # 2: Dark Slate Cog
        (56, 64, 80),     # 3: Mid Slate Steel
        (90, 105, 130),   # 4: Highlight Steel
        (16, 48, 100),    # 5: Vault Blue Shadow
        (28, 80, 155),    # 6: Vault Blue Main
        (50, 120, 210),   # 7: Vault Blue Highlight
        (160, 110, 10),   # 8: Gold/Yellow Shadow
        (245, 195, 20),   # 9: Gold/Yellow Main
        (255, 230, 100),  # 10: Gold/Yellow Highlight
        (180, 115, 75),   # 11: Flesh Shadow
        (250, 195, 150),  # 12: Flesh Main
        (255, 225, 195),  # 13: Flesh Highlight
        (210, 220, 230),  # 14: White/Light Gray (Glove, teeth, eye)
        (255, 255, 255),  # 15: Pure White
    ]

    # Vault-Tec Cog Wheel Frame (Outer radius ~21, Center at 23, 24)
    cx, cy = 23, 24
    for y in range(H):
        for x in range(W):
            dx = x - cx
            dy = y - cy
            dist2 = dx * dx + dy * dy
            if dist2 <= 22 * 22:
                # Add cog teeth every 30 degrees (12 teeth)
                import math
                angle = math.atan2(dy, dx)
                gear = math.cos(angle * 12)
                r_limit = 20.0 + 1.8 * gear
                if dist2 <= r_limit * r_limit:
                    if dist2 > 18 * 18:
                        pixels[y * W + x] = 4 if dy < 0 else 2 # Beveled cog rim
                    elif dist2 > 17 * 17:
                        pixels[y * W + x] = 1 # Inner groove
                    else:
                        pixels[y * W + x] = 5 # Inner deep blue background

    # Vault Boy Character Drawing
    # Blonde hair
    hair_pixels = [
        (20, 9), (21, 9), (22, 9), (23, 9), (24, 9), (25, 9),
        (18, 10), (19, 10), (20, 10), (21, 10), (22, 10), (23, 10), (24, 10), (25, 10), (26, 10), (27, 10),
        (17, 11), (18, 11), (19, 11), (20, 11), (21, 11), (22, 11), (23, 11), (24, 11), (25, 11), (26, 11), (27, 11), (28, 11),
        (16, 12), (17, 12), (18, 12), (19, 12), (20, 12), (21, 12), (22, 12), (23, 12), (24, 12), (25, 12), (26, 12), (27, 12), (28, 12), (29, 12),
        (16, 13), (17, 13), (18, 13), (19, 13), (20, 13), (21, 13), (27, 13), (28, 13), (29, 13), (30, 13),
        (15, 14), (16, 14), (17, 14), (18, 14), (28, 14), (29, 14), (30, 14),
        (15, 15), (16, 15), (17, 15), (29, 15), (30, 15),
        (15, 16), (16, 16), (30, 16)
    ]
    for x, y in hair_pixels:
        pixels[y * W + x] = 9
    # Hair highlights
    for x, y in [(21, 10), (22, 10), (23, 10), (20, 11), (21, 11), (22, 11), (19, 12), (20, 12)]:
        pixels[y * W + x] = 10
    # Hair shadow
    for x, y in [(16, 13), (17, 13), (28, 13), (29, 13), (15, 14), (16, 14), (15, 15), (15, 16)]:
        pixels[y * W + x] = 8

    # Face & Skin
    for y in range(13, 26):
        for x in range(17, 29):
            if y == 13 and (x < 22 or x > 26): continue
            if y == 14 and (x < 19 or x > 27): continue
            if y == 24 and (x < 19 or x > 27): continue
            if y == 25 and (x < 20 or x > 26): continue
            pixels[y * W + x] = 12

    # Face Shading & Features
    # Eyebrows
    pixels[15 * W + 20] = 8
    pixels[15 * W + 21] = 8
    pixels[15 * W + 25] = 8
    pixels[15 * W + 26] = 8

    # Left eye (open, smiling)
    pixels[17 * W + 20] = 15
    pixels[17 * W + 21] = 1
    pixels[18 * W + 20] = 15
    pixels[18 * W + 21] = 1

    # Right eye (winking wink: `>`)
    pixels[17 * W + 25] = 1
    pixels[17 * W + 27] = 1
    pixels[18 * W + 26] = 1

    # Nose
    pixels[20 * W + 23] = 11

    # Cheerful wide grin
    pixels[22 * W + 20] = 1
    pixels[23 * W + 21] = 1
    pixels[23 * W + 22] = 14 # white teeth
    pixels[23 * W + 23] = 14 # white teeth
    pixels[23 * W + 24] = 14 # white teeth
    pixels[23 * W + 23] = 14
    pixels[23 * W + 25] = 1
    pixels[22 * W + 26] = 1
    pixels[24 * W + 22] = 1
    pixels[24 * W + 23] = 1
    pixels[24 * W + 24] = 1

    # Neck
    for y in range(26, 28):
        for x in range(21, 25):
            pixels[y * W + x] = 11

    # Vault 13 Jumpsuit Body (y: 28..42, x: 10..36)
    for y in range(28, 43):
        for x in range(11, 35):
            dx = x - cx
            dy = y - cy
            if dx * dx + dy * dy <= 16.5 * 16.5:
                pixels[y * W + x] = 6

    # Suit yellow collar & center stripe
    for y in range(28, 31):
        pixels[y * W + 20] = 9
        pixels[y * W + 25] = 9
    for y in range(29, 41):
        pixels[y * W + 22] = 10
        pixels[y * W + 23] = 9

    # Suit shadows
    for y in range(32, 42):
        if pixels[y * W + 12] == 6: pixels[y * W + 12] = 5
        if pixels[y * W + 13] == 6: pixels[y * W + 13] = 5
        if pixels[y * W + 33] == 6: pixels[y * W + 33] = 5
        if pixels[y * W + 34] == 6: pixels[y * W + 34] = 5

    # Thumbs-Up Glove (Left hand pointing up at x: 31..38, y: 22..32)
    # Thumb pointing up
    for ty in range(22, 27):
        pixels[ty * W + 34] = 15
        pixels[ty * W + 35] = 14
        pixels[ty * W + 33] = 1 # outline
        pixels[ty * W + 36] = 1
    pixels[21 * W + 34] = 1 # thumb top outline
    pixels[21 * W + 35] = 1

    # Fist / fingers
    for fy in range(26, 32):
        for fx in range(32, 38):
            if fx in (32, 37) or fy in (26, 31):
                pixels[fy * W + fx] = 1 # outline
            else:
                pixels[fy * W + fx] = 14 if fx < 35 else 4

    # Crisp dark outline around hair & head
    for y in range(9, 26):
        for x in range(14, 32):
            if pixels[y * W + x] != 0 and pixels[y * W + x] != 1:
                for dy, dx in [(-1, 0), (1, 0), (0, -1), (0, 1)]:
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < H and 0 <= nx < W and pixels[ny * W + nx] == 5:
                        pixels[ny * W + nx] = 1

    save_indexed_png(out_path, W, H, pixels, PAL, trns_idx=0)


# --------------------------------------------------------------------------
# 2. BANNER (272x24) - Classic Fallout Wordmark + Post-Nuclear Subtitle + Cog
# --------------------------------------------------------------------------
def generate_banner(out_path):
    W, H = 272, 24
    pixels = [0] * (W * H)

    PAL = [
        (255, 0, 255),    # 0: Transparent chroma key
        (10, 12, 14),     # 1: Deep shadow / outline
        (140, 90, 10),    # 2: Metallic Gold Dark Bronze
        (200, 145, 15),   # 3: Metallic Gold Mid
        (245, 195, 25),   # 4: Metallic Gold Bright Body
        (255, 235, 120),  # 5: Metallic Gold Highlight / Specular
        (40, 50, 45),     # 6: Pip-Boy Charcoal Green
        (140, 185, 140),  # 7: Pip-Boy Light Phosphor Text
        (200, 235, 200),  # 8: Pip-Boy Bright Phosphor
        (30, 45, 75),     # 9: Vault Blue
        (230, 40, 30),    # 10: Warning Red Accent
    ]

    # We read the original 1-bit FALLOUT glyph coordinates from the current banner
    # and render them with a gorgeous 3D beveled metallic gold gradient!
    current_banner_path = 'D:/Games/E/retro-go-pro/cores/apps/fallout-go/art/banner.png'
    with open(current_banner_path, 'rb') as f:
        data = f.read()
    idat_idx = data.find(b'IDAT')
    raw = zlib.decompress(data[idat_idx+4:idat_idx+4+struct.unpack('>I', data[idat_idx-4:idat_idx])[0]])
    stride = 1 + (272 * 4 + 7) // 8
    
    # Extract binary mask of original FALLOUT wordmark
    fallout_mask = set()
    for y in range(24):
        row = raw[y * stride + 1 : (y + 1) * stride]
        for x in range(160): # Only the wordmark area
            val = (row[x // 2] >> 4) if (x % 2 == 0) else (row[x // 2] & 0xF)
            if val != 0:
                fallout_mask.add((x, y))

    # Render FALLOUT with rich metallic gold gradient & 3D shadow
    for x, y in fallout_mask:
        # First write dark drop shadow underneath
        for sdy in (1, 2):
            for sdx in (1, 2):
                sx, sy = x + sdx, y + sdy
                if 0 <= sx < W and 0 <= sy < H:
                    if (sx, sy) not in fallout_mask:
                        pixels[sy * W + sx] = 1

    for x, y in fallout_mask:
        # Check if top or left edge (highlight)
        is_top = (x, y - 1) not in fallout_mask or (x - 1, y) not in fallout_mask
        is_bottom = (x, y + 1) not in fallout_mask or (x + 1, y) not in fallout_mask
        
        if is_top and y < 8:
            pixels[y * W + x] = 5 # Specular highlight
        elif is_bottom:
            pixels[y * W + x] = 2 # Dark bronze bevel
        elif y < 9:
            pixels[y * W + x] = 4 # Bright gold
        elif y < 16:
            pixels[y * W + x] = 3 # Mid gold
        else:
            pixels[y * W + x] = 2 # Lower shadow

    # Industrial vertical separator at x: 154
    for y in range(4, 20):
        if y % 3 != 0:
            pixels[y * W + 154] = 6
            pixels[y * W + 155] = 7

    # Micro-font renderer for subtitle: "A POST NUCLEAR ROLE PLAYING GAME"
    FONT_MICRO = {
        'A': ["###", "# #", "###", "# #", "# #"],
        'B': ["## ", "# #", "## ", "# #", "## "],
        'C': ["###", "#  ", "#  ", "#  ", "###"],
        'D': ["## ", "# #", "# #", "# #", "## "],
        'E': ["###", "#  ", "## ", "#  ", "###"],
        'F': ["###", "#  ", "## ", "#  ", "#  "],
        'G': ["###", "#  ", "# #", "# #", "###"],
        'H': ["# #", "# #", "###", "# #", "# #"],
        'I': ["###", " # ", " # ", " # ", "###"],
        'K': ["# #", "## ", "#  ", "## ", "# #"],
        'L': ["#  ", "#  ", "#  ", "#  ", "###"],
        'M': ["# #", "###", "# #", "# #", "# #"],
        'N': ["## ", "# #", "# #", "# #", "# #"],
        'O': ["###", "# #", "# #", "# #", "###"],
        'P': ["###", "# #", "###", "#  ", "#  "],
        'R': ["###", "# #", "## ", "# #", "# #"],
        'S': ["###", "#  ", "###", "  #", "###"],
        'T': ["###", " # ", " # ", " # ", " # "],
        'U': ["# #", "# #", "# #", "# #", "###"],
        'Y': ["# #", "# #", "###", " # ", " # "],
        ' ': [" ", " ", " ", " ", " "],
        '-': [" ", " ", "###", " ", " "],
        '1': [" # ", "## ", " # ", " # ", "###"],
        '3': ["###", "  #", "###", "  #", "###"],
    }

    def draw_text(gx, gy, text, col_fg=7, col_sh=1):
        cur_x = gx
        for ch in text:
            g = FONT_MICRO.get(ch)
            if g:
                for row_idx, line in enumerate(g):
                    for col_idx, c in enumerate(line):
                        if c == '#':
                            px = cur_x + col_idx
                            py = gy + row_idx
                            if 0 <= px < W and 0 <= py < H:
                                if col_sh is not None and py + 1 < H and px + 1 < W and pixels[(py + 1) * W + px + 1] == 0:
                                    pixels[(py + 1) * W + px + 1] = col_sh
                                pixels[py * W + px] = col_fg
                cur_x += len(g[0]) + 1

    # Subtitle line 1: "A POST NUCLEAR" at x: 162, y: 5
    draw_text(162, 5, "A POST NUCLEAR", col_fg=8, col_sh=1)
    # Subtitle line 2: "ROLE PLAYING GAME" at x: 162, y: 13
    draw_text(162, 13, "ROLE PLAYING GAME", col_fg=7, col_sh=1)

    # Right side: Authentic Vault 13 Cog Emblem (x: 246..268, y: 2..21)
    cx, cy = 256, 12
    for y in range(2, 22):
        for x in range(246, 269):
            dx = x - cx
            dy = y - cy
            dist2 = dx * dx + dy * dy
            if dist2 <= 9.5 * 9.5:
                import math
                angle = math.atan2(dy, dx)
                gear = math.cos(angle * 8)
                r_limit = 8.5 + 1.2 * gear
                if dist2 <= r_limit * r_limit:
                    if dist2 > 6.5 * 6.5:
                        pixels[y * W + x] = 5 if dy < 0 else 3 # Gold gear teeth
                    elif dist2 > 5.5 * 5.5:
                        pixels[y * W + x] = 1 # Rim outline
                    else:
                        pixels[y * W + x] = 9 # Vault Blue center

    # "13" in center of Vault cog
    draw_text(253, 10, "13", col_fg=4, col_sh=None)

    save_indexed_png(out_path, W, H, pixels, PAL, trns_idx=0)


# --------------------------------------------------------------------------
# 3. BACKGROUND (320x240) - Authentic Vault-Tec Pip-Boy 2000 Phosphor CRT
# --------------------------------------------------------------------------
def generate_background(out_path):
    W, H = 320, 240
    pixels = [0] * (W * H)

    PAL = [
        (8, 12, 9),       # 0: Deep CRT Phosphor Dark (scanline dark)
        (12, 18, 14),     # 1: Deep CRT Phosphor Base (scanline light)
        (18, 28, 21),     # 2: Subtle terminal frame / grid
        (26, 42, 30),     # 3: Bevel / rule separator
        (35, 60, 42),     # 4: Watermark trefoil dark
        (50, 85, 58),     # 5: Watermark trefoil mid
        (75, 125, 85),    # 6: Watermark trefoil bright
        (110, 175, 120),  # 7: Bright phosphor highlight
        (15, 20, 17),     # 8: Outer vignette edge
    ]

    # Fill base with authentic CRT scanline texture
    for y in range(H):
        base_col = 0 if (y % 2 == 0) else 1
        for x in range(W):
            pixels[y * W + x] = base_col

    # Launcher layout guide rules:
    # y = 27 (separates top header/banner)
    # y = 213 (separates bottom status bar)
    for x in range(W):
        pixels[27 * W + x] = 3
        pixels[28 * W + x] = 2
        pixels[213 * W + x] = 3
        pixels[214 * W + x] = 2

    # Vignette border frame
    for y in range(H):
        for x in range(W):
            if x < 2 or x >= W - 2 or y < 2 or y >= H - 2:
                pixels[y * W + x] = 8

    # Subtle tech terminal grid on the right half (x: 155..315, y: 32..208)
    for y in range(32, 210, 16):
        for x in range(156, 316):
            pixels[y * W + x] = 2
    for x in range(156, 316, 16):
        for y in range(32, 210):
            pixels[y * W + x] = 2

    # Watermarked Vault-Tec Radiation Hazard Trefoil in the right preview area!
    # Centered at cx=236, cy=120, radius ~46
    cx, cy = 236, 120
    import math
    for y in range(70, 172):
        for x in range(186, 288):
            dx = x - cx
            dy = y - cy
            dist2 = dx * dx + dy * dy
            if dist2 <= 46 * 46:
                dist = math.sqrt(dist2)
                angle = (math.atan2(dy, dx) + math.pi * 2) % (math.pi * 2)
                # 3 blades centered at 30, 150, 270 degrees
                deg = math.degrees(angle)
                
                # Center circular hub:
                if dist <= 10.0:
                    pixels[y * W + x] = 6 if dist <= 8.5 else 4
                elif dist > 14.0:
                    # 3 Trefoil blades (each spans 60 degrees)
                    # Blade 1: 0..60, Blade 2: 120..180, Blade 3: 240..300
                    blade1 = (0 <= deg <= 60)
                    blade2 = (120 <= deg <= 180)
                    blade3 = (240 <= deg <= 300)
                    if blade1 or blade2 or blade3:
                        if dist >= 43.0:
                            pixels[y * W + x] = 4
                        elif dist >= 16.0:
                            # Inner blade with phosphor glow
                            pixels[y * W + x] = 5 if (y % 2 == 0) else 6

    # Micro tech labels in terminal header / footer:
    FONT_NANO = {
        '[': ["##", "# ", "# ", "# ", "##"],
        ']': ["##", " #", " #", " #", "##"],
        'V': ["# #", "# #", "# #", "# #", " # "],
        'A': ["###", "# #", "###", "# #", "# #"],
        'U': ["# #", "# #", "# #", "# #", "###"],
        'L': ["#  ", "#  ", "#  ", "#  ", "###"],
        'T': ["###", " # ", " # ", " # ", " # "],
        '-': ["   ", "   ", "###", "   ", "   "],
        'E': ["###", "#  ", "## ", "#  ", "###"],
        'C': ["###", "#  ", "#  ", "#  ", "###"],
        'P': ["###", "# #", "###", "#  ", "#  "],
        'I': ["###", " # ", " # ", " # ", "###"],
        'B': ["## ", "# #", "## ", "# #", "## "],
        'O': ["###", "# #", "# #", "# #", "###"],
        'Y': ["# #", "# #", "###", " # ", " # "],
        '2': ["###", "  #", "###", "#  ", "###"],
        '0': ["###", "# #", "# #", "# #", "###"],
        'R': ["###", "# #", "## ", "# #", "# #"],
        ' ': [" ", " ", " ", " ", " "],
        '.': [" ", " ", " ", " ", "#"],
        '1': [" #", "##", " #", " #", "###"],
    }

    def draw_nano_text(gx, gy, text, col):
        cur_x = gx
        for ch in text:
            g = FONT_NANO.get(ch)
            if g:
                for row_idx, line in enumerate(g):
                    for col_idx, c in enumerate(line):
                        if c == '#':
                            px = cur_x + col_idx
                            py = gy + row_idx
                            if 0 <= px < W and 0 <= py < H:
                                pixels[py * W + px] = col
                cur_x += len(g[0]) + 1

    # Terminal header watermark: "[ VAULT-TEC PIP-BOY 2000 ]" at (14, 218)
    draw_nano_text(12, 218, "[ VAULT-TEC PIP-BOY 2000 ]", 4)
    draw_nano_text(220, 218, "[ ROBCO TERM-OS v1.1 ]", 3)

    save_indexed_png(out_path, W, H, pixels, PAL, trns_idx=None)


if __name__ == '__main__':
    art_dir = 'D:/Games/E/retro-go-pro/cores/apps/fallout-go/art'
    generate_logo(os.path.join(art_dir, 'logo.png'))
    generate_banner(os.path.join(art_dir, 'banner.png'))
    generate_background(os.path.join(art_dir, 'background.png'))
    print('All Fallout artwork generated successfully!')
