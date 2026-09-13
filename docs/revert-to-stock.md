# Reverting to stock, and unrooting

Everything needed is in `work/backup/out/`, dumped from **this** device and
verified (65/65 checksums, `work/backup/VERIFIED`). All flashing steps are
**RED** under CLAUDE.md §2.1 — the human runs them.

## What can and cannot be undone

| | |
| --- | --- |
| System / vendor / product / system_ext | ✅ fully — `super.bin` is this device's own |
| Magisk root | ✅ fully — reflash `boot_a.bin` |
| dm-verity | ✅ re-enable by reflashing vbmeta without the disable flags |
| userdata | ✅ wiped |
| **Bootloader lock state** | ⚠️ possible but **dangerous** — see §5 |
| **Warranty bit** | ❌ **permanent.** `fastboot getvar warranty` went `yes`→`no` at unlock and cannot be restored |

OTAs will not apply while `boot`, `vbmeta` or `super` differ from stock — the
updater hash-checks them. Steps 1–3 restore all three.

---

## 1. Stock super (~17 min)

Undoes the debloat and any GSI. `system` is a logical partition *inside* super,
so this covers everything at once.

```
bin/mtk w super work/backup/out/super.bin --preloader work/backup/out/preloader_boot1.bin
```

If mtkclient will not attach, use the warm recipe: start the command so it is
polling, then `adb reboot` in another terminal (see TOOLCHAIN.md).

## 2. Stock boot — this is the unroot

```
adb reboot bootloader
fastboot flash boot work/backup/out/boot_a.bin
```

`boot_a.bin` is the unmodified dump, so Magisk's ramdisk patch is gone. Nothing
else is needed: the Magisk app and `/data/adb` disappear with the userdata wipe
in step 4.

Note this device is **non-GKI** — the ramdisk lives in `boot`, not `init_boot`
(which is all-zero). Reflashing `boot` is the whole job.

## 3. Re-enable dm-verity

We disabled it to flash a modified super (CLAUDE.md §6.2). Restoring stock means
putting it back — flash the same dumps **without** the disable flags:

```
fastboot flash vbmeta_a        work/backup/out/vbmeta_a.bin
fastboot flash vbmeta_system_a work/backup/out/vbmeta_system_a.bin
fastboot flash vbmeta_vendor_a work/backup/out/vbmeta_vendor_a.bin
```

⚠️ Do this **only after** step 1 completes. Verity on with a modified super is a
device that does not boot — that exact combination cost us a debugging session.

## 4. Wipe userdata

```
fastboot -w
fastboot reboot
```

Expect a slow first boot and the setup wizard. If you get "Can't load Android
system", choose **Factory data reset** — that is a `/data` encryption mismatch,
not a failure.

**Stop here unless you specifically need a locked bootloader.** At this point the
device is stock, unrooted, verity-enforcing, and takes OTAs. The only difference
from factory is an unlocked bootloader and the tripped warranty bit.

---

## 5. Relocking — read this before doing it

CLAUDE.md §2.2: *never relock with a non-stock system image on the device. That
is a hard brick.*

Relocking makes the bootloader enforce verified boot again. If **anything** on
the device fails verification — a modified partition, a vbmeta still carrying
disable flags, a non-stock boot — it will refuse to boot, and with the bootloader
locked you cannot flash a fix. BROM/mtkclient is the only way back, and that
depends on the preloader still being intact.

**Only relock if all of these are true:**

- [ ] Step 1 completed and `super.bin` was written in full
- [ ] Step 2 completed — stock `boot_a.bin`, no Magisk
- [ ] Step 3 completed — vbmeta flashed **without** disable flags
- [ ] Step 4 completed — userdata wiped
- [ ] The device **boots normally** and `getprop ro.boot.verifiedbootstate` reports **`green`**

That last one is the real gate. `orange` means something still fails
verification, and relocking then is the brick.

```
fastboot flashing lock
```

Do **not** write `seccfg.bin` back from the backup to relock. It reaches the same
state by a blunter route with no verification check in front of it.

If you relock and it will not boot, recovery is BROM + mtkclient with
`--preloader work/backup/out/preloader_boot1.bin`, writing `super.bin`,
`boot_a.bin` and the vbmeta set back. This works on this unit because BROM
security is open (`SBC`/`SLA`/`DAA` all false) — confirmed in §3 of CLAUDE.md.

---

## Quick reference — partial reverts

| goal | command |
| --- | --- |
| Unroot only, keep debloat | `fastboot flash boot work/backup/out/boot_a.bin` |
| GSI → debloated stock | `fastboot reboot fastboot` then `fastboot flash system work/releases/system-debloat-v1.img` |
| Undo our vendor patches | `fastboot flash vendor work/super/vendor_a.img` |
| Everything → debloated stock | `bin/mtk w super work/releases/super-debloat-v1.bin --preloader …` |
| Everything → factory stock | §1 above |
