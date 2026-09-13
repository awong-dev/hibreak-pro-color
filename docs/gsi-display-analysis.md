# GSI display analysis — why the panel tiles

Working notes live in `work/research/gsi-attempt-1.md` (gitignored, per-unit
data). This is the tracked summary.

## The tiling decoded (2026-09-13)

An independent report of the identical failure on the **mono** HiBreak Pro:
<https://xdaforums.com/t/bigme-hibreak-pro-dimensity-900-mt6877-e-ink-phone.4723924/post-90611816>
(no reply in that thread). Photo shows a lock screen repeated four times,
small, confined to one edge — the same artefact as here.

**That kills the CFA hypothesis.** The mono panel has no colour filter array, so
`eink_color_mapping*` / `eink_process_color_neon` were never involved. This is
the plain **grayscale packing** path, failing identically on both models.

### Reading the geometry

The panel is physically **landscape** (`eink_width=1648, eink_height=824`). Held
portrait, the panel's X axis runs top-to-bottom in a photo. So "four tiles
stacked vertically" is, in panel coordinates, **four identical copies side by
side along X**, filling roughly the first quarter of Y.

That is: **4× too much data consumed per line, buffer exhausted after ¼ of the
rows.** A clean 4:1 factor.

There is exactly one 4 in this panel's configuration:

```
eink_ldl      = 412 = 1648 / 4     four pixels per clock
eink_data_len = 8                  8-bit bus
eink_bit_num  = 5
```

The EPD source driver consumes **four pixels per data unit**. It is being fed
one byte per pixel where it expects four pixels packed per unit. Not colour, not
rotation, not AFBC compression — **pixel packing**.

### What this rules out

Everything tried on this device and found not to be the cause:

| tried | result |
| --- | --- |
| `ro.sf.hwrotation` 270 | no change — and stock's geometry is identical anyway |
| AFBC decompress properties | no change; the format they appeared to alter was a different buffer |
| `debug.renderengine.backend` vulkan | SF crash-loops in `MakeVulkan_Ganesh` |
| disable hardware composer (TrebleApp) | no change |
| `vendor.xrz.*` seeded from stock | no change |
| `vendor.xrz.*` SELinux context fix | **real defect fixed** (51 denials → 0) but no change to tiling |

And what stock proved is *not* different: client target size, pixel format
(`RGBA_8888`), composition mode, display geometry, rotation, density.

### Where the packing happens

`hwcomposer.mt6877.so` contains Bigme's own code:

```
xrz::EffectHandler::convertToGray(void*, void*, android::Rect, int, int)
OverlayEngine::convertLayerToGray(OverlayPortParam*, CommitInfo*)
xrz::EffectHandler::imageSmoothing(...)
```

plus `vendor.xrz.enable_gpu_image_process` (stock: `1`), which plausibly selects
whether that conversion runs on GPU or CPU. All of this is **vendor** code that
a GSI keeps. So the conversion routine is present — something upstream is not
invoking it, or is invoking it with the wrong parameters.

### Next

The remaining unknown is what stock's `DRM` framebuffer looks like **during an
actual commit** — format, modifier, and crucially pitch. `eink_ldl=412` predicts
a packed line considerably shorter than 1648 bytes. Reading it needs root on
stock, i.e. re-flashing the Magisk-patched `boot_a`
(`work/root/magisk_patched-30700_TKoNu.img`, sha `35dc44d5…`) and catching
`/sys/kernel/debug/dri/0/state` mid-refresh.
