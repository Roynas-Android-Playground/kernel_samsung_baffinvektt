#!/bin/bash

set -e

export ARCH=arm
export CROSS_COMPILE=arm-linux-androideabi-
export PATH=$PWD/toolchain/bin:$PATH
COMMON="O=out"

mkdir -p out
make $COMMON noperf_defconfig
make $COMMON savedefconfig
cp out/defconfig arch/arm/configs/noperf_defconfig
make $COMMON -j$(nproc)
