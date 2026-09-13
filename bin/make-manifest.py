#!/usr/bin/env python3
"""Build work/backup/MANIFEST.md from printgpt.txt + the dumped partitions (CLAUDE.md 5.3).

Read-only. Classifies every partition, verifies dumped size against the GPT
length, and -- the part that matters -- flags anything it cannot account for,
rather than quietly calling it OTHER.
"""
import hashlib, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPT  = os.path.join(ROOT, 'work/backup/printgpt.txt')
OUT  = os.path.join(ROOT, 'work/backup/out')
DEST = os.path.join(ROOT, 'work/backup/MANIFEST.md')
SUMS = os.path.join(ROOT, 'work/backup/SHA256SUMS')

def load_sums():
    """Reuse SHA256SUMS if present -- rehashing 13 GB for a report is wasteful,
    and §5.2 has already verified those digests read back stable."""
    if not os.path.exists(SUMS): return {}
    out = {}
    for ln in open(SUMS):
        parts = ln.split(maxsplit=1)
        if len(parts) == 2: out[parts[1].strip().lstrip('*')] = parts[0]
    return out

# 5.3: cannot be sourced from anyone else's device.
CRITICAL = {'nvram','nvdata','nvcfg','protect1','protect2','persist','seccfg',
            'preloader','waveform','proinfo','otp','sec1'}
SYSTEM   = {'boot','vbmeta','vbmeta_system','vbmeta_vendor','super','dtbo',
            'init_boot','vendor_boot','lk','tee','gz','md1img','spmfw','scp',
            'sspm','mcupm','dpm','pi_img','cam_vpu1','cam_vpu2','cam_vpu3'}
# Standard MediaTek/AOSP furniture -- present on essentially every MTK A/B device.
KNOWN_OTHER = {'misc','para','expdb','frp','metadata','boot_para','logo','vcom',
               'mrdump','flashinfo','userdata'}

def base(name):
    """Strip an _a/_b slot suffix. Returns (stem, slot_or_None)."""
    m = re.match(r'^(.*)_(a|b)$', name)
    return (m.group(1), m.group(2)) if m else (name, None)

def classify(stem):
    if stem in CRITICAL: return 'CRITICAL-UNIQUE'
    if stem in SYSTEM:   return 'SYSTEM'
    if stem in KNOWN_OTHER: return 'OTHER'
    return 'UNCLASSIFIED'

def human(n):
    for u in ('B','KiB','MiB','GiB'):
        if n < 1024 or u == 'GiB': return f'{n:.0f} {u}' if u=='B' else f'{n:.1f} {u}'
        n /= 1024

ANALYSIS = """
## All-zero partitions — not read failures

25 of the dumped files are entirely zero. This is a systematic pattern, not bad
reads:

- **Every `_b` partition is empty.** The device has never taken an OTA, so slot
  B has never been written. Every `_a` counterpart carries real data and real
  magic (`boot_a` = `ANDROID!`, `vbmeta_a` = `AVB0`, `lk_a` = MediaTek
  `0x58881688`).
- `init_boot_a` **and** `vendor_boot_a` are also zero — this build keeps the
  ramdisk in `boot_a` rather than using the GKI split, consistent with an
  Android 12 vendor.
- `otp`, `sec1`, `mrdump` are empty by nature.

If reads were failing we would expect corruption scattered across A-slot
partitions too. We do not.

## The preloader, and why it matters more than it looks

`preloader_boot1.bin` is a genuine MT6877 preloader: `COMBO_BOOT` magic, built
from `B651/mt6877_android14_qt` (the `B651` matches this unit's serial prefix).
`boot2` is a byte-identical mirror.

It carries an extractable **EMI v54** DRAM config (1288 bytes, `MTK_BLOADER_INFO_v54`).
That is the piece mtkclient could not find on its own, and the reason the first
BROM attempt hung forever at "Uploading stage 2".

The chain worth understanding: recovery from a bad flash means BROM + mtkclient;
mtkclient in BROM cannot load its DA without a DRAM config; its own search is
eMMC-only and fails on this UFS device. Without this file, a corrupted preloader
would close *both* recovery routes at once. With it, `--preloader` makes BROM
work directly.

## R1 — the waveform partition, answered

`waveform` is a **MediaTek-wrapped image**, not a raw blob:

```
0x000  magic 0x58881688, payload size 0x62ec10, name "waveform"
0x030  ext header 0x58891689, header length 0x200
0x200  payload begins -- an E Ink .awf file
       "570_VSK031_HV7501_EC061KH1C1_SC1452-FAB_TC.awf 2025.12.8.14:36:3:"
       6,482,960 bytes, real data to 0x62e808
0x62ee10+ MediaTek signature block (an embedded "Mediatek" cert1 X.509)
```

Extracted to `work/research/waveform.awf`
(sha256 `ce22da32a28f61393ff691c539d442f65978d4076b5b5cb1cb0b25921a8e5d26`).

Panel identifier **`EC061KH1C1`**, controller `SC1452-FAB`, plus `HV7501`.
`061` is consistent with the 6.1" panel. E Ink's `EC` prefix is used for its
colour families, which — if that reading is right — is the first thing on this
device corroborating the "Color" claim; software identity says only
`HiBreak`/`Smartphone`. Treat the prefix reading as inference, not fact.

Rewrapping for a flash back needs the MediaTek header **and** that signature
block preserved, so keep `waveform.bin`, not just the `.awf`.
"""


