# CLAUDE.md — HiBreak Pro Color ROM Project

Handoff brief for Claude Code. Read this fully before running anything.

---

## 1. Mission

Replace the stock Bigme OS on a **HiBreak Pro Color** with a clean AOSP/LineageOS-derived
system image, with the Kaleido 3 e-ink panel working, without losing the ability to return
to stock.

Ordered objectives — do not skip ahead:

1. **Verified full firmware backup.** Restore path proven by test-write.
2. **Bootloader unlocked.**
3. **Debloated stock super flashed.** Keeps the working colour display; strips vendor
   cruft; teaches the super workflow.
4. **GSI booting** with a driveable panel (greyscale acceptable at this stage).
5. **Colour refresh modes mapped and working.** This is the research objective.
6. *(stretch)* Lineage GSI + patched e-ink service as the daily driver.

Objective 5 is unsolved in public. Nobody has published a working GSI for this model.
Treat 1–3 as engineering and 4–6 as research.

---

## 2. Hard rules

These are not style preferences. Violating them can permanently destroy hardware.

### 2.1 Command classification

**GREEN — run autonomously.** Reads, analysis, file manipulation on the host, image
building.

```
python mtk.py printgpt
python mtk.py rl / rf
adb shell getprop / cat / ls          (read-only paths)
sha256sum, file, strings, binwalk
lpunpack, lpmake, mount -o ro, unsquashfs
anything writing only under ./work/
```

**RED — never execute. Print the command, explain it, stop, and wait for the human to run
it or explicitly say "run it".** Every one of these writes to the device.

```
python mtk.py w <anything>
python mtk.py e <anything>
python mtk.py da seccfg <anything>
python mtk.py da vbmeta <anything>
fastboot flash <anything>
fastboot -w
fastboot flashing unlock / unlock_critical
adb shell 'echo ... > /sys/...'       (panel writes can be persistent)
```

There is no "this one's safe" exception. Ask.

### 2.2 Gates

- **No RED command may be proposed at all until `./work/backup/VERIFIED` exists.** That
  file is created by the check in §5.2 and only by it. If it's missing, the answer to
  every flashing question is "we do the backup first."
- **`seccfg` is written last, or never.** Writing it back from the backup relocks the
  bootloader. Relocking with a non-stock system image on the device is a hard brick.
- **Never write `userdata` from a backup.** Wipe it instead.
- **Never propose relocking.** If the human asks, confirm they have flashed the full
  stock super and boot images back first.

### 2.3 Sourcing

Use images dumped from **this device** in preference to anything downloaded. Community
mega.nz/torrent dumps are for cross-reference only. The `waveform` and `nv*` partitions
are per-unit and must never come from someone else's device.

### 2.4 Uncertainty

Much of the public information for this device is from the **mono** HiBreak Pro. Where a
fact is inherited from the mono model rather than verified on this one, say so explicitly
rather than asserting it. Verify by enumeration on-device when possible.

---

## 3. Device facts

✅ = verified on this unit over adb, 2026-09-12. Everything else is still
inherited from the mono model or from the seller's spec sheet — see §2.4.

| | |
| --- | --- |
| Model | Bigme HiBreak Pro **Color** (HBPC) — *the device reports only `ro.product.model=HiBreak`, `ro.product.device=Smartphone`; nothing in software says Pro, Color, Kaleido or CFA* |
| SoC | MediaTek **MT6877** / Dimensity 1080 ✅ |
| Memory | 8 GB RAM / 256 GB UFS, no SD |
| Partitioning | **virtual** A/B, dynamic (`super`) ✅ — `ro.virtual_ab.enabled=true`, currently on slot `_a` |
| Stock Android | system **14** ✅ (sdk 34) — but **vendor/odm are Android 12** ✅ (sdk 31, `ro.vendor.api_level` 30) |
| Firmware | **`Bigme_HiBreak_V1.0_20251125`** ✅, security patch 2025-11-05. *Not* the 2.x line this file previously assumed |
| Panel | E Ink **Kaleido 3** CFA, 1648×824 ✅, 300 ppi ✅ mono / 150 ppi colour |
| Frontlight | 36-level warm/cold, `lm3630a` ✅ |
| Bootloader | locked ✅ (`flash.locked=1`, `verifiedbootstate=green`), **OEM unlocking toggle on** ✅ (`sys.oem_unlock_allowed=1`) |

Full record and the raw prop dump: `work/research/device-identity.md`.
`ro.vendor.xrz.screen_type=61` is the most likely panel discriminator if a mono
dump ever turns up to compare against.

