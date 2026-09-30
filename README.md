# wmsliver

Session tools for [Window Maker](https://www.windowmaker.org/) that take their look from your theme:

- **`wmsliver`** is a screen locker. The lock panel looks like a focused Window Maker window, over your wallpaper. It can show live dockapps (`wmcpuload`, `wmclock`, …) in a corner, and it switches to power-saver while locked.
- **`wmsliver-logout`** is a session menu with Lock, Log Out, Suspend, Reboot and Shut Down. It looks like one of your Window Maker menus.

Both read your Window Maker theme when they start, so they always match it. They're small C programs using Xlib, Xft, Imlib2 and PAM, with no toolkit and no daemon.

## Install

Needs X11, Xft, Xinerama, Imlib2 and PAM. The power-saver feature also needs sd-bus.
- Arch: `libx11 libxft libxinerama imlib2 pam systemd-libs`
- Debian: `libx11-dev libxft-dev libxinerama-dev libimlib2-dev libpam0g-dev libsystemd-dev`

```sh
make                 # without systemd: make POWER=0
sudo make install
```

Test the locker with a timer the first time, so a bug can't lock you out: `timeout 30 wmsliver`.

## Keys

Add shortcuts to `~/GNUstep/Defaults/WMRootMenu`, then restart Window Maker:

```
("Lock Screen", SHORTCUT, "Mod4+L", EXEC, wmsliver),
("Session...", SHORTCUT, "Mod4+Escape", EXEC, wmsliver-logout),
```

In the session menu:
- arrow keys, Tab or `j`/`k` move the selection and Enter runs it;
- **L**, **O**, **S**, **R** and **D** run an item directly;
- Escape or a click outside closes it.

Lock is pre-selected. To lock when idle, add `xss-lock -- wmsliver &` to your autostart.

## Configure

Both programs work without a config file.
- **Locker:** copy [`config.example`](config.example) to `~/.config/wmsliver/config` to set the picture, dockapps and their corner, and power-saver.
- **Session menu:** copy [`logout.example`](logout.example) to `~/.config/wmsliver/logout` to change commands, hide items or move the menu.

Colours, fonts and textures come from `~/GNUstep/Defaults/WindowMaker`: title bar, frame, menu and icon settings, and your wallpaper. All the usual texture types are supported.

## Notes

- **Dockapps:** wmsliver starts private copies of your dockapps and stops them on unlock, so the ones in your dock are never touched.
- **Security:** it's a small locker in the spirit of [slock](https://tools.suckless.org/slock/). It takes the keyboard and mouse, or refuses to lock if it can't. The password is kept out of swap, wiped after each try, and checked through PAM. For a hardened locker, see [xsecurelock](https://github.com/google/xsecurelock).

MIT licensed.
