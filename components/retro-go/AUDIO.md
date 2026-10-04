# Audio

Audio goes through *sinks*. Each sink is a driver in `components/retro-go/drivers/audio/` that implements `rg_audio_driver_t`. Which sinks exist is set per target in `config.h`:

```c
#define RG_AUDIO_USE_INT_DAC  0   // 0 = off, 1 = GPIO25, 2 = GPIO26, 3 = both (classic ESP32 only)
#define RG_AUDIO_USE_EXT_DAC  0   // 1 = I2S amp/DAC (e.g. MAX98357A)
#define RG_AUDIO_USE_USB      1   // 1 = USB audio dongle on the native USB (OTG) port (ESP32-S2/S3)
```

All three can be enabled together. The sink is chosen in the options menu and saved in `global.json`. If nothing is saved, the first enabled non-dummy sink is used (I2S before USB). Existing I2S targets are unaffected: with `RG_AUDIO_USE_USB` unset or 0, `usb.c` compiles to nothing.

## Sinks

| Sink | Driver | Needs |
|---|---|---|
| Speaker / Ext DAC | `i2s.c` | I2S pins, optional amp-enable GPIO |
| USB DAC | `usb.c` | ESP32-S3 OTG port, USB-C/3.5 mm dongle |
| Buzzer | `buzzer.c` | a buzzer pin |
| SDL2 | `sdl2.c` | desktop builds |
| Dummy | `dummy.c` | nothing |

### Dummy
Always present as the first entry. It outputs no sound but sleeps for the duration of each submitted buffer, so emulators keep running at the correct speed (audio is what paces them). It is also the fallback when a sink fails to initialise, so the system still boots.

## USB DAC

The ESP32-S3 acts as USB *host*. Any plain USB Audio Class 1.0 playback device works (nearly all 3.5 mm USB-C dongles): 2 channels, 16 bit, 48 or 44.1 kHz. UAC 2.0 devices are rejected.

- Plug the dongle in before or after boot; hot-plug is supported. With no dongle, it behaves like Dummy.
- The emulator rate (e.g. 32 kHz) is resampled to the dongle's rate (linear interpolation). The dongle's clock paces the emulator.
- On connect the driver unmutes the dongle and sets its hardware volume to max; volume is then applied in software.
- The USB stack stays installed once started (it cannot be reinstalled cleanly while a device is attached).
- The USB port is used by the host, so it can't be used for flashing or serial at the same time. Use the UART port for that.
- Requires ESP-IDF 5.x. The component's `CMakeLists.txt` already lists the IDF `usb` component; on chips without USB OTG it is empty, so other targets still build.

### Debugging
- If a dongle is refused, the log shows the reason and a hex dump of its descriptors.
- Debug builds print a `[usbaudio] streaming:` line every 5 s:
  - `underruns` rising means the emulator is running below full speed.
  - `peak in 0` means the emulator is sending silence (check the emulator's own sound option).
  - `peak in` > 0 with `peak out 0` means the driver is muting or dropping the audio.
  - `usb err` / `bad packets` > 0 means a USB transfer problem.

## Adding a sink
Implement `rg_audio_driver_t` (`init`, `deinit`, `submit` required), add the extern and a `sinks[]` entry guarded by a config flag in `rg_audio.c`.
