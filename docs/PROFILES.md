# Your own recording profiles

Mothlight shows its built-in profiles until it finds `ML/SETTINGS/PROFILES.TXT` on the card. Then it shows
yours instead (up to 16).

One profile per line, six fields separated by `|`. Spaces around fields don't matter, case doesn't matter, and
`#` starts a comment. A line that can't be read is skipped, and the console says which one.

```
# name            | mode | preset   | ratio | fps    | bits
3K Scope          | 1:1  | 3K       | 2.35  | 23.976 | 10
Doc 2.5K          | 1:1  | 2.5K     | 2.39  | 25     | 12
Anamorphic        | 1x3  | highest  | 2.39  | 23.976 | 10
Slow motion       | 3x3  | hfr      | 2.39  | 23.976 | 10
Full sensor 1080p | 3x3  | 1080p    | 16:9  | 23.976 | 12
Canon             | off  |          |       |        |
```

| Field | Values |
|---|---|
| name | up to 23 characters |
| mode | `off` (Canon H.264), `1:1` (crop, no binning), `1x3` (anamorphic), `3x3` (binned) |
| preset, 1:1 | `2.5K`, `2.8K`, `3K`, `1440p`, `1280p`, `fullres` |
| preset, 1x3 | `highest`, `higher`, `medium` |
| preset, 3x3 | `hfr` (high frame rate), `1080p` |
| ratio | `16:9`, `2:1`, `2.20`, `2.35`, `2.39` |
| fps | `23.976`, `25`, `30` (some presets support only some: Crop Mood decides) |
| bits | `14`, `12`, `11`, `10` (lossless) |

## What applying a profile does

It sets, by name, exactly the menu entries you would set by hand:

- *Movie > RAW video*: ON, *Data format* lossless, *Resolution* widest, *Aspect ratio* tallest (so the frame
  follows Crop Mood's preset)
- *Movie > Crop mood*: mode, *Preset*, *Aspect ratio*, *Framerate*, *Bit-depth*
- Canon's x5 zoom (Crop Mood's modes run there on the EOS M)

You can still change anything by hand afterwards.
