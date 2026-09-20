#!/bin/bash
# gui-query-test.sh - end-to-end GUI-side query verification (Linux)
# 0) rebuild the CLI test environment (self-contained)
# 1) run the GUI selftest job (OP_UPDATE via local CLI) -> writes local db
# 2) compile qt.c against app.o and run local db queries (no CLI, no SSH)
set -e
echo "=== 0. rebuild CLI test env ==="
bash /mnt/d/Repo/LinuxTestDrive/reposync/cli-test.sh >/dev/null

export SDL_VIDEODRIVER=dummy
export REPOSYNC_DB=/tmp/rt/gui-packages.db
export REPOSYNC_CONFIG_DIR=/tmp/rt/conf
export REPOSYNC_LIST_DIR=/tmp/rt/lists
export REPOSYNC_CONF=/tmp/rt/conf/reposync.conf
export REPOSYNC_TARGET=/tmp/rt/conf/target.sources
export REPOSYNC_STATUS=/tmp/rt/status
cd /mnt/d/Repo/LinuxTestDrive/reposync/gui
PATH=/mnt/d/Repo/LinuxTestDrive/reposync:$PATH

echo "=== 1. GUI selftest (OP_UPDATE via local CLI, writes db) ==="
./reposync-gui --gui-selftest 2>/dev/null | tail -6
echo "--- selftest_rc=$? (expect 0)"

echo "=== 2. local db query: all ==="
gcc -O2 -Wall -Wextra -std=gnu99 $(pkg-config --cflags sdl3 sqlite3) -c app.c -o app-qt.o
gcc -O2 -Wall -Wextra -std=gnu99 $(pkg-config --cflags sdl3 sqlite3) qt.c app-qt.o \
    backend_local.o backend_ssh2.o $(pkg-config --libs libssh2 sqlite3) -o qt
./qt
echo "--- exit=$?"

echo "=== 3. filter by suite=trixie comp=main ==="
./qt "" trixie main
echo "--- exit=$?"

echo "=== 4. by name hello ==="
./qt hello
echo "--- exit=$?"

echo "=== GUI QUERY TEST PASS ==="
