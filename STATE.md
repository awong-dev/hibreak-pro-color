## Status
Phase: **3** (GSI) — **a GSI boots.** Android 16 on the Android 12 vendor,
`boot_completed=1`, root via phh su. Blocker is the **Mali GPU driver**, not
e-ink. See `work/research/gsi-attempt-1.md`.
Backup verified: **y** — restore path **proven** by §5.4
Bootloader: **UNLOCKED** — `unlocked: yes`, `secure: no`, warranty bit tripped
Currently running: stock `Bigme_HiBreak_V1.0_20251125`, slot `_a`, **rooted (Magisk 30.7)**

Everything below happened 2026-09-12, one session.

---

## Done

### Toolchain (no device contact)
- macOS arm64 host set up; details in `TOOLCHAIN.md`, reproducible via
  `tools/bootstrap.sh`.
- `bin/mtk` → mtkclient V2.1.4 (`cd25cf9`), python3.11 venv.
- `bin/hibreak-shell` → `hibreak-tools` Debian container (`./work` at `/work`,
  loopback7084 `f49f973` at `/scripts`). Smoke-tested; loop-mounting ext4 over
  the macOS bind mount works. **Not yet used for real work** — `debugfs` on the
  host turned out to cover everything so far.
- lpunpack/lpmake/lpdump/lpadd/lpflash built native arm64.
- adb/fastboot 37.0.1, scrcpy, binwalk, squashfs-tools, e2fsprogs, coreutils.

### §5.1 pre-flight
- adb authorised; `sys.oem_unlock_allowed=1`.
- Identity, boot state, panel geometry, full `ro.vendor.xrz.*` →
  `work/research/device-identity.md`, `stock-props{,-all}.txt`.
- Confirmed on-device: MT6877, system Android 14, 824x1648 @ 300, slot `_a`,
  and the §7.2 frontlight sysfs path.
- **Corrected three CLAUDE.md facts**: firmware is `V1.0_20251125` (not the 2.x
  line), vendor is Android **12** not 14, and this is **virtual** A/B.

### §5.2/§5.3 backup — complete
- `printgpt` → 62 partitions. `waveform` **exists**. `super` is exactly
  12,884,901,888 bytes, matching the magic number `unpack.sh` checks.
- 61/61 non-userdata partitions dumped, plus `gpt.bin`, `gpt_backup.bin`.
- **Preloader captured** from the UFS boot LUNs via `--parttype boot1`/`boot2`
  (4 MiB each, byte-identical mirrors). Genuine MT6877 `COMBO_BOOT` from
  `B651/mt6877_android14_qt`, carrying an extractable **EMI v54** DRAM config —
  the piece mtkclient could not find on its own. This is what makes
  `--preloader` + BROM mode work, and why its absence would have closed both
  recovery routes at once.
