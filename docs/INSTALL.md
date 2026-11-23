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
3. The Magic Lantern installer starts (it is `ML-SETUP.FIR`, not a Canon firmware). It sets the camera's
   **boot flag** and makes the card bootable. Follow the screen, then restart the camera.

The boot flag tells the camera to look for `autoexec.bin` on a *bootable* card at power on. A normal card (not
made bootable by the installer) starts the camera as usual. **A bootable card without `autoexec.bin` does not:**
the camera hangs until you take the battery out. So never delete `autoexec.bin` from a Mothlight card and keep
using it; format the card in the camera instead, which also removes the bootable mark.

If the camera ever locks up, take the battery out at once (a stuck processor gets hot).

## 5. First start

- Magic Lantern's menu: **tap the screen with two fingers**. The *Modules* tab shows what loads; Mothlight turns on crop_rec, mlv_lite, mlv_snd, mlv_play,
  sd_uhs, file_man and moth_ui.
- Switch to **movie mode** and press **INFO**: the Mothlight panel opens.
- Set *Debug > SD Overclock* to **160MHz** and restart if you want 3K / 2.8K. Then measure the card in
  *Debug > Benchmarks > Card benchmark* and enter the result under *Movie > Mothlight > Card write speed*.

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
