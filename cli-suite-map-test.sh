#!/bin/bash
# 套件映射测试（需求.md 套件映射，cli/gui 共用同一套逻辑；gui 只是驱动 cli）
# 验证同步时包的目标套件选择：
#   1. abc/main -> abc/main          同名映射（套件、组件都同名）
#   2. abc-updates/main -> abc/main  变体回退（仅当目标完全没有 abc-updates）
#   3. abc/main <-> abc/contrib      跨组件禁止（两个方向都不行）
#   4. abc/main -> abc-updates/main  反向映射禁止
# 另覆盖 upgrade 路径与 Sources（Sync-Sources: yes）路径的套件映射。
set -u
cd "$(dirname "$0")" || exit 1

R=/tmp/rsm
# target.sources 放在 $R/tconf 而非 $REPOSYNC_CONFIG_DIR：update 会扫描配置目录
# 下所有 *.sources 当上游抓取，目标声明放同目录会被误当作上游仓库。
rm -rf $R
mkdir -p $R/upstream/dists/abc/main/binary-amd64 \
         $R/upstream/dists/abc/contrib/binary-amd64 \
         $R/upstream/dists/abc-updates/main/binary-amd64 \
         $R/upstream/dists/abc-updates/contrib/binary-amd64 \
         $R/upstream/dists/abc/main/source \
         $R/upstream/dists/abc/contrib/source \
         $R/upstream/dists/abc-updates/main/source \
         $R/upstream/dists/abc-updates/contrib/source \
         $R/upstream/pool/main $R/upstream/pool/contrib \
         $R/target/dists/abc/main/binary-amd64 \
         $R/target/dists/abc-updates/main/binary-amd64 \
         $R/target/pool/abc $R/target/pool/abc-updates \
         $R/target/srcpool/abc $R/target/srcpool/abc-updates \
         $R/tconf

BIN=$PWD/reposync
export REPOSYNC_CONFIG_DIR=$R/conf
export REPOSYNC_LIST_DIR=$R/lists
export REPOSYNC_TARGET=$R/tconf/target.sources
export REPOSYNC_CONF=$R/conf/reposync.conf
export REPOSYNC_DB=$R/packages.db
export REPOSYNC_MANUAL=$R/manual
mkdir -p $REPOSYNC_CONFIG_DIR $REPOSYNC_LIST_DIR

fail=0
pass() { printf '  ok: %s\n' "$1"; }
failx() { printf '  FAIL: %s\n' "$1"; fail=1; }
check() { if [ "$2" = "1" ]; then pass "$1"; else failx "$1"; fi; }

# ---------- 上游仓库（suites: abc / abc-updates, components: main / contrib） ----------
python3 - "$R" <<'PY'
import os, sys, hashlib
R = sys.argv[1]
def wb(p, s):
    with open(p, "w") as f:
        f.write(s)

# 假 .deb / .dsc（内容即版本标记；Size/MD5sum 由 python 计算写入索引）
wb(f"{R}/upstream/pool/main/aaa.deb",   "PKG:aaa V:1.0\n")
wb(f"{R}/upstream/pool/contrib/ccc.deb","PKG:ccc V:1.0\n")
wb(f"{R}/upstream/pool/main/bbb.deb",   "PKG:bbb V:2.0\n")
wb(f"{R}/upstream/pool/main/zzz-bin.deb","PKG:zzz-bin V:1.0\n")
wb(f"{R}/upstream/pool/main/www-bin.deb","PKG:www-bin V:1.0\n")
wb(f"{R}/upstream/pool/main/zzz.dsc",   "Source: zzz\nVersion: 1.0\n")
wb(f"{R}/upstream/pool/main/www.dsc",   "Source: www\nVersion: 1.0\n")

def pkg_rec(name, ver, comp, fn):
    p = os.path.join(R, "upstream", "pool", comp, fn)
    st = os.stat(p)
    md5 = hashlib.md5(open(p, "rb").read()).hexdigest()
    return "\n".join([f"Package: {name}", f"Version: {ver}",
                      "Architecture: amd64", "Maintainer: T <t@e.com>",
                      f"Description: {name}",
                      f"Filename: pool/{comp}/{fn}",
                      f"Size: {st.st_size}", f"MD5sum: {md5}", "", ""])

def src_body(name, ver, comp, fn):
    p = os.path.join(R, "upstream", "pool", comp, fn)
    st = os.stat(p)
    md5 = hashlib.md5(open(p, "rb").read()).hexdigest()
    binary = {"zzz": "zzz-bin", "www": "www-bin"}[name]
    return "\n".join([f"Package: {name}", f"Version: {ver}",
                      f"Binary: {binary}", f"Directory: pool/{comp}",
                      f"Files:\n {md5} {st.st_size} {fn}", "", ""])

