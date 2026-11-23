# Changelog

## 0.2.1 (24 Nov 2026, from the camera's log)

- The rear dial never reaches ML in LiveView: in manual movie exposure Canon uses it for the
  shutter (the log shows only 0x54 + 0x69 per click; Bilal says the same for Crop Mood). The
  quick bar now works with the keys the EOS M gives: SET moves to the next setting, UP / DOWN
  change it, the dial stays Canon's shutter and the bar follows it at 10 Hz.
- UP / DOWN on PROFILE open the profile list; closing it, or applying, goes back to the bar.
- Tap a tile to select it, tap PROFILE again for the list -- experimental: the touch position
  is read from Canon's RAM (0x4D868, from ML's EOS M notes), used only when plausible, logged.

## 0.2.0 (24 Nov 2026)

- **Quick bar**: INFO now opens a Canon-style strip over LiveView with shutter (and its angle),
  aperture, ISO, white balance and the profile. UP / DOWN choose, the rear dial changes the value
  using ML's own steppers (Canon's steps, the lens range), SET on PROFILE opens the profile list.
  Explains auto exposure and manual lenses instead of silently doing nothing. Crop Mood hides
  Canon's own controls in x5 zoom; this puts them back. Movie > Mothlight > "INFO opens" picks the
  bar, the profiles or nothing.
- No black flash on DOWN / SET: while the panel or bar is up, ML's gui_state holds a non-idle
  value so its EOS M long-press redraw does not run; REC / PLAY / two-finger tap hand it back.

## 0.1.2 (second camera test, 24 Nov 2026)

From the camera's own event log (ML/LOGS/MOTHLOG.TXT):
- RIGHT never reaches ML in LiveView on the EOS M (Canon acts on it; only its release and
  "info changed" arrive), and LEFT showed up as half-shutter. Themes are no longer on
  LEFT/RIGHT: choose them in Movie > Mothlight > Theme. The hint bar lists only keys that work.
- DOWN and SET go through ML's long-press check, which redraws the screen: the panel now
  repaints within a frame after any key.
- Applying runs in a helper task, so the panel stays on screen while LiveView restarts;
  INFO / MENU can close it mid-apply.
- BURST everywhere was the stock SD clock (the camera measured 37.5 MB/s): the panel says so
  and points to Debug > SD Overclock.
- the event log keeps listening 3 s after the panel closes; times print (no %u in Canon's printf)

## 0.1.1 (first camera test, 23 Nov 2026)

Found on the camera:
- the panel closed on every D-pad press (the EOS M's release codes reach modules as key 0)
- the top and bottom bars went black when navigating, with a black flash (Canon redrew on the releases)

Found by `sim/camsim`, the new harness that runs the real camera code against a fake EOS M:
- REC and PLAY were swallowed while the panel was open
- applying a profile could close the panel when Crop Mood restarted LiveView
- a profile could be half applied when crop_rec was not loaded
- the 2 s safety watchdog left ML's overlays off and leaked 338 KB
- a second INFO during start-up made the panel reopen by itself after the next close
- the panel stayed up after switching to photo mode or the display turning off
- long profile names ran under the verdict; the H.264 description ran off the screen

Easier to use:
- each profile says OK / EDGE / BURST in words, not only by colour
- the header shows the profile in use and the card; the record-time lines are larger
- the list wraps at both ends; INFO closes (it is what opens the panel)
- after a result, UP/DOWN keep browsing instead of closing the panel
- INFO in photo mode says once that Mothlight works in movie mode

## 0.1.0

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
