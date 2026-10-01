#!/usr/bin/env bash
cd /home/deshab/plasma-libs
mv -v libunwind* /home/deshab/xorg-libs/ 2>/dev/null | tail -2
echo "xorg=$(ls /home/deshab/xorg-libs | wc -l) plasma=$(ls /home/deshab/plasma-libs | wc -l)"