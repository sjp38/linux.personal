#!/bin/sh
rm -rf cscope.files

find . \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.s' -o -name '*.s' \) -print > cscope.files
cscope -i cscope.files
