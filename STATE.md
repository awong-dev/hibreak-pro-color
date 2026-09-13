## Status
Phase: 1   Backup verified: n   Bootloader: locked (OEM unlock toggle ON)
Currently running: stock `Bigme_HiBreak_V1.0_20251125`, slot `_a`

## Done
- [2026-09-12] Toolchain and workspace set up on macOS arm64. No device
  contact, no RED commands, nothing written to hardware. Details in
  `TOOLCHAIN.md`.
  - `bin/mtk` → mtkclient V2.1.4 (`cd25cf9`) in a python3.11 venv; `--help`
    and `rl --help` verified, `--skip` present.
  - `bin/hibreak-shell` → `hibreak-tools` Debian container, `./work` at
    `/work`, loopback7084 scripts (`f49f973`) at `/scripts`.
  - lpunpack/lpmake/lpdump/lpadd/lpflash built native arm64 → `tools/otatools/bin`,
    and again inside the container.
  - adb/fastboot 37.0.1, scrcpy, binwalk 3.1.0, squashfs-tools, e2fsprogs,
    coreutils.
  - `work/{backup/out,super,gsi,research}` created, empty.
  - Loop-mounting ext4 from the macOS bind mount inside Docker: **works**.
    Phase 2 can operate on `./work` directly.

## Done (cont.)
- [2026-09-12] §5.1 adb pre-flight **passed**. Read-only, nothing written.
  - Device authorised over adb; `sys.oem_unlock_allowed=1` — the gate that
    matters is open.
  - Identity, boot state, panel geometry and the `ro.vendor.xrz.*` namespace
    recorded → `work/research/device-identity.md`, `stock-props{,-all}.txt`.
  - Confirmed on-device: MT6877, system Android 14, 824x1648 @ 300, A/B on
    slot `_a`, and the §7.2 frontlight sysfs path.
  - Corrected three CLAUDE.md facts: firmware is V1.0_20251125 (not the 2.x
    line), vendor is Android **12** not 14, and this is **virtual** A/B.
  - Not confirmed: that this is the *Color* model. Software says only
    `HiBreak`/`Smartphone`. See device-identity.md.

## Open
- R1 waveform partition — not started, needs `printgpt` from the device
- R2 runtime waveform path — not started
- R3 `eink_debug` enumeration — **blocked on root**. `adb root` refused
  (production build); `/sys/devices/platform/eink` exists but exposes only bare
  platform-device attributes to uid 2000
- R4 sysfs ↔ EInk Center mapping — blocked on root. EInk Center is most likely
  `com.xrz.sys.control`
- R5 CFA-specific modes — not started
- R6 Bigme `libgui.so` diff — not started

## Next
1. Human: power the phone **fully off**, unplug, hold **both volume keys**, plug
   USB in. (Power + Vol-Up also reported.) Watch for the USB id to become
   `0x0E8D:0x0003`.
2. Agent: `bin/mtk printgpt | tee work/backup/printgpt.txt` — this is also the
   cheap test of whether the BROM handshake works at all on macOS.
3. Agent: `bin/mtk rl --skip=userdata work/backup/out` (13–20 GB, 15–30 min).
4. Agent: build `work/backup/MANIFEST.md` per §5.3 against the real printgpt
   output. Hunt for `waveform`.
5. Human: two off-machine copies, then the `VERIFIED` gate.

## Traps hit
- macOS cannot loop-mount ext4 and the loopback7084 scripts are GNU/Linux-only
  (`stat -c`, `mount -o loop`, `resize2fs`, `locate`) → all image-filesystem
  work goes through `bin/hibreak-shell`.
- Every `mtk.py` invocation died with `OSError: Unable to find libfuse`
  → `mfusepy` was installed but macFUSE was not, and mtkclient only catches
  `ImportError` → uninstalled `mfusepy`; the import now fails cleanly.
- `lpunpack_and_lpmake` doesn't build on modern macOS out of the box (three
  separate issues) → `tools/otatools-src/make-macos.sh`, see TOOLCHAIN.md.

## Unverified / watch out
- mtkclient has never talked to this device. The BROM path on macOS is
  **unproven**; find out before depending on it for the full dump.
- The device is a HiBreak Pro *Color* on the human's say-so, not on anything
  software reported. Expect this to resolve via R1.
- USB default mode is PTP, not the MTP §5.1 asks for. adb and mtkclient don't
  care, so this is not blocking — noted only so it isn't mistaken for a fault.
- Virtual A/B changes what `super` looks like. Check `printgpt`/`lpdump` for
  whether `_b` copies exist before trusting the loopback7084 repack geometry.
