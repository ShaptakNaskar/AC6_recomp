#!/usr/bin/env bash
# Linux counterpart of setup_and_build.bat: checks the prerequisites, locates
# the game data, runs codegen and builds the runtime.
#
# Usage: ./setup_and_build.sh [--clean] [--jobs N] [GAME]
#
#   GAME     An extracted game folder (the one holding default.xex) or a disc
#            image (.iso). Optional once assets/default.xex exists, or when a
#            single .iso sits in the repository root.
#   --clean  Delete the previous build directory first.
#   --jobs N Parallel compile jobs (default: CPU count). Lower it if the link
#            step runs out of memory.

set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

readonly PRESET="linux-amd64-relwithdebinfo"
readonly BUILD_DIR="out/build/${PRESET}"

clean=0
jobs=""
game=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean | -clean | clean) clean=1 ;;
    --jobs | -j)
      jobs="${2:?--jobs needs a number}"
      shift
      ;;
    -h | --help)
      sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) game="$1" ;;
  esac
  shift
done

die() {
  echo "[ERROR] $*" >&2
  exit 1
}

echo "========================================="
echo "Prerequisites Check"
echo "========================================="

for tool in cmake ninja clang clang++ pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || die "'$tool' not found on PATH."
done

clang_major="$(clang++ -dumpversion | cut -d. -f1)"
[[ "$clang_major" -ge 18 ]] || die "Clang 18 or newer is required (found ${clang_major})."

for module in gtk+-3.0 x11-xcb; do
  pkg-config --exists "$module" ||
    die "Development files for '$module' not found (see README: How to build > Linux)."
done

# SDL3 is vendored and picks its audio backends from the headers present at
# configure time; with none of them it still builds, just without sound.
if ! pkg-config --exists libpipewire-0.3 && ! pkg-config --exists libpulse &&
  ! pkg-config --exists alsa; then
  echo "[WARNING] No PipeWire, PulseAudio or ALSA development files found - the build will have no audio."
fi

grep -qw avx2 /proc/cpuinfo || echo "[WARNING] This CPU does not report AVX2; the build targets x86-64-v3 and will not run here."

echo "[OK] All prerequisites found!"
echo

echo "========================================="
echo "Game Data"
echo "========================================="

extract_iso() {
  local iso="$1"
  command -v extract-xiso >/dev/null 2>&1 ||
    die "extract-xiso is needed to unpack '${iso}'. Install it (https://github.com/XboxDev/extract-xiso) or pass an already extracted game folder instead."
  echo "Extracting '${iso}' to 'assets'..."
  mkdir -p assets
  extract-xiso -d assets "$iso" || die "Failed to extract the ISO."
}

if [[ -n "$game" ]]; then
  if [[ -d "$game" ]]; then
    [[ -f "$game/default.xex" ]] || die "'$game' has no default.xex - is it the extracted game folder?"
    if [[ -e assets && ! -L assets ]]; then
      die "An 'assets' folder already exists; remove it or run without a GAME argument."
    fi
    ln -sfn "$(readlink -f "$game")" assets
    echo "[OK] Linked assets -> $(readlink -f "$game")"
  elif [[ -f "$game" ]]; then
    extract_iso "$game"
  else
    die "'$game' does not exist."
  fi
elif [[ -f assets/default.xex ]]; then
  echo "[OK] 'assets/default.xex' already exists. Skipping extraction."
else
  shopt -s nullglob nocaseglob
  isos=(*.iso)
  shopt -u nullglob nocaseglob
  [[ ${#isos[@]} -gt 0 ]] ||
    die "No game data found. Pass the extracted game folder or an .iso, or place the .iso in this folder."
  echo "Found ISO: ${isos[0]}"
  extract_iso "${isos[0]}"
fi
[[ -f assets/default.xex ]] || die "assets/default.xex is still missing."
echo

echo "========================================="
echo "Building the Game"
echo "========================================="

if [[ "$clean" == 1 ]]; then
  echo "Cleaning previous build directory..."
  rm -rf "$BUILD_DIR"
else
  echo "Incremental build enabled. Use --clean for a clean build."
fi

build_args=()
[[ -n "$jobs" ]] && build_args=(-- "-j${jobs}")

echo "Step 1: Configuring CMake..."
cmake --preset "$PRESET" || die "Configuration failed."

echo "Step 2: Generating recompiled code..."
cmake --build --preset "$PRESET" --target ac6recomp_codegen "${build_args[@]}" || die "Codegen failed."

echo "Step 3: Re-configuring CMake..."
cmake --preset "$PRESET" || die "Re-configuration failed."

echo "Step 4: Building the runtime..."
if ! cmake --build --preset "$PRESET" "${build_args[@]}" >build.log 2>&1; then
  echo "[ERROR] Build failed. Last 50 lines of build.log:"
  echo "--------------------------------------------------"
  tail -n 50 build.log
  echo "--------------------------------------------------"
  exit 1
fi

# The runtime looks for the game files in an 'assets' folder next to the
# executable, so point the build directory at the one in the repository root.
if [[ ! -e "${BUILD_DIR}/assets" ]]; then
  ln -s "$(readlink -f assets)" "${BUILD_DIR}/assets"
fi

echo "[SUCCESS] Setup and Build completed successfully!"
echo "Run the game with: ${BUILD_DIR}/ac6recomp"
echo "DLC packages go in ${BUILD_DIR}/dlc (or set dlc_dir in ${BUILD_DIR}/ac6recomp.toml)."