Same SoC/RAM/storage/camera/battery as the mono HiBreak Pro. Different panel, different
Android base, different firmware line.

### Entering modes

- **BROM:** power fully off → unplug → hold **both volume keys** → plug USB in. (Power +
  Vol-Up also reported; try both.)
- **fastboot:** `adb reboot bootloader`
- **fastbootd:** `fastboot reboot fastboot`
- **Unstick anything:** hold **power 10–15 s**.
- **Post-flash boot quirk:** unplug USB → hold power ~10 s → press power again **within
  ~5 s**. Looks like a bootloop, isn't. Retry several times before diagnosing.

---

## 4. Workspace

```
./
├── CLAUDE.md              # this file
├── STATE.md               # running log — see §8. Update it every session.
├── TOOLCHAIN.md           # what's installed, what runs where, build gotchas
├── bin/
│   ├── mtk                # mtkclient wrapper (project venv)
│   └── hibreak-shell      # root Linux shell for ext4/super work
├── tools/
│   ├── env.sh             # `source tools/env.sh` — puts the above on PATH
│   ├── docker/            # Dockerfile for the Linux toolbox
│   ├── mtkclient/
│   ├── hibreak_pro_color_scripts/   # loopback7084 — HBPC super tooling
│   └── otatools/                    # lpunpack, lpmake
└── work/
    ├── backup/            # READ-ONLY once verified. chmod -R a-w it.
    │   ├── printgpt.txt
    │   ├── SHA256SUMS
    │   ├── MANIFEST.md    # generated, see §5.3
    │   ├── VERIFIED       # gate file
    │   └── out/*.bin
    ├── super/             # unpacked / modified super work area
    ├── gsi/               # downloaded + patched GSIs
    └── research/          # waveform analysis, sysfs dumps, notes
```

Host is macOS/arm64, which cannot loop-mount ext4. Anything that mounts, resizes
or fscks a filesystem image runs in the Linux container via `bin/hibreak-shell`;
mtkclient/adb/fastboot stay on the host because they need USB. See
`TOOLCHAIN.md`.

---

## 5. Phase 1 — Backup

**Objective:** a verified, duplicated, per-partition dump and a proven restore path.

### 5.1 Pre-flight (human, on device)

Settings → Developer Options: USB debugging **on**, OEM unlocking **on**, default USB mode
→ **MTP**. If OEM unlocking is greyed out, stop — nothing downstream works.

Agent: confirm and record identity before anything else.

```bash
adb shell getprop ro.product.model
adb shell getprop ro.board.platform
adb shell getprop ro.build.display.id
adb shell getprop ro.build.version.release
adb shell getprop | grep -iE 'xrz|eink|waveform|bigme' | tee work/research/stock-props.txt
```

`ro.vendor.xrz.*` is Bigme's e-ink config namespace. Keep that output — it's a map of what
the vendor layer expects and a lead for §7.

### 5.2 Dump and verify (GREEN)

```bash
python tools/mtkclient/mtk.py printgpt | tee work/backup/printgpt.txt
python tools/mtkclient/mtk.py rl --skip=userdata work/backup/out
```

~13–20 GB, 15–30 min.

Then verify. Create `VERIFIED` **only** if all of these pass:

```bash
cd work/backup/out
# 1. no zero-byte or suspiciously tiny files
find . -size -1k -ls
# 2. every partition in printgpt (except userdata) has a file
# 3. checksums recorded
sha256sum * > ../SHA256SUMS
# 4. re-read checksums to confirm the storage is stable
sha256sum -c ../SHA256SUMS
```

Then require the human to confirm **two off-machine copies exist** before writing
`VERIFIED`. Do not take "I'll do it later" — the gate exists precisely because that's what
everyone says.

```bash
touch work/backup/VERIFIED     # create the gate FIRST --
chmod -R a-w work/backup       # a-w makes the directory itself unwritable
```

### 5.3 Build the manifest (GREEN, high value)

Parse `printgpt.txt` into `work/backup/MANIFEST.md`: partition name, offset, size, whether
it has an A/B counterpart, file size on disk, sha256, and a classification column:

- `CRITICAL-UNIQUE` — cannot be sourced from anyone else: `nvram`, `nvdata`, `nvcfg`,
  `protect1`, `protect2`, `persist`, `seccfg`, `preloader`, **`waveform`**
- `SYSTEM` — `boot`, `vbmeta*`, `super`, `dtbo`, `init_boot`
- `OTHER`

