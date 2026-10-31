// retro-go shim: software TFT_eSprite and the Arduino calls of the game.
// Sprites are plain RGB565 buffers; pushSprite() hands the finished frame to
// retro-extra/main/main_arcade3d.cpp.
extern "C" {
#include <rg_system.h>
}
#include "glcdfont.h"
#undef min
#undef max
#include "TFT_eSPI.h"
#include "config.h"

SerialMock Serial;
ESPMock ESP;

uint32_t ESPMock::getPsramSize() { return 8 * 1024 * 1024; }
uint32_t ESPMock::getFreePsram() { return 0; }

unsigned long millis() { return (unsigned long)(rg_system_timer() / 1000); }
void delay(unsigned long ms) { rg_task_delay(ms); }
void randomSeed(long seed) { srand((unsigned)seed); }
int random(int max) { return max <= 0 ? 0 : rand() % max; }
int random(int min, int max) { return min >= max ? min : min + rand() % (max - min); }
int analogRead(uint8_t pin) { return (int)(rg_system_timer() & 1023); } // only seeds the random track
void pinMode(uint8_t pin, uint8_t mode) {}
void digitalWrite(uint8_t pin, uint8_t val) {}
int digitalRead(uint8_t pin)
{
    uint32_t b = a3d_buttons();
    if (pin == BTN_LEFT) return (b & A3D_BTN_LEFT) ? LOW : HIGH;
    if (pin == BTN_RIGHT) return (b & A3D_BTN_RIGHT) ? LOW : HIGH;
    return HIGH;
}

TFT_eSprite::TFT_eSprite(TFT_eSPI *tft)
    : rows(nullptr), _w(0), _h(0), cursor_x(0), cursor_y(0), text_color(0xFFFF), text_bgcolor(0),
      text_opaque(false), text_size(1) {}
TFT_eSprite::~TFT_eSprite() {}

// The frame sprite (the full screen) is drawn into strips of internal RAM:
// every fill of the road and the buildings goes through it several times a
// frame, and PSRAM is several times slower to write. Other sprites (the
// background strip, written once) stay in PSRAM.
#define A3D_STRIP_ROWS 30

void *TFT_eSprite::createSprite(int16_t w, int16_t h)
{
    rows = (uint16_t **)a3d_alloc((size_t)h * sizeof(uint16_t *));
    if (!rows)
        return nullptr;
    bool frame = (w == SCR_W && h == SCR_H);
    int fast = 0;
    for (int y = 0; y < h; y += A3D_STRIP_ROWS)
    {
        int n = h - y < A3D_STRIP_ROWS ? h - y : A3D_STRIP_ROWS;
        uint16_t *strip = (uint16_t *)(frame ? a3d_alloc_fast((size_t)w * n * 2) : a3d_alloc((size_t)w * n * 2));
        if (!strip)
            return nullptr;
        memset(strip, 0, (size_t)w * n * 2);
        for (int r = 0; r < n; r++)
            rows[y + r] = strip + r * w;
    }
    _w = w;
    _h = h;
    return rows;
}

void TFT_eSprite::deleteSprite() { rows = nullptr; _w = _h = 0; } // the memory stays with the app

void TFT_eSprite::pushSprite(int32_t x, int32_t y)
{
    if (rows)
        a3d_present(rows, _w, _h);
}

void TFT_eSprite::pushToSprite(TFT_eSprite *d, int32_t x, int32_t y)
{
    if (!rows || !d || !d->rows)
        return;
    int sx = 0, sy = 0, w = _w, h = _h;
    if (x < 0) { sx = -x; w += x; x = 0; }
    if (y < 0) { sy = -y; h += y; y = 0; }
    if (x + w > d->_w) w = d->_w - x;
    if (y + h > d->_h) h = d->_h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; r++)
        memcpy(d->rows[y + r] + x, rows[sy + r] + sx, (size_t)w * 2);
}

void TFT_eSprite::drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color)
{
    if (!rows || y < 0 || y >= _h)
        return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > _w) w = _w - x;
    if (w <= 0)
        return;
    uint16_t *p = rows[y] + x;
    if ((uintptr_t)p & 2) { *p++ = color; w--; }       // two pixels per store from here
    uint32_t c2 = color | ((uint32_t)color << 16);
    uint32_t *q = (uint32_t *)p;
    for (; w >= 8; w -= 8, q += 4) { q[0] = c2; q[1] = c2; q[2] = c2; q[3] = c2; }
    for (; w >= 2; w -= 2) *q++ = c2;
    if (w) *(uint16_t *)q = color;
}

void TFT_eSprite::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color)
{
    if (y < 0) { h += y; y = 0; }
    if (y + h > _h) h = _h - y;
    for (; h > 0; h--, y++)
        drawFastHLine(x, y, w, color);
}

void TFT_eSprite::fillSprite(uint16_t color) { fillRect(0, 0, _w, _h, color); }

