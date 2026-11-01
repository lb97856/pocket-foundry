# Pocket Foundry — Mali GPU Edition (`Mali-Adreno 24.2` + `PanVK v26.3.1`)

[![Release](https://img.shields.io/github/v/release/lb97856/pocket-foundry?label=Release%20%28Mali%20Edition%29&color=orange)](https://github.com/lb97856/pocket-foundry/releases/tag/v0.7.4-mali.1)
[![Downloads](https://img.shields.io/github/downloads/lb97856/pocket-foundry/total?color=brightgreen)](https://github.com/lb97856/pocket-foundry/releases)
[![Android 10+](https://img.shields.io/badge/Android-10%2B-3DDC84?logo=android&logoColor=white)](#getting-started)
[![GPU Support](https://img.shields.io/badge/GPU-ARM%20Mali%20%7C%20Adreno%20%7C%20Xclipse-0091bd)](#supported-gpus--architecture-matrix)
[![License](https://img.shields.io/github/license/lb97856/pocket-foundry)](LICENSE)
[![StandWithUkraine](https://raw.githubusercontent.com/vshymanskyy/StandWithUkraine/main/badges/StandWithUkraine.svg)](https://github.com/vshymanskyy/StandWithUkraine/blob/main/docs/README.md)

**Play your own copy of Factorio 2.1.x (Space Age) on Android — now with full hardware-accelerated 56–60 FPS support on ARM Mali GPUs!**
The official Linux ARM64 build of Factorio runs natively on the phone itself via glibc, Mesa Zink, and our custom **Mali-Adreno 24.2 Vulkan Shim + PanVK-kbase `v26.3.1`** driver stack: no CPU emulation, no streaming, and no game files bundled in the app.

---

## Real-Device Performance on Mali-G615 (56–60 FPS / 60 UPS Cap)

Tested on **Xiaomi Poco X6 Pro (MediaTek Dimensity 8300-Ultra, ARM Mali-G615 MC6)** running Factorio 2.1.24 in-game at a steady **56–60 FPS / 60 UPS** (hitting Factorio's native 60 FPS/UPS engine cap; highlighted with arrows in the top-right FPS counter):

| **60 FPS — Active Movement on Coal Patch (Left Stick)** | **60 FPS — Crash Site & Debris Rendering** |
|---|---|
| ![60 FPS on Mali-G615](docs/screenshots/mali_gameplay_60fps.jpg) | ![60 FPS on Mali-G615](docs/screenshots/mali_crashsite_60fps.jpg) |
| **60 FPS — Running Across Terrain** | **59 FPS — Full On-Screen Gamepad Overlay** |
| ![60 FPS on Mali-G615](docs/screenshots/mali_running_60fps.jpg) | ![59 FPS on Mali-G615](docs/screenshots/mali_spawn_59fps.jpg) |

> Full-resolution original captures (`2712×1220`) with FPS highlights are also available in the [`screens/`](screens/) directory.

| Classic Launcher & Setup Views | |
|---|---|
| ![Welcome: Pocket Foundry's blueprint](docs/screenshots/welcome.jpg) | ![Importing: the blueprint built as the import goes](docs/screenshots/importing.jpg) |
| ![Home over a factory drawn from your own copy](docs/screenshots/home.jpg) | ![Playing with the on-screen controller](docs/screenshots/playing.jpg) |

---

## Downloads (Release `v0.7.4-mali.1`)

- **[Download APK (`PocketFoundry-Mali-Adreno-24.2.apk` — 51.7 MB)](https://github.com/lb97856/pocket-foundry/releases/download/v0.7.4-mali.1/PocketFoundry-Mali-Adreno-24.2.apk)** — Ready-to-install Android package with both `Mali-Adreno 24.2` and `PanVK v26.3.1` pre-bundled out of the box.
- **[Download Standalone Driver (`Mali-Adreno-24.2.0-PocketFoundry-Glibc.zip` — 8.7 MB)](https://github.com/lb97856/pocket-foundry/releases/download/v0.7.4-mali.1/Mali-Adreno-24.2.0-PocketFoundry-Glibc.zip)** — Importable glibc driver archive (`Settings -> Driver -> Import`) for existing Pocket Foundry installations.
- **[Release Notes & SHA-256 Checksums](https://github.com/lb97856/pocket-foundry/releases/tag/v0.7.4-mali.1)**

---

## Architecture of the `Mali-Adreno 24.2` Driver Stack

Factorio on Linux ARM64 uses OpenGL 3.3+ with heavy BC1–BC7 (`S3TC`/`BPTC`/`RGTC`) compressed sprite atlases. On Android phones with ARM Mali GPUs, the proprietary `/vendor/lib64/egl/libGLES_mali.so` driver is built against Android's Bionic libc and cannot be loaded directly inside a pure Linux `glibc` container without symbol conflicts, while stock open-source PanVK struggles with CSF tiler memory exhaustion and aggressive MediaTek GPU downclocking.

To solve this, Pocket Foundry Mali Edition introduces a **two-layer glibc Vulkan driver stack**:

```text
Factorio 2.1.x (Linux ARM64 ELF, OpenGL 3.3)
   │
   ▼
Mesa Zink (OpenGL-on-Vulkan translation layer)
   │
   ▼
Layer 1: libvulkan_mali_adreno.so (Mali-Adreno 24.2 Vulkan Shim — runtime/mali-adreno/vulkan_adreno_shim.c)
   ├─ Vulkan Feature & Format Bridge (Desktop/Adreno capability spoofing for Zink)
   ├─ Dual-Mode BC1–BC7 Texture Engine (GPU ASTC/Compute passthrough OR Multi-threaded ARM64 NEON CPU decoder)
   ├─ MediaTek /proc/ged Hardware DVFS Booster (locks GPU clock at 836+ MHz during active rendering)
   └─ Multi-VkInstance Reference Counting (prevents premature ICD teardown & GL_OUT_OF_MEMORY 1285)
   │
   ▼
Layer 2: libvulkan_panfrost.so (PanVK-kbase v26.3.1 / build 122i)
   ├─ Direct ioctl backend for ARM Mali kbase kernel driver (/dev/mali0)
   ├─ CSF v10–v13 Incremental Rendering (flushes tiler heap on OOM instead of VK_ERROR_DEVICE_LOST)
   └─ JM v7–v9 Bifrost/Valhall support
   │
   ▼
Android Kernel: /dev/mali0 (Mali-G615 CSF kbase) + /proc/ged (MediaTek GPU Extension Device)
```

### Deep Dive into Driver Components ([`vulkan_adreno_shim.c`](runtime/mali-adreno/vulkan_adreno_shim.c))

#### 1. Vulkan Capability & Format Bridge
Mesa Zink requires desktop-class Vulkan features and format bits to expose a compliant OpenGL 3.3 context to Factorio. `libvulkan_mali_adreno.so` intercepts `vkGetPhysicalDeviceFeatures`, `vkGetPhysicalDeviceFeatures2`, `vkGetPhysicalDeviceProperties`, and `vkGetPhysicalDeviceFormatProperties` to enable and bridge:
- `textureCompressionBC`, `shaderClipDistance`, `shaderCullDistance`, `dualSrcBlend`, `fillModeNonSolid`, `wideLines`, `largePoints`, `samplerAnisotropy`, `multiViewport`, `independentBlend`, `logicOp`, and `robustBufferAccess`.
- Full format feature flags (`SAMPLED_IMAGE`, `BLIT_SRC`, `TRANSFER_SRC`, `TRANSFER_DST`, `SAMPLED_IMAGE_FILTER_LINEAR`) across all 16 Vulkan BC1–BC7 block-compressed formats (`VK_FORMAT_BC1_RGB_UNORM_BLOCK` through `VK_FORMAT_BC7_SRGB_BLOCK`).

#### 2. Dual-Mode BC1–BC7 Texture Decompression Engine
ARM Mali GPUs natively decode `ASTC` and `ETC2` in hardware, but lack dedicated silicon for desktop `BC4`/`BC5`/`BC6H`/`BC7` formats used by Factorio 2.1.x sprite atlases. Our driver provides two selectable profiles in **Settings -> Driver**:
- **`Mali-Adreno 24.2 (GPU Boost)` *(Default)***: Detects that the underlying `PanVK-kbase v26.3.1` backend natively advertises BCn emulation via GPU compute/ASTC shaders (`g_real_supports_bcn = 1`) and passes compressed textures zero-copy straight to the GPU.
- **`Mali-Adreno 24.2 (CPU BC1-BC7)` *(Forced via `VULKAN_MALI_CPU_BCN=1`)*:** Intercepts `vkCreateImage`, `vkGetImageMemoryRequirements`, `vkCmdCopyBufferToImage`, and `vkCmdCopyBufferToImage2`, transparently substituting `R8G8B8A8_UNORM` / `R8G8B8A8_SRGB` images on the GPU and decoding BC1, BC2, BC3, BC4, BC5, BC6H, and BC7 blocks in parallel across an **8-thread ARM64 NEON worker pool**. Ideal for older Mali GPUs or devices with 4–6 GB RAM.

#### 3. MediaTek `/proc/ged` Hardware GPU Clock Booster (`836+ MHz`)
Because Factorio is a 2D isometric engine, CPU-side entity updates (UPS) and GPU draw calls alternate in bursts. On MediaTek Dimensity/Helio chips, the kernel DVFS governor misinterprets these brief gaps and downclocks the Mali GPU to ~300 MHz, causing stutter (30–40 FPS).
- `libvulkan_mali_adreno.so` opens `/proc/ged` at initialization and dispatches periodic kernel boost packets during `vkQueueSubmit` / `vkQueuePresentKHR`.
- This keeps the Mali-G615 GPU frequency boosted at **836+ MHz**, delivering a rock-solid **56–60 FPS** (locked at Factorio's 60 FPS/UPS engine cap).
- On non-MediaTek chips (Google Tensor, Samsung Exynos), `/proc/ged` is absent and the booster cleanly disables itself with zero overhead.

#### 4. CSF Tiler Heap OOM Fix (`PanVK-kbase v26.3.1` Incremental Rendering)
During Factorio startup (at 55%–89% `Loading sprites...`), Factorio composites thousands of sprites into massive 4096×4096 atlases in parallel. On Valhall / 5th Gen CSF GPUs (`Mali-G610`, `G710`, `G615`, `G715`, `G720`, `G925`), older PanVK builds exhausted the kernel CSF tiler heap:
```text
mali 13000000.mali: CSF group 0 tiler heap OOM notification
zink: DEVICE LOST!
```
By pairing our shim with **`PanVK-kbase v26.3.1` (`build 122i`)**, CSF Incremental Rendering automatically flushes and recycles tiler heap chunks mid-renderpass when tiler memory pressure spikes, completing sprite loading cleanly at `Medium` or `High` quality.

#### 5. Save-Load Deadlock Prevention (`DRIRC_CONFIGURATION`)
In Factorio 2.1.x, enabling `vertexPipelineStoresAndAtomics` on PanVK triggers a shader compiler / CSF queue stall the moment a map finishes loading. Pocket Foundry automatically injects:
```ini
DRIRC_CONFIGURATION=pan_enable_vertex_pipeline_stores_atomics=false
```
for all Mali GPUs so saves load and run without freezing.

#### 6. Multi-`VkInstance` Reference Counting
During startup and window creation, SDL2 and Mesa Zink create a probe `VkInstance`, create the main rendering `VkInstance`, and then destroy the probe instance. Naive shims call `dlclose()` on the backend ICD as soon as the first `vkDestroyInstance` runs, corrupting the active GL context (`GL_OUT_OF_MEMORY (1285)`). Our shim atomically refcounts `g_active_instances` and keeps `libvulkan_panfrost.so` resident until the final instance is destroyed.

### Driver Environment Variables Reference

You can customize driver behavior in **Settings -> Driver -> Options**:

| Variable | Default on Mali | Description |
|---|---|---|
| `DRIRC_CONFIGURATION` | `pan_enable_vertex_pipeline_stores_atomics=false` | Disables vertex pipeline stores/atomics in PanVK/Zink to prevent Factorio 2.1.x map-load freezes. |
| `VULKAN_MALI_CPU_BCN` | `0` (`1` in CPU BC1-BC7 profile) | Set to `1` to force multi-threaded ARM64 NEON CPU decompression of BC1–BC7 textures into `RGBA8`. |
| `VULKAN_MALI_SPOOF_ADRENO` | `0` | Set to `1` to spoof `Adreno (TM) 740` (`vendorID=0x5143`, `deviceID=0x43050a01`) in `VkPhysicalDeviceProperties`. |
| `VULKAN_MALI_REAL_ICD` | `/usr/lib/libvulkan_panfrost.so` | Path to the underlying Vulkan ICD loaded by `libvulkan_mali_adreno.so`. |
| `PANVK_DEBUG` | *(auto)* | PanVK backend debug flags (e.g., `noafbc`, `bc_compute`, `sync`). |

---

## Getting Started

1. Install **[`PocketFoundry-Mali-Adreno-24.2.apk`](https://github.com/lb97856/pocket-foundry/releases/download/v0.7.4-mali.1/PocketFoundry-Mali-Adreno-24.2.apk)** from [Releases](https://github.com/lb97856/pocket-foundry/releases/tag/v0.7.4-mali.1).
2. Buy Factorio and download the **Linux (arm64) `.tar.xz`** archive from [factorio.com/download](https://factorio.com/download).
3. Open Pocket Foundry, tap **Import Factorio**, and pick the `.tar.xz` file.
4. On Mali phones, **Settings -> Driver** is pre-configured to **`Mali-Adreno 24.2 (GPU Boost)`** with **Medium** sprite quality and `DRIRC_CONFIGURATION=pan_enable_vertex_pipeline_stores_atomics=false`.
5. Tap **Continue** or **Main menu** and enjoy 56–60 FPS!

Requires Android 10 or later.

---

## Supported GPUs & Architecture Matrix

| GPU Family | Architectures / Models | Found in | Driver Used | Status |
|---|---|---|---|---|
| **ARM Mali (CSF v10–v13)** | **Mali-G615**, G610, G710, G310, G715, Immortalis-G715, G620, G720, Immortalis-G720, G625, G725, Immortalis-G925, G1-Ultra/Pro/Premium | MediaTek Dimensity 8100 / 8200 / **8300** / 8400 / 9000 / 9200 / 9300 / 9400, Tensor G2/G3/G4 | **Bundled `Mali-Adreno 24.2` + `PanVK v26.3.1`** | **56–60 FPS (60 FPS cap)** (Verified on Mali-G615) |
| **ARM Mali (JM v7–v9)** | Mali-G57, G68, G77, G78, G78AE, G51, G52, G71, G72, G76 | Dimensity 700–1200, Helio G85–G99, Exynos 990/2100, Tensor G1 | **Bundled `Mali-Adreno 24.2`** (or `CPU BC1-BC7` profile) | Supported |
| **Qualcomm Adreno 6xx–8xx** | Adreno 618–660, 730–750, 830–840 | Snapdragon 7xx / 8xx / 8 Gen 1–3 / 8 Elite | **Bundled `Turnip`** | **60 FPS** |
| **Samsung Xclipse** | Xclipse 920, 940, 950 | Exynos 2200, 2400, 2500 (Galaxy S22–S25) | Community `RADV-Xclipse` | **60 FPS** |
| **Other / Legacy GPUs** | PowerVR, Adreno 5xx | Pixel 10, older phones | Bundled `CPU (llvmpipe)` | Fallback |

---

## Documentation

- **[Phones and drivers](docs/phones.md)** — Driver details, Mali options, and tested devices.
- **[Playing](docs/playing.md)** — Touch controls, Bluetooth/USB gamepads, keyboards, settings, saves, and mods.
- **[Building and how it works](docs/building.md)** — How to compile the glibc runtime, Mesa, `vulkan_adreno_shim.c`, and the Android APK.

---

## Licence

GPL-3.0, as Termux:X11 is. Also bundled: glibc (LGPL-2.1), Mesa / PanVK / Turnip (MIT), SDL (zlib), liblzma (0BSD), Titillium Web (OFL-1.1), Apache Commons Compress and AndroidX (Apache-2.0).

Factorio is © Wube Software. Pocket Foundry isn't affiliated with Wube and includes none of its files; the game screenshots, the launcher's panels and buttons, and the Home screen show art from the player's own copy.