**Flag anything you cannot classify.** In particular: hunt for a partition named
`waveform`, and if it isn't there, list every small unexplained partition outside the
standard MediaTek set. Panel calibration is per-unit and per-screen-revision; this
partition is the single most important artifact in the backup and the key to objective 5.

### 5.4 Prove the write path (RED — human runs)

Re-write a partition with its own backup copy. Non-destructive, proves the restore path
works before it's needed in anger.

```
python tools/mtkclient/mtk.py w vbmeta_a work/backup/out/vbmeta_a.bin
```

Unplug, boot (mind the boot quirk). If it comes up, phase 1 is done.

---

## 6. Phase 2 — Unlock, then debloated stock

### 6.1 Unlock (RED — human runs, in order)

Factory-resets the device. HBPC reportedly needs more than the standard sequence.

```
adb reboot bootloader
fastboot devices
fastboot flashing unlock            # confirm with Vol-Up on device; wait
fastboot flashing unlock_critical
# then, in BROM, belt-and-braces:
python mtk.py e metadata,userdata,md_udc
python mtk.py da seccfg unlock
```

Afterwards the wipe clears Developer Options — re-enable USB debugging and MTP.

### 6.2 Debloat stock super (do this before any GSI)

Best risk-adjusted step available: keeps the working colour display stack, removes the
vendor cruft, and exercises the super unpack/repack workflow you need later.

Use `tools/hibreak_pro_color_scripts` (loopback7084). Workflow:

```
UNPACK      super.bin → system / vendor / product / system_ext
MOUNTALL    (root) mount for editing
SAFEDEBLOAT strip cruft, fix CN locale defaults
            ← agent: review the removal list before it runs; diff it against
              a list of packages the e-ink stack depends on
UNMOUNTALL  unmount + shrink
REPACK      → new super image
```

Then (RED): `python mtk.py w super work/super/new_super.bin` — takes ~17 min.

**Agent responsibilities here:** before REPACK, enumerate what SAFEDEBLOAT removes and
cross-check against packages referenced by `ro.vendor.xrz.*` props, the EInk Center app,
and anything holding the waveform file open. Removing a display-stack dependency is a
silent path to a black screen.

**Known failure mode:** upgrading 2.7 → 2.9 and then running the same debloat sequence has
been reported to soft-brick. Build the debloat against a super dumped from the version
actually on the device; don't reuse a 2.7-derived super on 2.9.

**Known recovery:** one user hit a black screen after flashing a modified super and
recovered by writing their original `super.bin` back. That only worked because they had
it. See §2.2.

---

## 7. Phase 3 — GSI and the colour problem

### 7.1 Why this is research, not a recipe

Every published GSI and every e-ink patcher for these phones targets the **mono** HiBreak
Pro. For the Color:

- Kaleido 3 is a mono panel with a colour filter array bonded on. It needs waveform modes
  the mono device doesn't have.
- Waveform data is often specific not just to the model but to the **hardware revision of
  the screen**, so it must be extracted from stock per-device before flashing a custom
  ROM.
- The A9-derived accessibility service (the thing that makes GSIs usable on these phones)
  opens a waveform file at a fixed path. On a GSI that file isn't there unless it's put
  there.
- Bigme ships a modified `libgui.so` exposing a `repaintEverything` method that mainline
  AOSP SurfaceFlinger lacks — which is why refresh transactions that work on stock
  (1004/1005) don't exist on a GSI, and why a freshly-flashed GSI looks like a dead
  screen. ✅ **Confirmed on this device, and it is worse than that**: there are
  *four* non-AOSP exports including a new `ISurfaceComposer` AIDL binder method,
  and the Java-facing `XrzEinkManager` lives in the framework. A GSI replaces
  `libgui.so`, SurfaceFlinger *and* the framework, so all of it goes at once.
  See `work/research/eink-stack.md`.
- ⚠️ `/system/eink_key` is a second `.awf` — **for a 10.3" panel** (`EC103KH2C1`),
  not this 6.1" one (`EC061KH1C1`). Do not treat it as this device's waveform.

**Expected first result: it boots, greyscale only, limited refresh control.** That is
success for objective 4. Colour is objective 5.

### 7.2 Open research questions

Work these on **stock, before flashing anything**, because stock is where the answers are.

