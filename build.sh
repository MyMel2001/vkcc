#!/bin/sh
set -eu

rm -rf build
mkdir -p build

cc -O3 -Wall -Wextra -std=c11 \
   -I/home/linuxbrew/.linuxbrew/include -I/usr/include \
   vkcc.c -L/usr/lib64 -L/usr/lib -l:libvulkan.so.1 \
   -o build/vkcc

ln -sf vkcc build/vkc++
ln -sf vkcc build/vkgcc
ln -sf vkcc build/vkg++

cc -O2 -Wall vkcc_check.c -o build/vkcc_check

(cd build && ./vkcc_check)
