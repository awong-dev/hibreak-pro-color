# The 4× tiling on a GSI — SOLVED (2026-09-17)

**Objective 4 is reached**: TrebleDroid Android 16 boots on this device and the
Kaleido panel shows the UI correctly (greyscale), via a small HWC2 interposer in
`/vendor` — `tools/hwcshim/`. Nothing in `/system` is patched.

Background and the earlier (partly wrong) reasoning: `docs/gsi-display-analysis.md`.

---

## What was actually wrong

On a GSI, Bigme's vendor HWC (`hwcomposer.mt6877.so`) takes the RGBA8888 client
target from SurfaceFlinger, converts it to **gray8 on the CPU**
(`OverlayEngine::processImage` → `xrz::EffectHandler::convertToGray`, into a
separate 1648-pitch buffer) and commits *that* buffer with
`DRM_IOCTL_EINK_UPDATE`.

The kernel's update path (`drm_eink_update_ioctl` → `eink_pre_process` →
`0xffffffac48c3f8c4`) does **not** look at any format field. It reads the
committed fd as **4 bytes per pixel, pitch = width × 4**, and runs its own
`eink_convert_rgb888_gray_neon`. Feeding it gray8 makes every 4 gray pixels
collapse into one → four ¼-width copies side by side, ¼ of the rows → exactly
the artefact seen here and on the mono model.

So stock commits the **RGBA client target itself**. Which HWC state makes stock
skip the CPU conversion is still unknown (`CommitInfo.format` at +0x54 gates
it — `processImage` skips `convertToGray` when it is non-zero — but no store to
that field was found; it is presumably set through a per-layer path Bigme's
SurfaceFlinger drives). It does not matter: the shim substitutes the client
target's fd in the commit and the picture is correct.

### The ioctl struct, as the kernel reads it

`DRM_IOCTL_EINK_UPDATE = 0xC04464CF` (`DRM_IOWR(0x40+0x8f, 68)`), `0xC00464D1`
is `wait_vsync`.

```
+0x00 mode           EinkRefreshMode (0xb2 NORMAL on the GSI; remapped by remap_eink_mode)
+0x04..+0x10 rect    left, top, right, bottom (inclusive)
+0x14 dither
+0x1c,+0x20,+0x24    overridden from kernel globals when those are non-zero (eink_pre_process)
+0x28 fd             dma-buf / ion share fd of the *RGBA* source
+0x2c                NOT READ by the kernel (the HWC writes CommitInfo.format here)
+0x30 x, +0x34 y     source offset
+0x38 width, +0x3c height   the kernel clamps the rect to [x, y, x+w-1, y+h-1]
```

`stride=412` → a 412-px-wide strip (that experiment produced one full-height
copy squished 4:1, confirming both the 4 B/px reading and the width semantics).
`format` 1/2/3 → no change, confirming the field is ignored.

The kernel picks between two pipelines on `disp_get_eink_panel_type()`:
panel_type `0` (device tree `eink_panel_type=0`, and the debugfs static is
unset `-1`) → the greyscale path above; non-zero → a colour path
(`0xffffffac48c40740`, which reads the same width/height/x/y fields). **That
selector is the lead for objective 5** — stock presumably sets the panel type
(`eink_set_panel_type`, debugfs `panel_type` write handler, or from the
waveform info) before colour mapping engages.

### What did *not* matter