| # | Question | How to attack |
| --- | --- | --- |
| R1 | What is the waveform partition called and what format is the blob? | `printgpt`; `file`/`binwalk`/`strings` the dump; compare against known E Ink `.wbf` structure |
| R2 | ✅ **ANSWERED** — `/dev/block/by-name/waveform` → `/data/waveform.bin` (+`.bak`) → `/sys/kernel/debug/eink_debug/waveform`, `machine_waveform`. `/system/bin/xrz_updater --update-waveform` is the writer. See `work/research/eink-stack.md` | |
| R3 | What does `/sys/kernel/debug/eink_debug` actually expose on *this* unit? | Enumerate on rooted stock — **do not assume the mono table below is complete** |
| R4 | Which sysfs writes correspond to which EInk Center setting? | Poll/diff sysfs while toggling each mode in the stock UI |
| R5 | Which modes are CFA/colour-specific? | Whatever appears in R4 but not in the mono `EinkRefreshMode` table |
| R6 | ✅ **ANSWERED** — four non-AOSP exports, not one: `repaintEverything()`, `setLayerRefreshMode(String8 const&, uint)`, `Transaction::setRefreshMode(sp<SurfaceControl> const&, int)`, **and a new AIDL binder method** `ISurfaceComposer::remoteSetLayerRefreshMode`. Java side is `xrz.framework.manager.XrzEinkManager` via `libXrzFramework_runtime.so` | |

Mono-model `EinkRefreshMode` codes, **as a starting hypothesis only** (decompiled from the
mono Pro, incomplete for CFA):

`INIT 1 · DU 2 · GC16 4 · GC4 8 · A2 16 · GL16 32 · GLR16 64 · GLD16 128 · GU16 132 ·
GU4 136 · INPUT 137 · CLEAN 176 · HD 177 · NORMAL 178 · FAST 179 · HANDWRITE 1029 ·
AUTO 32768`

Frontlight path ✅ **confirmed on this unit**:
`/sys/devices/platform/11d01000.i2c7/i2c-7/7-0036` → `lm3630a_cold_light`,
`lm3630a_warm_light`, `lm3630a_version`.

`ro.vendor.xrz.default_refresh_mode` is **178**, which is `NORMAL` in the table
above — weak but real support for the mono mode numbering carrying over.

### 7.3 GSI selection

Vendor is **Android 12** (sdk 31, `ro.vendor.api_level` 30), not 14 — stock runs
an Android 14 system on top of it. Use an **arm64 A/B (`arm64_bvN`) GSI**;
Android 14 is known-good on this vendor because that is what stock ships.
TrebleDroid, ponces' AOSP, or a LineageOS treble build. Going newer than 14 is
plausible but is one more variable on top of an already-unsolved panel problem —
match stock first.

Do **not** assume `vbbot`'s prebuilt HiBreak Pro image works here — it targets the mono
panel and a 1.x vendor. If tried, treat it strictly as a diagnostic.

### 7.4 Flash sequence (RED — human runs)

Use `vbmeta*` from **our own dump**.

```
adb reboot bootloader
fastboot -w
fastboot flash vbmeta_a        --disable-verity --disable-verification <ours>
fastboot flash vbmeta_b        --disable-verity --disable-verification <ours>
fastboot flash vbmeta_system_a --disable-verity --disable-verification <ours>
fastboot flash vbmeta_system_b --disable-verity --disable-verification <ours>
fastboot flash vbmeta_vendor_a --disable-verity --disable-verification <ours>
fastboot flash vbmeta_vendor_b --disable-verity --disable-verification <ours>
fastboot reboot fastboot
fastboot flash system work/gsi/<image>.img
fastboot reboot
```

### 7.5 First boot — black screen is expected

```bash
adb devices                                        # GREEN — is it actually alive?
adb shell service call SurfaceFlinger 1008 i32 1   # disable HW overlays
```

Alternatives: power-lock then power-unlock; or `scrcpy` and tick *Disable HW overlays* in
Developer Options. Keep `scrcpy` running **before** any reboot — it's the only way to see
the UI when the panel isn't refreshing.

On unpatched GSIs the overlay setting resets each boot; bind it to `sys.boot_completed`
via a Magisk service script.

### 7.6 Phh Treble settings

Misc → **Disable SF GL backpressure**, **Disable SF HWC backpressure**, **MediaTek GED KPI
support**, **Force allow Always-On Display** (else the last frame stays burned in when
locked).

IMS: Create IMS APN · Install IMS APK for MediaTek R+ vendor · Request IMS network · Force
present 4G Calling. Telephony: Allow binder thread on incoming calls · Force display 5G ·
Disable "Voice Call In" route. Expect to enter the carrier APN by hand; telephony is the
flakiest part of MediaTek GSIs.

