# Stock Himax WE2 Firmware — SenseCAP Watcher (recovery images)

Official factory Himax WiseEye2 co-processor firmware for the Seeed Studio
SenseCAP Watcher, downloaded from Seeed's GitHub releases. Keep these as
gold-standard recovery images: if a custom WE2 build bricks the Himax during
development, reflash one of these to restore factory behavior.

**Do NOT modify these files.** They are byte-identical to Seeed's release assets.

## Primary recovery image (newest)

- **File:** `sensecap_watcher_20250102.img`
- **Release:** `20250102` (published 2025-01-02) — latest Watcher WE2 release
- **Source URL:** https://github.com/Seeed-Studio/sscma-example-we2/releases/download/20250102/sensecap_watcher_20250102.img
- **Release page:** https://github.com/Seeed-Studio/sscma-example-we2/releases/tag/20250102
- **Size:** 573,440 bytes (matches release asset size exactly)
- **SHA-256:** `65e0606c9739cf88816a89ca5e34ccb7aa44d230bdd537a995066f999c02cf97`
- **Magic:** `ckBS` (bytes `63 6b 42 53`) — verified, matches our fork's image format

## Fallback recovery image (older)

- **File:** `sensecap_watcher_20241106.img`
- **Release:** `20241106` (published 2024-11-06)
- **Source URL:** https://github.com/Seeed-Studio/sscma-example-we2/releases/download/20241106/sensecap_watcher_20241106.img
- **Release page:** https://github.com/Seeed-Studio/sscma-example-we2/releases/tag/20241106
- **Size:** 548,864 bytes (matches release asset size exactly)
- **SHA-256:** `12176ebec49abb37a67508ba6267af5185cbfb09884bc5611cf7648df128d3b0`
- **Magic:** `ckBS` — verified

## Notes

- These are the **Himax WE2 co-processor** images, not the ESP32-S3 host firmware.
  The Watcher ships two firmwares (ESP32 + Himax); only the Himax side lives here.
- Both images were downloaded 2026-09-28 and byte-size-checked against the
  GitHub release asset metadata before hashing.
- The same release also ships `grove_vision_ai_v2_*.img` (Grove Vision AI V2
  board) and `sensecap_a1102_*.img` (A1102) — **do not flash those to the
  Watcher**; only the `sensecap_watcher_*` images apply.
- Our fork's clean rebuild (`../watcher_sscma_v0.1.img`, unmodified 1.0.x
  source) shares the same `ckBS` image format and remains a valid secondary
  fallback, but these Seeed-built images are the authoritative factory restore.
- Reflash method (when needed, with John's approval): xmodem over the Himax
  UART, e.g. `python3 xmodem/xmodem_send.py --port=/dev/ttyACM0
  --baudrate=921600 --file=sensecap_watcher_20250102.img` per the
  sscma-example-we2 docs. Never flash without explicit approval.
