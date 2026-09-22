#!/bin/bash
# combo.sh - build, then upstream via mini test, then Sync-Sources:no regression, in ONE wsl session
cd /mnt/d/Repo/LinuxTestDrive/reposync || exit 1
make > /tmp/combo-make.log 2>&1 || { echo MAKE_FAIL; tail -5 /tmp/combo-make.log; exit 1; }
bash mini-source-test.sh > /tmp/combo-mini.log 2>&1
echo "mini_exit=$?"
bash quick-no-sources.sh > /tmp/combo-nosrc.log 2>&1
echo "nosrc_exit=$?"