---

## 8. Phase 4 — Lineage

### Route A — patch a Lineage GSI (realistic)

The existing e-ink support is a **system.img patcher**, not a device tree:

- `damianmqr/a9_accessibility_service` — upstream (Hisense A9)
- `vbbot/HibreakProPatchingService` — mono HiBreak Pro fork
- An /e/OS fork exists, so the approach generalises to Lineage-derived bases

Colour-specific work on top:

1. Extract the waveform blob (R1)
2. Determine the expected path (R2) and ship it in the patched image, or push via Magisk
3. Extend the mode table with CFA modes (R4/R5)
4. Rebuild and flash

⚠️ vbbot's April 2025 release post warns *"Do not patch the image with the script in the
repo, it will brick your device"* — referring to the script's state at that time vs. his
prebuilt release. Check the repo's current README and commit history before running it.

Known snag: Lineage bases (unlike AOSP) need root + adb to get the display up on first
boot, and setup wizard has been reported to hang on Lineage/GAPPs builds.

### Route B — real device port

1. GPL kernel source — request from Bigme in writing; without it the EPD driver is opaque
2. Vendor blobs — `lpunpack` super, write `extract-files.sh` / `proprietary-files.txt`
3. Device tree for `mt6877` — adapt from a Dimensity 900/1080 tree (Redmi Note 11 Pro 5G,
   Realme 9 Pro+)
4. EPD layer — reimplement the refresh policy as a real HAL, CFA-aware

1–3 are a few weekends. 4 is why nobody has finished this on the mono model.

---

## 9. Recovery runbook

| Situation | Action |
| --- | --- |
| Won't boot after flash | Unplug, hold power 10 s, press power again within 5 s. Repeat several times. |
| Wedged / unresponsive | Hold power 10–15 s to force off, then BROM |
| Restore one partition | `python mtk.py w <name> work/backup/out/<name>.bin` |
| Black screen after bad super | Write the original `super.bin` back |
| Back to stock | Write everything back **except userdata**; `seccfg` last or never |
| Factory reset from BROM | MTKMETAUtility → Factory Reset Meta |

**OTAs will not apply** once `boot`, `vbmeta`, or `super` are modified — the updater
hash-checks them. Restore all three first. Stock recovery is reportedly non-functional for
sideloading anyway.

---

## 10. STATE.md convention

Update at the end of every session. Sessions will be long and context will be lost.

```markdown
## Status
Phase: <1-4>   Backup verified: <y/n>   Bootloader: <locked/unlocked>
Currently running: <stock 2.x / debloated stock / GSI name>

## Done
- [date] what happened, what was flashed, sha256 of what

## Open
- R1..R6 status, one line each

## Traps hit
- <symptom> → <cause> → <fix>. Keep these; they're the expensive knowledge.
```

---

## 11. References

- XDA: [HiBreak Pro Color / Color S](https://xdaforums.com/t/bigme-hibreak-pro-color-color-s-root-magisk-debloat-general-impressions.4764527/) — **primary thread for this device**
- XDA: [HiBreak Pro (MT6877)](https://xdaforums.com/t/bigme-hibreak-pro-dimensity-900-mt6877-e-ink-phone.4723924/) — root/backup/unlock, mostly transferable
- XDA: [HiBreak Pro Development Thread](https://xdaforums.com/t/bigme-hibreak-pro-development-thread.4731041/) — GSI + e-ink driver reversing
- [loopback7084/hibreak_pro_color_scripts](https://github.com/loopback7084/hibreak_pro_color_scripts)
- [bkerler/mtkclient](https://github.com/bkerler/mtkclient)
- [damianmqr/a9_accessibility_service](https://github.com/damianmqr/a9_accessibility_service) · [vbbot/HibreakProPatchingService](https://github.com/vbbot/HibreakProPatchingService)
- Vasu's mono-Pro guides: [unlock](https://vbh.ai/unlocking-the-bootloader-and-rooting-the-hibreak-pro/) · [backup/firmware](https://vbh.ai/hibreak-pro-guide3-an-extremely-rubbish-way-to-upgrade-your-hibreak-pro-to-the-latest-firmware/) · [GSI](https://vbh.ai/hibreak-pro-guide-4-flashing-the-android-15-gsi/)
- r/Bigme — most active place for current status

If a GSI boots on the Color, post the partition table and findings to the HBPC thread. As
of the last public discussion the answer to "are there any GSIs for the HBPC?" was still
no.
