# Canon and Mothlight on one card

**Mothlight never replaces Canon's firmware.** Canon 2.0.2 stays in the camera's own memory, untouched; Mothlight
is a program on the SD card that the camera runs *next to* Canon's firmware when it starts. Take the card out
and the camera is stock. So "keeping the original firmware" needs no extra step: it is always there.

One card can start either way.

## Choose at power-on: hold SET

| Startup mode | Camera on | Camera on with **SET** held |
|---|---|---|
| **Mothlight first** (default) | Mothlight | plain Canon |
| **Canon first** | plain Canon | Mothlight |

The mode is the file `ML/SETTINGS/REQUIRE.SET` on the card: present means *Canon first*.

Change it:

- **On the camera:** ML menu (two-finger tap) > *Prefs* > *Config options* > *SET at startup*:
  "Bypass loading ML" = Mothlight first, "Required to load ML" = Canon first.
- **On a computer**, with the card mounted:

  ```sh
  scripts/card-mode.sh /Volumes/EOS_DIGITAL canon      # start as Canon, hold SET for Mothlight
  scripts/card-mode.sh /Volumes/EOS_DIGITAL mothlight  # start as Mothlight, hold SET for Canon
  scripts/card-mode.sh /Volumes/EOS_DIGITAL status
  ```

Either way your photos, Canon settings and Mothlight settings stay as they are.

## A copy of the camera's firmware

On its first start Magic Lantern copies the camera's two ROM chips to the card, `ML/LOGS/ROM0.BIN` and
`ML/LOGS/ROM1.BIN` (16 MB each). Copy them to your computer and keep them. They are this camera's own firmware
and calibration, useful if something ever needs diagnosing. They are specific to your camera and Canon's
copyright: don't share them publicly.

Canon's official 2.0.2 update file is on Canon's support site, if the camera ever needs a firmware reflash.

## Leaving Mothlight completely

Run *Menu > Setup > Firmware Ver. 2.0.2* with the Mothlight card in: the installer offers to remove the boot
flag. Then format the card in the camera. See [INSTALL.md](INSTALL.md#uninstall).
