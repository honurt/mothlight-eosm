# Roadmap

## 0.1 (this release): foundation

- [x] Pinned Crop Mood engine building with today's toolchain; upstream core as a second engine
- [x] moth_ui: full-screen profile panel, 11 themes, data-rate budget, sensor-crop diagram, learning, REC tally
- [x] PC simulators (themes, end-to-end session) and unit tests
- [ ] **First test on a real EOS M** (docs/TESTING.md), then tag v0.1.0

## 0.2: the recording screen

- A Mothlight LiveView HUD option: profile, budget, card time left, battery, cleaner than ML's bars for RAW
- Buffer-fill meter while recording (from MLV Lite's slot usage)
- Card benchmark from the panel, written straight into *Card write speed*
- Battery and temperature in the status bar

## 0.3: more of the camera, same UI

- Pages in the panel: **Rec** (profiles), **Look** (picture style, WB, ISO), **Assist** (zebras, peaking, false
  colour, magic zoom presets), **Card** (overclock, spanning, free space)
- Profile editor on the camera, saved to PROFILES.TXT
- Per-theme palettes, written to the overlay palette like M-Film does (more colours than ML's defaults)

## Engine work

- Port Crop Mood's EOS M presets to the upstream `magiclantern_simplified` core (Crop Mood's core changes:
  raw.c geometry, fps-engio, mlv_lite preview), so Mothlight can follow upstream
- QEMU boot test in CI once an EOS M ROM can be provided by the developer locally

## Ideas

- Shot log: every clip's profile, measured bit/px and duration in a CSV on the card
- LUT preview for H.264 proxies
