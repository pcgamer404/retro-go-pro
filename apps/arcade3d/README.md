# Arcade 3D (standalone game port)

OutRun-style racing game (`davidmonterocrespo24/esp32s3-arcade-3d`, written for Arduino + TFT_eSPI), run on Retro-Go through the `components/arcade3d/shim` layer.

- No ROM needed. Register it in the launcher as a port/app named `arcade3d` (see MIGRATION.md), or build it as an **RG Ports** raw `.bin` (`build/arcade3d.bin`) and put it in your ports folder.
- Controls: Left/Right steer, A = gas, B = brake. MENU = Retro-Go menu (reset, screenshot).
- It renders a 320x240 RGB565 (little-endian) frame; the scaler handles other screens.
- Needs PSRAM (frame buffers are allocated there).
