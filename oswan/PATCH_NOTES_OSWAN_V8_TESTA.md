Oswan Retro-Go v8 (Test A)

Baseline: original oswan.zip (untouched otherwise).
Only change: components/oswan/WSRender.c, two lines (sprite Z-buffer rejection
disabled for Rockman EXE WS visibility). main/main.c, WSInput.c, WS.c and all
other files are byte-identical to the original.

Test: D-pad, A, B, Start, Select (X/Y toggle), Select+Start menu.
Build/runtime not verified in this environment.
