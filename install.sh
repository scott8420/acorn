#!/usr/bin/env bash
# Acorn install.
#
#   ./install.sh                 install to ~/.local (no sudo)
#   ./install.sh --system        install to /usr/local (sudo)
#   ./install.sh --prefix DIR    install to DIR
#   ./install.sh --build         run ./build.sh first
#
# Installs the binary, the engine plugins (lib/acorn/engines), the .desktop file, the scalable + symbolic hicolor icons,
# and share/acorn/install-manifest.txt listing every path written, which
# uninstall.sh removes exactly. sudo is used only if the prefix isn't writable.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

APP=acorn
APP_ID=io.github.scott8420.Acorn
PREFIX="$HOME/.local"
DO_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --system) PREFIX=/usr/local ;;
        --prefix) shift; PREFIX="${1:?--prefix needs a directory}" ;;
        --build)  DO_BUILD=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

[[ $DO_BUILD -eq 1 ]] && ./build.sh

if [[ ! -x build/$APP ]]; then
    echo "install.sh: build/$APP not found -- run ./build.sh (or pass --build)" >&2
    exit 1
fi

SUDO=()
probe="$PREFIX"
while [[ ! -e "$probe" ]]; do probe="$(dirname "$probe")"; done
if [[ ! -w "$probe" ]]; then SUDO=(sudo); fi

BINDIR="$PREFIX/bin"
APPDIR="$PREFIX/share/applications"
ICONDIR="$PREFIX/share/icons/hicolor"
SHAREDIR="$PREFIX/share/$APP"
ENGINEDIR="$PREFIX/lib/$APP/engines"
MANIFEST="$SHAREDIR/install-manifest.txt"

written=()
put() {  # put SRC DEST MODE
    "${SUDO[@]}" install -D -m "$3" "$1" "$2"
    written+=("$2")
}

tmp_desktop="$(mktemp)"
trap 'rm -f "$tmp_desktop"' EXIT
sed "s|@BINDIR@|$BINDIR|g" "data/$APP_ID.desktop.in" > "$tmp_desktop"

put "build/$APP"                                       "$BINDIR/$APP"                                         755
put "$tmp_desktop"                                     "$APPDIR/$APP_ID.desktop"                              644
put "data/$APP_ID.svg"                                 "$ICONDIR/scalable/apps/$APP_ID.svg"                   644
put "resources/icons/scalable/apps/$APP-logo-symbolic.svg" "$ICONDIR/symbolic/apps/$APP_ID-symbolic.svg"      644

# Engine plugins (s002): whatever engines this build produced. The app finds
# them at <bin>/../lib/acorn/engines; a missing engine just isn't listed.
shopt -s nullglob
engines=(build/engines/acorn-engine-*.so)
shopt -u nullglob
[[ ${#engines[@]} -eq 0 ]] && echo "  note: no engine plugins in build/engines -- Acorn will list no engines"
for so in "${engines[@]}"; do
    put "$so" "$ENGINEDIR/$(basename "$so")" 755
done

manifest_tmp="$(mktemp)"
printf '%s\n' "${written[@]}" "$MANIFEST" > "$manifest_tmp"
"${SUDO[@]}" install -D -m 644 "$manifest_tmp" "$MANIFEST"
rm -f "$manifest_tmp"

command -v gtk-update-icon-cache   >/dev/null && "${SUDO[@]}" gtk-update-icon-cache -q -t -f "$ICONDIR" || true
command -v update-desktop-database >/dev/null && "${SUDO[@]}" update-desktop-database -q "$APPDIR" || true

echo "Installed Acorn to $PREFIX"
echo "  manifest: $MANIFEST"
case ":$PATH:" in *":$BINDIR:"*) ;; *) echo "  note: $BINDIR is not on your PATH" ;; esac
