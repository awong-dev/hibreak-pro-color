# The pixel-packing problem — start here

**Read this first if you are picking up the GSI display work.**
Background: `docs/gsi-display-analysis.md`. Device facts: `CLAUDE.md` §3.

---

## The one-paragraph version

A GSI boots on this device (Android 16 on the Android 12 vendor, root via phh
su). Android composes a **correct** image — verified by `screencap`. The panel
refreshes on demand. But the picture on the glass is the frame repeated **four
times** and confined to about a quarter of the panel. The factor is exactly 4,
and the only 4 in the panel's configuration is `eink_ldl = 412 = 1648/4` — the
EPD source driver takes **four pixels per data unit** and is being fed **one
byte per pixel**. It is a packing mismatch, not colour, rotation or compression.

---

## The device is currently on stock

The session ended with a full revert. To resume you must rebuild the GSI state:

```bash
# 1. GSI system  (~2 min)
adb reboot bootloader
fastboot reboot fastboot
fastboot flash system work/gsi/td16-system.img

# 2. dm-verity OFF again — the revert re-enabled it, and a GSI will not
#    boot with it on. Flags precede `flash`.
fastboot --disable-verity --disable-verification flash vbmeta_a        work/backup/out/vbmeta_a.bin
fastboot --disable-verity --disable-verification flash vbmeta_system_a work/backup/out/vbmeta_system_a.bin
fastboot --disable-verity --disable-verification flash vbmeta_vendor_a work/backup/out/vbmeta_vendor_a.bin

# 3. vendor with the SELinux property-context fix (optional but recommended:
#    without it the composer cannot manage its own state — 51 avc denials)
fastboot flash vendor work/super/vendor_a-xrzfix2.img

fastboot -w
fastboot reboot
```

Expect a factory-reset prompt (`/data` belongs to stock). First boot is slow.
Root is phh's built-in `su` — no Magisk needed. If ADB root is refused, that is
a Magisk-style settings toggle and does not apply here; phh su should just work.

Back to stock at any time: `docs/revert-to-stock.md`.

---

## The evidence

### Android's output is correct
`adb shell su -c 'screencap -p /data/local/tmp/s.png'` produces a clean
824×1648 UI. Everything wrong is **below** Android. This is the single most
useful fact — it eliminates the entire framework and app layer.

### The geometry decodes to 4:1
The panel is physically **landscape** (`eink_width=1648, eink_height=824`). Held
portrait, its X axis runs top-to-bottom in a photo. So "four tiles stacked
vertically" means four identical copies **side by side along X**, filling about
the first quarter of Y — i.e. 4× too much data consumed per line, buffer
exhausted after ¼ of the rows.

### It is not colour
An independent report of the identical artefact on the **mono** HiBreak Pro:
<https://xdaforums.com/t/bigme-hibreak-pro-dimensity-900-mt6877-e-ink-phone.4723924/post-90611816>
(never answered). A mono panel has no CFA, so `eink_color_mapping*` and
`eink_process_color_neon` are not involved.

### The panel's data path
From `/sys/firmware/devicetree/base/eink/`:

```
eink_width 1648   eink_height 824    eink_fresh_hz 85   eink_max_fps 15
eink_ldl   412    eink_data_len 8    eink_bit_num 5
line   lsl 12  ldl 412  lel 46  lbl 8  lgonl 412
frame  fsl 1   fdl 824  fel 15  fbl 4
```

`ldl 412 = 1648/4` with an 8-bit bus ⇒ 4 pixels per clock, 2 bits per pixel.

### Where the conversion lives
`hwcomposer.mt6877.so` (vendor, kept by a GSI) contains Bigme's own code:

```
xrz::EffectHandler::convertToGray(void*, void*, android::Rect, int, int)
xrz::EffectHandler::imageSmoothing(void*,void*,void*,void*, Rect, int, int)
OverlayEngine::convertLayerToGray(OverlayPortParam*, CommitInfo*)
OverlayEngine::triggerEink(sp<FrameInfo>)
DrmDevice::triggerEinkCommit(CommitInfo*)
DRM_IOCTL_EINK_UPDATE
```

Kernel side (`/proc/kallsyms`, all present under the GSI):

```
drm_eink_update_ioctl   drm_eink_get_type_ioctl   drm_eink_reload_waveform_ioctl
drm_eink_wait_vsync_ioctl   drm_eink_create_user_fence_ioctl
eink_process / _index / _neon      eink_convert_rgb888_gray_neon
eink_convert_rgba_to_rgb16_neon    set_eink_dither_mp   remap_eink_mode
```

