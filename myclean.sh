#!/bin/bash
make mrproper
make clean
cp config.bak ./.config
make menuconfig
