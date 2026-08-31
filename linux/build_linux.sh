#!/bin/sh
# Build the Linux plugin and place insbots_i486.so next to the DLL.
set -e
cd "$(dirname "$0")"
make clean >/dev/null 2>&1 || true
make
ls -l ../insbots_i486.so
