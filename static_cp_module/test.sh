#!/bin/bash
insmod static_cp_module.ko name1=sphinx name2=libquantum cache1=64 cache2=64
rmmod static_cp_module
dmesg |tail
