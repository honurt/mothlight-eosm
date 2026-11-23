# Camera test checklist

Run before every release, on a real EOS M 2.0.2, with a fresh card. Note the version, engine and card model
in the release notes.

## Boot

- [ ] Camera starts with the card; ML's boot message appears; no "card LED stays on"
- [ ] *Debug > Free memory* looks normal; *Modules* tab shows moth_ui loaded, no red errors
- [ ] Holding SET at power on starts without ML

## Panel

- [ ] Movie mode, LiveView: INFO opens the panel; the iris animation is smooth; ML overlays are gone
- [ ] UP/DOWN and the wheel move the highlight; the list scrolls past 7 profiles; the detail pane follows
- [ ] LEFT/RIGHT change theme live; the theme is remembered after a restart
- [ ] MENU, INFO and half-shutter close it; ML overlays and Canon's display come back
- [ ] Opening in photo mode or while recording does nothing
- [ ] The ML menu (two-finger tap) still opens normally when the panel is closed

## Applying

For each built-in profile:
- [ ] SET applies it; LiveView refreshes (zoom x5); the toast shows the RAW size
- [ ] *Movie > Crop mood* and *Movie > RAW video* show the expected settings
- [ ] A 10 s recording plays back in mlv_play and opens in MLV App

## Recording

- [ ] Tally frame appears while recording, and disappears after
- [ ] After a clip longer than 3 s: "Learned: N-bit = x.xx bit/px"; *Movie > Mothlight > Compression* shows it
- [ ] 2.5K Cinema records continuously for 2 minutes at 160 MHz (or note the card)
- [ ] 3K Scope records a burst; note how many seconds

## Robustness

- [ ] Open the panel, pull the lens: no hang
- [ ] Open the panel, let the camera dim/power-save: no hang, buttons work after
- [ ] PROFILES.TXT with a bad line: the other lines load, the console names the bad line
