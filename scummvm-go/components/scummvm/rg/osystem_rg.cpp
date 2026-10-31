/* ScummVM OSystem backend for Retro-Go. GPL-2.0-or-later, like ScummVM. */

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "osystem_rg.h"
#include "rg_glue.h"
#include "backends/saves/default/default-saves.h"
#include "backends/timer/default/default-timer.h"
#include "backends/events/default/default-events.h"
#include "backends/fs/posix/posix-fs-factory.h"
#include "common/util.h"
#include "common/textconsole.h"
#include "common/config-manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

namespace RgBackend {

static const OSystem::GraphicsMode s_graphicsModes[] = {
	{ "default", "Default", 0 },
	{ 0, 0, 0 }
};

OSystem_RG::OSystem_RG(const char *saveDir, const char *configFile)
	: _mixer(nullptr), _audioRunning(false), _saveDir(saveDir), _configFile(configFile),
	  _gameWidth(320), _gameHeight(200), _gamePixels(nullptr), _gameDirty(false),
	  _cursorBuf(nullptr), _cursorW(0), _cursorH(0), _cursorHotX(0), _cursorHotY(0),
	  _cursorKey(0), _cursorVisible(false), _cursorDirty(false), _cursorX(160), _cursorY(100),
	  _fx(160.0f), _fy(100.0f), _holdTime(0.0f), _lastMoveUs(0), _prevButtons(0), _lastInputUs(0),
	  _startUs(0), _lastPresentUs(0), _waitedUs(0), _inTimers(false) {
	_fsFactory = new POSIXFilesystemFactory();
	_pfGame = Graphics::PixelFormat::createFormatCLUT8();
	memset(_palette, 0, sizeof(_palette));
}

OSystem_RG::~OSystem_RG() {
	delete _mixer;
	free(_gamePixels);
	free(_cursorBuf);
}

void OSystem_RG::initBackend() {
	ConfMan.registerDefault("fullscreen", true);
	ConfMan.registerDefault("aspect_ratio", true);

	_startUs = rgx_time_us();
	_lastPresentUs = _startUs;
	_lastMoveUs = _startUs;

	_timerManager = new DefaultTimerManager();
	_savefileManager = new DefaultSaveFileManager(_saveDir);

	initGraphics();
	initAudio();
	EventsBaseBackend::initBackend();
}

// ------------------------------------------------------------------ features
bool OSystem_RG::hasFeature(OSystem::Feature f) { return false; }
void OSystem_RG::setFeatureState(OSystem::Feature f, bool enable) {}
bool OSystem_RG::getFeatureState(OSystem::Feature f) { return false; }

// ------------------------------------------------------------------ time
uint32 OSystem_RG::getMillis(bool skipRecord) {
	return (uint32)((rgx_time_us() - _startUs) / 1000);
}

void OSystem_RG::pollTimers() {
	// iMUSE / SCUMM timers: no timer thread, so service them whenever the engine calls us.
	if (_inTimers || !_timerManager)
		return;
	_inTimers = true;
	static_cast<DefaultTimerManager *>(_timerManager)->handler();
	_inTimers = false;
}

void OSystem_RG::delayMillis(uint msecs) {
	int64 t0 = rgx_time_us();
	while (msecs) {
		uint slice = msecs > 5 ? 5 : msecs;
		rgx_sleep_us((uint64)slice * 1000);
		pollTimers();
		msecs -= slice;
	}
	_waitedUs += rgx_time_us() - t0;
}

void OSystem_RG::getTimeAndDate(TimeDate &td) const {
	time_t curTime = time(0);
	struct tm t = *localtime(&curTime);
	td.tm_sec = t.tm_sec;
	td.tm_min = t.tm_min;
	td.tm_hour = t.tm_hour;
	td.tm_mday = t.tm_mday;
	td.tm_mon = t.tm_mon;
	td.tm_year = t.tm_year;
	td.tm_wday = t.tm_wday;
}

// ------------------------------------------------------------------ mutexes (real ones: the mixer runs on another core)
OSystem::MutexRef OSystem_RG::createMutex() { return (MutexRef)rgx_mutex_create(); }
void OSystem_RG::lockMutex(MutexRef m) { rgx_mutex_lock((void *)m); }
void OSystem_RG::unlockMutex(MutexRef m) { rgx_mutex_unlock((void *)m); }
void OSystem_RG::deleteMutex(MutexRef m) { rgx_mutex_delete((void *)m); }

// ------------------------------------------------------------------ misc
void OSystem_RG::logMessage(LogMessageType::Type type, const char *message) {
	fputs(message, stdout);
}

Common::String OSystem_RG::getSystemLanguage() const { return "en_US"; }

Common::String OSystem_RG::getDefaultConfigFileName() { return _configFile; }

void OSystem_RG::quit() {
	rgx_exit();
}

// ------------------------------------------------------------------ audio
static void audioCallback(int16_t *stereo, int frames, void *ud) {
	static_cast<OSystem_RG *>(ud)->mixAudio(stereo, frames);
}

void OSystem_RG::mixAudio(int16 *stereo, int frames) {
	if (_mixer)
		_mixer->mixCallback((byte *)stereo, (uint)frames * 4);
}

void OSystem_RG::initAudio() {
	_mixer = new Audio::MixerImpl(this, 22050);
	_mixer->setReady(false);
	if (rgx_sound_enabled()) {
		_mixer->setReady(true);
		rgx_audio_start(&audioCallback, this);
		_audioRunning = true;
	}
}

Audio::Mixer *OSystem_RG::getMixer() { return _mixer; }

// ------------------------------------------------------------------ graphics
const OSystem::GraphicsMode *OSystem_RG::getSupportedGraphicsModes() const { return s_graphicsModes; }
int OSystem_RG::getDefaultGraphicsMode() const { return 0; }
bool OSystem_RG::setGraphicsMode(int mode) { return true; }
int OSystem_RG::getGraphicsMode() const { return 0; }
void OSystem_RG::resetGraphicsScale() {}

void OSystem_RG::initGraphics() {
	initSize(320, 200);
}

void OSystem_RG::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
	if (format && format->bytesPerPixel != 1)
		warning("RG backend: only 8-bit paletted games are supported");

