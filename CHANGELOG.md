# Changelog

## 0.4.0 (24 Nov 2026)

- **Tap to focus, the reliable way.** Crop Mood runs every preset in x5 and hides Canon's layer
  (the focus box with it); its author notes x5 autofocus is inaccurate and x10 is where it works,
  and a tap in x5 moves Canon's magnified area instead of a focus point. Now, in Crop Mood
  presets with an AF lens: tap a spot and Mothlight moves Canon's AF point there, zooms to x10
  with Canon's own focus box, focuses, holds a moment, re-centres and goes back to x5 (the
  framing is unchanged). Half-press does the same while held. Never while recording; MF lenses,
  H.264 and "Canon's (x5)" in Settings keep the old behaviour. Every step is logged.
- **No Canon icon flash after closing** in Crop Mood x5: Mothlight used to switch Canon's layer
  back on, which crop_rec then hid again. It now leaves it as Crop Mood wants.
- **Magic Lantern's menu in Mothlight's look** (engine patch 0002): graphite header and footer,
  orange tab underline, orange marker on the selected row, orange value highlight and scroll bar,
  and a **Mothlight tab first** (Settings, Profiles, Theme, INFO, focus, ...). This changes
  autoexec.bin. (The upstream engine keeps ML's look; the tab is added at the end there.)
- Quick bar: the MENU tile is gone; the **battery** sits top-left where Canon puts it.
- Settings: "Screen while recording RAW". The vertical lines seen while filming are only in
  Canon's live preview in Crop Mood (checked in a recorded clip: the RAW is clean); "ML framing"
  shows a clean, slower preview.
- Fixed before release by the simulator: REC during a focus job started RAW at x10 (MLV Lite saw
  LiveView idle; the job now holds ML's GUI state like the bar); a two-finger tap during a focus
  job looped forever re-sending itself; a lost half-press release could leave x10 on.
- Corrupt settings are repaired at start-up before ML's menu shows them.
- Sound effects: not added. ML's audio playback is compiled out on the EOS M, and on the 650D /
  700D / 100D (same processor) turning it on breaks the module menu or destabilises the camera.

## 0.3.0 (24 Nov 2026)

- **Settings page**, in Mothlight's look, from the quick bar's new MENU tile or Movie > Mothlight >
  Settings. The things you actually change on an EOS M, in plain words with a help line each:
  theme, animations, INFO button, recording frame; SD card speed (ML's overclock), the card speed
  the verdicts assume, sound with RAW, small hacks; ML overlays, zebras, focus peaking, false
  colour. ML's own menu stays one two-finger tap away for everything else.
- **Focus punch-in**: a FOCUS tile on the bar zooms to x10 (UP) and back (DOWN). Closing the bar
  zooms back; REC at x10 zooms back first, then records RAW.
- **Battery and warnings**: the MENU tile shows the battery and counts what could spoil a take:
  low battery, movie exposure on Auto, RAW off with a RAW profile, card almost full, SD overclock
  off when the profile needs it, sound on at a risky rate, a BURST profile. The worst one shows
  above the bar when it opens; all of them at the top of Settings.
- **Save my profile**: Settings > Save Crop Mood's settings as a profile reads what Crop Mood is set
  to and adds it to PROFILES.TXT as e.g. "My 2.5K 2.35 10-bit" (no duplicates, 16 at most).
- **Welcome page** on the first start after installing: how the buttons work and a setup check
  (Crop Mood, RAW recording, SD speed, movie exposure, sound). Again from Settings > About.
- INSTALL.TXT in the download: three steps, and why the installer screen says "Magic Lantern
  install" (ML-SETUP.FIR is ML's signed installer; Mothlight ships it untouched).
- The bar's seven tiles have widths that fit what they show; aperture range written compactly.

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
