# Chime

A 1990s-style desktop and window manager for X11: teal wallpaper, beveled frames, Start menu, and a taskbar on **each monitor**.

## Build

```bash
sudo apt install g++ pkg-config libx11-dev libxrandr-dev libxinerama-dev \
    xorriso cpio gzip squashfs-tools qemu-system-x86 curl
make            # host binary: ./chime
make image      # remaster TinyCorePure64 into chime.iso / chime.img
make run        # boot the ISO in QEMU
```

`make image` downloads [TinyCorePure64 15.0](http://www.tinycorelinux.net/15.x/x86_64/release/) (32 MB), injects Xfbdev plus `chime` / `cabinet` / `volicon` / `editor`, and boots straight into the desktop.

## QEMU

```bash
qemu-system-x86_64 -enable-kvm -m 512 -cdrom chime.iso -vga std -display gtk -usb -device usb-tablet -nic none
```

Use `-vga std` so the guest gets `/dev/fb0` (Xfbdev). If X fails, check `/tmp/Xfbdev.log` in the guest.

## Multi-monitor

The WM queries **RandR** (preferred) then **Xinerama**:

- desktop on every screen
- a taskbar on every screen (Start / tasks on that screen / clock)
- maximize and Alt+Tab stay on the monitor that owns the window

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

Start menu: Programs (Terminal / Editor / Cabinet), Documents, Settings, Run, Shut Down.

**Editor** (Start → Programs → Editor, or the desktop notepad icon) is a Notepad-style app with a blinking caret, arrow-key movement, and New / Open / Save.

**Display Properties** (Start → Settings): wallpaper, color schemes, and **Screen resolution**. On Xorg, Apply switches mode immediately (`xrandr`). On the TinyCore/Xfbdev image the choice is saved to `~/.chime/xmode` and used the next time X starts.

Drag a dotted rectangle on the desktop or in Cabinet to select several icons or files. Ctrl+click toggles, Shift+click ranges, Ctrl+A selects all.
