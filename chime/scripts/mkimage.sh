#!/bin/sh
# Remaster TinyCorePure64: inject Xfbdev + chime into the initrd, keep original boot.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

ISO_URL=${ISO_URL:-http://www.tinycorelinux.net/15.x/x86_64/release/TinyCorePure64-15.0.iso}
ISO_MD5=11e9b4ce52825d9d221515e90e5ac1c3
DL=$ROOT/dl/TinyCorePure64-15.0.iso
WORK=$ROOT/build/image
OUT_ISO=$ROOT/chime.iso
OUT_IMG=$ROOT/chime.img
# vesafb in QEMU: nomodeset keeps simpledrm from stealing VGA so /dev/fb0 exists.
KCMDLINE="loglevel=3 cde nomodeset vga=791 video=vesafb:ywrap,mtrr:3"

if [ ! -x "$ROOT/chime" ] || [ ! -x "$ROOT/cabinet" ] || [ ! -x "$ROOT/volicon" ] || [ ! -x "$ROOT/editor" ]; then
    echo "build chime, cabinet, volicon, and editor first (make)" >&2
    exit 1
fi
if ! command -v unsquashfs >/dev/null; then
    echo "need unsquashfs (package squashfs-tools)" >&2
    exit 1
fi

# Official first; ibiblio's tcz tree is incomplete (neofetch 404s there).
ISO_MIRRORS="
$ISO_URL
http://www.tinycorelinux.net/15.x/x86_64/release/TinyCorePure64-15.0.iso
https://mirror.cpsc.ucalgary.ca/mirror/tinycorelinux/15.x/x86_64/release/TinyCorePure64-15.0.iso
https://tinycorelinux.mirrorservice.org/15.x/x86_64/release/TinyCorePure64-15.0.iso
https://distro.ibiblio.org/tinycorelinux/15.x/x86_64/release/TinyCorePure64-15.0.iso
"

http_get() {
    # Download $1 to $2. HTTP 404 is a normal miss (TinyCore omits empty .dep
    # files); anything else is logged. Body is discarded unless the status is 200.
    local url=$1 dest=$2 code
    code=$(curl -sS -L --retry 3 --retry-delay 1 --connect-timeout 15 --max-time 180 \
        -o "$dest" -w "%{http_code}" "$url" || echo "000")
    if [ "$code" = "200" ] && [ -s "$dest" ]; then
        return 0
    fi
    rm -f "$dest"
    if [ "$code" != "404" ] && [ "$code" != "200" ]; then
        echo "  $url -> HTTP $code" >&2
    fi
    return 1
}

mkdir -p "$ROOT/dl" "$WORK"
if [ ! -f "$DL" ] || ! echo "$ISO_MD5  $DL" | md5sum -c - >/dev/null 2>&1; then
    echo "downloading TinyCorePure64 ISO..."
    ok=0
    for url in $ISO_MIRRORS; do
        [ -n "$url" ] || continue
        echo "  $url"
        if http_get "$url" "$DL" && echo "$ISO_MD5  $DL" | md5sum -c - >/dev/null 2>&1; then
            ok=1
            break
        fi
        rm -f "$DL"
    done
    if [ "$ok" != 1 ]; then
        echo "could not download TinyCorePure64-15.0.iso" >&2
        exit 1
    fi
    echo "$ISO_MD5  $DL" | md5sum -c -
fi

rm -rf "$WORK"
mkdir -p "$WORK/iso" "$WORK/overlay" "$WORK/bundle"

echo "extracting ISO..."
xorriso -osirrox on -indev "$DL" -extract / "$WORK/iso" >/dev/null 2>&1
chmod -R u+w "$WORK/iso"
cp "$WORK/iso/boot/corepure64.gz" "$WORK/core-orig.gz"

echo "unsquashing X + aterm into overlay..."
# Skip TinyCore's flwm/wbar — Chime is the session WM.
OV=$WORK/overlay
for tcz in "$WORK/iso/cde/optional"/*.tcz; do
    [ -f "$tcz" ] || continue
    base=$(basename "$tcz" .tcz)
    case "$base" in
        flwm|wbar) continue ;;
    esac
    unsquashfs -f -n -d "$OV" "$tcz" >/dev/null
done

echo "adding neofetch and alsa..."
TCZREPO=${TCZREPO:-http://www.tinycorelinux.net/15.x/x86_64/tcz}
TC_KERNEL=${TC_KERNEL:-6.6.8-tinycore64}
TCZ_HIT_REPO=
TCZ_MIRRORS="
$TCZREPO
http://www.tinycorelinux.net/15.x/x86_64/tcz
https://tinycorelinux.mirrorservice.org/15.x/x86_64/tcz
https://mirror.cpsc.ucalgary.ca/mirror/tinycorelinux/15.x/x86_64/tcz
"
tcz_name() {
    local n
    n=$(printf '%s' "$1" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    n=${n%.tcz}
    n=$(printf '%s' "$n" | sed "s/KERNEL/${TC_KERNEL}/g")
    printf '%s.tcz' "$n"
}
# Squashfs magic is "hsqs". A cached 404 HTML page must be thrown out.
tcz_valid() {
    local mag
    [ -s "$1" ] || return 1
    mag=$(dd if="$1" bs=4 count=1 2>/dev/null || true)
    [ "$mag" = "hsqs" ]
}
tcz_try_mirrors() {
    local dest=$1 name=$2 repo
    for repo in $TCZ_MIRRORS; do
        [ -n "$repo" ] || continue
        if http_get "$repo/$name" "$dest"; then
            TCZ_HIT_REPO=$repo
            return 0
        fi
    done
    return 1
}
# TinyCore does not publish a .dep when there are no dependencies (HTTP 404).
# Ask only one repo so we do not print a stack of 404s across mirrors.
tcz_fetch_dep() {
    local name=$1 dest=$ROOT/dl/tcz/$name.dep repo
    [ -f "$dest" ] && [ -s "$dest" ] && return 0
    repo=${TCZ_HIT_REPO:-$TCZREPO}
    [ -n "$repo" ] || return 0
    http_get "$repo/$name.dep" "$dest" || true
}
tcz_fetch() {
    local name dep
    name=$(tcz_name "$1")
    mkdir -p "$ROOT/dl/tcz" "$WORK/tczseen"
    if [ -f "$WORK/tczseen/$name" ]; then
        return 0
    fi
    if [ -f "$ROOT/dl/tcz/$name" ] && ! tcz_valid "$ROOT/dl/tcz/$name"; then
        echo "  replacing corrupt $name"
        rm -f "$ROOT/dl/tcz/$name"
    fi
    if [ ! -f "$ROOT/dl/tcz/$name" ]; then
        echo "  downloading $name"
        if ! tcz_try_mirrors "$ROOT/dl/tcz/$name" "$name"; then
            echo "  skip $name (not in repo)" >&2
            return 1
        fi
    fi
    touch "$WORK/tczseen/$name"
    tcz_fetch_dep "$name"
    if [ -f "$ROOT/dl/tcz/$name.dep" ]; then
        while read -r dep || [ -n "$dep" ]; do
            dep=$(printf '%s' "$dep" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
            [ -n "$dep" ] || continue
            case "$dep" in \#*) continue ;; esac
            tcz_fetch "$dep" || true
        done < "$ROOT/dl/tcz/$name.dep"
    fi
    return 0
}
tcz_install() {
    local name dep
    name=$(tcz_name "$1")
    mkdir -p "$WORK/tczinst"
    [ -f "$WORK/tczinst/$name" ] && return 0
    if ! tcz_fetch "$name"; then
        return 0
    fi
    if ! tcz_valid "$ROOT/dl/tcz/$name"; then
        echo "  skip install $name (download missing)" >&2
        return 0
    fi
    touch "$WORK/tczinst/$name"
    if [ -f "$ROOT/dl/tcz/$name.dep" ]; then
        while read -r dep || [ -n "$dep" ]; do
            dep=$(printf '%s' "$dep" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
            [ -n "$dep" ] || continue
            case "$dep" in \#*) continue ;; esac
            tcz_install "$dep" || true
        done < "$ROOT/dl/tcz/$name.dep"
    fi
    unsquashfs -f -n -d "$OV" "$ROOT/dl/tcz/$name" >/dev/null
}
tcz_install neofetch.tcz
tcz_install alsa.tcz
tcz_install alsamixergui.tcz

echo "bundling chime, cabinet, volicon, editor, and libraries..."
# Copy host-linked .so files into /opt/chime so the guest does not need matching
# Debian packages. run wraps exec with that directory on the loader path.
DEST=$WORK/bundle
mkdir -p "$DEST"
for bin in chime cabinet volicon editor; do
    cp "$ROOT/$bin" "$DEST/$bin"
    chmod +x "$DEST/$bin"
    ldd "$ROOT/$bin" | awk '/=>/ {print $3} $1 ~ /^\// {print $1}' | while read -r lib; do
        [ -n "$lib" ] && [ -f "$lib" ] || continue
        cp -L "$lib" "$DEST/$(basename "$lib")"
    done
    interp=$(readelf -l "$ROOT/$bin" | sed -n 's/.*\[Requesting program interpreter: \(.*\)\]/\1/p')
    if [ -n "$interp" ] && [ -f "$interp" ]; then
        cp -L "$interp" "$DEST/$(basename "$interp")"
    fi
done

mkdir -p "$OV/opt/chime" "$OV/usr/local/bin" "$OV/etc/sysconfig" \
    "$OV/etc/skel" "$OV/home/tc"
cp -a "$DEST"/. "$OV/opt/chime/"

cat > "$OV/opt/chime/run" << 'EOF'
#!/bin/sh
D=/opt/chime
LD=$(ls "$D"/ld-linux-x86-64.so.* 2>/dev/null | head -1)
if [ -n "$LD" ]; then
    exec "$LD" --library-path "$D" "$D/chime" "$@"
fi
export LD_LIBRARY_PATH="$D${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$D/chime" "$@"
EOF
chmod +x "$OV/opt/chime/run"
cp "$OV/opt/chime/run" "$OV/usr/local/bin/chime"
chmod +x "$OV/usr/local/bin/chime"
cat > "$OV/usr/local/bin/cabinet" << 'EOF'
#!/bin/sh
D=/opt/chime
LD=$(ls "$D"/ld-linux-x86-64.so.* 2>/dev/null | head -1)
if [ -n "$LD" ]; then
    exec "$LD" --library-path "$D" "$D/cabinet" "$@"
fi
export LD_LIBRARY_PATH="$D${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$D/cabinet" "$@"
EOF
chmod +x "$OV/usr/local/bin/cabinet"
cat > "$OV/usr/local/bin/volicon" << 'EOF'
#!/bin/sh
D=/opt/chime
LD=$(ls "$D"/ld-linux-x86-64.so.* 2>/dev/null | head -1)
if [ -n "$LD" ]; then
    exec "$LD" --library-path "$D" "$D/volicon" "$@"
fi
export LD_LIBRARY_PATH="$D${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$D/volicon" "$@"
EOF
chmod +x "$OV/usr/local/bin/volicon"
cat > "$OV/usr/local/bin/editor" << 'EOF'
#!/bin/sh
D=/opt/chime
LD=$(ls "$D"/ld-linux-x86-64.so.* 2>/dev/null | head -1)
if [ -n "$LD" ]; then
    exec "$LD" --library-path "$D" "$D/editor" "$@"
fi
export LD_LIBRARY_PATH="$D${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$D/editor" "$@"
EOF
chmod +x "$OV/usr/local/bin/editor"
mkdir -p "$OV/opt/chime/wallpapers" "$OV/home/tc/.chime/wallpapers"

# Stock wallpaper so Display Properties has a photo besides the 32x32 patterns.
# PPM (P6) loads in the WM with no ImageMagick in the guest.
if command -v python3 >/dev/null 2>&1; then
    echo "writing default wallpaper..."
    python3 - "$OV/opt/chime/wallpapers/clouds.ppm" << 'PY'
import sys
w, h = 320, 240
path = sys.argv[1]
with open(path, "wb") as f:
    f.write(f"P6\n{w} {h}\n255\n".encode())
    for y in range(h):
        t = y / max(1, h - 1)
        for x in range(w):
            s = x / max(1, w - 1)
            r = int(18 + 36 * s + 40 * (1 - t))
            g = int(118 + 48 * (1 - t) + 18 * s)
            b = int(128 + 36 * t)
            n = ((x * 13 + y * 7) % 97) / 97.0
            if n > 0.74:
                r = min(255, r + 90)
                g = min(255, g + 78)
                b = min(255, b + 55)
            f.write(bytes((r, g, b)))
PY
    cp -f "$OV/opt/chime/wallpapers/clouds.ppm" "$OV/home/tc/.chime/wallpapers/clouds.ppm"
fi

printf 'Xfbdev\n' > "$OV/etc/sysconfig/Xserver"
printf 'chime\n' > "$OV/etc/sysconfig/desktop"

# TinyCore's stock .xsession launches Xvesa, which does not exist here.
# Xfbdev needs /dev/fb0 and usually has to run as root (tc has passwordless sudo).
cat > "$OV/etc/skel/.xsession" << 'EOF'
#!/bin/sh
mkdir -p /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix
i=0
while [ ! -e /dev/fb0 ] && [ "$i" -lt 40 ]; do
    i=$((i + 1))
    sleep 0.25
done
sudo chmod 666 /dev/fb0 /dev/input/mice 2>/dev/null || true
geom=""
if [ -f "$HOME/.chime/xmode" ]; then
    geom=$(tr -d ' \r\n' < "$HOME/.chime/xmode")
fi
if [ -n "$geom" ]; then
    W=${geom%x*}
    H=${geom#*x}
    sudo fbset -xres "$W" -yres "$H" 2>/dev/null || true
    sudo Xfbdev -screen "${geom}x16" -mouse /dev/input/mice,5 -nolisten tcp >/tmp/Xfbdev.log 2>&1 &
else
    sudo Xfbdev -mouse /dev/input/mice,5 -nolisten tcp >/tmp/Xfbdev.log 2>&1 &
fi
export XPID=$!
waitforX || { echo failed in waitforX; cat /tmp/Xfbdev.log 2>/dev/null; exit 1; }
"$DESKTOP" 2>/tmp/wm_errors &
export WM_PID=$!
volicon >/tmp/volicon.log 2>&1 &
[ -x "$HOME/.setbackground" ] && "$HOME/.setbackground"
[ -x "$HOME/.mouse_config" ] && "$HOME/.mouse_config" &
[ -d /usr/local/etc/X.d ] && find /usr/local/etc/X.d -type f -o -type l | sort | while read F; do . "$F"; done
[ -d "$HOME/.X.d" ] && find "$HOME/.X.d" -type f -o -type l | sort | while read F; do . "$F"; done
EOF
chmod 700 "$OV/etc/skel/.xsession"
cp "$OV/etc/skel/.xsession" "$OV/home/tc/.xsession"
chmod 700 "$OV/home/tc/.xsession"

cat > "$OV/opt/bootsync.sh" << 'EOF'
#!/bin/sh
/usr/bin/sethostname chime
echo Xfbdev > /etc/sysconfig/Xserver
echo chime > /etc/sysconfig/desktop
ldconfig >/dev/null 2>&1 || true
modprobe vesafb >/dev/null 2>&1 || true
modprobe snd-hda-intel >/dev/null 2>&1 || true
modprobe snd-intel8x0 >/dev/null 2>&1 || true
chmod 666 /dev/fb0 /dev/input/mice /dev/snd/* 2>/dev/null || true
/opt/bootlocal.sh &
EOF
chmod +x "$OV/opt/bootsync.sh"

cat > "$OV/opt/bootlocal.sh" << 'EOF'
#!/bin/sh
(
    i=0
    while [ ! -e /dev/fb0 ] && [ "$i" -lt 40 ]; do
        i=$((i + 1))
        sleep 0.25
    done
    chmod 666 /dev/fb0 /dev/input/mice 2>/dev/null || true
    [ -e /tmp/.X11-unix/X0 ] && exit 0
    mkdir -p /tmp/.X11-unix
    chmod 1777 /tmp/.X11-unix
    if [ -x /usr/local/bin/Xfbdev ]; then
        geom=""
        [ -f /home/tc/.chime/xmode ] && geom=$(tr -d ' \r\n' < /home/tc/.chime/xmode)
        extra=""
        if [ -n "$geom" ]; then
            W=${geom%x*}
            H=${geom#*x}
            fbset -xres "$W" -yres "$H" 2>/dev/null || true
            extra="-screen ${geom}x16"
        fi
        Xfbdev $extra -mouse /dev/input/mice,5 -nolisten tcp >/tmp/Xfbdev.log 2>&1 &
        i=0
        while [ ! -e /tmp/.X11-unix/X0 ] && [ "$i" -lt 40 ]; do
            i=$((i + 1))
            sleep 0.25
        done
    fi
    if [ -e /tmp/.X11-unix/X0 ]; then
        su - tc -c 'DISPLAY=:0 exec chime'
    fi
) &
EOF
chmod +x "$OV/opt/bootlocal.sh"

onboot=$WORK/iso/cde/onboot.lst
if [ -f "$onboot" ]; then
    grep -v -E '^(flwm|wbar)\.tcz$' "$onboot" > "$onboot.new"
    mv "$onboot.new" "$onboot"
fi

cat > "$WORK/iso/boot/isolinux/isolinux.cfg" << EOF
DEFAULT tc
PROMPT 0
TIMEOUT 0
LABEL tc
KERNEL /boot/vmlinuz64
INITRD /boot/corepure64.gz
APPEND $KCMDLINE
EOF

if [ -f "$WORK/iso/EFI/BOOT/grub/grub.cfg" ]; then
    cat > "$WORK/iso/EFI/BOOT/grub/grub.cfg" << EOF
set timeout=0
set default=0
menuentry "chime" {
  linux /boot/vmlinuz64 $KCMDLINE
  initrd /boot/corepure64.gz
}
EOF
fi

echo "packing initrd (original TinyCore + overlay)..."
( cd "$OV" && find . | cpio -o -H newc --quiet ) > "$WORK/overlay.cpio"
( zcat "$WORK/core-orig.gz"; cat "$WORK/overlay.cpio" ) | gzip -9 > "$WORK/iso/boot/corepure64.gz"

echo "building ISO (keep TinyCore El Torito + isohybrid MBR)..."
rm -f "$OUT_ISO"
xorriso -indev "$DL" -outdev "$OUT_ISO" \
    -boot_image any replay \
    -map "$WORK/iso/boot/corepure64.gz" /boot/corepure64.gz \
    -map "$WORK/iso/boot/isolinux/isolinux.cfg" /boot/isolinux/isolinux.cfg \
    -map "$WORK/iso/cde/onboot.lst" /cde/onboot.lst \
    -map "$WORK/iso/EFI/BOOT/grub/grub.cfg" /EFI/BOOT/grub/grub.cfg \
    >/dev/null

cp -f "$OUT_ISO" "$OUT_IMG"
echo "wrote $OUT_ISO"
echo "wrote $OUT_IMG"
ls -lh "$OUT_ISO"