	if (_gamePixels && width == _gameWidth && height == _gameHeight)
		return;

	free(_gamePixels);
	_gameWidth = width;
	_gameHeight = height;
	_gamePixels = (byte *)calloc(width * height, 1);
	_cursorX = width / 2;
	_cursorY = height / 2;
	_fx = (float)_cursorX;
	_fy = (float)_cursorY;
	rgx_video_init(width, height);
	_gameDirty = true;
}

void OSystem_RG::setPalette(const byte *colors, uint start, uint num) {
	if (start >= 256)
		return;
	if (start + num > 256)
		num = 256 - start;
	memcpy(_palette + start * 3, colors, num * 3);
	rgx_video_set_palette(colors, start, num);
	_gameDirty = true;
}

void OSystem_RG::grabPalette(byte *colors, uint start, uint num) const {
	if (start >= 256)
		return;
	if (start + num > 256)
		num = 256 - start;
	memcpy(colors, _palette + start * 3, num * 3);
}

void OSystem_RG::copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) {
	if (!_gamePixels || w <= 0 || h <= 0)
		return;
	const byte *src = (const byte *)buf;
	if (x < 0) { src -= x; w += x; x = 0; }
	if (y < 0) { src -= y * pitch; h += y; y = 0; }
	if (x + w > (int)_gameWidth) w = _gameWidth - x;
	if (y + h > (int)_gameHeight) h = _gameHeight - y;
	if (w <= 0 || h <= 0)
		return;

	byte *dst = _gamePixels + y * _gameWidth + x;
	for (int i = 0; i < h; i++) {
		memcpy(dst, src, w);
		src += pitch;
		dst += _gameWidth;
	}
	_gameDirty = true;
}

Graphics::Surface *OSystem_RG::lockScreen() {
	_surface.init(_gameWidth, _gameHeight, _gameWidth, _gamePixels,
	              Graphics::PixelFormat::createFormatCLUT8());
	return &_surface;
}

void OSystem_RG::unlockScreen() {
	_gameDirty = true;
}

void OSystem_RG::drawCursor(byte *dst) {
	int x0 = _cursorX - _cursorHotX;
	int y0 = _cursorY - _cursorHotY;
	for (uint cy = 0; cy < _cursorH; cy++) {
		int py = y0 + (int)cy;
		if (py < 0 || py >= (int)_gameHeight)
			continue;
		const byte *src = _cursorBuf + cy * _cursorW;
		byte *row = dst + py * _gameWidth;
		for (uint cx = 0; cx < _cursorW; cx++) {
			int px = x0 + (int)cx;
			if (px < 0 || px >= (int)_gameWidth)
				continue;
			if (src[cx] != (byte)_cursorKey)
				row[px] = src[cx];
		}
	}
}

