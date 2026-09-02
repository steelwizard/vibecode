# Chime

A 1990s-style desktop and window manager for X11: teal wallpaper, beveled frames, Start menu, and a taskbar on **each monitor**.

## Build

```bash
sudo apt install g++ pkg-config libx11-dev libxrandr-dev libxinerama-dev \
    xorriso cpio gzip squashfs-tools qemu-system-x86 curl
make            # host binary: ./chime
./scripts/install.sh   # compile, install, register a login-screen session
make image      # remaster TinyCorePure64 into chime.iso / chime.img
make run        # boot the ISO in QEMU
```

`./scripts/install.sh` puts the desktop on this machine and adds a **Chime** entry to the display manager (SDDM, GDM, LightDM). Log out, pick Chime in the session menu, then sign in. `sudo make uninstall` removes it.

`make image` downloads [TinyCorePure64 15.0](http://www.tinycorelinux.net/15.x/x86_64/release/) (32 MB), injects Xfbdev plus `chime` / `cabinet` / `volicon` / `editor`, and boots straight into the desktop.

## QEMU

```bash
qemu-system-x86_64 -enable-kvm -m 512 -cdrom chime.iso -vga std -display gtk -usb -device usb-tablet -nic none
```

Use `-vga std` so the guest gets `/dev/fb0` (Xfbdev). If X fails, check `/tmp/Xfbdev.log` in the guest.

## Multi-monitor

The WM queries **RandR** (preferred) then **Xinerama**:

- wallpaper and a taskbar on every screen (Start / tasks on that screen / clock)
- desktop icons on the primary monitor only; other screens are extra space for windows
- maximize and Alt+Tab stay on the monitor that owns the window

Start → Settings → **Settings** arranges the screens: drag the numbered boxes to align them (they snap to edges), pick a resolution per monitor, mark the primary, and **Identify** flashes a number on each head. Apply writes `~/.chime/monitors` and calls RandR so the layout sticks after login.

The live image uses **Xfbdev** (one framebuffer). For real multi-head, run `./chime` as the session WM on Xorg.

## Keys

| Key | Action |
|-----|--------|
| Click taskbar button | raise, or minimize if already focused |
| Double-click title | maximize on this monitor |
| Alt+Tab | cycle windows on the current monitor |
| Alt+F4 | close |
| Ctrl+Esc / Super | Start menu on the pointer's monitor |
| Arrow keys | Start menu, desktop icons, and Display Properties lists |
| Enter | Open the highlighted Start item or desktop icon |

Start menu: Programs (applications from the XDG data dirs), Documents, Settings, Run, Shut Down (shut down / restart / close Chime).

Icons on the wallpaper come from the XDG Desktop folder (`~/Desktop`, or `XDG_DESKTOP_DIR` in `~/.config/user-dirs.dirs`). They appear on the primary monitor; other screens stay empty so they are extra desktop space. Drop files, folders, or `.desktop` launchers there. If that folder does not exist yet, Chime creates it (and `~/Documents`) with My Computer, My Documents, Terminal, and Editor shortcuts.

Start → Programs lists `~/.local/share/applications` and `/usr/share/applications` (plus `$XDG_DATA_DIRS`). Entries with `Hidden`, `NoDisplay`, or `OnlyShowIn` for another desktop are skipped. Installers that ship a `.desktop` file show up here; Chime installs Cabinet, Editor, and Terminal.

**Editor** (Start → Programs → Editor, or the desktop notepad icon) is a Notepad-style app with a blinking caret, arrow-key movement, and New / Open / Save.

**Display Properties** (Start → Settings) has three tabs. **Background** keeps the 32×32 patterns (Bricks, Weave, …) separate from **pictures** (JPEG/PNG/BMP/PPM via Browse, stored in `~/.chime/wallpapers`). Tile, Center, or Stretch the picture on top of the pattern. **Appearance** is the color schemes. **Settings** is the multi-monitor layout. On Xorg, Apply moves CRTCs immediately. On the TinyCore/Xfbdev image there is only one head.

Drag a dotted rectangle on the desktop or in Cabinet to select several icons or files. Ctrl+click toggles, Shift+click ranges, Ctrl+A selects all.
