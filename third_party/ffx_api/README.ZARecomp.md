# AMD FSR SDK API headers (ZA Recomp)

From AMD's FSR SDK v2.4.0 (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v2.4.0`): `Kits/FidelityFX/api/include/`
(`ffx_api.h`, `ffx_api_types.h`, `vk/ffx_api_vk.h`) and `Kits/FidelityFX/upscalers/include/ffx_upscale.h`, with their
`#include` paths flattened. MIT; `LICENSE.md` is the SDK's `Kits/FidelityFX/docs/license.md`, which also covers the signed
DLLs.

`runtime/nvn/vulkan/fsr3.cpp` loads AMD's signed `amd_fidelityfx_loader_vk.dll` (which loads
`amd_fidelityfx_upscaler_vk.dll`) at run time from next to the exe: FSR 3.1's upscaler. Not in git (36 MB):
`tools/za/dist.sh` downloads both from the same tag and checks their SHA-256.
