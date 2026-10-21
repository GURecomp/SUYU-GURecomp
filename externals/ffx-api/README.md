# ffx-api headers (FSR 4 bridge, [Debug] nvn_fsr_backend = 4)

Only these headers from GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag **v1.1.4**, commit
c6efa6bf7f2027b3ec94f28578bb5965eabb9e55: `ffx-api/include/ffx_api/{ffx_api.h, ffx_api_types.h, ffx_upscale.h,
dx12/ffx_api_dx12.h}` and `LICENSE.txt`. Licence: MIT, "Copyright (C) 2024 Advanced Micro Devices, Inc." (the same block is
at the top of each header). Same copies as `E:\GUU test\fsr4-probe\third_party\ffx_sdk_v1.1.4`. Not the v2.x SDK
(binary-only licence).

No AMD binaries are vendored. At run time `src/video_core/renderer_vulkan/nvn_fsr4.cpp` loads the driver's own
`%SystemRoot%\System32\amd_fidelityfx_dx12.dll` (installed by the AMD driver) and calls its five exported entry points.
