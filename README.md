# wmsliver

A small, fast screen locker for [Window Maker](https://www.windowmaker.org/) that wears your theme.

Most X11 lockers look the same on every desktop. wmsliver looks like it belongs in Window Maker:

- **Uses your theme.** The lock panel is drawn as a focused Window Maker window, with your title bar texture, bevels, resize bar, frame border colours and fonts. The background is your current wallpaper. It reads the theme each time it locks, so switching themes just works.
- **Shows real dockapps.** Show `wmcpuload`, `wmclock` or any other dockapp in a corner of the lock screen. wmsliver runs its own private copies, so the ones in your dock are left alone.
- **Saves power while locked.** With [power-profiles-daemon](https://gitlab.freedesktop.org/upower/power-profiles-daemon), it switches to `power-saver` on lock and restores your previous profile on unlock.
- **Stays small.** One C file using Xlib, Xft, Imlib2 and PAM. No toolkit, no daemon, and it locks almost instantly.

The panel has a profile picture, your user name and a password box. That's all.

## Build and install

Dependencies:

| | Arch | Debian / Ubuntu |
|---|---|---|
| X11, Xft, Xinerama | `libx11 libxft libxinerama` | `libx11-dev libxft-dev libxinerama-dev` |
| Imlib2 | `imlib2` | `libimlib2-dev` |
| PAM | `pam` | `libpam0g-dev` |
| sd-bus (power saving, optional) | `systemd-libs` | `libsystemd-dev` |

```sh
make
sudo make install          # /usr/local/bin/wmsliver and /etc/pam.d/wmsliver
```

Without systemd (Void, Devuan, Slackware, ...), build without the power-saving feature:

```sh
make POWER=0
```

`/etc/pam.d/wmsliver` just includes your `login` auth stack. If it isn't installed, wmsliver uses the `login` service directly.

## Try it safely first

A locker bug can lock you out, so the first time, run it with a timer that kills it:

```sh
timeout -k 2 30 wmsliver
```

The screen unlocks after 30 seconds whatever happens. Try your password before the timer runs out. If you ever get stuck, switch to a text console (Ctrl+Alt+F2) and run `pkill wmsliver`.

## Lock with a key

Add an entry with a shortcut to your root menu, `~/GNUstep/Defaults/WMRootMenu`:

```
("Lock Screen", SHORTCUT, "Mod4+L", EXEC, wmsliver),
```

If the shortcut doesn't work straight away, restart Window Maker so it re-reads the menu. Tap the shortcut rather than holding it: wmsliver waits up to a second for the key to be released before taking the keyboard.

To lock when idle, use [xss-lock](https://bitbucket.org/raymonad/xss-lock) in `~/GNUstep/Library/WindowMaker/autostart`:

```sh
xset s 600
xss-lock -- wmsliver &
```

## Configure

wmsliver works without a config file. To change anything, copy [`config.example`](config.example) to `~/.config/wmsliver/config` (or `/etc/wmsliver.conf` for all users):

```ini
# picture above your name (default: ~/.face, then ~/.face.icon)
avatar = ~/Pictures/me.png

# text in the title bar
title = Screen Locked

# dockapps, left to right, with arguments if needed (up to 8)
dockapp = wmcpuload
dockapp = wmclock -12

# bottom-left, bottom-right, top-left or top-right
dock = bottom-left

# power-saver while locked (needs power-profiles-daemon)
powersave = yes

# colour of "Wrong password"
error_color = #e05050
```

Command line:

```
wmsliver [-c config] [-a avatar] [-v]
```

- `-c`: use this config file instead of the default ones.
- `-a`: use this picture, overriding the config.
- `-v`: print the version.

Colours, fonts and textures aren't set here. They come from Window Maker. wmsliver reads `/etc/WindowMaker/WindowMaker`, then your `$GNUSTEP_USER_ROOT/Defaults/WindowMaker` (default `~/GNUstep`), and uses:

| Theme key | Used for |
|---|---|
| `FTitleBack`, `FTitleColor`, `WindowTitleFont`, `TitleJustify` | title bar |
| `FrameFocusedBorderColor`, `FrameBorderWidth` | window border, avatar frame |
| `ResizebarBack` | resize bar |
| `MenuTextBack`, `MenuTextColor`, `MenuTitleFont`, `MenuTextFont` | panel body, user name |
| `IconTitleBack`, `FrameSelectedBorderColor`, `HighlightTextColor` | password field |
| `IconBack` | dock tiles |
| `WorkspaceBack` | background, if no wallpaper has been set with `wmsetbg` |

It supports the usual texture types: `solid`, `[m]{h,v,d}gradient`, and `{t,s,c,f,m}pixmap`. Textures that combine a tiled pixmap with a gradient (`t{h,v,d}gradient`) are drawn as the gradient only.

## How the dockapps work

A dockapp is a small program that draws into a 64×64 "icon window". Window Maker normally takes that window into a dock tile. On lock, wmsliver:

1. starts a new copy of each configured dockapp;
2. waits until Window Maker has put it in a tile;
3. moves the icon window into the lock screen;
4. stops the copy on unlock.

wmsliver never touches the copies already in your dock. Moving a live dockapp out of Window Maker's tile and back confuses Window Maker, which then shows it as a normal window. The dockapps get no keyboard or mouse input while locked.

## Security notes

wmsliver works the way [slock](https://tools.suckless.org/slock/) does:

- **Keyboard and mouse:** it takes both over. If it can't within about a second, it exits without locking, rather than showing a lock screen that doesn't protect anything.
- **Password:** kept in memory that can't be written to swap, and wiped after each attempt. The check goes through PAM.
- **Staying on top:** if another window appears over it, it raises itself back.
- **Dockapps:** a dockapp crashing can't take the locker down with it.

It's a small, easily audited locker, not a hardened one. For example, X11 has no protocol that guarantees a locker stays on top the way Wayland's `ext-session-lock` does. If you need stronger isolation (a separate authentication process, protection against a crashed compositor), look at [xsecurelock](https://github.com/google/xsecurelock).

## License

MIT, see [LICENSE](LICENSE).
