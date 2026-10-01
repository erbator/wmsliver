# wmsliver

A screen locker and a logout menu for [Window Maker](https://www.windowmaker.org/), both dressed in your current theme.

- **`wmsliver`** locks the screen with a little Window Maker-style window over your wallpaper. It can show your dockapps while locked, and it goes easy on the battery until you're back.
- **`wmsliver-logout`** pops up a menu with Lock, Log Out, Suspend, Reboot and Shut Down.

Switch themes and they follow along. Small C, no toolkit.

## Get it running

```sh
make            # no systemd? make POWER=0
sudo make install
```

You'll need the dev packages for X11, Xft, Xinerama, Imlib2, PAM and (optionally) libsystemd.

First time, try `timeout 30 wmsliver`, so a bug can't lock you out for good.

Then give them keys in `~/GNUstep/Defaults/WMRootMenu` and restart Window Maker:

```
("Lock Screen", SHORTCUT, "Mod4+L", EXEC, wmsliver),
("Session...", SHORTCUT, "Mod4+Escape", EXEC, wmsliver-logout),
```

## Tweaking

Nothing needs configuring. If you want to pick dockapps, a picture or menu commands, start from [`config.example`](config.example) and [`logout.example`](logout.example). They go in `~/.config/wmsliver/`.

MIT licensed. Have fun.
