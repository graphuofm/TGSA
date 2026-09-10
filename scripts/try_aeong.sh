#!/bin/bash
# Best-effort AeonG (Memgraph-based) build via conda env. Will likely fail at
# some hard-coded toolchain dep; we capture exactly where and write
# baselines/AeonG/BUILD_NOTES.md. Time-boxed: aborts after 90 min.
#
# NOTE: do NOT set -u here. Conda's gcc-activate script references
# $SYS_SYSROOT before defining it, which trips strict mode.

set -e
ROOT=/home/jding/Barry
NOTES=$ROOT/baselines/AeonG/BUILD_NOTES.md
LOG=$ROOT/logs/try_aeong.log

{ echo "# AeonG build attempt — best-effort (no sudo)"
  echo
  echo "Started: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
} > "$NOTES"

eval "$(/home/jding/miniconda3/bin/conda shell.bash hook)"

# Make sure we have the deps Memgraph cmake will look for.
echo "[aeong] installing memgraph deps via conda..." | tee -a "$LOG"
conda create -n mgbld -y --override-channels -c conda-forge \
    cmake make ninja gcc_linux-64=12 gxx_linux-64=12 \
    bison flex m4 \
    openssl libffi libxml2 \
    protobuf gflags glog \
    boost-cpp jemalloc fmt spdlog \
    python=3.10 \
    > "$LOG.deps" 2>&1
DEPS=$?
echo "[aeong] deps install exit $DEPS" | tee -a "$LOG"

if [ $DEPS -ne 0 ]; then
  { echo "## Stage 1 (conda deps): FAILED"; echo
    tail -40 "$LOG.deps"; } >> "$NOTES"
  exit 1
fi

conda activate mgbld
export PATH="$CONDA_PREFIX/bin:$PATH"
# Symlink generic gcc/g++ so cmake auto-detects.
ln -sf x86_64-conda-linux-gnu-gcc "$CONDA_PREFIX/bin/gcc" 2>/dev/null
ln -sf x86_64-conda-linux-gnu-g++ "$CONDA_PREFIX/bin/g++" 2>/dev/null

cd $ROOT/baselines/AeonG || exit 1

# Their init script downloads + builds local libs. Try it without root
# (the script is supposed to honor CONDA_PREFIX-style installs).
echo "[aeong] running ./init..." | tee -a "$LOG"
timeout 1800 ./init > "$LOG.init" 2>&1
INIT=$?
echo "[aeong] init exit $INIT" | tee -a "$LOG"

if [ $INIT -ne 0 ]; then
  { echo "## Stage 2 (./init): FAILED (exit $INIT)"
    echo "Likely the toolchain-vXYZ activation block. Tail of init log:"
    echo
    echo '```'; tail -50 "$LOG.init"; echo '```'
  } >> "$NOTES"
  exit 1
fi

# Configure + build memgraph proper.
mkdir -p build && cd build
echo "[aeong] cmake configure..." | tee -a "$LOG"
timeout 1800 cmake -GNinja \
   -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
   -DCMAKE_INSTALL_PREFIX=$CONDA_PREFIX \
   .. > "$LOG.cmake" 2>&1
CMK=$?
echo "[aeong] cmake exit $CMK" | tee -a "$LOG"

if [ $CMK -ne 0 ]; then
  { echo "## Stage 3 (cmake): FAILED (exit $CMK)"
    echo '```'; tail -60 "$LOG.cmake"; echo '```'
  } >> "$NOTES"
  exit 1
fi

echo "[aeong] ninja memgraph..." | tee -a "$LOG"
timeout 3600 ninja memgraph > "$LOG.ninja" 2>&1
NJ=$?
echo "[aeong] ninja exit $NJ" | tee -a "$LOG"

if [ $NJ -ne 0 ]; then
  { echo "## Stage 4 (ninja memgraph): FAILED (exit $NJ)"
    echo '```'; tail -60 "$LOG.ninja"; echo '```'
  } >> "$NOTES"
  exit 1
fi

{ echo "## SUCCESS"
  echo "Built memgraph at $(pwd)/memgraph"
} >> "$NOTES"
