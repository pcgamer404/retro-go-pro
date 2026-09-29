Oswan v3 - MegaMan sprite visibility test

Target: Rockman EXE WS / MegaMan Battle Network WS.

Change: the sprite-vs-FG rejection was disabled. The existing renderer was rejecting sprite pixels whenever ZBuf was set and SPR_LAYR was clear. This build lets sprites render over the FG layer so we can verify whether that priority path is the cause of MegaMan's invisible sprite.

This is intentionally a targeted graphics test, not yet claimed as hardware-accurate. If MegaMan appears, the next revision should implement the correct priority behavior rather than leaving sprites globally on top.