void TFT_eSprite::drawPixel(int32_t x, int32_t y, uint16_t color)
{
    if (rows && x >= 0 && x < _w && y >= 0 && y < _h)
        rows[y][x] = color;
}

void TFT_eSprite::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color)
{
    drawFastHLine(x, y, w, color);
    drawFastHLine(x, y + h - 1, w, color);
    for (int i = 0; i < h; i++) { drawPixel(x, y + i, color); drawPixel(x + w - 1, y + i, color); }
}

void TFT_eSprite::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int n = dx - dy + 1; n > 0; n--) // at most one step per pixel of the longer side, twice
    {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void TFT_eSprite::fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t color)
{
    if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
    if (y1 > y2) { std::swap(y1, y2); std::swap(x1, x2); }
    if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
    if (y0 == y2)
    {
        int a = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int b = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        drawFastHLine(a, y0, b - a + 1, color);
        return;
    }
    // Edges stepped in 16.16 fixed point: three divisions a triangle instead of
    // two 64-bit divisions a row (a library call on the LX7; the road and the
    // buildings are thousands of rows a frame).
    int64_t s02 = (((int64_t)(x2 - x0)) << 16) / (y2 - y0);
    int64_t s01 = y1 > y0 ? (((int64_t)(x1 - x0)) << 16) / (y1 - y0) : 0;
    int64_t s12 = y2 > y1 ? (((int64_t)(x2 - x1)) << 16) / (y2 - y1) : 0;
    int ys = y0 < 0 ? 0 : y0, ye = y2 < _h ? y2 : _h - 1;
    int64_t a = ((int64_t)x0 << 16) + s02 * (ys - y0) + 0x8000;
    int64_t b = 0x8000 + ((ys < y1 || y1 == y2) && y1 > y0
                          ? ((int64_t)x0 << 16) + s01 * (ys - y0)
                          : ((int64_t)x1 << 16) + s12 * (ys - y1));
    for (int y = ys; y <= ye; y++)
    {
        if (y == y1 && y1 != y2)          // the short edge changes here
            b = ((int64_t)x1 << 16) + 0x8000;
        int xa = (int)(a >> 16), xb = (int)(b >> 16);
        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        drawFastHLine(xa, y, xb - xa + 1, color);
        a += s02;
        b += (y < y1 || y1 == y2) && y1 > y0 ? s01 : s12;
    }
}

void TFT_eSprite::fillEllipse(int32_t x, int32_t y, int32_t rx, int32_t ry, uint16_t color)
{
    if (rx <= 0 || ry <= 0)
        return;
    for (int dy = -ry; dy <= ry; dy++)
    {
        int dx = (int)(rx * sqrtf(1.0f - (float)(dy * dy) / (float)(ry * ry)));
        drawFastHLine(x - dx, y + dy, 2 * dx + 1, color);
    }
}

void TFT_eSprite::fillCircle(int32_t x, int32_t y, int32_t r, uint16_t color)
{
    if (r <= 0) { drawPixel(x, y, color); return; }
    fillEllipse(x, y, r, r, color);
}

void TFT_eSprite::drawCircle(int32_t x0, int32_t y0, int32_t r, uint16_t color)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y)
    {
        drawPixel(x0 + x, y0 + y, color); drawPixel(x0 - x, y0 + y, color);
        drawPixel(x0 + x, y0 - y, color); drawPixel(x0 - x, y0 - y, color);
        drawPixel(x0 + y, y0 + x, color); drawPixel(x0 - y, y0 + x, color);
        drawPixel(x0 + y, y0 - x, color); drawPixel(x0 - y, y0 - x, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void TFT_eSprite::setTextColor(uint16_t color) { text_color = color; text_opaque = false; }
void TFT_eSprite::setTextColor(uint16_t fg, uint16_t bg) { text_color = fg; text_bgcolor = bg; text_opaque = true; }
void TFT_eSprite::setTextSize(uint8_t size) { text_size = size ? size : 1; }
void TFT_eSprite::setCursor(int16_t x, int16_t y) { cursor_x = x; cursor_y = y; }

// Text: TFT_eSPI's default font (the 5x7 GLCD font in a 6x8 cell), scaled by
// the text size, so the game's HUD comes out as on the original display.
void TFT_eSprite::print(const char *str)
{
    int s = text_size;
    for (; *str; str++)
    {
        const unsigned char *glyph = &a3d_glcdfont[(unsigned char)*str * 5];
        if (text_opaque)
            fillRect(cursor_x, cursor_y, 6 * s, 8 * s, text_bgcolor);
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 8; row++)
                if (glyph[col] & (1 << row))
                    fillRect(cursor_x + col * s, cursor_y + row * s, s, s, text_color);
        cursor_x += 6 * s;
    }
}

void TFT_eSprite::print(String str) { print(str.c_str()); }
void TFT_eSprite::print(int n) { char p[16]; snprintf(p, sizeof(p), "%d", n); print(p); }
void TFT_eSprite::print(float n) { char p[24]; snprintf(p, sizeof(p), "%.2f", (double)n); print(p); }
