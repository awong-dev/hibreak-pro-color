#!/bin/bash
# One-shot setup for a fresh checkout on macOS (Apple Silicon).
# Safe to re-run: every step skips work that is already done. Touches nothing
# on the device -- see CLAUDE.md 2.1.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

say() { printf '\n==> %s\n' "$*"; }

say "Homebrew packages"
brew install libusb scrcpy e2fsprogs squashfs binwalk coreutils
brew install --cask android-platform-tools

say "Upstream checkouts"
clone() { [ -d "$2" ] || git clone --depth 1 "$1" "$2"; }
clone https://github.com/bkerler/mtkclient.git                  tools/mtkclient
clone https://github.com/loopback7084/hibreak_pro_color_scripts.git tools/hibreak_pro_color_scripts

say "mtkclient venv (python3.11 -- 3.14 lacks wheels for keystone/unicorn/capstone)"
if [ ! -x tools/venv/bin/python ]; then
  python3.11 -m venv tools/venv
  tools/venv/bin/pip install -q --upgrade pip wheel setuptools
fi
# Deliberately NOT mtkclient's full requirements.txt:
#   pyside6/shiboken6 are the GUI, flake8 is dev-only, and mfusepy raises
#   OSError (not ImportError) without macFUSE, which kills *every* mtk.py run.
tools/venv/bin/pip install -q \
  pyusb pycryptodome pycryptodomex colorama pyserial capstone unicorn keystone-engine
tools/venv/bin/pip uninstall -y -q mfusepy 2>/dev/null || true

say "otatools (native arm64)"
[ -x tools/otatools/bin/lpunpack ] || tools/otatools/build-macos.sh

say "Linux toolbox image"
docker image inspect hibreak-tools >/dev/null 2>&1 || tools/docker/build.sh

say "Workspace"
mkdir -p work/backup/out work/super work/gsi work/research

say "Smoke test"
bin/mtk --help >/dev/null && echo "  mtk ok"
tools/otatools/bin/lpdump --help >/dev/null 2>&1 || true; echo "  lpdump ok"
adb version >/dev/null && echo "  adb ok"
bin/hibreak-shell bash -c 'command -v lpunpack resize2fs >/dev/null' && echo "  container ok"

cat <<'EOF'

Done. Now:  source tools/env.sh
Read CLAUDE.md (mission + GREEN/RED rules), then STATE.md (where we are).
EOF