pkgset = {
    "abc/main": [("aaa", "1.0", "aaa.deb"), ("zzz-bin", "1.0", "zzz-bin.deb")],
    "abc/contrib": [("ccc", "1.0", "ccc.deb")],
    "abc-updates/main": [("bbb", "2.0", "bbb.deb"), ("www-bin", "1.0", "www-bin.deb")],
    "abc-updates/contrib": [],
}
for sc, entries in pkgset.items():
    suite, comp = sc.split("/")
    body = "".join(pkg_rec(n, v, comp, fn) for n, v, fn in entries)
    wb(f"{R}/upstream/dists/{suite}/{comp}/binary-amd64/Packages", body)

srcset = {
    "abc/main": [("zzz", "1.0", "zzz.dsc")],
    "abc-updates/main": [("www", "1.0", "www.dsc")],
    "abc/contrib": [], "abc-updates/contrib": [],
}
for sc, entries in srcset.items():
    suite, comp = sc.split("/")
    body = "".join(src_body(n, v, comp, fn) for n, v, fn in entries)
    wb(f"{R}/upstream/dists/{suite}/{comp}/source/Sources", body)
PY

# ---------- 源声明 / 目标声明 / 命令集 ----------
cat > $REPOSYNC_CONFIG_DIR/upstream.sources <<EOF
Types: deb
URIs: file://$R/upstream
Suites: abc abc-updates
Components: main contrib
Architectures: amd64
EOF

write_target() { # $1=Suites 行, $2=Components 行
    cat > $REPOSYNC_TARGET <<EOF
Types: deb
URIs: file://$R/target
Suites: $1
Components: $2
Architectures: amd64
EOF
}

write_conf() { # $1=Sync-Sources
    cat > $REPOSYNC_CONF <<EOF
Active-Repo: plain
Sync-Sources: $1

Repo-Type: plain
Path: $R/target
Import-Cmd: cp -t $R/target/pool/%suite%
Source-Import-Cmd: cp -t $R/target/srcpool/%suite%
Remove-Cmd: rm -f $R/target/pool
EOF
}