def main():
    if not os.path.exists(GPT):
        sys.exit(f'missing {GPT} -- run `bin/mtk printgpt | tee work/backup/printgpt.txt` first')
    parts = []
    for ln in open(GPT):
        m = re.match(r'^([A-Za-z0-9_]+):\s+Offset (0x[0-9a-f]+), Length (0x[0-9a-f]+)', ln)
        if m:
            parts.append((m.group(1), int(m.group(2),16), int(m.group(3),16)))
    names = {p[0] for p in parts}
    sums = load_sums()

    rows, problems, unclassified = [], [], []
    for name, off, length in parts:
        stem, slot = base(name)
        counterpart = (f'{stem}_b' if slot=='a' else f'{stem}_a') if slot else None
        has_ab = counterpart in names if counterpart else False
        cls = classify(stem)
        if cls == 'UNCLASSIFIED': unclassified.append(name)

        path = os.path.join(OUT, f'{name}.bin')
        if name == 'userdata':
            status, size, digest = 'SKIPPED (by design, 5.2)', None, '—'
        elif not os.path.exists(path):
            status, size, digest = '**NOT DUMPED**', None, '—'
            problems.append(f'{name}: no file')
        else:
            size = os.path.getsize(path)
            if size != length:
                status = f'**TRUNCATED {100*size/length:.1f}%**'
                problems.append(f'{name}: {size:,} of {length:,} bytes')
            else:
                status = 'ok'
            digest = sums.get(f'{name}.bin')
            if digest is None:
                h = hashlib.sha256()
                with open(path,'rb') as f:
                    for chunk in iter(lambda: f.read(1<<22), b''):
                        h.update(chunk)
                digest = h.hexdigest()
        rows.append((name, off, length, has_ab, size, status, digest, cls))

    with open(DEST,'w') as f:
        f.write('# Backup manifest\n\nGenerated by `bin/make-manifest.py` from '
                '`printgpt.txt` and `out/`. See CLAUDE.md §5.3.\n\n')
        f.write(f'Partitions in GPT: **{len(parts)}**. '
                f'Dumped: **{sum(1 for r in rows if r[5]=="ok")}**. '
                f'Problems: **{len(problems)}**.\n\n')

        if problems:
            f.write('## Problems\n\n')
            for p in problems: f.write(f'- {p}\n')
            f.write('\n')

        have_pl = os.path.exists(os.path.join(OUT, 'preloader_boot1.bin'))
        f.write('## Coverage beyond the GPT\n\n')
        if have_pl:
            f.write('The preloader is **not** in the GPT — on this UFS device it lives in the\n'
                    'boot LUNs (`LU1`/`LU2`, 4 MiB each), which `mtk rl` does not read. It has\n'
                    'been captured separately via `--parttype boot1` / `boot2` and is present\n'
                    'as `preloader_boot1.bin` / `preloader_boot2.bin`.\n\n')
        else:
            f.write('**The preloader is missing.** It is not in the GPT — on this UFS device it\n'
                    'lives in the boot LUNs (`LU1`/`LU2`), which `mtk rl` does not read. Capture\n'
                    'it with `bin/mtk r pl out/preloader_boot1.bin --parttype boot1`.\n\n')
        f.write('Still not covered, and not coverable:\n\n'
                '- `userdata` — skipped by design (§5.2) and wiped rather than restored (§2.2).\n'
                '- **RPMB** — mtkclient\'s UFS RPMB read fails (`unpack requires a buffer of 12\n'
                '  bytes`). Authenticated and non-restorable by design, so this costs us nothing.\n'
                '- **SoC efuses** — secure-boot config burned into silicon. Not backupable by\n'
                '  any tool. Currently SBC/SLA/DAA are all *disabled*, which is the safety net\n'
                '  the whole recovery story rests on.\n\n')

        f.write('## Partitions\n\n')
        f.write('| Partition | Offset | GPT size | A/B | On disk | Status | Class | sha256 |\n')
        f.write('|---|---|---|---|---|---|---|---|\n')
        for name, off, length, has_ab, size, status, digest, cls in rows:
            f.write(f'| `{name}` | `0x{off:x}` | {human(length)} | {"yes" if has_ab else "—"} '
                    f'| {human(size) if size is not None else "—"} | {status} | {cls} '
                    f'| `{digest[:16]}…` |\n')

        WHY = {
            'gpt':        'primary partition table, dumped by mtkclient alongside the partitions',
            'gpt_backup': 'backup partition table at the end of the device',
            'preloader_boot1': 'UFS boot LUN 1 — **the preloader**. CRITICAL-UNIQUE. '
                               'Pass to `--preloader` to make BROM mode usable',
            'preloader_boot2': 'UFS boot LUN 2 — mirror of boot1 (verified byte-identical)',
        }
        extras = sorted(f_[:-4] for f_ in os.listdir(OUT)
                        if f_.endswith('.bin') and f_[:-4] not in names)
        if extras:
            f.write('\n## Extra files (not GPT partitions)\n\n')
            for e in extras:
                sz = os.path.getsize(os.path.join(OUT, f'{e}.bin'))
                f.write(f'- `{e}.bin` — {human(sz)} — {WHY.get(e, "unexplained; review")}\n')

        f.write('\n## Unclassified\n\n')
        if unclassified:
            f.write('Outside the standard MediaTek/AOSP set — **review these**:\n\n')
            for n in unclassified:
                off, length = next((o,l) for nm,o,l in parts if nm==n)
                f.write(f'- `{n}` — {human(length)} at `0x{off:x}`\n')
        else:
            f.write('None. Every partition matched a known MediaTek/AOSP name.\n')

        f.write(ANALYSIS)

        f.write('\n## Full sha256\n\n```\n')
        for name, _,_,_,_,_, digest, _ in rows:
            if digest != '—': f.write(f'{digest}  {name}.bin\n')
        f.write('```\n')
    print(f'wrote {DEST}: {len(parts)} partitions, {len(problems)} problems, '
          f'{len(unclassified)} unclassified')

if __name__ == '__main__':
    main()
