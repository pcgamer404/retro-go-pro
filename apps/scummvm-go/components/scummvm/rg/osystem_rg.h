/* ScummVM OSystem backend for Retro-Go (ESP32 / ESP32-S3).
 * Replaces the Odroid-Go backend from johannesbehr/scumm. GPL-2.0-or-later, like ScummVM. */
#ifndef PLATFORM_RG_H
#define PLATFORM_RG_H

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "common/scummsys.h"
#include "common/events.h"
#include "common/queue.h"
#include "common/str.h"
#include "backends/base-backend.h"
#include "backends/mutex/mutex.h"
#include "graphics/palette.h"
#include "graphics/surface.h"
#include "graphics/pixelformat.h"
#include "audio/mixer_intern.h"
#include "audio/mixer.h"

namespace RgBackend {

class OSystem_RG : public EventsBaseBackend, public PaletteManager {
public:
	OSystem_RG(const char *saveDir, const char *configFile);
	virtual ~OSystem_RG();

	virtual void initBackend();

	virtual bool hasFeature(OSystem::Feature f);
	virtual void setFeatureState(OSystem::Feature f, bool enable);
	virtual bool getFeatureState(OSystem::Feature f);

	virtual bool pollEvent(Common::Event &event);

	virtual uint32 getMillis(bool skipRecord = false);
	virtual void delayMillis(uint msecs);
	virtual void getTimeAndDate(TimeDate &t) const;

	virtual MutexRef createMutex();
	virtual void lockMutex(MutexRef mutex);
	virtual void unlockMutex(MutexRef mutex);
	virtual void deleteMutex(MutexRef mutex);

	virtual void logMessage(LogMessageType::Type type, const char *message);
	virtual Audio::Mixer *getMixer();
	virtual PaletteManager *getPaletteManager() { return this; }
	virtual Common::String getSystemLanguage() const;
	virtual Common::String getDefaultConfigFileName();
	virtual void quit();

	// Graphics (8-bit paletted only)
	int getDefaultGraphicsMode() const;
	bool setGraphicsMode(int mode);
	void resetGraphicsScale();
	int getGraphicsMode() const;
	inline Graphics::PixelFormat getScreenFormat() const { return _pfGame; }
	virtual const OSystem::GraphicsMode *getSupportedGraphicsModes() const;
	Common::List<Graphics::PixelFormat> getSupportedFormats() const {
		Common::List<Graphics::PixelFormat> list;
		list.push_back(Graphics::PixelFormat::createFormatCLUT8());
		return list;
	}
	void initSize(uint width, uint height, const Graphics::PixelFormat *format = NULL);
	int16 getHeight() { return _gameHeight; }
	int16 getWidth() { return _gameWidth; }
	void setPalette(const byte *colors, uint start, uint num);
	void grabPalette(byte *colors, uint start, uint num) const override;
	void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h);
	Graphics::Surface *lockScreen();
	void unlockScreen();
	void updateScreen();
	void setShakePos(int shakeOffset);
	void setFocusRectangle(const Common::Rect &rect);
	void clearFocusRectangle();
	void showOverlay();
	void hideOverlay();
	Graphics::PixelFormat getOverlayFormat() const;
	void clearOverlay();
	void grabOverlay(void *buf, int pitch);
	void copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h);
	virtual int16 getOverlayHeight();
	virtual int16 getOverlayWidth();
	bool showMouse(bool visible);
	void warpMouse(int x, int y);
	void setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY,
	                    uint32 keycolor, bool dontScale = false,
	                    const Graphics::PixelFormat *format = NULL);

	// called from the audio task
	void mixAudio(int16 *stereo, int frames);

private:
	void initGraphics();
	void initAudio();
	void pollTimers();
	void readInput();
	void pushMouse(Common::EventType type);
	void pushKey(Common::KeyCode code, uint16 ascii, byte flags, bool down);
	void drawCursor(byte *dst);

	Audio::MixerImpl *_mixer;
	bool _audioRunning;

	Common::String _saveDir, _configFile;

	// Graphics
	uint16 _gameWidth, _gameHeight;
	Graphics::PixelFormat _pfGame;
	byte _palette[3 * 256];
	byte *_gamePixels;
	Graphics::Surface _surface;
	bool _gameDirty;

	// Cursor
	byte *_cursorBuf;
	uint _cursorW, _cursorH;
	int _cursorHotX, _cursorHotY;
	uint32 _cursorKey;
	bool _cursorVisible;
	bool _cursorDirty;
	int _cursorX, _cursorY;
	float _fx, _fy, _holdTime;
	int64 _lastMoveUs;

	// Events / input
	Common::Queue<Common::Event> _eventQueue;
	uint32 _prevButtons;
	int64 _lastInputUs;

	// Timing
	int64 _startUs;
	int64 _lastPresentUs;
	int64 _waitedUs;
	bool _inTimers;
};

} // namespace RgBackend

#endif
