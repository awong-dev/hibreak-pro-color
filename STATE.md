## Status
Phase: 1   Backup verified: n   Bootloader: locked
Currently running: stock (version unknown — device has not been connected yet)

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

## Open
- R1 waveform partition — not started, needs `printgpt` from the device
- R2 runtime waveform path — not started
- R3 `eink_debug` enumeration — not started, needs root on stock
- R4 sysfs ↔ EInk Center mapping — not started
- R5 CFA-specific modes — not started
- R6 Bigme `libgui.so` diff — not started

## Next
1. Human: Developer Options → USB debugging on, **OEM unlocking on**, USB mode
   MTP. If OEM unlocking is greyed out, stop (CLAUDE.md §5.1).
2. Agent: record device identity and `ro.vendor.xrz.*` props → `work/research/stock-props.txt`.
3. Agent: `bin/mtk printgpt | tee work/backup/printgpt.txt`, then
   `bin/mtk rl --skip=userdata work/backup/out` (13–20 GB, 15–30 min).
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
- Everything in CLAUDE.md §3/§7 inherited from the mono HiBreak Pro is still
  unverified on this unit.