- RPMB **not** captured — mtkclient's UFS RPMB read throws `unpack requires a
  buffer of 12 bytes`. Authenticated and non-restorable by design, so nothing
  lost.
- 65 files, 13 GB. SHA256SUMS recorded and re-verified **65/65 OK, 0 failed**,
  no zero-byte files. `MANIFEST.md`: 0 problems, 0 unclassified.
- 25 files are all-zero — established as the unpopulated B slot plus genuinely
  empty partitions, not failed reads. Later confirmed independently by fastboot
  (`slot-successful:b: no`).

### Gate
- `work/backup/VERIFIED` written, `work/backup` chmod `a-w` (test write denied).
- **Deviation: ONE off-machine copy, not the two §5.2 requires** — put to the
  human, accepted deliberately. Recorded so nobody assumes the stricter rule
  held.
- §5.2's command order is wrong and was fixed in CLAUDE.md: `chmod -R a-w` must
  come *after* `touch VERIFIED`, or the touch fails on a read-only directory.

### §5.4 write path — proven
- `bin/mtk w vbmeta_a work/backup/out/vbmeta_a.bin`. Booted first time, no boot
  quirk needed. `verifiedbootstate=green` afterwards — AVB *validated the
  partition we wrote*, which is stronger than a successful boot alone.

### §6.1 unlock — done
- Fastboot exercised read-only first (`fastboot getvar all` →
  `work/research/fastboot-getvar.txt`). Pre-unlock it read `unlocked: no`,
  `secure: yes`, `warranty: yes`.
  - `is-userspace: no` — LK fastboot, not fastbootd. Logical partitions need
    `fastboot reboot fastboot` first (§7.4 already does this).
  - `max-download-size` 128 MiB — larger images must sparse-split over fastboot.
- `fastboot flashing unlock` **succeeded with no on-screen confirmation prompt
  at all** — no Vol-Up press, contrary to §6.1. Verified by getvar rather than
  trusting the OKAY: `unlocked` no→yes, `secure` yes→no, `warranty` yes→no.
  Worth posting to the HBPC thread.
- `unlock_critical` run. **Cannot be independently verified** — MTK's LK exposes
  no getvar for it. Its real test is the `vbmeta` flash at §7.4; a permissions
  refusal there is the signal it did not take.
- Reboot observed: fastboot(`0x201C`) → preloader(`0x2000`) → Android(`0x2008`)
  in 101 s, coming up **PTP-only with no adb** — Developer Options wiped, which
  independently confirms the factory reset ran.
- Post-unlock: `verifiedbootstate=orange`, `flash.locked=0`,
  `vbmeta.device_state=unlocked`.
- **Skipped §6.1's belt-and-braces `mtk e metadata,userdata` and
  `mtk da seccfg unlock`.** They exist because the standard sequence
  "reportedly needs more" on this model — it did not. Extra RED writes against a
  problem we do not have is risk without benefit, and `da seccfg unlock` touches
  the partition §2.2 singles out as most dangerous. Revisit only if §7.4's
  vbmeta flash is refused.
- Note `md_udc` **does not exist on this device**; §6.1's erase list names it.

### Root — Magisk 30.7
- Patched **`boot`**, not `init_boot`. `init_boot_a` is all-zero and the 11.5 MB
  ramdisk lives in `boot_a` (header **v2**, page 2048, non-GKI) — the opposite
  of standard Android 14 guidance. Checking the header first avoided a wasted
  flash.
- Magisk patched `boot.img` on-device; verified before flashing by decompressing
  both ramdisks: 463 → 472 cpio entries, adding `.backup/.magisk` and
  `overlay.d/sbin/magisk.xz`, kernel byte-identical.
- `fastboot flash boot` → booted first time, no boot quirk needed.
  `verifiedbootstate=orange`, `magisk -v` = `30.7:MAGISK:R`.
- **vbmeta was NOT touched.** `secure: no` on an unlocked bootloader was enough;
  the `--disable-verity --disable-verification` step many MTK guides insist on
  proved unnecessary here.
- Root over ADB was rejected three times (`W Magisk : su: request rejected
  (2000)`). Not a broken install — a Magisk permission setting. Fixed from the
  Magisk UI; `scrcpy` was what made that UI usable, since the e-ink panel
  redraws too poorly to trust toggle state.

### Kernel driver mapped (rooted)
- 360 `eink` symbols from `/proc/kallsyms` → `work/research/kallsyms-eink.txt`.
- **Six DRM ioctls** on `/dev/dri/card0`: update, get_type, **reload_waveform**,
  wait_vsync, create/release_user_fence.
- **CFA colour pipeline is kernel-side NEON**: `eink_process_color_neon`,
  `eink_color_mapping{,_region,_AIE,_AIE_region,_cvt}`,
  `eink_color_enhance_process*`, `set_eink_dither_mp`. `remap_eink_mode` carries
  `disable_regal` / `disable_4bit_switch` — there is a **Regal** path.
- **Waveform comes from LK**, not a file (`eink_init_waveform_from_lk`) — which
  finally explains why `/data/waveform.bin` does not exist.
- **Structural conclusion**: a GSI replaces libgui/SurfaceFlinger/framework but
  replaces none of the above. Panel driver, colour mapping, waveform and the
  ioctl API all survive. A GSI loses the *policy* layer, not the ability to
  drive the panel.

### EInk Center install source — RESOLVED
- `/system/app/xSettings/xSettings.apk` bundles `assets/apk/xMenu.apk`
  (3,151,521 bytes — exact size match for `com.xrz.sys.control`), plus
  StandbyApp, FaceRegister, SouGoInput, DawoYuji, LevBoll. Settings installs
  them on first boot, hence `installerPackageName=com.android.settings`.
- The init `preinstall` service pointing at `/system/preinstall` is a red
  herring — that path does not exist, `pm preinstall` does nothing, and
  `xrz_start.sh` touches `/data/preinstall.done` regardless.
- ✅ **`xSettings` is NOT in the debloat removal list.** The last blocking
  unknown for objective 3 is cleared.

### §6.2 debloat — DONE, BOOTING
- Verified on-device after boot: `bookmall`, `appstore`, `youtube`, `music`
  **removed**; `com.xrz.sys.control`, `res.service`, `mutidisplay` **present**.
  The review's verdict that the removal list is safe for the e-ink stack is now
  confirmed empirically, not just by analysis.
- **It did NOT boot at first.** Cause: dm-verity. See Traps.

### §6.2 debloat — FLASHED
- Ran `tools/debloat/0[2-5]` and flashed `super.new.bin`. Images shrank:
  product 2.62→1.34 GB, system 2.43→2.15 GB, system_ext 738→683 MB,
  vendor 681→669 MB. `lpdump` of the flashed image is structurally sound.
- **Saved for revert**: `work/releases/super-debloat-v1.bin`, sha256
  `f87cb9fb…`, verified byte-identical to what was flashed.
  ⚠️ The first copy attempt was corrupt — correct apparent size, only 2.39 GB
  allocated vs 4.92 GB. Passed a size check, failed the checksum. **Verify
  large copies by checksum, never by `ls`.**

### GSI images staged (not yet flashed)
| image | notes |
| --- | --- |
| `td16-system.img` | TrebleDroid **A16** arm64 vanilla, 2.21 GB. phh patches + TrebleApp. **Chosen.** |
| `a17-system.img` | Google official **A17** (CP41.260814.003.B1), 2.09 GB. No phh patches. |
| `td15-system.img` | TrebleDroid A15 arm64-ab, fallback. |

**VNDK is a non-issue between them**: A15, A16 and A17 *all* lack `vndk-31`,
which our vendor declares (`ro.vndk.version=31`, and vendor ships none itself).
Only A17 lacks VNDK entirely (28/29 in the TrebleDroid builds). Whether the
missing snapshot matters is untested — the linker may fall back. An earlier
claim in this file that VNDK absence *blocks* a GSI was wrong.

### §6.2 review (before flashing)
- `super.bin` unpacked with `lpunpack` in ~10 s → `work/super/`. Inspected with
  `debugfs` — no mount, no container, no root.
- **Review complete → `work/research/debloat-review.md`.** The removal list is
  safe for the e-ink stack; the scripts are not safe to run as-is. Three
  defects, see Open below.
- E-ink stack mapped → `work/research/eink-stack.md` (R2, R6, part of R3).

---

## Open

- R1 waveform partition — **ANSWERED**. `waveform`, 16 MiB at `0x4cd00000`, no
  A/B. MediaTek image (`0x58881688`) wrapping a 6,482,960-byte E Ink `.awf`,
  plus a MediaTek signature block. Panel `EC061KH1C1`, controller `SC1452-FAB`.
  Extracted → `work/research/waveform.awf`.
- R2 runtime waveform path — **ANSWERED**. `/dev/block/by-name/waveform` →
  `/data/waveform.bin` (+`.bak`) → `/sys/kernel/debug/eink_debug/waveform` and
  `machine_waveform`. `/system/bin/xrz_updater --update-waveform` is the writer.
- R3 `eink_debug` enumeration — **ANSWERED**. **23 nodes**, not the 5 inferred
  from strings. Full table with values in `work/research/eink-stack.md`; raw
  captures in `eink_debug-{ls,values}.txt`. Driver is `v4.92_20251104`,
  VCOM −2250 mV, temperature 29 °C, `frame_mode=0x4` (= `GC16` in the mono
  table, so the numbering carries over).
- R4 EInk Center mapping — **ANSWERED**. *Policy*: per-app SQLite DB
  (`com.xrz.eink.display.policy`) seeded from `/system/etc/display_policy`
  (101 KB JSON, 228 packages). *Mechanism*: the debugfs nodes **are** the write
  path — `eink_saturation_write`, `eink_global_dither_write`,
  `eink_global_mode_write`, `eink_wf_ota_write`, `eink_manual_refresh_write` all
  exist in the kernel. The `r--r--r--` mode bits understate the driver; root can
  override. No hidden ioctl. **Writing them is RED.**
- R5 CFA-specific modes — **ANSWERED**. Colour is a **separate enum** from
  `EinkRefreshMode`: `COLOR_MODE_DEFAULT/COMIC/MAGAZINE/VIDEO/CUSTOM`, applied
  **per package**. `0` DEFAULT · `1` MAGAZINE *(elimination only, untested)* ·
  **`2` COMIC ✅ measured** · **`3` VIDEO ✅** (all 11 users are video apps) ·
  `4` CUSTOM. Live DB is `/data/system/disp_policy.db`, table `policy_org`.
  ⚠️ An earlier note here claimed 1=COMIC/2=MAGAZINE "from declaration order";
  that order was actually alphabetical from `sort`, i.e. not evidence, and the
  measurement contradicts it.
- R6 Bigme `libgui.so` — **ANSWERED**, and larger than §7.1 described. Four
  non-AOSP exports: `repaintEverything()`,
  `setLayerRefreshMode(String8 const&, uint)`,
  `Transaction::setRefreshMode(sp<SurfaceControl> const&, int)`, and a new AIDL
  binder method `ISurfaceComposer::remoteSetLayerRefreshMode`. Java side is
  `xrz.framework.manager.XrzEinkManager`, which lives in the framework.

### Blocking the debloat
- 🔴 `repack.sh` omits `lpmake --virtual-ab`; our super's header declares
  `virtual_ab_device`. Plausible brick. One-flag fix.
- 🔴 `repack.sh` silently drops `system_b` (~57 MB of real extents).
- ⚠️ `hosts.txt` is absent from the repo and there is no `set -e`, so the
  telemetry half of `safedebloat.sh` no-ops while reporting success.
- ✅ ~~Unresolved: EInk Center source~~ — **resolved**, see above. It ships
  inside `xSettings.apk`, which the debloat does not touch.

---

## Next
1. **Try TrebleDroid A15** (`work/gsi/td15-system.img`, downloaded). One release
   closer to the A12 vendor; its Skia may tolerate this Mali blob. Cheapest
   meaningful test of the GPU problem.
2. Force **HWC-only composition** so SurfaceFlinger never calls RenderEngine.
   ⚠️ NOT `service call SurfaceFlinger 1008 i32 1` — that *forces* GPU
   composition, the path that hangs. §7.5's advice is from the mono device and
   is backwards here.
3. Drive **`drm_eink_update_ioctl`** directly, bypassing SurfaceFlinger.
2. *(objective 6, later)* LineageOS 23 GSI with phh patches — plan written up in
   `docs/lineage-23-gsi-build.md`. **Not buildable on this Mac**: needs ~500 GB
   and a Linux host, so it is a cloud-VM job (~$10–20 on GCP). And it is a
   **port, not a build** — AndyCGYan's LOS+phh project stops at LineageOS 22, so
   LOS 23 means rebasing 75 patches plus the GSI device tree onto a newer tree.
2. `COLOR_MODE_MAGAZINE = 1` is the last untested cell — same method: set
   Magazine on any app, diff `/data/system/disp_policy.db`.
3. Reverse the **ioctl numbers** for the six `drm_eink_*_ioctl` entries and the
   structs they take. That is what a GSI-side panel driver would need, and it is
   offline work on the kernel in `boot_a`.
4. §7 GSI. Vendor is Android **12**; `max-download-size` is 128 MiB.

---

## Traps hit
- **A GSI on this device does not fail on e-ink — it fails on the GPU.** The
  e-ink driver, the LK-loaded waveform and the DRM ioctls all work fine under
  Android 16. SurfaceFlinger wedges inside `/vendor/lib64/egl/libGLES_mali.so`
  (`osup_sync_object_wait`) during `flushGL()`, and switching to Vulkan makes it
  abort in `SkiaGpuContext::MakeVulkan_Ganesh` instead. Both GPU paths broken.
- **`bootanim=running` with `boot_completed=1` means the display never
  progressed**, not that boot failed. On e-ink a looping boot animation is easy
  to mistake for a reboot loop — check `/proc/uptime`: if it climbs
  monotonically there are no reboots.
- **USB product name identifies the running system.** `TrebleDroid vanilla` vs
  `HiBreak` vs `Android` in `ioreg` is more reliable than guessing from the USB
  PID, which I misread as fastboot.
- **"Can't load Android system" after flashing a GSI is a `/data` encryption
  mismatch**, not a GSI failure. `fastboot -w` alone did not clear it; the
  on-screen factory reset did. Erase `metadata` as well as `userdata`.
- **A debloated super will not boot until dm-verity is disabled.** `system`,
  `vendor`, `product`, `system_ext` carry hashtree descriptors; changing their
  contents invalidates the root hashes and the kernel refuses to mount them.
  Symptom: preloader starts, dies ~4 s in, retries once, falls back to fastboot
  — while `slot-successful:a` still reads `yes` and `slot-retry-count:a` still
  `7`, because LK never records a failed slot. Fix:
  `fastboot --disable-verity --disable-verification flash vbmeta{,_system,_vendor}_a`
  with our own dumps (flags **before** `flash`).
  → Being unlocked does not help: unlocking relaxes AVB *signature* checking,
  dm-verity is a separate kernel mechanism driven by the hashtree. Magisk's
  patched `boot` booting fine without this is misleading — `boot` carries a
  *hash* descriptor, not a hashtree. I reasoned from that and got it wrong.
  Once set, the flags persist across later flashes.
- **mtkclient hangs at "Uploading stage 2" in BROM mode on this device.** Stage 2
  runs from DRAM which BROM has not initialised; mtkclient's search for a DRAM
  config matches the storage CID against bundled preloaders, but that code is
  **eMMC-only** and this is **UFS** — so it matched dozens of wrong preloaders
  (`DA exceed max num 0xc0070005`, repeatedly). Hits every UFS MediaTek device;
  not macOS-specific.
  → Use **preloader mode**, or now `--preloader work/backup/out/preloader_boot1.bin`.
- Reaching preloader mode: a **cold plug-in boots straight past the window** into
  Android. What works is **warm** — start `mtk <cmd>` polling *first*, then
  `adb reboot`.
- mtkclient sometimes refuses a device already in DA mode ("Please disconnect,
  start mtkclient and reconnect") — but **not always**: the `--parttype boot1`
  dumps attached fine straight from DA mode. If refused, hold power 10–15 s and
  use the warm-reboot recipe.
- Every `mtk.py` invocation died with `OSError: Unable to find libfuse` →
  `mfusepy` installed without macFUSE, and mtkclient only catches `ImportError`.
  Fixed by uninstalling `mfusepy`.
- `lpunpack_and_lpmake` doesn't build on modern macOS (three separate issues) →
  `tools/otatools/build-macos.sh`, documented in TOOLCHAIN.md.
- The loopback7084 scripts are GNU/Linux-only (`stat -c`, `mount -o loop`,
  `resize2fs`, `locate`) → `bin/hibreak-shell`. **But note**: only *modifying*
  an image needs that. `debugfs` reads ext4 natively on macOS with no mount and
  no root, which is how all the analysis was actually done.
- `_b` partitions dumping as all-zero is **normal** here. Slot B has never been
  written; fastboot's `slot-successful:b: no` confirms it independently.
- `/system/eink_key` is a second `.awf` but for a **10.3" panel**
  (`EC103KH2C1`), not this 6.1" one. Confirmed **not in use**: the driver's
  `machine_waveform` reports the 6.1" string from our `waveform` partition.
- `/data/waveform.bin` and `/data/waveform.bak` appear in `system_a` strings but
  **do not exist** on a running device. The live waveform source is the
  partition, not a `/data` cache — do not build on those paths.
- Magisk denying ADB (`su: request rejected (2000)`) is a settings toggle, not a
  broken root. Use `scrcpy` to drive the Magisk UI — the e-ink panel does not
  redraw toggles reliably enough to tell whether a setting took.
- CLAUDE.md §3 calls the frontlight "36-level"; the raw sysfs `lm3630a_cold_light`
  reads **182**, so the underlying range is wider. Do not assume 0–36.

---

## Not backed up, and not backupable
- `userdata` (~241 GB) — skipped by design, wiped at unlock. Human confirmed the
  device was new with no personal data, so this cost nothing.
- **RPMB** — mtkclient's UFS read fails; authenticated and non-restorable anyway.
- **SoC efuses** — burned into silicon. `SBC`/`SLA`/`DAA` are all *disabled*, and
  that is the safety net the entire recovery story rests on. `seccfg` is the
  lever that could change it, which is why §2.2 says write it last or never.

---

## Watch out
- **`unlock_critical` is unverified** (see above). First real test is §7.4.
- The device is a HiBreak Pro **Color** on the human's say-so plus one piece of
  evidence: the waveform names panel `EC061KH1C1`, and E Ink's `EC` prefix is
  used for its colour families. Software identity still says only
  `HiBreak`/`Smartphone`. Treat the prefix reading as inference, not fact.
- USB default mode is PTP, not the MTP §5.1 asks for. adb and mtkclient don't
  care — noted only so it isn't mistaken for a fault.
- `work/backup/` is `chmod a-w`. To add to it you must `chmod u+w` first; do not
  do that casually.
