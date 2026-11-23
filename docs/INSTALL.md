# Installing Mothlight on a Canon EOS M

Mothlight is Magic Lantern plus extras, so it installs exactly like Magic Lantern does.

## What you need

- The **original Canon EOS M** (2012). Not the M2, M3, M5, M6, M10, M50, M100 or M200: those are different cameras.
- **Canon firmware 2.0.2.** Check it in *Menu > Setup (wrench) > Firmware Ver.* If yours is older, update with
  Canon's official 2.0.2 first; Mothlight refuses to load on any other version.
- A fast **UHS-I SD card** (SanDisk Extreme / Extreme PRO class, 64-128 GB). Cards that look fast on paper can
  still refuse to overclock: Samsung EVO Plus, for one, is reported to stay near 38 MB/s on the EOS M.
- A charged battery. Don't let the camera power off during the install.

## 1. Back up the card

Formatting erases everything on the card. Copy your photos and videos to a computer first.

## 2. Format the card in the camera

*Menu > Setup > Format card.* Formatting in the camera gives the file system Canon expects.

## 3. Copy Mothlight to the card

Unzip `Mothlight-<version>-cropmood-<date>.zip` and copy **everything** to the **root** of the card:

```
autoexec.bin
ML-SETUP.FIR
ML/            (modules, fonts, settings, scripts...)
```

Eject the card properly, then put it in the camera.

## 4. Set the boot flag (once per camera)

1. Turn the camera on in **photo mode**. If the firmware entry is greyed out, switch to a creative shooting
   mode (P, Tv, Av or M) first.
2. *Menu > Setup > Firmware Ver. 2.0.2*, then start the update.
3. A plain screen titled **"Magic Lantern install"** appears. That is expected: it is `ML-SETUP.FIR`, the
   Magic Lantern installer Mothlight is built on (not a Canon firmware). It sets the camera's **boot flag** and
   makes the card bootable. When it says *Please restart your camera*, switch off and on.

   Why it doesn't look like Mothlight: that file is encrypted for Canon's bootloader and only Magic Lantern's
   maintainers can build it. It is also the code that writes the boot flag, so Mothlight ships it untouched
   rather than risk the one step that could hurt the camera. You only see it once per camera.

The boot flag tells the camera to look for `autoexec.bin` on a *bootable* card at power on. A normal card (not
made bootable by the installer) starts the camera as usual. **A bootable card without `autoexec.bin` does not:**
the camera hangs until you take the battery out. So never delete `autoexec.bin` from a Mothlight card and keep
using it; format the card in the camera instead, which also removes the bootable mark.

If the camera ever locks up, take the battery out at once (a stuck processor gets hot).

## 5. First start

- Magic Lantern's menu: **tap the screen with two fingers**. The *Modules* tab shows what loads; Mothlight turns on crop_rec, mlv_lite, mlv_snd, mlv_play,
  sd_uhs, file_man and moth_ui.
- Switch to **movie mode**. After a few seconds Mothlight's **Welcome** page opens: how the buttons work, and a
  setup check (Crop Mood, RAW recording, SD speed, movie exposure, sound). *Start shooting* closes it; it comes
  back from *Settings > About > Welcome and setup check*.
- Press **INFO** for the quick bar. Its **MENU** tile opens Mothlight's settings.
- In Mothlight's settings, set *SD card speed (overclock)* to **160 MHz** and restart if you want 3K / 2.8K.
  To be exact about your card, measure it in ML's *Debug > Benchmarks > Card benchmark* and enter the result
  under *Card speed for OK / BURST*.

## Keep Canon one button away

Canon's firmware is never replaced. Hold **SET** at power-on to start plain Canon, or make the card start as
Canon by default and hold SET for Mothlight. Also copy `ML/LOGS/ROM0.BIN` and `ROM1.BIN` (your camera's own
firmware backup, written on the first start) to your computer. Details: [SWITCHING.md](SWITCHING.md).

## Picture quality setting

Crop Mood records longer with Canon's image quality set to **RAW** (*Menu > Shooting > Image quality*). It
asks for this itself if it isn't set.

## Uninstall

- **Stop using it:** use a normal card, formatted in the camera. The camera starts as stock.
- **Remove the boot flag:** with a Mothlight card in, run *Firmware Ver. 2.0.2* again; the installer offers
  to remove it.
- **Start once without Magic Lantern:** hold **SET** while turning the camera on.

## If something goes wrong

- **Black screen, or the card LED stays on at power on:** take the battery out at once. Start with **SET** held
  to skip Magic Lantern, or use a normal card. Then report it on GitHub with the Mothlight version and engine.
- **"Load crop_rec / mlv_lite" message in the panel:** turn those modules on in the Modules tab, restart.
- **Recording stops after a few seconds:** the card can't keep up. Pick a profile marked *continuous*, or
  lower the bit depth.