So both the conversion routine **and** the ioctl API survive a GSI. Something
upstream is not invoking them, or is invoking them with wrong parameters.

---

## Ruled out — do not repeat these

| tried | outcome |
| --- | --- |
| `ro.sf.hwrotation` 0/90/180/270 | no change. Stock's geometry is *identical* to the GSI's — `real 824x1648`, `installOrientation ROTATION_270`, rotation 0 |
| AFBC / decompress props (`vendor.debug.hwc.disp_support_decompress`, `debug.mediatek.disp_decompress`) | no change. **Warning:** three 1648×824 framebuffers coexist; sampling a different one produced a false "format changed" conclusion |
| `debug.renderengine.backend=skiavkthreaded` | SF crash-loops in `SkiaGpuContext::MakeVulkan_Ganesh` |
| TrebleApp → disable hardware composer | no change |
| Seeding all 26 `vendor.xrz.*` from stock, composer restarted after | no change |
| `vendor.xrz.*` via `/vendor/build.prop` | **does not load** — init honours only `ro.*` there |
| `vendor.xrz.*` SELinux context fix | **real defect fixed** (51 denials → 0) but tiling unchanged. Keep it anyway: `patches/vendor-xrz-property-context.md` |
| `setenforce 0` | refused — this kernel has permissive mode compiled out |

Stock also proved these are **not** different from the GSI: client target size
(`1648×824`), pixel format (`RGBA_8888`, `default-format=1`), composition mode
(`usesClientComposition=true`), density, rotation. Reference dumps are in
`work/research/stock-ref/`.

One genuine difference remains unexplained: stock composes **8 CLIENT + 5
DEVICE** layers; the GSI does **100% CLIENT** — the vendor HWC accepts no layer
for hardware composition.

---

## Next experiments, best first

### 1. Measure stock's framebuffer pitch during a live commit
The decisive number. If `eink_ldl=412` is right, stock's packed line should be
far shorter than the GSI's 1648 bytes, and the ratio is the answer.

Needs root on stock — reflash `work/root/magisk_patched-30700_TKoNu.img`
(sha `35dc44d5…`) plus `work/releases/system-debloat-v1.img`, install
`work/root/Magisk-v30.7.apk`, set Superuser Access to "Apps and ADB".

```bash
adb shell su -c 'cat /sys/kernel/debug/dri/0/framebuffer'   # format/modifier/pitch
adb shell su -c 'cat /sys/kernel/debug/dri/0/state'          # plane geometry
```

⚠️ `crtc=(null)` when idle — e-ink only attaches a buffer during a refresh, so
sample *while* the screen updates.

### 2. Reverse `DRM_IOCTL_EINK_UPDATE`
`hwcomposer.mt6877.so` calls it and `mtk_commit(drm_eink_update)` names the
struct. Recovering the ioctl number and struct layout would let a userspace
program drive the panel directly, bypassing SurfaceFlinger entirely — the
long-term answer for objectives 4 **and** 5, and it is offline work on the
kernel in `work/backup/out/boot_a.bin`.

```bash
# kernel is gzip at offset 2048 in boot_a.bin; see the session notes
python3 -c "..."; strings vmlinux.bin | grep -i eink
```

### 3. Why does the vendor HWC reject every layer?
Stock gets 5 DEVICE layers, the GSI gets 0. `HWCDisplay::validate` and
`getClientTargetSupport` are the entry points in `hwcomposer.mt6877.so`.
If the HWC accepted layers, it would do its own packing.

### 4. Frame dumps from the kernel
`save_level` writes to **`/data/bmp/`** (hardcoded; create it first or it
silently does nothing). Level `1` produced no output — the level semantics are
not understood. Writing `/sys/...` is **RED** (CLAUDE.md §2.1).

---

## Tools

| | |
| --- | --- |
| `bin/fbtry [prop val]` | set a property, restart composer+SF, decode every DRM framebuffer |
| `bin/apply-xrz-props` | seed all 26 `vendor.xrz.*` from stock's captured values |
| `docs/framebuffer-tinkering.md` | full manual command reference, including traps |
| `work/research/stock-ref/` | stock's props, `dumpsys display`, `dumpsys SurfaceFlinger` |
| `work/research/gsi-debug/` | GSI's SF stack trace, dmesg, graphics props |

⚠️ CLAUDE.md §7.5 says to disable HW overlays (`service call SurfaceFlinger 1008
i32 1`). On **this** device that *forces GPU composition* — the path that hung.
That advice is from the mono model. Stock's `1004`/`1005` refresh calls come
from Bigme's `libgui.so` extensions and do not exist on a GSI.
