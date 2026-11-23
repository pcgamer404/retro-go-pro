#!/bin/bash
# Builds the host test library from the project's real dynarec.c.  usage:  bash build.sh [out.so]
# EXTRA="-DJIT_LOCAL_REGS=0" ./build.sh libdyn_L0.so      (test other register configurations)
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ=${PROJ:-$(cd "$HERE/../.." && pwd)}
SRC=$PROJ/main/nanojava/src
OUT=${1:-libdyn_v2.so}
gcc -g -O1 -shared -fPIC -DJIT_VERBOSE=1 $EXTRA -I$SRC/vm -I$SRC/pal -I$SRC -o $OUT $SRC/vm/dynarec.c $HERE/xjit_exec_host.c $HERE/stubs2.c
