#!/usr/bin/env bash
# Acorn uninstall.
#
#   ./uninstall.sh                 remove from ~/.local
#   ./uninstall.sh --system        remove from /usr/local (sudo)
#   ./uninstall.sh --prefix DIR    remove from DIR
#   ./uninstall.sh --purge         also remove settings, recents and cache
#
# Removes exactly the paths in share/acorn/install-manifest.txt, and only
# those inside the prefix. --purge never touches your solutions or databases.
set -euo pipefail

APP=acorn
APP_ID=io.github.scott8420.Acorn
PREFIX="$HOME/.local"
PURGE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --system) PREFIX=/usr/local ;;
        --prefix) shift; PREFIX="${1:?--prefix needs a directory}" ;;
        --purge)  PURGE=1 ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "uninstall.sh: unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

PREFIX="$(realpath -m "$PREFIX")"
MANIFEST="$PREFIX/share/$APP/install-manifest.txt"
if [[ ! -f "$MANIFEST" ]]; then
    echo "uninstall.sh: no manifest at $MANIFEST -- nothing installed there?" >&2
    exit 1
fi

SUDO=()
[[ -w "$PREFIX" ]] || SUDO=(sudo)

removed=0
while IFS= read -r p; do
    [[ -z "$p" ]] && continue
    real="$(realpath -m "$p")"
    case "$real" in
        "$PREFIX"/*) ;;
        *) echo "  skip (outside prefix): $p"; continue ;;
    esac
    if [[ -e "$real" || -L "$real" ]]; then
        "${SUDO[@]}" rm -f -- "$real"
        echo "  removed $real"
        removed=$((removed + 1))
    fi
done < "$MANIFEST"
"${SUDO[@]}" rmdir --ignore-fail-on-non-empty "$PREFIX/share/$APP" 2>/dev/null || true

ICONDIR="$PREFIX/share/icons/hicolor"
APPDIR="$PREFIX/share/applications"
command -v gtk-update-icon-cache   >/dev/null && [[ -d "$ICONDIR" ]] && "${SUDO[@]}" gtk-update-icon-cache -q -t -f "$ICONDIR" || true
command -v update-desktop-database >/dev/null && [[ -d "$APPDIR" ]]  && "${SUDO[@]}" update-desktop-database -q "$APPDIR" || true

if [[ $PURGE -eq 1 ]]; then
    for d in "${XDG_CONFIG_HOME:-$HOME/.config}/$APP" \
             "${XDG_DATA_HOME:-$HOME/.local/share}/$APP/recent.json" \
             "${XDG_CACHE_HOME:-$HOME/.cache}/$APP"; do
        if [[ -e "$d" ]]; then rm -rf -- "$d"; echo "  purged $d"; fi
    done
fi

echo "Uninstalled Acorn from $PREFIX ($removed files)."