`eink_ldl = 412` (the panel's 4-px-per-clock bus) — coincidence; the packing
below the ioctl is identical on stock and GSI. `ro.sf.hwrotation`, AFBC props,
`vendor.xrz.*` props, HWC layer acceptance (0 DEVICE layers on the GSI is
still true and still harmless), the vbmeta/dm-verity state.

---

## The fix: `tools/hwcshim`

An HWC2 interposer installed as `/vendor/lib64/hw/hwcomposer.mtk_common.so`
(libhardware tries `ro.hardware.hwcomposer=mtk_common` before `mt6877`, so the
original is untouched). It dlopens the real HWC, wraps `getFunction`, rewrites
the real HWC's GOT for `mtk_commit`/`ioctl`/`convertToGray`/`processImage`/
libdrm, and — the fix — with `vendor.debug.hwc.shim.commit_use_ct=1` (default)
replaces the committed fd with the RGBA client target's fd captured at
`processImage` entry. Everything else it does is logging; `README.md` there has
the knobs and how to read the log.

Build/package: `tools/hwcshim/build.sh && tools/hwcshim/make-vendor.sh` →
`work/super/vendor_a-hwcshim.img` (= `vendor_a-xrzfix2.img` + the shim).

⚠️ **In-place `cp` into `/vendor` does not work** even with free space: the
copy truncates the file to 0 bytes and fails `ENOSPC`. A 0-byte
`hwcomposer.mtk_common.so` makes the composer unable to load *any* HWC on its
next restart. Always reflash `vendor` instead (≈1 min).

---

## Rebuilding the GSI state from stock

```bash
adb reboot bootloader
# 1. vbmeta: dm-verity OFF, from *bootloader* fastboot (fastbootd cannot see
#    the physical vbmeta partitions -> "No such file or directory")
fastboot --disable-verity --disable-verification flash vbmeta_a        work/backup/out/vbmeta_a.bin
fastboot --disable-verity --disable-verification flash vbmeta_system_a work/backup/out/vbmeta_system_a.bin
fastboot --disable-verity --disable-verification flash vbmeta_vendor_a work/backup/out/vbmeta_vendor_a.bin
# 2. logical partitions from fastbootd
fastboot reboot fastboot
fastboot flash system work/gsi/td16-system.img
fastboot flash vendor work/super/vendor_a-hwcshim.img
fastboot -w
fastboot reboot
```

Then, **every time `/data` is wiped**, SurfaceFlinger wedges in the Mali GL
driver at boot (black screen, `dumpsys SurfaceFlinger` times out, bootanim
never stops). Fix from a root shell (phh `su`) and it comes straight up:

```bash
adb shell "su -c 'setprop persist.sys.phh.enable_sf_gl_backpressure false; setprop persist.sys.phh.enable_sf_hwc_backpressure false; setprop ctl.restart surfaceflinger'"
```

The props persist, but on the *next* boot SF still hung once and needed just
the `ctl.restart surfaceflinger`. Consider that the first thing to try on any
black screen.

Back to stock: `docs/revert-to-stock.md`.

---

## Diagnostics that were decisive

- `adb logcat -s hwcshim`: `client target: 1648x824 stride=1648 fmt=RGBA_8888`,
  `convertToGray IN: … pitch=6592B in_place=0`, the commit struct, and with
  `vendor.debug.hwc.shim.preview N` an ASCII rendering of the committed buffer
  at both candidate pitches (coherent at 1648 → it was 1 B/px gray).
- Kernel disassembly (`work/backup/out/boot_a.bin` → gzip at 2048 →
  `vmlinux.bin`; KASLR base for the `kallsyms-eink.txt` addresses is
  `0xffffffac48480000`, i.e. file offset = addr − base). Wrap the raw Image in
  a minimal ELF and `llvm-objdump` it — the recipe is in the session notes
  under `work/research/gsi-shim/`.
- `commit_stride=412`: one copy, full width, ¼ height — the observation that
  fixed the model.

## Open

- Objective 5. Start from `disp_get_eink_panel_type()` and the kernel's
  colour pipeline entry at `0xffffffac48c40740`; the shim can drive `mode`,
  `dither` and the three global-overridable fields per commit.
- Refresh policy: everything commits as `mode=0xb2` (NORMAL). Descriptor 71
  (`setLayerRefreshMode`) is wired in the shim (`refresh_mode` knob) but
  untested for effect.
- SF's boot-time GL hang on Mali — a phh/Skia issue, not e-ink.
