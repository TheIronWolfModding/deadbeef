#!/usr/bin/env bash
#
# build-win.sh - build DeaDBeeF for Windows, plus the LyricBar plugin.
#
# This is the Windows counterpart of ci_scripts/build.sh. It must be run from
# an MSYS2 MINGW64/UCRT64 shell (the toolchain used by the project's Windows
# CI in .github/workflows/windowsbuild.yml). MSVC is not supported.
#
# The LyricBar plugin lives one directory up, in ../deadbeef-lyricbar, and is
# built as a DLL and dropped into the player's plugins folder.
#
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LYRICBAR_DIR="$(cd -- "$ROOT/.." && pwd)/deadbeef-lyricbar"

# Upstream URLs used by ci_scripts/build.sh / windows CI.
PREMAKE_URL="https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-windows.zip"
PREMAKE_ZIP="premake-5.0.0-beta2-windows.zip"
DISPATCH_URL="https://github.com/DeaDBeeF-for-Windows/swift-corelibs-libdispatch/releases/download/release%2F6.1.1/ddb-xdispatch-win-latest.zip"
DISPATCH_ZIP="ddb-xdispatch-win-latest.zip"
DEPS_URL="https://github.com/kuba160/deadbeef-windows-deps.git"

BUILD_RELEASE=1
BUILD_DEBUG=0
BUILD_LYRICBAR=1
SKIP_DEPS=0
JOBS="$(nproc 2>/dev/null || echo 4)"
VERSION="$(cat "$ROOT/build_data/VERSION" 2>/dev/null || echo unknown)"

die()  { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Builds DeaDBeeF for Windows (MSYS2/MinGW-w64) and, by default, the LyricBar
plugin from $LYRICBAR_DIR.

Options:
  --release         build release_windows only (default)
  --debug           build debug_windows only
  --both            build release_windows and debug_windows
  --no-lyricbar     skip building the LyricBar plugin
  --lyricbar-only   build only LyricBar (no player)
  --skip-deps       do not download premake/libdispatch/theme helper repos
  -j N              parallel jobs (default: $JOBS)
  -h, --help        show this help

Requirements (inside MSYS2 UCRT64/MINGW64 shell):
  premake5, clang, make, wget/curl, unzip; libdispatch and the plugin
  dependencies listed in README.md. Missing helper downloads are automated.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)       BUILD_RELEASE=1; BUILD_DEBUG=0 ;;
        --debug)         BUILD_RELEASE=0; BUILD_DEBUG=1 ;;
        --both)          BUILD_RELEASE=1; BUILD_DEBUG=1 ;;
        --no-lyricbar)   BUILD_LYRICBAR=0 ;;
        --lyricbar-only) BUILD_RELEASE=0; BUILD_DEBUG=0; BUILD_LYRICBAR=1 ;;
        --skip-deps)     SKIP_DEPS=1 ;;
        -j)              JOBS="${2:?-j needs a number}"; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# environment
# ---------------------------------------------------------------------------
case "${MSYSTEM:-}" in
    MINGW64|UCRT64) ;;
    "")
        die "Run this from an MSYS2 MINGW64/UCRT64 shell (mingw64.exe / ucrt64.exe).
     See README.md > Windows for the prerequisite pacman packages."
        ;;
    *)
        die "Unsupported MSYSTEM '${MSYSTEM}' - expected MINGW64 or UCRT64."
        ;;
esac

command -v make >/dev/null   || die "make not found"
command -v clang >/dev/null  || die "clang not found (pacman -S mingw-w64-...-clang)"
command -v unzip >/dev/null  || die "unzip not found"

if command -v wget >/dev/null; then
    FETCH=(wget -q -O)
elif command -v curl >/dev/null; then
    FETCH=(curl -fsSL -o)
else
    die "need wget or curl to download build prerequisites"
fi

cd "$ROOT"

# ---------------------------------------------------------------------------
# prerequisites: premake, libdispatch, theme/icon helper repo
# ---------------------------------------------------------------------------
PREMAKE=""
if [[ -x ./premake5 ]]; then
    PREMAKE=./premake5
elif [[ -x ./premake5.exe ]]; then
    PREMAKE=./premake5.exe
elif command -v premake5 >/dev/null 2>&1; then
    PREMAKE="$(command -v premake5)"
elif [[ "$SKIP_DEPS" -eq 0 ]]; then
    info "downloading premake5"
    "${FETCH[@]}" "$PREMAKE_ZIP" "$PREMAKE_URL"
    unzip -o -q "$PREMAKE_ZIP"
    PREMAKE=./premake5
fi
[[ -n "$PREMAKE" ]] || die "premake5 not found - install it or allow downloads (no --skip-deps)"

