# hwcshim — HWC2 interposer for the Bigme HWC

**This is what makes a GSI's display work on this device** (2026-09-17). The
vendor HWC pre-converts the client target to gray8 and commits it, but the
kernel reads the committed fd as RGBA (4 B/px) and converts itself — hence the
4× tiling. With `commit_use_ct` (default on) the shim commits the RGBA client
target's fd instead. Details: `docs/pixel-packing-investigation.md`.

Sits between the vendor `composer@2.3-service` and `hwcomposer.mt6877.so`, logs
what SurfaceFlinger hands the HWC and what the HWC hands the kernel, and can
replay Bigme's per-layer `setLayerRefreshMode` (HWC2 descriptor **71**), which
AOSP SurfaceFlinger never sends. Built for the pixel-packing investigation —
`docs/pixel-packing-investigation.md`.

```
tools/hwcshim/build.sh          # NDK -> out/hwcomposer.mtk_common.so
tools/hwcshim/make-vendor.sh    # -> work/super/vendor_a-hwcshim.img (from vendor_a-xrzfix2.img)
```

Installed as `/vendor/lib64/hw/hwcomposer.mtk_common.so`. libhardware tries
`hwcomposer.<ro.hardware.hwcomposer>.so` (`mtk_common`) before
`hwcomposer.<ro.hardware>.so` (`mt6877`), so the shim wins and the original is
untouched. Revert: `fastboot flash vendor work/super/vendor_a-xrzfix2.img`.

## What it hooks

| side | hook | how | what you learn |
| --- | --- | --- | --- |
| IN | `setClientTarget` | HWC2 getFunction patch | client-target gralloc geometry: w/h/**stride**/format/usage/alloc size, dataspace, raw `native_handle` ints |
| IN | `setLayerBuffer`, `setLayer*` | same | per-layer buffer geometry + frame/crop/z/blend/transform |
| IN | `validateDisplay`, `getChangedCompositionTypes` | same | a per-layer table each frame: requested vs. granted composition type |
| IN | `createLayer` | same | applies `refresh_mode` knob via descriptor 71 |
| OUT | `xrz::EffectHandler::convertToGray` | GOT slot rewrite in the real HWC | src/dst pointers (in-place?), rect, **pitch**; row-continuity score of the output at pitch, pitch/4 and width |
| OUT | `OverlayEngine::convertLayerToGray` / `processImage` | GOT | `CommitInfo.format/mode/dither/rect`, source fd, buffer w/h/stride (BuildID-gated struct peeks) |
| OUT | `mtk_commit(drm_eink_update)` + `ioctl` | GOT | the full 68-byte `DRM_IOCTL_EINK_UPDATE` struct: mode, area, dither, **fd, format, stride, height** |
| OUT | `drmModeAddFB2WithModifiers`, `drmModeAtomicCommit`, `drmModeSetCrtc` | GOT | DRM FB fourcc/**pitch**/modifier and what gets committed to the CRTC |
| — | `dump` | HWC2 | counters + last commit in `dumpsys SurfaceFlinger` (search `[hwcshim]`) |

Struct offsets are only used when the loaded HWC's BuildID is
`8a5cbea66efcb94a1a6cfdb53a239598` (firmware `V1.0_20251125`); otherwise those
peeks are skipped and everything ABI-defined still works.

## Knobs (root shell; then `pkill -f composer@2.3`, init respawns it)

```
setprop vendor.debug.hwc.shim.commit_use_ct 0   # 1 = THE FIX (default); 0 = original tiled behaviour
setprop vendor.debug.hwc.shim.commit_stride N   # rewrite drm_eink_update WIDTH (it is not a stride); 0 = leave
setprop vendor.debug.hwc.shim.commit_height N   # rewrite HEIGHT; 0 = leave
setprop vendor.debug.hwc.shim.commit_mode N     # rewrite EinkRefreshMode; -1 = leave
setprop vendor.debug.hwc.shim.commit_format N   # rewrite +0x2c (the kernel ignores it); -1 = leave
setprop vendor.debug.hwc.shim.log 2             # 0 quiet, 1 default, 2 verbose (every call)
setprop vendor.debug.hwc.shim.refresh_mode 178  # push EinkRefreshMode NORMAL to every layer via desc 71
setprop vendor.debug.hwc.shim.refresh_mode -1   # (default) never call it -> baseline
setprop vendor.debug.hwc.shim.preview 2         # ASCII-preview the next 2 gray conversions / commits
setprop vendor.debug.hwc.shim.disable 1         # pure passthrough (needs composer restart)
```

All knobs are re-read on every `validateDisplay` and every commit, so they take
effect on the next frame without a restart. `preview` is consumed as it is used.

## Reading the log

```
adb logcat -s hwcshim
```

Startup lines to check first:

- `real HWC BuildID … -> struct peeks ENABLED`
- `real HWC descriptors present: … 71 … (71 = Bigme setLayerRefreshMode is PRESENT)`
- `GOT hooks installed: N slots` — expect ≥ 9; each `hook … slots=` line should be ≥ 1

Then the pitch question, which is the point of all this:

- `client target: 1648x824 stride=S …` — what SF allocated (`bytes/row@32bpp`, `@8bpp`).
- `convertToGray IN: … pitch=P in_place=1` and `OUT: … row-continuity score … pitch/4:a pitch:b` —
  a **low** score at `pitch/4` means the HWC wrote compact gray8 (1 byte/px).
- `EINK_UPDATE(mtk_commit): … fd=F format=X stride=S height=H` — what the kernel is told.
- With `preview`, two ASCII renderings of the committed buffer at `stride` and `stride*4`:
  the one that looks like the screen is the pitch the content is at. Four small
  copies side by side = the buffer is being read at 4× the pitch it was written.
- `drmModeAddFB2: … fmt=XXXX pitches=[…] (pitch/w=… B/px)` — if a frame goes out
  via the plane path instead of the ioctl, this is the pitch the scanout uses.

## Safety

No device writes happen from this directory. Flashing `vendor_a-hwcshim.img`
is RED (CLAUDE.md §2.1). If the composer crash-loops after install, the display
is dead but adb still works: `setprop vendor.debug.hwc.shim.disable 1` then
`pkill -f composer@2.3`, or reflash `vendor_a-xrzfix2.img`.

**Do not `cp` a rebuilt shim into `/vendor` on the device.** It truncates to
0 bytes and fails `ENOSPC` regardless of free space, and a 0-byte module stops
the composer loading any HWC on restart. Reflash `vendor` (≈1 min).