rmpool() { rm -rf $R/target/pool/* $R/target/srcpool/*; \
            rm -f $R/target/dists/abc/main/binary-amd64/Packages \
                  $R/target/dists/abc-updates/main/binary-amd64/Packages; \
            mkdir -p $R/target/pool/abc $R/target/pool/abc-updates \
                     $R/target/srcpool/abc $R/target/srcpool/abc-updates; }

# ================= 二进制映射（Sync-Sources: no） =================
write_conf no

# --- 场景 A：目标只有 abc/main；abc-updates/main -> abc/main（变体回退） ---
write_target "abc" "main"
$BIN update >$R/update1.log 2>&1 || { echo "FAIL: update"; cat $R/update1.log; exit 1; }

$BIN install -y aaa >/dev/null 2>&1
check "A: aaa(abc/main) -> pool/abc（同名）" \
      "$([ -f $R/target/pool/abc/aaa.deb ] && echo 1 || echo 0)"

$BIN install -y bbb >/dev/null 2>&1
check "A: bbb(abc-updates/main) -> pool/abc（变体回退）" \
      "$([ -f $R/target/pool/abc/bbb.deb ] && echo 1 || echo 0)"
check "A: bbb 未落入 pool/abc-updates" \
      "$([ -f $R/target/pool/abc-updates/bbb.deb ] && echo 0 || echo 1)"

$BIN install -y ccc 2>$R/ccc.err >/dev/null
check "A: ccc(abc/contrib) 被跳过（目标无 contrib，跨组件禁止）" \
      "$([ -f $R/target/pool/abc/ccc.deb ] && echo 0 || echo 1)"
check "A: ccc 输出跳过警告" \
      "$(grep -q 'skipping ccc' $R/ccc.err && echo 1 || echo 0)"

# --- upgrade 路径：目标已有 aaa 1.0 / bbb 2.0，上游 bbb 升到 3.0 ---
python3 - "$R" <<'PY'
import os, sys, hashlib
R = sys.argv[1]
def wb(p, s):
    with open(p, "w") as f:
        f.write(s)
def pkg_rec(name, ver, comp, fn):
    p = os.path.join(R, "upstream", "pool", comp, fn)
    st = os.stat(p)
    md5 = hashlib.md5(open(p, "rb").read()).hexdigest()
    return "\n".join([f"Package: {name}", f"Version: {ver}",
                      "Architecture: amd64", "Maintainer: T <t@e.com>",
                      f"Description: {name}",
                      f"Filename: pool/{comp}/{fn}",
                      f"Size: {st.st_size}", f"MD5sum: {md5}", "", ""])
def trec(name, ver):
    return "\n".join([f"Package: {name}", f"Version: {ver}",
                      "Architecture: amd64", "Maintainer: T <t@e.com>",
                      f"Description: {name}", "", ""])
# 上游 bbb 2.0 -> 3.0
wb(f"{R}/upstream/pool/main/bbb.deb", "PKG:bbb V:3.0\n")
wb(f"{R}/upstream/dists/abc-updates/main/binary-amd64/Packages",
   pkg_rec("bbb", "3.0", "main", "bbb.deb") + pkg_rec("www-bin", "1.0", "main", "www-bin.deb"))
# 目标 Packages：模拟后端已导入 aaa 1.0 / bbb 2.0 到 abc/main
wb(f"{R}/target/dists/abc/main/binary-amd64/Packages",
   trec("aaa", "1.0") + trec("bbb", "2.0"))
PY
rm -rf $R/lists
$BIN update >$R/update2.log 2>&1 || { echo "FAIL: update2"; cat $R/update2.log; exit 1; }
$BIN upgrade -y >/dev/null 2>&1
check "A-upgrade: bbb 升到 3.0 且回退落在 pool/abc" \
      "$(grep -q 'V:3.0' $R/target/pool/abc/bbb.deb 2>/dev/null && echo 1 || echo 0)"
check "A-upgrade: aaa 未被动（1.0 不变）" \
      "$(grep -q 'V:1.0' $R/target/pool/abc/aaa.deb 2>/dev/null && echo 1 || echo 0)"

# --- 场景 D：目标只有 abc-updates；abc -> abc-updates 反向映射禁止 ---
rmpool
write_target "abc-updates" "main"
$BIN install -y aaa 2>$R/aaa.err >/dev/null
check "D: aaa(abc/main) 被跳过（反向映射禁止）" \
      "$([ -f $R/target/pool/abc-updates/aaa.deb ] && echo 0 || echo 1)"
check "D: aaa 输出跳过警告" \
      "$(grep -q 'skipping aaa' $R/aaa.err && echo 1 || echo 0)"

$BIN install -y bbb >/dev/null 2>&1
check "D: bbb(abc-updates/main) -> pool/abc-updates（同名）" \
      "$([ -f $R/target/pool/abc-updates/bbb.deb ] && echo 1 || echo 0)"

# --- 场景 B：目标 abc + abc-updates；同名优先，不回退 ---
rmpool
write_target "abc abc-updates" "main"
$BIN install -y bbb >/dev/null 2>&1
check "B: bbb -> pool/abc-updates（目标有 abc-updates，同名优先）" \
      "$([ -f $R/target/pool/abc-updates/bbb.deb ] && echo 1 || echo 0)"
check "B: bbb 未落入 pool/abc" \
      "$([ -f $R/target/pool/abc/bbb.deb ] && echo 0 || echo 1)"
$BIN install -y aaa >/dev/null 2>&1
check "B: aaa(abc/main) -> pool/abc（同名）" \
      "$([ -f $R/target/pool/abc/aaa.deb ] && echo 1 || echo 0)"

# --- 场景 C：目标声明 main+contrib；contrib 同名映射允许（组件映射方向） ---
rmpool
write_target "abc" "main contrib"
$BIN install -y ccc >/dev/null 2>&1
check "C: ccc(abc/contrib) -> pool/abc（目标有 contrib，同名允许）" \
      "$([ -f $R/target/pool/abc/ccc.deb ] && echo 1 || echo 0)"
$BIN install -y aaa >/dev/null 2>&1
check "C: aaa(abc/main) -> pool/abc（同名 main）" \
      "$([ -f $R/target/pool/abc/aaa.deb ] && echo 1 || echo 0)"

# --- 场景 C2：目标只有 abc/contrib；abc/main 跨组件映射被禁 ---
rmpool
write_target "abc" "contrib"
$BIN install -y aaa 2>$R/c2.err >/dev/null
check "C2: aaa(abc/main) 被跳过（abc/main -> abc/contrib 跨组件禁止）" \
      "$([ -f $R/target/pool/abc/aaa.deb ] && echo 0 || echo 1)"
check "C2: aaa 输出跳过警告" \
      "$(grep -q 'skipping aaa' $R/c2.err && echo 1 || echo 0)"
$BIN install -y ccc >/dev/null 2>&1
check "C2: ccc(abc/contrib) -> pool/abc（同名 contrib 允许）" \
      "$([ -f $R/target/pool/abc/ccc.deb ] && echo 1 || echo 0)"

# ================= Sources 映射（Sync-Sources: yes） =================
write_conf yes
rmpool
write_target "abc" "main"
rm -rf $R/lists
$BIN update >$R/update3.log 2>&1 || { echo "FAIL: update3"; cat $R/update3.log; exit 1; }

$BIN install -y zzz-bin >/dev/null 2>&1
check "S: zzz-bin(abc/main) -> pool/abc（同名）" \
      "$([ -f $R/target/pool/abc/zzz-bin.deb ] && echo 1 || echo 0)"
check "S: source zzz(abc/main) -> srcpool/abc（同名）" \
      "$([ -f $R/target/srcpool/abc/zzz.dsc ] && echo 1 || echo 0)"

$BIN install -y www-bin >/dev/null 2>&1
check "S: www-bin(abc-updates/main) -> pool/abc（变体回退）" \
      "$([ -f $R/target/pool/abc/www-bin.deb ] && echo 1 || echo 0)"
check "S: source www(abc-updates/main) -> srcpool/abc（变体回退）" \
      "$([ -f $R/target/srcpool/abc/www.dsc ] && echo 1 || echo 0)"

# ================= 汇总 =================
if [ $fail -eq 0 ]; then
    echo "suite-map: ALL PASS"
    exit 0
else
    echo "suite-map: FAILED"
    exit 1
fi
