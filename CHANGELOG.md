# Changelog

## 0.1.0 (unreleased, waiting for its first camera test)

- Crop Mood engine (Bilal Fakhouri, `crop_rec_4k_mlv_snd` @ 5f0cda84) pinned and building with GCC 13 / Python 3.
- Upstream engine (`magiclantern_simplified` dev @ 142472e9) as an alternative build.
- `moth_ui` module:
  - full-screen recording-profile panel with iris open/close, gliding selection, sliding detail pane,
    animated sensor-crop diagram and data-rate gauge
  - 11 built-in profiles (3K, 2.8K, 2.5K, 1440p, 5.2K / 4.4K anamorphic, 1080p full sensor, 46.8p, H.264)
    and `ML/SETTINGS/PROFILES.TXT` for your own
  - learns compressed bit/px per bit depth from every clip
  - 11 themes, switched live with LEFT / RIGHT
  - REC tally frame, LiveView budget item
- Card zip enables crop_rec, mlv_lite, mlv_snd, mlv_play, sd_uhs/sd_clock, file_man and moth_ui by default.