void OSystem_RG::updateScreen() {
	pollTimers();

	int64 now = rgx_time_us();
	if (!_gamePixels || (!_gameDirty && !(_cursorDirty && _cursorVisible))) {
		// keep Retro-Go's frame accounting alive while the picture is static
		if (now - _lastPresentUs >= 33000) {
			rgx_tick(0);
			_lastPresentUs = now;
			_waitedUs = 0;
		}
		return;
	}

	byte *dst = rgx_video_pixels();
	memcpy(dst, _gamePixels, (size_t)_gameWidth * _gameHeight);
	if (_cursorVisible && _cursorBuf)
		drawCursor(dst);
	rgx_video_present();

	now = rgx_time_us();
	int64 busy = (now - _lastPresentUs) - _waitedUs;
	if (busy < 0 || now - _lastPresentUs > 50000)
		busy = 0;
	rgx_tick(busy);
	_lastPresentUs = now;
	_waitedUs = 0;
	_gameDirty = false;
	_cursorDirty = false;
}

void OSystem_RG::setShakePos(int) {}
void OSystem_RG::setFocusRectangle(const Common::Rect &) {}
void OSystem_RG::clearFocusRectangle() {}

// No overlay yet: engine-side dialogs (GMM, error boxes) are not drawn. See README.
void OSystem_RG::showOverlay() {}
void OSystem_RG::hideOverlay() {}
Graphics::PixelFormat OSystem_RG::getOverlayFormat() const { return _pfGame; }
void OSystem_RG::clearOverlay() {}
void OSystem_RG::grabOverlay(void *, int) {}
void OSystem_RG::copyRectToOverlay(const void *, int, int, int, int, int) {}
int16 OSystem_RG::getOverlayHeight() { return 0; }
int16 OSystem_RG::getOverlayWidth() { return 0; }

bool OSystem_RG::showMouse(bool visible) {
	bool last = _cursorVisible;
	_cursorVisible = visible;
	_cursorDirty = true;
	return last;
}

void OSystem_RG::warpMouse(int x, int y) {
	_cursorX = CLIP(x, 0, (int)_gameWidth - 1);
	_cursorY = CLIP(y, 0, (int)_gameHeight - 1);
	_fx = (float)_cursorX;
	_fy = (float)_cursorY;
	_cursorDirty = true;
}

void OSystem_RG::setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY,
                                uint32 keycolor, bool dontScale, const Graphics::PixelFormat *format) {
	free(_cursorBuf);
	_cursorBuf = nullptr;
	_cursorW = _cursorH = 0;
	_cursorDirty = true;

	if (!buf || !w || !h)
		return;
	if (format && format->bytesPerPixel != 1) {
		warning("RG backend: ignoring non-paletted mouse cursor");
		return;
	}
	_cursorBuf = (byte *)malloc(w * h);
	if (!_cursorBuf)
		return;
	memcpy(_cursorBuf, buf, w * h);
	_cursorW = w;
	_cursorH = h;
	_cursorHotX = hotspotX;
	_cursorHotY = hotspotY;
	_cursorKey = keycolor;
}

// ------------------------------------------------------------------ input
void OSystem_RG::pushMouse(Common::EventType type) {
	Common::Event ev;
	ev.type = type;
	ev.mouse.x = _cursorX;
	ev.mouse.y = _cursorY;
	_eventQueue.push(ev);
}

void OSystem_RG::pushKey(Common::KeyCode code, uint16 ascii, byte flags, bool down) {
	Common::Event ev;
	ev.type = down ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
	ev.kbd.keycode = code;
	ev.kbd.ascii = ascii;
	ev.kbd.flags = flags;
	_eventQueue.push(ev);
}

