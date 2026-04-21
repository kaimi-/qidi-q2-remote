#!/bin/bash
set -euo pipefail

SRC_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRELOAD_DST="${PRELOAD_DST:-/home/mks/qd2_remote_input.so}"
PYTHON_DST="${PYTHON_DST:-/opt/qd2-remote}"
VENV_DIR="${VENV_DIR:-$PYTHON_DST/venv}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
UNIT_DST="/etc/systemd/system/qd2-remote.service"
BUILT_SO="$SRC_ROOT/preload/qd2_remote_input.so"

#
# Preload source selection.
#
# Non-interactive override via env:
#   PRELOAD_MODE=build              -> build from source
#   PRELOAD_MODE=existing           -> use existing file; path from PRELOAD_SRC
#   PRELOAD_SRC=/path/to/lib.so     -> existing .so to install
#
mode="${PRELOAD_MODE:-}"
src_path="${PRELOAD_SRC:-}"

prompt_mode() {
  if [ ! -t 0 ]; then
    mode="build"
    return
  fi
  echo "Preload library (.so):"
  echo "  1) Build locally from $SRC_ROOT/preload"
  echo "  2) Use an existing prebuilt .so file"
  local choice
  while true; do
    read -r -p "Select [1/2]: " choice
    case "$choice" in
      1|b|build)     mode="build";    return ;;
      2|e|existing)  mode="existing"; return ;;
      *) echo "  invalid choice" ;;
    esac
  done
}

prompt_path() {
  if [ -n "$src_path" ]; then return; fi
  if [ ! -t 0 ]; then
    echo "ERROR: PRELOAD_MODE=existing but PRELOAD_SRC not set" >&2
    exit 1
  fi
  local def="$BUILT_SO"
  while true; do
    read -r -e -p "Path to existing .so [${def}]: " src_path
    src_path="${src_path:-$def}"
    if [ -f "$src_path" ] && [ -r "$src_path" ]; then
      return
    fi
    echo "  not a readable file: $src_path"
  done
}

if [ -z "$mode" ]; then
  prompt_mode
fi

case "$mode" in
  build)
    echo "[*] Building preload library"
    make -C "$SRC_ROOT/preload"
    src_path="$BUILT_SO"
    ;;
  existing)
    prompt_path
    echo "[*] Using existing preload: $src_path"
    ;;
  *)
    echo "ERROR: unknown PRELOAD_MODE='$mode' (expected 'build' or 'existing')" >&2
    exit 1
    ;;
esac

if ! file "$src_path" 2>/dev/null | grep -q 'ELF.*aarch64'; then
  echo "WARNING: $src_path does not look like an aarch64 ELF; installing anyway" >&2
fi

echo "[*] Installing preload to $PRELOAD_DST"
install -m 0755 "$src_path" "$PRELOAD_DST"

echo "[*] Installing Python package to $PYTHON_DST"
mkdir -p "$PYTHON_DST"
cp -r "$SRC_ROOT/python/qd2_remote" "$PYTHON_DST/"
cp    "$SRC_ROOT/python/requirements.txt" "$PYTHON_DST/"

resolve_uv() {
  if [ -n "${UV_BIN:-}" ] && [ -x "$UV_BIN" ]; then
    echo "$UV_BIN"; return 0
  fi
  if command -v uv >/dev/null 2>&1; then
    command -v uv; return 0
  fi
  local candidates=()
  if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
    local sudo_home
    sudo_home=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    [ -n "$sudo_home" ] && candidates+=("$sudo_home/.local/bin/uv" "$sudo_home/.cargo/bin/uv")
  fi
  candidates+=(
    "${HOME:-/root}/.local/bin/uv"
    "${HOME:-/root}/.cargo/bin/uv"
    "/root/.local/bin/uv"
    "/home/mks/.local/bin/uv"
    "/usr/local/bin/uv"
  )
  local c
  for c in "${candidates[@]}"; do
    if [ -x "$c" ]; then echo "$c"; return 0; fi
  done
  return 1
}

UV="$(resolve_uv || true)"
if [ -z "$UV" ]; then
  cat >&2 <<EOF
ERROR: 'uv' not found in PATH or standard locations.

sudo strips PATH by default, so a user-local uv at ~/.local/bin/uv is
invisible. Fix one of:

  1) Pass the path explicitly:
       sudo UV_BIN=/home/mks/.local/bin/uv bash install.sh

  2) Preserve PATH for this run:
       sudo env "PATH=\$PATH" bash install.sh

  3) Install uv system-wide:
       sudo install -m 0755 /home/mks/.local/bin/uv /usr/local/bin/uv

  4) Or install from scratch:
       curl -LsSf https://astral.sh/uv/install.sh | sh
EOF
  exit 1
fi
echo "[*] Using uv at $UV"

if [ -x "$VENV_DIR/bin/python" ]; then
  echo "[*] Reusing existing venv at $VENV_DIR"
else
  echo "[*] Creating venv at $VENV_DIR (uv)"
  "$UV" venv "$VENV_DIR" --python "$PYTHON_BIN"
fi

echo "[*] Installing Python deps into venv (uv)"
"$UV" pip install --python "$VENV_DIR/bin/python" -r "$PYTHON_DST/requirements.txt"

echo "[*] Installing systemd unit"
sed "s|@VENV_PYTHON@|$VENV_DIR/bin/python|g; s|@PYTHON_DST@|$PYTHON_DST|g" \
    "$SRC_ROOT/scripts/qd2-remote.service" > "$UNIT_DST"
chmod 0644 "$UNIT_DST"
systemctl daemon-reload

echo "[*] Installing start.sh wrapper to /home/mks/QD_Q2/bin/start.sh (backup saved)"
WRAP=/home/mks/QD_Q2/bin/start.sh
if [ -f "$WRAP" ] && [ ! -f "$WRAP.bak" ]; then
  cp "$WRAP" "$WRAP.bak"
fi
install -m 0755 "$SRC_ROOT/scripts/start.sh" "$WRAP"

echo "[*] Done."
echo "    enable:  systemctl enable --now qd2-remote.service"
echo "    open:    http://<printer-ip>:18080/"
