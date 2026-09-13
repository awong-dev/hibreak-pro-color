# source tools/env.sh -- puts the host-side toolchain on PATH. bash and zsh.
#
# e2fsprogs is keg-only on Homebrew; coreutils supplies the GNU stat/sha256sum
# that the loopback7084 scripts assume (`stat -c`).
if [ -z "${HIBREAK_ROOT:-}" ]; then
  if [ -n "${BASH_SOURCE:-}" ]; then
    _hb_src="${BASH_SOURCE[0]}"
  elif [ -n "${ZSH_VERSION:-}" ]; then
    _hb_src="$(eval 'echo ${(%):-%x}')"
  else
    _hb_src="$0"
  fi
  HIBREAK_ROOT="$(cd "$(dirname "$_hb_src")/.." && pwd)"
  unset _hb_src
fi
export HIBREAK_ROOT
export PATH="$HIBREAK_ROOT/bin:$HIBREAK_ROOT/tools/otatools/bin:/opt/homebrew/opt/e2fsprogs/sbin:/opt/homebrew/opt/e2fsprogs/bin:/opt/homebrew/opt/coreutils/libexec/gnubin:/opt/homebrew/bin:$PATH"
