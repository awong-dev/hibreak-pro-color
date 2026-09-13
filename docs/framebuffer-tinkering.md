# Poking at the e-ink framebuffer by hand

All runtime-only — **a reboot reverts everything here** except `persist.*`.

## The loop

```bash
bin/fbtry                                    # show current state
bin/fbtry <property> <value>                 # set it, restart composer+SF, re-read
```

`bin/fbtry` prints the real DRM framebuffers the panel driver sees, decoded
(fourcc → human format, linear vs compressed, bytes per pixel), plus SF's client
target and whether anything is still alive.

⚠️ **Three 1648x824 framebuffers coexist.** At defaults you will see an
`ABGR8888` compressed one and an `ABGR2101010` linear one from `OverlayEngine_0`,
plus an `RGB332` linear one from the composer's HwBinder thread. Do not assume
the one you are looking at is the one being scanned out — that is a mistake I
made and it produced a false "fixed it" conclusion. Which buffer reaches the
panel is only visible during an actual commit (`/sys/kernel/debug/dri/0/state`
shows `crtc=(null)` when idle, which is most of the time on e-ink).

## Properties worth trying

Composition / compression (MediaTek HWC):

| property | values | notes |
| --- | --- | --- |
| `vendor.debug.hwc.disp_support_decompress` | 0 / 1 | claim the display can(not) decompress AFBC |
| `vendor.debug.hwc.mdp_support_decompress` | 0 / 1 | same for the MDP path |
| `vendor.debug.hwc.mdp_support_compress` | 0 / 1 | |
| `debug.mediatek.disp_decompress` | 0 / 1 | |

RenderEngine backend (SF):

| value | result observed here |
| --- | --- |
| `skiagl` | default. Hung in Mali `osup_sync_object_wait` until phh GL backpressure was disabled |
| `skiaglthreaded` | untried |
| `skiavk` | untried |
| `skiavkthreaded` | **crash-loops** — aborts in `SkiaGpuContext::MakeVulkan_Ganesh` |

```bash
bin/fbtry debug.renderengine.backend skiaglthreaded
```

Rotation — **`ro.` is write-once**, so changing it needs a reboot first:

```bash
adb shell su -c 'setprop ro.sf.hwrotation 270'   # 0/90/180/270
adb shell su -c 'setprop ctl.restart surfaceflinger'
```

Bigme's own, read by the vendor HWC (`hwcomposer.mt6877.so`). All **unset** on a
GSI; stock seeds them from `ro.vendor.xrz.*`:

```
vendor.xrz.screen_type              61     (stock value — the panel discriminator)
vendor.xrz.default_refresh_mode     178    (NORMAL)
vendor.xrz.global_refresh_mode      178
vendor.xrz.force_global_refresh_mode
vendor.xrz.gray_256_dither
vendor.xrz.driver_color_enhance     0
vendor.xrz.color_restoration_enabled
vendor.xrz.anti_flicker_enabled     1
vendor.xrz.auto_clean_enabled       1
vendor.xrz.disable_eink_vsync
vendor.xrz.fps_limit   vendor.xrz.hwc_idle   vendor.xrz.is_scrolling
```

⚠️ The composer is **denied** permission to set several of these itself:

```
avc: denied { set } for property=vendor.xrz.hwc_idle
     scontext=u:r:hal_graphics_composer_default:s0
     tcontext=u:object_r:vendor_default_prop:s0
```

Bigme put the `vendor.xrz.*` property contexts in *system* sepolicy, which a GSI
replaces. Setting them from a root shell works; the composer still cannot write
its own state. `setenforce 0` does **not** work — this kernel has permissive
mode compiled out.

## Restarting things

```bash
adb shell su -c 'setprop ctl.restart surfaceflinger'
adb shell su -c 'pkill -f composer@2.3'          # respawns automatically
adb shell su -c 'stop; start'                    # whole framework, slow
```

## Reading state

```bash
adb shell su -c 'cat /sys/kernel/debug/dri/0/framebuffer'   # formats/modifiers
adb shell su -c 'cat /sys/kernel/debug/dri/0/state'         # plane/CRTC geometry
adb shell su -c 'cat /sys/kernel/debug/eink_debug/frame_data'
adb shell su -c 'dmesg | grep -E "eink|frame done"'
adb shell dumpsys SurfaceFlinger | head -40
adb shell su -c 'screencap -p /data/local/tmp/s.png'        # what Android *thinks* it drew
```

`screencap` is the one to remember: it shows Android's composition, which is
**correct**. Any difference from the panel is below Android.

## SurfaceFlinger service calls

```bash
adb shell su -c 'service call SurfaceFlinger 1008 i32 1'   # disable HW overlays -> FORCES GPU
adb shell su -c 'service call SurfaceFlinger 1008 i32 0'   # re-enable
```

⚠️ CLAUDE.md §7.5 recommends `1008 i32 1`. On **this** device that forces GPU
composition — the path that hung. That advice is from the mono model. It also
needs root; as shell it returns `Operation not permitted`.

Stock's `1004`/`1005` refresh calls come from Bigme's `libgui.so` extensions and
**do not exist** on a GSI.

## RED — ask before running (CLAUDE.md §2.1)

Writing to `/sys/...` is RED because panel state can persist:

```
adb shell su -c 'echo 1 > /sys/kernel/debug/eink_debug/save_level'
```

The writable `eink_debug` nodes are `anti_alias`, `anti_flicker`, `clean_a2`,
`enable`, `print_level`, `save_level` by mode bits — but the kernel also has
write handlers for `saturation`, `global_dither`, `global_mode`, `wf_ota` and
`manual_refresh` despite their `r--r--r--` bits, and root can override those.

`save_level` writes frame dumps to **`/data/bmp/`** (path is hardcoded in the
kernel; create the directory first or it silently does nothing). Level `1` alone
produced no dumps here — the level semantics are not understood yet.
