// retro-go shim for the Arduino calls the game makes (after the upstream
// emulator/Arduino.h, with retro-go behind it instead of Raylib)
#ifndef Arduino_h
#define Arduino_h

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <algorithm>

typedef uint8_t byte;
typedef bool boolean;
#define String std::string

#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2
#define HIGH 0x1
#define LOW 0x0
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif

unsigned long millis();
void delay(unsigned long ms);
void randomSeed(long seed);
int random(int max);
int random(int min, int max);
int analogRead(uint8_t pin);
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);       // LOW while the button is held (pull-ups)

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif
#define abs(x) ((x)>0?(x):-(x))
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))

class SerialMock {
public:
    void begin(long) {}
    void println(const char *s) { printf("%s\n", s); }
    void println(String s) { printf("%s\n", s.c_str()); }
    void println(long n) { printf("%ld\n", n); }
    void println(unsigned long n) { printf("%lu\n", n); }
    void println(int n) { printf("%d\n", n); }
    void println(float n) { printf("%f\n", n); }
    void print(long n) { printf("%ld", n); }
    void print(unsigned long n) { printf("%lu", n); }
    void print(const char *s) { printf("%s", s); }
    void print(String s) { printf("%s", s.c_str()); }
    void print(int n) { printf("%d", n); }
    void print(float n) { printf("%f", n); }
};
extern SerialMock Serial;

#define PROGMEM
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))

class ESPMock {
public:
    uint32_t getPsramSize();
    uint32_t getFreePsram();
};
extern ESPMock ESP;

// The retro-go side (retro-extra/main/main_arcade3d.cpp)
extern "C" {
void a3d_present(uint16_t *const *rows, int width, int height);  // one finished frame, row by row
uint32_t a3d_buttons(void);                                      // bit 0 left, bit 1 right, bit 2 brake, bit 3 gas
void *a3d_alloc(size_t bytes);                                   // sprite memory (PSRAM)
void *a3d_alloc_fast(size_t bytes);                              // internal RAM if there is room, else PSRAM
int64_t a3d_micros(void);                                        // microseconds, for timing
}
#define A3D_BTN_LEFT  1
#define A3D_BTN_RIGHT 2
#define A3D_BTN_BRAKE 4
#define A3D_BTN_GAS   8

// the game's own entry points (src/car_game.cpp, the Arduino sketch)
void setup();
void loop();

#endif
