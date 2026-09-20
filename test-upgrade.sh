#!/usr/bin/env bash
# sim upgrade 测试：人造降级场景
set -uo pipefail
cd /mnt/d/Repo/LinuxTestDrive/reposync

T=$(mktemp -d /tmp/reposync-up.XXXXXX)
CFG="$T/sources.list.d"; LST="$T/lists"
mkdir -p "$CFG" "$LST"
cat > "$CFG/debian.sources" <<'EOF'
Types: deb
URIs: http://mirrors.ustc.edu.cn/debian
Suites: trixie
Components: main
Architectures: amd64
EOF

export REPOSYNC_CONFIG_DIR="$CFG" REPOSYNC_LIST_DIR="$LST" REPOSYNC_DB="$T/packages.db"
sudo apt-get update >/dev/null 2>&1 || { echo "apt update failed"; exit 1; }
./reposync update >/dev/null
./reposync db-import >/dev/null

echo "=== downgrade htop & cowsay in db (artificial) ==="
python3 - "$T/packages.db" <<'PYEOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("UPDATE packages SET version='1.0-1' WHERE name IN ('htop','cowsay')")
db.commit()
for n, v in db.execute("SELECT name, version FROM packages WHERE name IN ('htop','cowsay')"):
    print(f"  {n}: {v}")
db.close()
PYEOF

echo "=== SIM UPGRADE (-y) ==="
./reposync sim upgrade -y
echo "sim rc=$?"

echo "=== db after upgrade ==="
python3 - "$T/packages.db" <<'PYEOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
for n, v in db.execute("SELECT name, version FROM packages WHERE name IN ('htop','cowsay')"):
    print(f"  {n}: {v}")
db.close()
PYEOF

echo "=== TEST DIR: $T ==="
