#!/bin/sh
# Build Chime and install it as a selectable X11 session on this machine.
# Usage:
#   ./scripts/install.sh              # deps, compile, install (needs sudo)
#   ./scripts/install.sh --uninstall
#   PREFIX=/usr ./scripts/install.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}
FROM_MAKE=0
UNINSTALL=0
SKIP_DEPS=0
SKIP_BUILD=0

usage() {
    cat <<EOF
Build Chime and install a login-screen session (xsessions).

  ./scripts/install.sh [options]

  --prefix DIR     install prefix (default: $PREFIX, or \$PREFIX)
  --destdir DIR    staged-install root (packaging; skips apt)
  --skip-deps      do not apt-install build/runtime packages
  --skip-build     install previously built binaries only
  --uninstall      remove files this script installed
  --from-make      invoked by make install (skip build)
  -h, --help       show this help
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift 2 ;;
        --prefix=*) PREFIX=${1#--prefix=}; shift ;;
        --destdir) DESTDIR=$2; shift 2 ;;
        --destdir=*) DESTDIR=${1#--destdir=}; shift ;;
        --skip-deps) SKIP_DEPS=1; shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        --from-make) FROM_MAKE=1; SKIP_BUILD=1; SKIP_DEPS=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

BINDIR=${BINDIR:-$PREFIX/bin}
LIBEXECDIR=${LIBEXECDIR:-$PREFIX/libexec/chime}
DATADIR=${DATADIR:-$PREFIX/share/chime}
XSESSIONDIR=${XSESSIONDIR:-$PREFIX/share/xsessions}
SYS_XSESSIONDIR=/usr/share/xsessions

as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
        return
    fi
    if sudo -n true >/dev/null 2>&1; then
        sudo "$@"
        return
    fi
    # No TTY (agent/CI): ask for a password in the desktop session.
    if [ ! -t 0 ]; then
        if [ -z "${SUDO_ASKPASS:-}" ]; then
            for ap in /usr/bin/ksshaskpass /usr/bin/ssh-askpass; do
                if [ -x "$ap" ]; then
                    SUDO_ASKPASS=$ap
                    export SUDO_ASKPASS
                    break
                fi
            done
        fi
        if [ -n "${SUDO_ASKPASS:-}" ]; then
            sudo -A "$@"
            return
        fi
        if command -v pkexec >/dev/null 2>&1; then
            pkexec "$@"
            return
        fi
    fi
    sudo "$@"
}

need_root_for() {
    # True if we cannot write this path (or its parent) without privilege.
    dest=$1
    if [ -e "$dest" ]; then
        [ -w "$dest" ] && return 1
        return 0
    fi
    parent=$(dirname "$dest")
    while [ ! -d "$parent" ]; do
        parent=$(dirname "$parent")
    done
    [ -w "$parent" ] && return 1
    return 0
}

run_install() {
    if [ -n "$DESTDIR" ]; then
        "$@"
        return
    fi
    if need_root_for "$PREFIX" || need_root_for "$SYS_XSESSIONDIR"; then
        as_root "$@"
    else
        "$@"
    fi
}

is_debian() {
    [ -r /etc/debian_version ]
}

pkg_installed() {
    dpkg-query -W -f '${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'
}

ensure_deps() {
    [ "$SKIP_DEPS" = 1 ] && return 0
    [ -n "$DESTDIR" ] && return 0
    is_debian || {
        echo "not a Debian/Ubuntu system; install g++, pkg-config, libx11-dev," >&2
        echo "libxrandr-dev, libxinerama-dev, libdbus-1-dev, and an Xorg input driver yourself." >&2
        return 0
    }

    build="g++ make pkg-config python3 libx11-dev libxrandr-dev libxinerama-dev libdbus-1-dev"
    # Wayland-only desktops often omit this; without it an X11 session has no
    # keyboard or mouse.
    runtime="xserver-xorg-core xserver-xorg-input-libinput x11-xserver-utils xterm"
    missing=
    for p in $build $runtime; do
        pkg_installed "$p" || missing="$missing $p"
    done
    if [ -n "$missing" ]; then
        echo "installing packages:$missing"
        as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y $missing
    fi
}

