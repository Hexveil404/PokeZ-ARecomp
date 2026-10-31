#!/bin/bash
# ZA Recomp release for Linux x86-64, built natively (the Linux counterpart of tools/za/dist.sh):
# build/dist/ZA Recomp-<version>-linux-x86_64.AppImage, and from the same files ZA Recomp-<version>-linux-x86_64.zip
# (a folder to unpack anywhere: no FUSE needed) and zarecomp_<version>_amd64.deb (Debian / Ubuntu / Mint: installs into
# /opt/zarecomp, with a zarecomp command and a menu entry). No game code or game files: the start screen reads the player's
# own .xci / .nsp with their prod.keys (zaimport/) or an extracted exefs + romfs, and recompiles the game on their
# machine (lasgen/ = tools/gen.py frozen by PyInstaller, zig/ = the compiler, gamelib-src/ = the headers it includes).
# Built against this machine's glibc: the AppImage runs on distributions at least as new (Linux Mint 22 / Ubuntu 24.04
# here). tools/linux/appimage.sh (podman, Ubuntu 22.04) is upstream's portable way for LAS Recomp.
#
# Needs: what tools/za/build-linux.sh checks, the .NET 8 SDK (dotnet-sdk-8.0: the importer, self-contained so players
# need no .NET) and patchelf. Downloads once, pinned and hash-checked where the tools allow: zig (tools/fetch_zig.sh),
# appimagetool and its runtime (as tools/linux/package.sh), the helpers' Python packages (PyPI), LibHac (NuGet).
# ZAIMPORT=0: no importer (a test build: extracted game files only). ZIP=0 / DEB=0: no .zip / no .deb.
set -euo pipefail
cd "$(dirname "$0")/../.."
ARCH=x86_64
L=$PWD/build/linux/$ARCH
P=$L/deps
B=$L/za
APPDIR=$L/za-AppDir
VERSION=$(sed -n 's/^#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
OUT="build/dist/ZA Recomp-$VERSION-linux-$ARCH.AppImage"
ZAIMPORT=${ZAIMPORT:-1}
ZIP=${ZIP:-1}
DEB=${DEB:-1}
PY=${PYTHON:-python3}  # (the helpers' venvs; e.g. PYTHON=python3.12 in tools/linux/Containerfile's image)

for t in patchelf llvm-strip readelf objdump $([ "$ZIP" = 1 ] && echo zip) $([ "$DEB" = 1 ] && echo dpkg-deb); do
    command -v $t >/dev/null || { echo "dist-linux: $t missing (sudo apt install patchelf llvm binutils zip dpkg)"; exit 1; }
done
if [ "$ZAIMPORT" = 1 ] && ! command -v dotnet >/dev/null; then
    echo "dist-linux: the .NET 8 SDK is missing (sudo apt install dotnet-sdk-8.0), or ZAIMPORT=0 for a test build"
    exit 1
fi

# 1. the app (start screen + runtime) against the pinned SDL3 / FFmpeg / shaderc
tools/za/build-linux.sh
# 2. the compiler the start screen builds the game code with
[ -x build/zig/zig ] || ZIG_TARGET=$ARCH-linux tools/fetch_zig.sh
# 3. the generator helper (its own venv: only what gen.py needs)
GV=$L/gen-venv
if [ ! -f "$GV/.ok" ]; then
    rm -rf "$GV" && "$PY" -m venv "$GV"
    "$GV/bin/pip" install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 pyinstaller==6.22.3
    touch "$GV/.ok"
fi
"$GV/bin/python" -m PyInstaller --noconfirm --clean --log-level WARN --distpath "$L/helper" --workpath "$L/pyinstaller" \
    tools/lasgen.spec
# 4. the button icons + opt-in battle layout preparer (zaprompts + ooz; all resources come from the player's game)
mkdir -p build/ooz
O=third_party/ooz
g++ -O2 -shared -fPIC -Itools/za/ooz_posix -include tools/za/ooz_posix/ooz_posix.h -include sys/stat.h \
    -Dmain=ooz_main $O/kraken.cpp $O/bitknit.cpp $O/lzna.cpp $O/ooz_lib.cpp -o build/ooz/libooz.so
PV=$L/prompts-venv
if [ ! -f "$PV/.ok" ]; then
    rm -rf "$PV" && "$PY" -m venv "$PV"
    "$PV/bin/pip" install -q pillow==13.0.0 numpy==2.5.4 fonttools==4.67.0 cffsubr==0.4.0 texture2ddecoder==1.0.6 \
        pyinstaller==6.22.3
    touch "$PV/.ok"
fi
"$PV/bin/python" -m PyInstaller --noconfirm --clean --log-level WARN --distpath "$L/helper" \
    --workpath "$L/pyinstaller-prompts" tools/zaprompts.spec
"$L/helper/zaprompts/zaprompts" --xbox-attack-layout-version  # refuse a package with an older/incomplete helper
# 5. the game file importer (self-contained single file: no .NET on the player's machine)
if [ "$ZAIMPORT" = 1 ]; then
    dotnet publish tools/zaimport -c Release -r linux-x64 --self-contained true -p:PublishSingleFile=true \
        -p:EnableCompressionInSingleFile=true -p:DebugType=none -o "$L/zaimport" > /dev/null
fi

# ---- AppDir: everything next to the executable (runtime/paths.c rt_resource), libraries in usr/lib
rm -rf "$APPDIR" && mkdir -p "$APPDIR/usr/bin/gamelib-src" "$APPDIR/usr/lib"
D=$APPDIR/usr/bin
cp "$B/lasrecomp" "$D/"
llvm-strip --strip-debug "$D/lasrecomp"  # (symbols stay: crash backtraces name functions)
cp -R "$L/helper/lasgen" "$L/helper/zaprompts" build/zig "$D/"
cp third_party/ooz/COPYING "$D/zaprompts/COPYING-GPLv3.txt"
cp third_party/ooz/README.ZARecomp.md "$D/zaprompts/README-ooz.md"
if [ "$ZAIMPORT" = 1 ]; then mkdir "$D/zaimport" && cp "$L/zaimport/zaimport" "$D/zaimport/"; fi
cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
   "$D/gamelib-src/"
cp docs/icon/icon_1024.png "$D/icon.png"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$D/"  # (start screen font, OFL)
cp third_party/fonts/Baloo2/OFL.txt "$D/Baloo2-OFL.txt"
cp third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf "$D/"  # (game text font without the player's own, OFL)
cp third_party/fonts/BIZUDPGothic/OFL.txt "$D/BIZUDPGothic-OFL.txt"
cp third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$D/"  # (last-resort fonts)
cp third_party/fonts/DejaVu/LICENSE "$D/DejaVu-LICENSE.txt"
cp README.md LICENSE "$D/"  # (licenses and source links of the bundled components: README "License")
[ -f README.pt-BR.md ] && cp README.pt-BR.md "$D/"
mkdir -p "$D/docs" && cp docs/XBOX-ATTACK-LAYOUT.md "$D/docs/"
cp tools/za/za-default.ini "$D/za-default.ini"  # (not larecomp.ini: the data folder holds the player's own)
cp tools/linux/AppRun "$APPDIR/" && chmod +x "$APPDIR/AppRun"
cat > "$APPDIR/zarecomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ZA Recomp
Comment=Pokémon Legends: Z-A, recompiled for PC (bring your own game files)
Exec=lasrecomp
Icon=zarecomp
Categories=Game;
Terminal=false
EOF
cp docs/icon/icon_1024.png "$APPDIR/zarecomp.png" && ln -sf zarecomp.png "$APPDIR/.DirIcon"

# libraries: what lasrecomp needs (DT_NEEDED, recursively) that deps/ provides; anything else must be a system one
# (glibc, libstdc++, the Vulkan loader: a bundled loader would not find the user's GPU drivers)
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
SYSTEM_LIBS='^(libc|libm|libdl|libpthread|librt|libresolv|libstdc\+\+|libgcc_s|libvulkan)\.so|^ld-linux'
todo=("$D/lasrecomp")
while [ ${#todo[@]} -gt 0 ]; do
    f=${todo[0]}; todo=("${todo[@]:1}")
    for n in $(needed "$f"); do
        [ -e "$APPDIR/usr/lib/$n" ] && continue
        if [ -e "$P/lib/$n" ]; then
            cp -L "$P/lib/$n" "$APPDIR/usr/lib/$n"
            patchelf --set-rpath '$ORIGIN' "$APPDIR/usr/lib/$n"
            todo+=("$APPDIR/usr/lib/$n")
        elif ! [[ $n =~ $SYSTEM_LIBS ]]; then
            echo "dist-linux: $(basename "$f") needs $n, neither bundled nor a base system library"; exit 1
        fi
    done
done
patchelf --set-rpath '$ORIGIN/../lib' "$D/lasrecomp"
if ls "$APPDIR/usr/lib" | grep -iE 'x264|x265|xvid|postproc|vulkan'; then  # (no GPL codecs, no Vulkan loader)
    echo "dist-linux: library that must not ship"; exit 1
fi
! nm "$D/lasrecomp" | grep -q ' f_70'  # (no game code inside)
echo "bundled: $(ls "$APPDIR/usr/lib" | tr '\n' ' ')"
du -sh "$APPDIR"

# ---- AppImage (pinned appimagetool and runtime, as tools/linux/package.sh)
mkdir -p build/linux/src build/dist
fetch() {  # url sha256 file
    [ -f "build/linux/src/$3" ] || { curl -fsSL -o "build/linux/src/$3.part" "$1" && mv "build/linux/src/$3.part" "build/linux/src/$3"; }
    echo "$2  build/linux/src/$3" | sha256sum -c --quiet
}
fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
      ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 appimagetool-x86_64.AppImage
fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
      2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d runtime-x86_64
chmod +x build/linux/src/appimagetool-x86_64.AppImage
rm -f "$OUT"
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=$ARCH build/linux/src/appimagetool-x86_64.AppImage --no-appstream \
    --runtime-file build/linux/src/runtime-x86_64 "$APPDIR" "$OUT"
ls -la "$OUT"

# ---- zip: the AppDir as a folder (AppRun starts it; everything it writes still goes to ~/.local/share/zarecomp)
if [ "$ZIP" = 1 ]; then
    ZOUT="$PWD/build/dist/ZA Recomp-$VERSION-linux-$ARCH.zip"
    ZD=$L/zip/"ZA Recomp-$VERSION-linux-$ARCH"
    rm -rf "$L/zip" "$ZOUT" && mkdir -p "$ZD"
    cp -R "$APPDIR/usr" "$APPDIR/zarecomp.desktop" "$APPDIR/zarecomp.png" "$ZD/"
    cp "$APPDIR/AppRun" "$ZD/zarecomp"  # (AppRun finds usr/bin next to itself)
    (cd "$L/zip" && zip -qry9 "$ZOUT" "ZA Recomp-$VERSION-linux-$ARCH")  # (-y: links as links; modes are kept)
    ls -la "$ZOUT"
fi

# ---- deb: /opt/zarecomp/{bin,lib} (lasrecomp's rpath is $ORIGIN/../lib, its resources sit next to it), a zarecomp
# command, the menu entry and its icon. Depends on the glibc the build needed (this machine's, or 2.35 from CI's
# Ubuntu 22.04 image), the C++ runtime and the system's Vulkan loader; SDL3 opens X11 / Wayland / audio at run time.
if [ "$DEB" = 1 ]; then
    DV=${VERSION#v}
    DR=$L/deb
    DOUT="$PWD/build/dist/zarecomp_${DV}_amd64.deb"
    rm -rf "$DR" "$DOUT" && mkdir -p "$DR/DEBIAN" "$DR/opt/zarecomp" "$DR/usr/bin" "$DR/usr/share/applications" \
        "$DR/usr/share/icons/hicolor/1024x1024/apps" "$DR/usr/share/pixmaps" "$DR/usr/share/doc/zarecomp"
    cp -R "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$DR/opt/zarecomp/"
    printf '#!/bin/sh\nexec /opt/zarecomp/bin/lasrecomp "$@"\n' > "$DR/usr/bin/zarecomp" && chmod 755 "$DR/usr/bin/zarecomp"
    sed 's/^Exec=.*/Exec=zarecomp/' "$APPDIR/zarecomp.desktop" > "$DR/usr/share/applications/zarecomp.desktop"
    cp docs/icon/icon_1024.png "$DR/usr/share/icons/hicolor/1024x1024/apps/zarecomp.png"
    cp docs/icon/icon_1024.png "$DR/usr/share/pixmaps/zarecomp.png"  # (menus that skip hicolor's 1024 size)
    cp LICENSE "$DR/usr/share/doc/zarecomp/copyright"
    chmod -R u+rwX,go+rX,go-w "$DR"  # (root-owned files: writable by root only, whatever the umask was)
    GLIBC=$({ find "$DR/opt" -type f \( -name '*.so*' -o -perm -u+x \) -exec objdump -T {} + 2>/dev/null || true; } |
        grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | sed 's/GLIBC_//')  # (|| true: zig's scripts aren't ELF files)
    [ -n "$GLIBC" ] || { echo "dist-linux: no glibc version found for the .deb"; exit 1; }
    MAINT=${DEB_MAINTAINER:-ZA Recomp <noreply@github.com>}
    cat > "$DR/DEBIAN/control" <<EOF
Package: zarecomp
Version: $DV
Architecture: amd64
Maintainer: $MAINT
Installed-Size: $(du -sk --exclude=DEBIAN "$DR" | cut -f1)
Depends: libc6 (>= $GLIBC), libstdc++6, libgcc-s1, libvulkan1
Recommends: mesa-vulkan-drivers | vulkan-icd
Section: games
Priority: optional
Homepage: https://github.com/Hexveil404/PokeZ-ARecomp
Description: Pokémon Legends: Z-A, recompiled for PC (bring your own game files)
 ZA Recomp is a static-recompilation PC port. No game code or game files are
 included: the start screen reads your own .xci / .nsp with your prod.keys and
 builds the game code on this machine. Data, saves and settings live in
 ~/.local/share/zarecomp. Needs a Vulkan 1.3 driver and a CPU with AVX2.
EOF
    dpkg-deb --root-owner-group -Zxz --build "$DR" "$DOUT" >/dev/null  # (xz: older dpkg can't read zstd)
    ls -la "$DOUT"
fi
