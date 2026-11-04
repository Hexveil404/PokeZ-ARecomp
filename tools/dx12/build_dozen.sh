#!/bin/bash
# DirectX 12 for ZA Recomp: Mesa's Dozen (Vulkan on Direct3D 12) with this game's fixes, and what it needs next to the
# exe, into build/dx12/ (tools/za/dist.sh copies it into the release):
#   build/dx12/dx12/   vulkan_dzn.dll, dzn_icd.x86_64.json, dxil.dll and the MSYS2 DLLs vulkan_dzn.dll links
#   build/dx12/D3D12/  D3D12Core.dll of the Direct3D 12 Agility SDK (the exe exports D3D12SDKVersion 620 / D3D12SDKPath:
#                      runtime/platform/windows.c), which has the host-pointer heaps Dozen's memory import needs
# Needs: MSYS2 UCRT64 with clang, meson, ninja, python-mako, python-yaml, bison, flex; git; 7-Zip.
# The fixes (tools/dx12/dozen-za.patch, Mesa 26.2.5):
#   - dzn_BindBufferMemory2 placed buffers on host-pointer heaps without the ALLOW_CROSS_ADAPTER flag it computed
#     (the guest memory import failed: the world drew nothing);
#   - buffer descriptors' custom views, kept until their buffer is destroyed, used up the 1M-slot bindless heap within
#     a minute when descriptors move through a long-lived ring: storage buffers take the base view, uniform buffers
#     views on 32 KB steps;
#   - a 3D image has no array slices in Direct3D 12: barriers over a layered pass's slices named subresources that
#     don't exist (the command list was refused).
set -e
cd "$(dirname "$0")/../.."
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
MESA=26.2.5 SHA=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5 AGILITY=1.620.1
W=build/dx12-work O=build/dx12
CURL=/c/Windows/System32/curl.exe  # (MSYS2's curl fails on some certificate setups)
Z7="/c/Program Files/7-Zip/7z.exe"
mkdir -p $W
[ -f $W/mesa-$MESA.tar.xz ] || $CURL -fsSL -o $W/mesa-$MESA.tar.xz https://archive.mesa3d.org/mesa-$MESA.tar.xz
echo "$SHA  $W/mesa-$MESA.tar.xz" | sha256sum -c -
if [ ! -d $W/mesa-$MESA ]; then
    tar -xJf $W/mesa-$MESA.tar.xz -C $W
    (cd $W/mesa-$MESA && git apply -p1 "$OLDPWD/tools/dx12/dozen-za.patch")
fi
[ -d $W/build ] || meson setup $W/build $W/mesa-$MESA --buildtype=release -Dvulkan-drivers=microsoft-experimental \
    -Dgallium-drivers= -Dplatforms=windows -Dllvm=disabled -Dopengl=false -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dglx=disabled -Dgbm=disabled -Dvideo-codecs= -Dmicrosoft-clc=disabled -Dspirv-to-dxil=false -Dbuild-tests=false \
    -Dvulkan-layers= -Dtools=
ninja -C $W/build src/microsoft/vulkan/vulkan_dzn.dll
rm -rf $O && mkdir -p $O/dx12 $O/D3D12
cp $W/build/src/microsoft/vulkan/vulkan_dzn.dll $O/dx12/
printf '{\n    "ICD": {\n        "api_version": "1.1.354",\n        "library_arch": "64",\n        "library_path": ".\\\\vulkan_dzn.dll"\n    },\n    "file_format_version": "1.0.1"\n}\n' > $O/dx12/dzn_icd.x86_64.json
# the MSYS2 DLLs it links (and theirs), next to it: the loader loads its dependencies from its own folder
need=$(objdump -p $O/dx12/vulkan_dzn.dll | awk '/DLL Name/ {print $3}')
while [ -n "$need" ]; do
    set -- $need; d=$1; shift; need="$*"
    [ -f /c/msys64/ucrt64/bin/$d ] && [ ! -f $O/dx12/$d ] || continue
    cp /c/msys64/ucrt64/bin/$d $O/dx12/
    need="$need $(objdump -p /c/msys64/ucrt64/bin/$d | awk '/DLL Name/ {print $3}')"
done
# dxil.dll (Microsoft's DXIL validator and signer, which Dozen loads): from the same Mesa release's Windows build
[ -f $W/mesa3d-$MESA-release-msvc.7z ] || $CURL -fsSL -o $W/mesa3d-$MESA-release-msvc.7z \
    https://github.com/pal1000/mesa-dist-win/releases/download/$MESA/mesa3d-$MESA-release-msvc.7z
"$Z7" e -y -o$O/dx12 $W/mesa3d-$MESA-release-msvc.7z x64/dxil.dll > /dev/null
# the Agility SDK's D3D12Core.dll (its "distributable files")
[ -f $W/agility-$AGILITY.nupkg ] || $CURL -fsSL -o $W/agility-$AGILITY.nupkg \
    https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/$AGILITY/microsoft.direct3d.d3d12.$AGILITY.nupkg
"$Z7" e -y -o$O/D3D12 $W/agility-$AGILITY.nupkg build/native/bin/x64/D3D12Core.dll > /dev/null
"$Z7" e -y -o$O $W/agility-$AGILITY.nupkg LICENSE.txt > /dev/null && mv $O/LICENSE.txt $O/D3D12-Agility-LICENSE.txt
cp $W/mesa-$MESA/docs/license.rst $O/Mesa-LICENSE.rst
ls -la $O/dx12 $O/D3D12
