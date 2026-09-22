#!/bin/bash
# cleanup.sh - remove build artifacts and debug leftovers (run BEFORE mirror sync)
cd /mnt/d/Repo/LinuxTestDrive/reposync || exit 1
make clean >/dev/null 2>&1
rm -rf dbg-artifacts
rm -f run2.sh run3.sh run4.sh diag-src.sh run-mini.sh dbg-up.sh inspect.sh peek.sh ps.sh ps2.sh poll2.sh
echo CLEANED
ls