# Testing

## On a PC, before anything goes on a card

```
make -C sim test          # unit tests, key routing, then camsim
```

`sim/camsim` compiles the real `modules/moth_ui/moth_ui.c` against a fake EOS M and checks, after
every frame the module draws, that the screen equals the panel, that navigation never makes Canon
redraw (the black flash), that closing restores Canon's display, Global Draw and the memory, and that
nothing leaks. The fake camera is pessimistic on purpose: Canon redraws its LiveView screen on every
button it receives, Crop Mood restarts LiveView while applying, buttons arrive the way ML's
`module.c` delivers them (D-pad releases as key 0), and the module's task can stall.

29 scenarios (REC / PLAY / menu / mode switch / display off with the panel open, applying with
modules missing, a stalled task, out of memory, double INFO, corrupt config, every profile in every
theme with nothing clipped ...) and a randomised soak:

```
for s in $(seq 1 48); do SEED=$s ONLY=soak sim/camsim/camsim work/cropmood/data/fonts; done
```

It runs under AddressSanitizer and UBSan. A bug found on the camera gets a scenario that fails
without the fix.

## Camera test checklist

Run before every release, on a real EOS M 2.0.2, with a fresh card. Note the version, engine and card model
in the release notes.

### Boot

- [ ] Camera starts with the card; ML's boot message appears; no "card LED stays on"
- [ ] *Debug > Free memory* looks normal; *Modules* tab shows moth_ui loaded, no red errors
- [ ] Holding SET at power on starts without ML

### Panel

- [ ] Movie mode, LiveView: INFO opens the panel; the iris animation is smooth; ML overlays are gone
- [ ] UP/DOWN and the wheel move the highlight, wrapping at both ends; no black flash; the top and bottom bars stay
- [ ] LEFT/RIGHT change theme live; the theme is remembered after a restart
- [ ] MENU, INFO and half-shutter close it; ML overlays and Canon's display come back
- [ ] REC with the panel open closes it and starts recording; PLAY opens playback
- [ ] INFO in photo mode: Canon's INFO works; the first press says "Mothlight works in movie mode"
- [ ] After applying, pressing UP/DOWN dismisses the result and keeps the panel open
- [ ] The ML menu (two-finger tap) still opens normally when the panel is closed

### Quick bar (INFO, default)

- [ ] INFO in movie mode opens the bar at the bottom; LiveView stays visible above it
- [ ] SET moves between SHUTTER, APERTURE, ISO, WB, FOCUS, PROFILE; UP / DOWN change the value
- [ ] Battery at the top-left, like Canon's; red when low; gone when the bar closes
- [ ] The rear dial changes the shutter (Canon) and the bar follows at once
- [ ] Tapping a tile selects it; tapping PROFILE again opens the list (check MOTHLOG.TXT "touch" lines)
- [ ] Shutter shows its angle (green near 180); aperture shows the lens range
- [ ] With Canon's Movie exposure on Auto, SHUTTER / APERTURE say "by Canon" and UP / DOWN explain why
- [ ] A manual (adapted) lens: APERTURE says so
- [ ] UP / DOWN on PROFILE open the profile list; INFO there goes back to the bar; REC starts RAW
- [ ] No black flash on DOWN or SET
- [ ] Second time the bar opens: the most serious warning shows above it (if any)

### Focus punch-in (FOCUS tile)

- [ ] UP: x10 within about a second, the bar stays up; DOWN: back to where it was (x1 or x5)
- [ ] INFO at x10: the bar closes and the zoom goes back
- [ ] REC at x10: the zoom goes back first, then RAW records (not at x10)
- [ ] Zoomed with Canon's own zoom first: closing the bar leaves that zoom alone

### Focus in Crop Mood presets (AF lens)

- [ ] Tap the picture: x10 at that spot within ~1 s, Canon's own focus box, it focuses, ~1 s later back to x5
- [ ] The framing after it is the same as before (nothing shifted)
- [ ] Half-press: x10 and focus while held; release: back to x5
- [ ] No ML overlay flash on half-press; no Canon icons flashing after closing the bar in x5
- [ ] REC while at x10: back to x5 first, then RAW records
- [ ] MF lens / H.264 profile: tap and half-press behave as Canon's
- [ ] Settings > Tap / half-press focus: Canon's (x5) gives the old behaviour back
- [ ] ML/LOGS/MOTHLOG.TXT has "focus: step" lines (send it if the spot is off)

### Recording preview

- [ ] Settings > Screen while recording RAW: "ML framing" removes the vertical lines on screen (clip unaffected)

### Magic Lantern's menu (two-finger tap)

- [ ] Mothlight is the first tab (an M), with Settings, Profiles, Theme ... in it
- [ ] Graphite header and footer, orange underline under the current tab, orange marker on the selected row
- [ ] Editing a value: orange highlight; scroll bar orange; submenus with an orange rule under the title

### Settings (Mothlight tab > Settings)

- [ ] Opens full screen; UP / DOWN scroll; the help line explains the selected row
- [ ] Theme: SET switches it right there; Animations, INFO button, Recording frame change
- [ ] SD card speed: SET steps Off / 160 / 192 / 240 and says to restart; ML's Debug > SD Overclock agrees
- [ ] Sound, Small hacks, Zebras, Focus peaking, False color match ML's own menu after changing
- [ ] ML overlays Off: after closing, ML's overlays stay off (and On brings them back)
- [ ] Warnings at the top (try Movie exposure Auto): the selection stays on its row when one goes away
- [ ] Save Crop Mood's settings: set Crop Mood by hand in ML's menu, save, the profile appears as "My ..."
      in the list and in ML/SETTINGS/PROFILES.TXT; saving again says "Already saved"
- [ ] INFO goes back to the bar (or closes, if opened from ML's menu)

### Welcome (first start after installing)

- [ ] A few seconds after the first start in movie mode, the Welcome page opens by itself
- [ ] Setup check: Crop Mood and RAW recording "Ready", SD speed and Movie exposure as set
- [ ] "Start shooting" closes it and it does not come back after a restart
- [ ] Settings > About > Welcome and setup check shows it again; INFO returns to Settings

### Applying

For each built-in profile:
- [ ] SET applies it; LiveView refreshes (zoom x5); the toast shows the RAW size
- [ ] *Movie > Crop mood* and *Movie > RAW video* show the expected settings
- [ ] A 10 s recording plays back in mlv_play and opens in MLV App

### Recording

- [ ] Tally frame appears while recording, and disappears after
- [ ] After a clip longer than 3 s: "Learned: N-bit = x.xx bit/px"; *Movie > Mothlight > Compression* shows it
- [ ] 2.5K Cinema records continuously for 2 minutes at 160 MHz (or note the card)
- [ ] 3K Scope records a burst; note how many seconds

### Robustness

- [ ] Open the panel, pull the lens: no hang
- [ ] Open the panel, let the camera dim/power-save: no hang, buttons work after
- [ ] PROFILES.TXT with a bad line: the other lines load, the console names the bad line
