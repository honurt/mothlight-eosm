# RAW video on the EOS M: what to expect

## Why the card is the limit

The EOS M's SD interface writes about **38-40 MB/s** stock. Crop Mood's SD overclock raises the clock to 160 MHz,
where a good UHS-I card writes about **65-70 MB/s** (192 and 240 MHz work on fewer cards; when a card can't take
it, Canon falls back to a slow safe mode). RAW video needs:

```
MB/s = width x height x fps x (compressed bits per pixel) / 8 / 1,000,000
```

Lossless compression (LJ92) gives roughly 6.0 bit/px at 10-bit, 6.8 at 11-bit, 7.6 at 12-bit and 9.0 at 14-bit
for typical scenes at ISO 100-800. Noise and fine detail make it worse. **Mothlight measures each clip you
record and uses your real numbers from then on** (*Movie > Mothlight > Compression*).

## The built-in profiles

Starting estimates. *Stock* is about 38 MB/s, *160 MHz* about 65 MB/s:

| Profile | RAW frame | MB/s | stock | 160 MHz |
|---|---|---|---|---|
| 3K Scope | 3072x1308 10-bit 23.976 | 72.2 | burst | burst |
| 3K Wide 2.39 | 3072x1286 10-bit 23.976 | 71.0 | burst | burst |
| 2.8K Scope | 2880x1226 10-bit 23.976 | 63.4 | burst | edge |
| 2.5K Cinema | 2520x1072 10-bit 23.976 | 48.5 | burst | continuous |
| 2.5K 12-bit | 2520x1054 12-bit 23.976 | 60.4 | burst | edge |
| 1440p 16:9 | 2560x1438 10-bit 23.976 | 66.1 | burst | edge |
| 5.2K Anamorphic | 1736x2178 1x3 10-bit 23.976 | 67.9 | burst | edge |
| 4.4K Anamorphic | 1472x1846 1x3 10-bit 23.976 | 48.8 | burst | continuous |
| 1080p Full Sensor | 1736x1160 12-bit 23.976 | 45.8 | burst | continuous |
| Slow-mo 46.8p | 1736x976 10-bit 46.8 | 59.4 | burst | edge |

- **continuous**: needs at most 85% of the card's speed. Records until the card is full.
- **edge**: 85-105%. Simple scenes record continuously, busy or noisy ones may stop.
- **burst**: the camera's RAM buffer fills and recording stops after a while (for 3K, typically tens of seconds).

## Getting the most out of 3K

- SD Overclock **160 MHz**, Image quality **RAW**, a SanDisk Extreme PRO class card.
- **10-bit**, and keep ISO low (100-400): less noise compresses better.
- **2.39:1** is a little lighter than 2.35:1 (fewer lines).
- Turn off what you don't need while recording: histogram and waveform cost CPU, so the recorder falls behind sooner.
- Watch the panel: if the gauge's fill passes the tick, expect a burst.

## Crop and lenses

1:1 modes read the sensor pixel for pixel from a smaller area, so they crop in. The panel's sensor diagram shows
how much, and what your lens becomes:

| Mode | Width on sensor | vs full frame | a 50mm looks like |
|---|---|---|---|
| 3K | 13.2 mm | 2.72x | ~135mm |
| 2.8K | 12.4 mm | 2.90x | ~145mm |
| 2.5K | 10.8 mm | 3.30x | ~165mm |
| 1x3 / 3x3 | full width | 1.61x | ~80mm |

3K and 2.8K are close to **Super 16**: C-mount and Super 16 cine lenses cover them, and fast wide APS-C primes
become normal lenses.

## After shooting

Open `.MLV` files in **MLV App** (free, macOS / Windows / Linux). It reads focus-pixel maps for Crop Mood's modes,
de-squeezes 1x3 footage, and exports ProRes or CinemaDNG.
