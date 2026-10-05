#!/bin/sh
set -eu
repo=$1
out=$2
rtk proxy python3 "$(dirname "$0")/source-extract.py" "$repo" "$out"
rtk proxy g++ -std=c++23 -Wall -Wextra -Werror -pedantic "$out/extracted-check.cpp" -o "$out/extracted-check"
rtk proxy "$out/extracted-check"
