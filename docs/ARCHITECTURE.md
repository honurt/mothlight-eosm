# How Mothlight is built

```
engines.lock          pinned engine commits (reproducible builds)
patches/<engine>/     our changes to an engine, applied by scripts/fetch.sh
modules/moth_ui/      the Mothlight UI module (this project's code)
card/                 files added to the card zip (module enable flags, readme)
sim/                  PC simulators: theme gallery, end-to-end session, unit tests
art/                  logo, monograms, previews (CC BY-NC-SA)
scripts/              setup, fetch, build
```

## Engines

Mothlight doesn't fork Magic Lantern's code. It pins an engine and keeps its own changes as patches, so moving
to a newer engine means changing one line in `engines.lock`, rebuilding, and testing on a camera.

| Engine | Source | Use |
|---|---|---|
| `cropmood` (default) | Bilal Fakhouri's Crop Mood, `crop_rec_4k_mlv_snd` | 2.5K-3K, 1x3, 3x3 HFR, real-time preview, SD overclock |
| `upstream` | `reticulatedpines/magiclantern_simplified` `dev` | newest core and build system; basic crop modes only |

Crop Mood's 2023-era build system needs four small fixes to run with today's tools
(`patches/cropmood/0001`): Python 3 module scripts, a GNU Make 4.3 escaping change, `-fcommon` for GCC 10+, and
bash as the make shell. Bringing Crop Mood's EOS M presets to the upstream core is the main item in
[ROADMAP.md](../ROADMAP.md).

## The moth_ui module

| File | What | Builds on a PC |
|---|---|---|
| `moth_ui.c` | camera glue: task, keys, menus, applying profiles, tally, learning | no |
| `panel.c` | the full-screen panel: layout, animation, dirty-region drawing | yes |
| `themes.c` | the 11 themes (palette indices and shapes) | yes |
| `ui_gfx.c` | spans, rounded rects, bars, dither, icons into an 8-bit canvas | yes |
| `ui_text.c` | transparent text from ML's own RBF fonts | yes |
| `budget.c` | data-rate model and learning | yes |
| `profiles.c` | built-in profiles, PROFILES.TXT parser, EOS M frame sizes | yes |

It talks to Crop Mood and MLV Lite only through **named menu entries** (`menu_set_value_from_script`, the
same calls as Lua). It needs no core patches, and keeps working when those modules change internally.

### Performance rules

The EOS M has one ARM926 core at about 400 MHz with no FPU, and while recording RAW it is busy compressing and
writing. So the module:

- runs one low-priority task, sleeping 100 ms in LiveView and 250 ms while recording;
- **never animates while recording**. The tally is four `bmp_fill`s every 250 ms;
- uses integer and fixed-point maths only (easing curves are 8.8 fixed point);
- draws the panel into its own buffer (338 KB, allocated only while open) and copies **only rows that changed**
  to the screen. In the simulated session 2 frames in 3 copy nothing; the panel averages 66 KB copied
  per frame (a full screen is 338 KB);
- reads menu values every 2 s, only in LiveView, and never while recording;
- starts after boot as a module, so it adds nothing to the ~538 KB ML loads at power on (the EOS M stops
  booting at about 727 KB; `scripts/build.sh` checks this every build).

### Safety rules

- Every canvas has hard bounds: no clip rectangle can reach past the memory behind it.
- If the panel's task stops drawing for 2 s, the key handler hands the buttons back to the camera.
- Recording, the ML menu, or leaving LiveView close the panel at once.
- The module loads only on EOS M 2.0.2 (`is_camera` check).

## Simulators

`make -C sim test` runs the unit tests. `make -C sim preview` renders every theme, and
`make -C sim e2e-video` plays a whole session (LiveView, INFO, browse, theme, apply, record, learn) at the
camera's 720 x 480. Both use the real panel code and ML's real fonts. Around the panel, the LiveView picture
and ML's own status bars are stand-ins.

What only a camera can tell: timing under load, Canon's own redraws, and Crop Mood's real frame sizes. Test on
the EOS M before every release.

## Releasing

1. Bump `VERSION`, update `CHANGELOG.md`.
2. `scripts/build.sh`, then install the zip on the camera and run through *docs/TESTING.md*.
3. Tag `vX.Y.Z`. CI attaches the zip to the GitHub release.
