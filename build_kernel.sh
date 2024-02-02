#!/bin/bash

export ARCH=arm
export CROSS_COMPILE=arm-linux-androideabi-
export PATH=$PWD/toolchain/bin:$PATH

make $COMMON baffinvektt_00_defconfig
make $COMMON savedefconfig
cp defconfig arch/arm/configs/baffinvektt_00_defconfig
make $COMMON -j$(nproc)
