# wmsliver

```sh
make
sudo make install
```

You'll need the dev packages for X11, Xft, Xinerama, Imlib2, PAM and (optionally) libsystemd.

| Build option | Default | What it does |
|---|---|---|
| `POWER` | `1` | `POWER=0` builds without libsystemd and drops the power-saver feature |
| `PREFIX` | `/usr/local` | where `make install` puts things |
| `DESTDIR` | empty | staging directory, for packagers |
| `CFLAGS` | `-O2` | the usual |

First time, try `timeout 30 wmsliver`, so a bug can't lock you out for good.

Then give them keys in `~/GNUstep/Defaults/WMRootMenu` and restart Window Maker:

```
("Lock Screen", SHORTCUT, "Mod4+L", EXEC, wmsliver),
("Session...", SHORTCUT, "Mod4+Escape", EXEC, wmsliver-logout),
```

## Locker settings

Put these in `~/.config/wmsliver/config`, or `/etc/wmsliver.conf` for everyone. [`config.example`](config.example) has them all commented out.

| Setting | Default | What it does |
|---|---|---|
| `avatar` | `~/.face`, then `~/.face.icon` | picture above your name (any image, cropped square) |
| `title` | `Screen Locked` | text in the title bar |
| `dockapp` | none | a dockapp to show, with arguments if needed. Repeat the line for more (up to 8) |
| `dock` | `bottom-left` | corner for the dockapps: `bottom-left`, `bottom-right`, `top-left`, `top-right` |
| `powersave` | `yes` | switch to power-saver while locked (needs power-profiles-daemon) |
| `error_color` | `#e05050` | colour of "Wrong password" |

Command line: `wmsliver [-c config] [-a avatar] [-v]`. `-c` uses a different config file, `-a` a different picture, and `-v` prints the version.

## Menu settings

Put these in `~/.config/wmsliver/logout`, or `/etc/wmsliver-logout.conf` for everyone. See [`logout.example`](logout.example).

| Setting | Default | What it does |
|---|---|---|
| `title` | `Session` | text in the menu's title bar |
| `position` | `bottom-left` | `bottom-left`, `bottom-right`, `top-left`, `top-right` or `center` |
| `lock` | `wmsliver` | command for Lock |
| `logout` | `pkill -TERM -n -x wmaker` | command for Log Out |
| `suspend` | `wmsliver & sleep 1; exec systemctl suspend` | command for Suspend (locks first) |
| `reboot` | `systemctl reboot` | command for Reboot |
| `shutdown` | `systemctl poweroff` | command for Shut Down |

Leave a command empty (`reboot =`) to hide that item. Commands run through `/bin/sh`.

In the menu:
- arrow keys, Tab or `j`/`k` move the selection and Enter runs it;
- **L**, **O**, **S**, **R** and **D** run an item directly;
- Escape or a click outside closes it.

Command line: `wmsliver-logout [-c config] [-v]`.

## Theme and environment

Colours, fonts and textures aren't set in the config files. They come from your Window Maker theme: `/etc/WindowMaker/WindowMaker`, overridden by `~/GNUstep/Defaults/WindowMaker`. These are the theme keys they read:

- **Window parts:** `FTitleBack`, `FTitleColor`, `WindowTitleFont`, `TitleJustify`, `FrameBorderWidth`, `FrameBorderColor`, `FrameFocusedBorderColor`, `FrameSelectedBorderColor`, `ResizebarBack`.
- **Menus:** `MenuTitleBack`, `MenuTitleColor`, `MenuTitleFont`, `MenuTextBack`, `MenuTextColor`, `MenuTextFont`, `MenuStyle`, `HighlightColor`, `HighlightTextColor`.
- **Icons and background:** `IconBack`, `IconTitleBack`, `WorkspaceBack`, `PixmapPath`.

The background is your current wallpaper when one has been set (with `wmsetbg`). If not, they draw `WorkspaceBack`.

Environment variables they respect:
- `GNUSTEP_USER_ROOT`: where your GNUstep folder lives, if not `~/GNUstep`.
- `XDG_CONFIG_HOME`: where `wmsliver/` config lives, if not `~/.config`.