if [[ "$SKIP_DEPS" -eq 0 ]]; then
    if [[ ! -d xdispatch_ddb ]]; then
        info "downloading libdispatch (xdispatch_ddb)"
        "${FETCH[@]}" "$DISPATCH_ZIP" "$DISPATCH_URL"
        unzip -q "$DISPATCH_ZIP"
    fi
    if [[ ! -d deadbeef-windows-deps ]]; then
        info "downloading theme/icon helpers"
        git clone --depth 1 "$DEPS_URL" deadbeef-windows-deps
    fi
fi

# ---------------------------------------------------------------------------
# player
# ---------------------------------------------------------------------------
build_player() {
    local config="$1"   # release | debug
    local console_flag=()
    if [[ "$config" == debug ]]; then
        console_flag=(--debug-console)
    fi

    info "generating makefiles for $config"
    "$PREMAKE" --standard "${console_flag[@]}" gmake2

    info "building config=${config}_windows"
    make -j"$JOBS" "config=${config}_windows" CC=clang CXX=clang++

    if [[ -d deadbeef-windows-deps ]]; then
        local outdir="bin/$config"
        info "installing theme/icons into $outdir"
        mkdir -p "$outdir/share/themes" "$outdir/share/icons"
        cp -r deadbeef-windows-deps/Windows-10        "$outdir/share/themes/" 2>/dev/null || true
        cp -r deadbeef-windows-deps/Windows-10-Icons  "$outdir/share/icons/"  2>/dev/null || true
    fi
}

if [[ "$BUILD_RELEASE" -eq 1 ]]; then build_player release; fi
if [[ "$BUILD_DEBUG"   -eq 1 ]]; then build_player debug;   fi

# ---------------------------------------------------------------------------
# LyricBar plugin (external project in the parent directory)
# ---------------------------------------------------------------------------
build_lyricbar() {
    [[ -d "$LYRICBAR_DIR" ]] || die "LyricBar source not found at $LYRICBAR_DIR"
    command -v glib-compile-resources >/dev/null || die "glib-compile-resources not found"

    cd "$LYRICBAR_DIR"

    local header_dir="$LYRICBAR_DIR/.headers"
    if [[ -f "$ROOT/include/deadbeef/deadbeef.h" && -f "$ROOT/plugins/gtkui/gtkui_api.h" ]]; then
        mkdir -p "$header_dir/deadbeef"
        cp -f "$ROOT"/include/deadbeef/*.h "$header_dir/deadbeef/"
        cp -f "$ROOT"/plugins/gtkui/gtkui_api.h "$header_dir/deadbeef/"
    else
        die "DeaDBeeF plugin headers not found in $ROOT (needed to build LyricBar)"
    fi
    info "LyricBar headers: $header_dir"

    # panels.glade is embedded here; always regenerate to avoid stale UI.
    glib-compile-resources --generate-source src/resources.xml
    glib-compile-resources --generate-header src/resources.xml
    rm -f resources.o

    local plugin="ddb_lyricbar_gtk3.dll"
    local common_flags="-I$header_dir -I$ROOT/shared/windows/include -O2 -g \
-include $ROOT/shared/windows/mingw32_layer.h \
-D_WIN32 -D_POSIX_C_SOURCE -DUSE_STDIO"

    info "building LyricBar DLL: $plugin"
    make -j"$JOBS" lyricbar \
        CC=clang CXX=clang++ \
        GTKMM=gtkmm-3.0 GTK=gtk+-3.0 \
        LYRICBAR="$plugin" \
        COPTS="$common_flags" \
        CXXOPTS="$common_flags" \
        LDFLAGS="-Wl,--export-all-symbols"

    [[ -f "$plugin" ]] || die "LyricBar build finished but $plugin is missing"

    # Make it part of the Windows package(s) that were built.
    local configs=()
    [[ "$BUILD_RELEASE" -eq 1 ]] && configs+=(release)
    [[ "$BUILD_DEBUG"   -eq 1 ]] && configs+=(debug)
    if [[ "${#configs[@]}" -eq 0 ]]; then
        configs=(release)   # --lyricbar-only: still stage into the default tree
    fi
    for cfg in "${configs[@]}"; do
        if [[ -d "$ROOT/bin/$cfg/plugins" ]]; then
            cp -f "$plugin" "$ROOT/bin/$cfg/plugins/"
            info "installed $plugin -> bin/$cfg/plugins/"
        else
            info "built $plugin at $LYRICBAR_DIR/$plugin (no bin/$cfg/plugins to stage into yet)"
        fi
    done

    cd "$ROOT"
}

if [[ "$BUILD_LYRICBAR" -eq 1 ]]; then build_lyricbar; fi

info "done (DeaDBeeF version $VERSION)"
