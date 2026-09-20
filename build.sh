#!/usr/bin/env bash
# 构建脚本：转 LF + make（防御 Windows 行尾问题）
# 构建 reposync (CLI, Linux) 和 reposync-gui (GTK3 + libssh)
set -euo pipefail
cd /mnt/d/Repo/LinuxTestDrive/reposync

find . -name '*.c' -o -name '*.h' -o -name 'Makefile' -o -name '*.sh' | while read -r f; do
  sed -i 's/\r$//' "$f"
done

make clean
make
echo "=== build done ==="
ls -la reposync gui/reposync-gui