install_files() {
    if [ ! -x "$ROOT/chime" ] || [ ! -x "$ROOT/cabinet" ] || [ ! -x "$ROOT/volicon" ] || [ ! -x "$ROOT/editor" ]; then
        echo "build binaries first (make)" >&2
        exit 1
    fi

    rootpfx=${DESTDIR}${PREFIX}
    bindir=${DESTDIR}${BINDIR}
    libexec=${DESTDIR}${LIBEXECDIR}
    data=${DESTDIR}${DATADIR}
    xsess=${DESTDIR}${XSESSIONDIR}
    apps=${DESTDIR}${PREFIX}/share/applications

    echo "installing to ${DESTDIR:-}$PREFIX"
    run_install mkdir -p "$libexec" "$bindir" "$data/wallpapers" "$xsess" "$apps"
    run_install install -m 755 "$ROOT/chime" "$ROOT/cabinet" "$ROOT/volicon" "$ROOT/editor" "$libexec/"

    sess=$ROOT/build/chime-session
    desk=$ROOT/build/chime.desktop
    mkdir -p "$ROOT/build"
    sed -e "s|@LIBEXECDIR@|$LIBEXECDIR|g" -e "s|@DATADIR@|$DATADIR|g" \
        "$ROOT/scripts/chime-session.in" > "$sess"
    chmod 755 "$sess"
    sed -e "s|@BINDIR@|$BINDIR|g" \
        "$ROOT/share/xsessions/chime.desktop.in" > "$desk"
    chmod 644 "$desk"
    run_install install -m 755 "$sess" "$bindir/chime-session"
    # Relative so the bin dir can move together.
    run_install ln -sf chime-session "$bindir/chime"

    wall=$data/wallpapers/clouds.ppm
    if command -v python3 >/dev/null 2>&1; then
        python3 "$ROOT/scripts/gen-wallpaper.py" "$ROOT/build/clouds.ppm"
        run_install install -m 644 "$ROOT/build/clouds.ppm" "$wall"
    else
        echo "python3 not found; skipping default wallpaper" >&2
    fi

    run_install install -m 644 "$desk" "$xsess/chime.desktop"

    for app in chime-cabinet chime-editor chime-terminal; do
        run_install install -m 644 "$ROOT/share/applications/$app.desktop" "$apps/$app.desktop"
    done

    # GDM/LightDM only scan /usr/share/xsessions. SDDM also looks in
    # /usr/local/share/xsessions; copy both when they differ.
    if [ -z "$DESTDIR" ] && [ "$XSESSIONDIR" != "$SYS_XSESSIONDIR" ]; then
        if need_root_for "$SYS_XSESSIONDIR" || [ "$(id -u)" -eq 0 ]; then
            run_install mkdir -p "$SYS_XSESSIONDIR"
            run_install install -m 644 "$desk" "$SYS_XSESSIONDIR/chime.desktop"
        fi
    fi
}

uninstall_files() {
    echo "removing Chime from ${DESTDIR:-}$PREFIX"
    run_install rm -f \
        "${DESTDIR}${LIBEXECDIR}/chime" \
        "${DESTDIR}${LIBEXECDIR}/cabinet" \
        "${DESTDIR}${LIBEXECDIR}/volicon" \
        "${DESTDIR}${LIBEXECDIR}/editor" \
        "${DESTDIR}${BINDIR}/chime-session" \
        "${DESTDIR}${BINDIR}/chime" \
        "${DESTDIR}${XSESSIONDIR}/chime.desktop" \
        "${DESTDIR}${DATADIR}/wallpapers/clouds.ppm" \
        "${DESTDIR}${PREFIX}/share/applications/chime-cabinet.desktop" \
        "${DESTDIR}${PREFIX}/share/applications/chime-editor.desktop" \
        "${DESTDIR}${PREFIX}/share/applications/chime-terminal.desktop"
    if [ -z "$DESTDIR" ]; then
        run_install rm -f "$SYS_XSESSIONDIR/chime.desktop"
    fi
    run_install rmdir "${DESTDIR}${LIBEXECDIR}" 2>/dev/null || true
    run_install rmdir "${DESTDIR}${DATADIR}/wallpapers" 2>/dev/null || true
    run_install rmdir "${DESTDIR}${DATADIR}" 2>/dev/null || true
    run_install rmdir "${DESTDIR}${PREFIX}/share/applications" 2>/dev/null || true
    run_install rmdir "${DESTDIR}${XSESSIONDIR}" 2>/dev/null || true
}

if [ "$UNINSTALL" = 1 ]; then
    uninstall_files
    echo "removed Chime session. Pick another session at the login screen."
    exit 0
fi

if [ "$FROM_MAKE" != 1 ]; then
    ensure_deps
fi
if [ "$SKIP_BUILD" != 1 ]; then
    make -C "$ROOT" -j"$(nproc 2>/dev/null || echo 1)"
fi
install_files

echo
echo "Chime is installed. At the login screen, open the session menu and choose Chime."
echo "  session: ${DESTDIR:-}$XSESSIONDIR/chime.desktop"
if [ -z "$DESTDIR" ] && [ -f "$SYS_XSESSIONDIR/chime.desktop" ]; then
    echo "  session: $SYS_XSESSIONDIR/chime.desktop"
fi
echo "  binaries: $LIBEXECDIR"
echo "Log out (do not reboot unless you want to) and select Chime before signing in."
echo "Start → Shut Down offers Shut down, Restart, and Close Chime (return to the login screen)."
