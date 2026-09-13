## Status
Phase: **2** (debloated stock super)
Backup verified: **y** — restore path **proven** by §5.4
Bootloader: **UNLOCKED** — `unlocked: yes`, `secure: no`, warranty bit tripped
Currently running: stock `Bigme_HiBreak_V1.0_20251125`, slot `_a`, rooted: **no**

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

### §6.2 review (no flashing yet)
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
- R3 `eink_debug` enumeration — **partial**. Five nodes from strings:
  `anti_alias`, `anti_flicker`, `clean_a2`, `waveform`, `machine_waveform`.
  Full enumeration **blocked on root**.
- R4 sysfs ↔ EInk Center mapping — **blocked on root**. EInk Center is
  `com.xrz.sys.control`.
- R5 CFA-specific modes — not started. Blocked behind R3/R4.
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
- ⚠️ **Unresolved**: EInk Center (`com.xrz.sys.control`) lives in `/data/app`,
  not `/system`. The init service runs `xrz_start.sh preinstall
  /system/preinstall`, but that directory does not exist in this image, and the
  recorded installer is `com.android.settings`. So what happens to EInk Center
  after flashing a debloated super and wiping is **not established**.

---

## Next
1. Decide root (Magisk). It gates R3/R4/R5 — the entire colour research — and
   would also let us resolve the EInk Center question above.
2. §6.2 debloat, only after applying the fixes in `debloat-review.md`.
3. §7 GSI. Remember vendor is Android **12**, and `max-download-size` is 128 MiB.

---

## Traps hit
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
  (`EC103KH2C1`), not this 6.1" one. Do not mistake it for this device's
  waveform.

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
