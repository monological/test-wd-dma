#!/usr/bin/env bash
set -euo pipefail

: "${SDK_DIR:?source aws-fpga sdk_setup.sh first}"

ROOT="$(cd "$(dirname "$0")" && pwd)"
FD_SRC="$ROOT/../firedancer/src"

# ─── Firedancer C sources we need ────────────────────────────────────────────
FD_C_SRCS=(
  "$FD_SRC/util/fd_util.c"
  "$FD_SRC/util/fd_util_linux.c"

  "$FD_SRC/util/log/fd_log.c"
  "$FD_SRC/util/env/fd_env.c"
  "$FD_SRC/util/cstr/fd_cstr.c"

  # shmem helpers
  "$FD_SRC/util/shmem/fd_numa_linux.c"
  "$FD_SRC/util/shmem/fd_shmem_admin.c"   # ← provides _private_boot/_halt
  "$FD_SRC/util/shmem/fd_shmem_user.c"
)

# single-thread tile helper (no atomics)
FD_CPP_SRC="$FD_SRC/util/tile/fd_tile_nothreads.cxx"

# ─── Paths ───────────────────────────────────────────────────────────────────
INC_AWS="$SDK_DIR/userspace/include"
INC_MGMT="$SDK_DIR/userspace/fpga_libs/fpga_mgmt"
INC_TANGO="$FD_SRC/tango"
INC_UTIL="$FD_SRC/util"
INC_WD="$FD_SRC/wiredancer/c"
LIB_AWS="$SDK_DIR/userspace/lib"

# ─── Flags ───────────────────────────────────────────────────────────────────
DEFS="-DFD_LOG_STYLE=0 -DFD_ENV_STYLE=FD_ENV_STYLE_LINUX -DFD_SHMEM_STYLE=FD_SHMEM_STYLE_LINUX"
CFLAGS="-O2 -std=gnu17   -mavx2 -D_GNU_SOURCE $DEFS"
CXXFLAGS="-O2 -std=gnu++17 -mavx2 -D_GNU_SOURCE $DEFS"

# ─── Compile C ───────────────────────────────────────────────────────────────
OBJS=()
for src in test_dma.c wd_f1.c "${FD_C_SRCS[@]}"; do
  obj="$(basename "${src%.*}").o"
  gcc  $CFLAGS   -include linux/mman.h \
       -I"$INC_AWS" -I"$INC_MGMT" -I"$INC_TANGO" -I"$INC_UTIL" -I"$INC_WD" \
       -c "$src" -o "$obj"
  OBJS+=("$obj")
done

# ─── Compile C++ (tile helper) ───────────────────────────────────────────────
g++  $CXXFLAGS \
     -I"$INC_AWS" -I"$INC_MGMT" -I"$INC_TANGO" -I"$INC_UTIL" -I"$INC_WD" \
     -c "$FD_CPP_SRC" -o fd_tile_nothreads.o
OBJS+=("fd_tile_nothreads.o")

# ─── Link ────────────────────────────────────────────────────────────────────
g++ -mavx2 "${OBJS[@]}" \
    -L"$LIB_AWS" -lfpga_mgmt -lfpga_pci -lutils -lpthread -lrt \
    -o test_dma

export LD_LIBRARY_PATH="$LIB_AWS:${LD_LIBRARY_PATH:-}"
echo "Built ./test_dma"
