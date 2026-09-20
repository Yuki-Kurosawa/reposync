#!/usr/bin/env bash
# db-import 测试 + 交叉验证（python3 独立通道读 SQLite 对比 dpkg status）
set -uo pipefail
cd /mnt/d/Repo/LinuxTestDrive/reposync

T=$(mktemp -d /tmp/reposync-db.XXXXXX)
DB="$T/packages.db"
export REPOSYNC_DB="$DB"

echo "### DB-IMPORT ###"
./reposync db-import
echo "RC=$?"

echo "### CROSS-CHECK ###"
python3 - "$DB" <<'PYEOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
c = db.cursor()

total = c.execute("SELECT COUNT(*) FROM packages").fetchone()[0]
inst  = c.execute("SELECT COUNT(*) FROM packages WHERE status='install ok installed'").fetchone()[0]
print(f"db total records : {total}")
print(f"db installed     : {inst}")

# 独立统计 status 文件（不经过 sqlite）
import re
recs = open("/var/lib/dpkg/status", "rb").read().decode()
st_installed = 0
for m in re.finditer(r"(?m)^Status: install ok installed$", recs):
    st_installed += 1
print(f"status file installed (regex) : {st_installed}")
print("installed MATCH" if inst == st_installed else "installed MISMATCH")

# 抽样包对比版本
samples = ["bash", "libc6", "coreutils", "dpkg", "apt"]
for name in samples:
    row = c.execute("SELECT version, architecture, status FROM packages WHERE name=?", (name,)).fetchone()
    m = re.search(rf"(?m)^Package: {name}$", recs)
    if not m:
        print(f"{name}: NOT IN STATUS"); continue
    # 提取该记录块的 Version
    block = recs[m.end():]
    v = re.search(r"(?m)^Version: (.+)$", block).group(1).strip()
    ok = row is not None and row[0] == v
    print(f"  {name}: db={row[0] if row else None} status={v} {'OK' if ok else 'MISMATCH'}")

print("meta:")
for k, v in c.execute("SELECT key, value FROM meta ORDER BY key"):
    print(f"  {k} = {v}")
db.close()
PYEOF

echo "### TEST DIR: $T ###"