void OSystem_RG::readInput() {
	int64 now = rgx_time_us();
	if (now - _lastInputUs < 8000)
		return;
	_lastInputUs = now;

	uint32 b = rgx_buttons();

	// Retro-Go menus (Menu = game menu, Option = options menu)
	if (b & (RGX_MENU | RGX_OPTION)) {
		rgx_menu((b & RGX_OPTION) != 0);
		_prevButtons = rgx_buttons();
		_lastMoveUs = rgx_time_us();
		_lastPresentUs = _lastMoveUs;
		_waitedUs = 0;
		_gameDirty = true; // redraw after the menu overlay
		return;
	}

#define PRESSED(m)  ((b & (m)) && !(_prevButtons & (m)))
#define RELEASED(m) (!(b & (m)) && (_prevButtons & (m)))

	// Retro-Go save/load slots map onto SCUMM's quicksave hotkeys (Alt+1 / Ctrl+1)
	int q = rgx_take_quicksave_request();
	if (q == 1) {
		pushKey(Common::KEYCODE_1, '1', Common::KBD_ALT, true);
		pushKey(Common::KEYCODE_1, '1', Common::KBD_ALT, false);
	} else if (q == 2) {
		pushKey(Common::KEYCODE_1, '1', Common::KBD_CTRL, true);
		pushKey(Common::KEYCODE_1, '1', Common::KBD_CTRL, false);
	}

	// Cursor movement with acceleration; Select = precision mode
	float dt = (float)(now - _lastMoveUs) / 1000000.0f;
	_lastMoveUs = now;
	if (dt > 0.1f)
		dt = 0.1f;

	int dx = ((b & RGX_RIGHT) ? 1 : 0) - ((b & RGX_LEFT) ? 1 : 0);
	int dy = ((b & RGX_DOWN) ? 1 : 0) - ((b & RGX_UP) ? 1 : 0);
	if (dx || dy)
		_holdTime += dt;
	else
		_holdTime = 0.0f;

	static const float baseSpeed[3] = { 90.0f, 140.0f, 220.0f }; // px/s at native resolution
	int sp = rgx_cursor_speed();
	if (sp < 0) sp = 0;
	if (sp > 2) sp = 2;
	float accel = 1.0f + (_holdTime > 0.8f ? 0.8f : _holdTime) * 2.5f;
	if (b & RGX_SELECT)
		accel *= 0.3f;

	_fx += dx * baseSpeed[sp] * accel * dt;
	_fy += dy * baseSpeed[sp] * accel * dt;
	if (_fx < 0.0f) _fx = 0.0f;
	if (_fy < 0.0f) _fy = 0.0f;
	if (_fx > (float)(_gameWidth - 1)) _fx = (float)(_gameWidth - 1);
	if (_fy > (float)(_gameHeight - 1)) _fy = (float)(_gameHeight - 1);

	int nx = (int)_fx, ny = (int)_fy;
	if (nx != _cursorX || ny != _cursorY) {
		_cursorX = nx;
		_cursorY = ny;
		_cursorDirty = true;
		pushMouse(Common::EVENT_MOUSEMOVE);
	}

	// Buttons
	if (PRESSED(RGX_A))  pushMouse(Common::EVENT_LBUTTONDOWN);
	if (RELEASED(RGX_A)) pushMouse(Common::EVENT_LBUTTONUP);
	if (PRESSED(RGX_B))  pushMouse(Common::EVENT_RBUTTONDOWN);
	if (RELEASED(RGX_B)) pushMouse(Common::EVENT_RBUTTONUP);

	if (PRESSED(RGX_START))  pushKey(Common::KEYCODE_ESCAPE, Common::ASCII_ESCAPE, 0, true);
	if (RELEASED(RGX_START)) pushKey(Common::KEYCODE_ESCAPE, Common::ASCII_ESCAPE, 0, false);

	// X = '.' (skip dialogue line), Y = space (pause)
	if (PRESSED(RGX_X))  pushKey(Common::KEYCODE_PERIOD, '.', 0, true);
	if (RELEASED(RGX_X)) pushKey(Common::KEYCODE_PERIOD, '.', 0, false);
	if (PRESSED(RGX_Y))  pushKey(Common::KEYCODE_SPACE, ' ', 0, true);
	if (RELEASED(RGX_Y)) pushKey(Common::KEYCODE_SPACE, ' ', 0, false);

	// L = quicksave, R = quickload (Alt+1 / Ctrl+1)
	if (PRESSED(RGX_L)) {
		pushKey(Common::KEYCODE_1, '1', Common::KBD_ALT, true);
		pushKey(Common::KEYCODE_1, '1', Common::KBD_ALT, false);
	}
	if (PRESSED(RGX_R)) {
		pushKey(Common::KEYCODE_1, '1', Common::KBD_CTRL, true);
		pushKey(Common::KEYCODE_1, '1', Common::KBD_CTRL, false);
	}

#undef PRESSED
#undef RELEASED

	_prevButtons = b;
}

bool OSystem_RG::pollEvent(Common::Event &event) {
	pollTimers();
	readInput();
	if (_eventQueue.empty())
		return false;
	event = _eventQueue.pop();
	return true;
}

} // namespace RgBackend
