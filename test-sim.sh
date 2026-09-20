#!/usr/bin/env bash
# 模拟器对照测试 v2：reposync sim vs apt-get -s（apt 只读模拟）
set -uo pipefail
cd /mnt/d/Repo/LinuxTestDrive/reposync

echo "=== refresh system apt indexes (baseline) ==="
sudo apt-get update >/dev/null 2>&1 || { echo "apt update failed"; exit 1; }

T=$(mktemp -d /tmp/reposync-sim.XXXXXX)
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
./reposync update >/dev/null
./reposync db-import >/dev/null

# 自动挑选一个未安装的测试包（cowsay -> vim -> build-essential -> figlet）
PKG="${1:-}"
if [ -z "$PKG" ]; then
  for cand in cowsay figlet vim build-essential btop; do
    if ! dpkg-query -W -f='${db:Status-Abbrev}' "$cand" 2>/dev/null | grep -q '^ii'; then
      PKG="$cand"; break
    fi
  done
fi
[ -z "$PKG" ] && { echo "no uninstalled candidate found"; exit 1; }
echo "=== test package: $PKG ==="

echo
echo "=== SIM INSTALL $PKG (-y) ==="
./reposync sim install -y "$PKG"
echo "sim rc=$?"

echo
echo "=== APT SIMULATE install $PKG ==="
apt-get -s install "$PKG" 2>/dev/null | grep -E '^(Inst|Remv) '

echo
echo "=== COMPARE ==="
python3 - "$T/packages.db" "$PKG" <<'PYEOF'
import sqlite3, subprocess, sys
db_path, pkg = sys.argv[1], sys.argv[2]
db = sqlite3.connect(db_path)

ours = dict(db.execute("SELECT name,version FROM packages WHERE status='install ok installed'"))

# apt 预测终态：当前已装(ii) + Inst 行覆盖
cur = {}
for line in subprocess.run(["dpkg-query", "-W", "-f=${db:Status-Abbrev} ${Package} ${Version}\n"],
                           capture_output=True, text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[0] == "ii":
        cur[parts[1]] = parts[2]
out = subprocess.run(["apt-get", "-s", "install", pkg],
                     capture_output=True, text=True).stdout
apt_new = {}
for line in out.splitlines():
    if line.startswith("Inst "):
        parts = line.split()
        apt_new[parts[1]] = parts[2].strip("()")
apt_final = dict(cur)
apt_final.update(apt_new)

only_sim = sorted(set(ours) - set(apt_final))
only_apt = sorted(set(apt_final) - set(ours))
mism = sorted((n, ours[n], apt_final[n]) for n in set(ours) & set(apt_final)
              if ours[n] != apt_final[n])
print(f"sim db installed: {len(ours)} | apt-predicted final: {len(apt_final)}")
print(f"apt will install/upgrade {len(apt_new)}: {sorted(apt_new)}")
print("only in SIM  :", only_sim if only_sim else "none")
print("only in APT  :", only_apt if only_apt else "none")
print("version diffs:", mism if mism else "none")
db.close()
PYEOF

echo
echo "=== SIM REMOVE htop (real installed pkg) ==="
./reposync sim remove -y htop
echo "sim rc=$?"
python3 - "$T/packages.db" "htop" <<'PYEOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
row = db.execute("SELECT status FROM packages WHERE name=?", (sys.argv[2],)).fetchone()
print(f"{sys.argv[2]} status after sim remove: {row[0] if row else '(removed row)'}")
db.close()
PYEOF

echo
echo "=== APT SIMULATE remove htop ==="
apt-get -s remove htop 2>/dev/null | grep -E '^(Remv|Inst) '

echo "=== TEST DIR: $T ==="
