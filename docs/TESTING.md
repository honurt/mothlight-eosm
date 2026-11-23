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
- [ ] UP / DOWN move between SHUTTER, APERTURE, ISO, WB, PROFILE; the rear dial changes the value
- [ ] Shutter shows its angle (green near 180); aperture shows the lens range
- [ ] With Canon's Movie exposure on Auto, SHUTTER / APERTURE say "auto (Canon)" and the dial explains why
- [ ] A manual (adapted) lens: APERTURE says so
- [ ] SET (or the dial) on PROFILE opens the profile list; INFO closes; REC starts RAW
- [ ] No black flash on DOWN or SET

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
