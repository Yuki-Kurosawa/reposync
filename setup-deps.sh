#!/usr/bin/env bash
# 安装开发依赖：libsqlite3-dev（SQLite 为 public domain）
set -uo pipefail
sudo apt-get install -y libsqlite3-dev
echo "=== check ==="
ls -la /usr/include/sqlite3.h
pkg-config --modversion sqlite3 2>/dev/null || echo "no pkg-config info"
