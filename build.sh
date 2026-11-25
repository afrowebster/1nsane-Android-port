#!/bin/sh
# Build 1nsane from your own copy of the game (GOG release, setup_1nsane_1.0_(18948).exe).
#
#   ./build.sh "setup_1nsane_1.0_(18948).exe"           # -> dist/1nsane.apk
#   ./build.sh "setup_1nsane_1.0_(18948).exe" desktop   # -> ./insane (Linux; run: ./insane game/app save)
#
# apk: needs python3 (with venv). It runs the APK builder (builder/inbuild.py) from source. The runtime
#      template APK is built with Gradle when ANDROID_HOME (or ~/android-sdk) has an Android SDK with NDK
#      27.2.12479018 and CMake 3.22.1 (JDK 17), otherwise it is downloaded from the latest release.
# desktop: needs innoextract, clang, make and the SDL2 / GLES3 development packages.
set -e
cd "$(dirname "$0")"
INST="$1"; WHAT="${2:-apk}"
REPO=afrowebster/1nsane-Android-port
die() { echo "error: $*" >&2; exit 1; }
[ -n "$INST" ] || die "usage: $0 setup_1nsane_1.0_(18948).exe [apk|desktop]"

PY=.venv/bin/python
if [ ! -x "$PY" ]; then
    python3 -m venv .venv
    .venv/bin/pip install -q pefile==2024.8.26 capstone==5.0.9 av==19.0.2 numpy pillow
fi

if [ "$WHAT" = apk ]; then
    mkdir -p dist
    SDK="${ANDROID_HOME:-$HOME/android-sdk}"
    if [ -d "$SDK/ndk/27.2.12479018" ]; then
        if [ ! -d android/deps/SDL2-2.30.9 ]; then
            mkdir -p android/deps
            curl -sfL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz | tar -xz -C android/deps
        fi
        echo "sdk.dir=$SDK" > android/local.properties
        (cd android && ./gradlew -q assembleRelease)
        cp android/app/build/outputs/apk/release/app-release-unsigned.apk dist/template.apk
    else
        echo "no Android SDK: downloading the runtime template APK from the latest release"
        curl -sfL -o dist/template.apk "https://github.com/$REPO/releases/latest/download/insane-runtime-template.apk" \
            || die "could not download the template APK"
    fi
    $PY builder/inbuild.py "$INST" -o dist/1nsane.apk --template dist/template.apk
    exit 0
fi

if [ "$WHAT" = desktop ]; then
    if [ ! -f game/app/Game.exe ]; then
        mkdir -p game && (cd game && innoextract -s "$INST")
    fi
    sha() { sha256sum "$1" | cut -d' ' -f1; }
    [ "$(sha game/app/Game.exe)" = 441d506092289f48ac330238ec98b4b9edb408f65baaf26fd9ab1ccf88b9df34 ] \
        || die "Game.exe is not the tested GOG version (1.0, build 18948)"
    $PY tools/lift.py game/app/Game.exe gen/exe --name exe --hooks tools/hooks.txt
    $PY tools/lift.py game/app/IFC21.dll gen/ifc --name ifc
    python3 tools/make_icon.py game/app/goggame-1207658658.ico game/icon_res game/app/icon.bmp >/dev/null
    for v in cm fmv invictus; do   # the runtime plays the videos re-encoded to MPEG-1 (no Indeo decoder)
        [ -f game/app/Data/Anims/$v.mpg ] || $PY tools/recode_video.py game/app/Data/Anims/$v.avi game/app/Data/Anims/$v.mpg
    done
    make -j"$(nproc)"
    echo "built ./insane  (run: ./insane game/app save)"
    exit 0
fi
die "unknown target $WHAT (apk or desktop)"
