#!/bin/sh
# Remaster TinyCorePure64: inject Xfbdev + chime into the initrd, keep original boot.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

ISO_URL=${ISO_URL:-https://tinycorelinux.mirrorservice.org/15.x/x86_64/release/TinyCorePure64-15.0.iso}
ISO_MD5=11e9b4ce52825d9d221515e90e5ac1c3
DL=$ROOT/dl/TinyCorePure64-15.0.iso
WORK=$ROOT/build/image
OUT_ISO=$ROOT/chime.iso
OUT_IMG=$ROOT/chime.img
# vesafb in QEMU: nomodeset keeps simpledrm from stealing VGA so /dev/fb0 exists.
KCMDLINE="loglevel=3 cde nomodeset vga=791 video=vesafb:ywrap,mtrr:3"

if [ ! -x "$ROOT/chime" ] || [ ! -x "$ROOT/cabinet" ] || [ ! -x "$ROOT/volicon" ]; then
    echo "build chime, cabinet, and volicon first (make)" >&2
    exit 1
fi
if ! command -v unsquashfs >/dev/null; then
    echo "need unsquashfs (package squashfs-tools)" >&2
    exit 1
fi

mkdir -p "$ROOT/dl" "$WORK"
if [ ! -f "$DL" ] || ! echo "$ISO_MD5  $DL" | md5sum -c - >/dev/null 2>&1; then
    echo "downloading TinyCorePure64 ISO..."
    curl -L --fail --retry 3 -o "$DL" "$ISO_URL"
    echo "$ISO_MD5  $DL" | md5sum -c -
fi

rm -rf "$WORK"
mkdir -p "$WORK/iso" "$WORK/overlay" "$WORK/bundle"

echo "extracting ISO..."
xorriso -osirrox on -indev "$DL" -extract / "$WORK/iso" >/dev/null 2>&1
chmod -R u+w "$WORK/iso"
cp "$WORK/iso/boot/corepure64.gz" "$WORK/core-orig.gz"

echo "unsquashing X + aterm into overlay..."
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
TCZREPO=${TCZREPO:-https://tinycorelinux.mirrorservice.org/15.x/x86_64/tcz}
TC_KERNEL=${TC_KERNEL:-6.6.8-tinycore64}
tcz_name() {
    local n=${1%.tcz}
    n=$(printf '%s' "$n" | sed "s/KERNEL/${TC_KERNEL}/g")
    printf '%s.tcz' "$n"
}
tcz_fetch() {
    local name
    name=$(tcz_name "$1")
    mkdir -p "$ROOT/dl/tcz" "$WORK/tczseen"
    if [ -f "$WORK/tczseen/$name" ]; then
        return 0
    fi
    touch "$WORK/tczseen/$name"
    if [ ! -f "$ROOT/dl/tcz/$name" ]; then
        echo "  downloading $name"
        curl -L --fail --retry 3 -o "$ROOT/dl/tcz/$name" "$TCZREPO/$name"
    fi
    if [ ! -f "$ROOT/dl/tcz/$name.dep" ]; then
        curl -fsSL --retry 2 -o "$ROOT/dl/tcz/$name.dep" "$TCZREPO/$name.dep" || rm -f "$ROOT/dl/tcz/$name.dep"
    fi
    if [ -f "$ROOT/dl/tcz/$name.dep" ]; then
        while read -r dep || [ -n "$dep" ]; do
            dep=$(echo "$dep" | tr -d '\r')
            [ -n "$dep" ] || continue
            case "$dep" in \#*) continue ;; esac
            tcz_fetch "$dep"
        done < "$ROOT/dl/tcz/$name.dep"
    fi
}
tcz_install() {
    local name
    name=$(tcz_name "$1")
    mkdir -p "$WORK/tczinst"
    [ -f "$WORK/tczinst/$name" ] && return 0
    touch "$WORK/tczinst/$name"
    tcz_fetch "$name"
    if [ -f "$ROOT/dl/tcz/$name.dep" ]; then
        while read -r dep || [ -n "$dep" ]; do
            dep=$(echo "$dep" | tr -d '\r')
            [ -n "$dep" ] || continue
            case "$dep" in \#*) continue ;; esac
            tcz_install "$dep"
        done < "$ROOT/dl/tcz/$name.dep"
    fi
    unsquashfs -f -n -d "$OV" "$ROOT/dl/tcz/$name" >/dev/null
}
tcz_install neofetch.tcz
tcz_install alsa.tcz
tcz_install alsamixergui.tcz

echo "bundling chime, cabinet, volicon, and libraries..."
DEST=$WORK/bundle
mkdir -p "$DEST"
for bin in chime cabinet volicon; do
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
mkdir -p "$OV/opt/chime/wallpapers" "$OV/home/tc/.chime/wallpapers"

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
sudo Xfbdev -mouse /dev/input/mice,5 -nolisten tcp >/tmp/Xfbdev.log 2>&1 &
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
        Xfbdev -mouse /dev/input/mice,5 -nolisten tcp >/tmp/Xfbdev.log 2>&1 &
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
