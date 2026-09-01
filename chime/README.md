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

`make image` downloads [TinyCorePure64 15.0](https://tinycorelinux.mirrorservice.org/15.x/x86_64/release/) (32 MB), injects Xfbdev plus `chime`, and boots straight into the desktop.

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

Start menu: Programs (Terminal / Editor / Cabinet), Run, Shut Down.
