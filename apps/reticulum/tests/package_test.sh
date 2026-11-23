#!/bin/sh
# SPDX-License-Identifier: MIT
# Tests the built package on a PC (Linux or WSL; python3 with python3-cryptography and python3-serial; x86-64 for the
# amd64 deb): installs it with `dpkg-deb -x` into a scratch root (nothing is installed on the machine) and checks
#   - the package contents (control, modes, no __pycache__, no tests, licence files, maintainer scripts; the app's tile:
#     the .desktop file, the icon, one ELF file (the app, of the deb's architecture, run with --version when it can be)),
#   - the unit file (systemd-analyze verify, when present),
#   - the bridge started from the installed layout the way the unit starts it (PYTHONPATH, temporary HOME and
#     XDG_RUNTIME_DIR, the installed tree read only): hello, status, a second bridge (exit 3), service.stop (exit 0),
#     nothing written under the installed tree,
#   - the maintainer scripts (postinst compiles, prerm and postrm remove the bytecode) with /opt remapped,
#   - the bridge's own tests (apps/reticulum/bridge/tests, unchanged, copied next to the installed bridge) against the
#     installed rns and lxmf.
#   sh apps/reticulum/tests/package_test.sh reticulum_0.2.0_amd64.deb [--no-unit-tests]
set -u
deb=${1:?usage: package_test.sh DEB [--no-unit-tests]}
skip_tests=${2:-}
here=$(cd -- "$(dirname -- "$0")" && pwd)
app=$(dirname "$here")
tmp=$(mktemp -d "${TMPDIR:-/tmp}/reticulum-pkgtest.XXXXXX")
fail=0
ok() { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; fail=1; }
check() { desc=$1; shift; if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi; }
trap 'chmod -R u+w "$tmp" 2>/dev/null; rm -rf "$tmp"' EXIT

root=$tmp/root
mkdir -p "$root" "$tmp/ctl"
dpkg-deb -I "$deb" control > "$tmp/control" || { echo "not a deb: $deb"; exit 2; }
dpkg-deb -x "$deb" "$root"
dpkg-deb -e "$deb" "$tmp/ctl"
dpkg-deb -c "$deb" > "$tmp/listing"
echo "== control"; cat "$tmp/control"

# helpers: each returns 0 when the property holds
listing_lacks() { ! grep -E "$1" "$tmp/listing"; }
elf_files() { find "$root" -type f | while read -r f; do [ "$(head -c4 "$f" | od -An -c | tr -d ' ')" = "177ELF" ] && echo "$f"; done; }
only_app_elf() { [ "$(elf_files)" = "$root/usr/share/APPLaunch/bin/reticulum" ]; }
arch_ok() { want=$(sed -n 's/^Architecture: //p' "$tmp/control"); out=$(file -b "$root/usr/share/APPLaunch/bin/reticulum"); case $want in amd64) echo "$out" | grep -q 'x86-64';; arm64) echo "$out" | grep -q 'aarch64';; *) return 1;; esac; }
none_found() { ! find "$@" | grep -q .; }
only_expected_paths() { ! (cd "$root" && find . -type f) | sed 's|^\./||' | grep -Ev '^(opt/reticulum/|usr/lib/systemd/user/reticulum\.service$|usr/share/doc/reticulum/|usr/share/APPLaunch/bin/reticulum$|usr/share/APPLaunch/applications/reticulum\.desktop$|usr/share/APPLaunch/share/images/reticulum\.png$)' | grep -q .; }
modes_ok() { ! awk '{print $1}' "$tmp/listing" | grep -Ev '^(-rw-r--r--|-rwxr-xr-x|drwxr-xr-x)$' | grep -q .; }
scripts_exec() { for s in postinst prerm postrm; do [ -x "$tmp/ctl/$s" ] || return 1; done; }
modules_present() { for m in __init__ __main__ client options portlock rnsconfig server service; do [ -f "$root/opt/reticulum/bridge/reticulum_bridge/$m.py" ] || return 1; done; }
first_run_config() { c=$home/.reticulum/config; [ "$(grep -c 'type = ' "$c")" = 1 ] && grep -q 'type = AutoInterface' "$c" && grep -q 'enabled = no' "$c" && grep -q 'outgoing = False' "$c"; }
has_all() { f=$1; shift; for p in "$@"; do grep -qF -- "$p" "$f" || return 1; done; }

echo "== contents"
check "control: Package reticulum" grep -q '^Package: reticulum$' "$tmp/control"
check "control: Depends python3 (>= 3.10), python3-cryptography, python3-serial" \
    grep -q '^Depends: .*python3 (>= 3.10).*python3-cryptography.*python3-serial' "$tmp/control"
check "control: Recommends python3-bleak" grep -q '^Recommends: python3-bleak$' "$tmp/control"
check "no __pycache__ or .pyc" listing_lacks '__pycache__|\.pyc$'
check "no tests, peer.py, measure.py, requirements.txt, run_tests.sh" listing_lacks '/tests/|peer\.py|measure\.py|requirements\.txt|run_tests\.sh'
check "one ELF file: the app binary (the service is pure Python)" only_app_elf
check "the app binary is built for the deb's architecture" arch_ok
check "the app binary is 755" sh -c 'grep -E "[.]/usr/share/APPLaunch/bin/reticulum$" "$1" | grep -q -- "^-rwxr-xr-x"' _ "$tmp/listing"
check "the .desktop file: fullscreen, Exec and Icon resolve, X-Portrait, no X-MinSize" sh -c 'd="$1/usr/share/APPLaunch/applications/reticulum.desktop"; grep -q "^X-Fullscreen=true$" "$d" && grep -q "^Exec=/usr/share/APPLaunch/bin/reticulum$" "$d" && grep -q "^Icon=share/images/reticulum.png$" "$d" && grep -q "^X-Portrait=480x640$" "$d" && ! grep -q X-MinSize "$d" && test -x "$1/usr/share/APPLaunch/bin/reticulum" && test -s "$1/usr/share/APPLaunch/share/images/reticulum.png"' _ "$root"
check "the icon is a 100x100 PNG" sh -c 'file -b "$1/usr/share/APPLaunch/share/images/reticulum.png" | grep -q "PNG image data, 100 x 100"' _ "$root"
check "control: Depends the libraries of the binary too" grep -q '^Depends: .*libfreetype6.*libinput10.*libxkbcommon0' "$tmp/control"
check "no build-info with a host path" sh -c '! grep -q "/home/\|/mnt/" "$1/usr/share/doc/reticulum/build-info" 2>/dev/null' _ "$root"
check "no symbolic link" none_found "$root" -type l
check "no setuid, setgid or world-writable file" none_found "$root" '(' -perm -4000 -o -perm -2000 -o -perm -0002 ')'
check "only opt/reticulum, the unit, usr/share/doc/reticulum and the app (binary, .desktop, icon)" only_expected_paths
for f in LICENSE.rns LICENSE.lxmf copyright; do check "usr/share/doc/reticulum/$f" test -s "$root/usr/share/doc/reticulum/$f"; done
check "LICENSE.rns holds the Reticulum License" grep -q 'harm to human beings' "$root/usr/share/doc/reticulum/LICENSE.rns"
check "LICENSE.lxmf holds the Reticulum License" grep -q 'language model training' "$root/usr/share/doc/reticulum/LICENSE.lxmf"
check "modes: files 644 or 755, directories 755" modes_ok
check "the unit and the licence files are 644" sh -c '! grep -E "reticulum\.service|/LICENSE\.|/copyright" "$1" | grep -v -- "^-rw-r--r--" | grep -q .' _ "$tmp/listing"
check "maintainer scripts are 755" scripts_exec
for s in postinst prerm postrm; do check "sh -n $s" sh -n "$tmp/ctl/$s"; done
check "bridge modules present" modules_present
check "bridge copy equals apps/reticulum/bridge/reticulum_bridge" diff -r -x __pycache__ "$app/bridge/reticulum_bridge" "$root/opt/reticulum/bridge/reticulum_bridge"

echo "== the app binary"
want=$(sed -n 's/^Architecture: //p' "$tmp/control")
if [ "$want" = amd64 ] && [ "$(uname -m)" = x86_64 ]; then
    if "$root/usr/share/APPLaunch/bin/reticulum" --version > "$tmp/version.txt" 2>&1; then
        ok "reticulum --version: $(head -1 "$tmp/version.txt")"
        check "--version names the app and the version" grep -q 'Reticulum (reticulum) 0\.' "$tmp/version.txt"
    else
        echo "SKIP  the binary does not run here (missing library?): $(head -1 "$tmp/version.txt")"
    fi
else
    echo "SKIP  --version (not an amd64 deb on an x86-64 machine)"
fi

echo "== unit"
unit=$root/usr/lib/systemd/user/reticulum.service
if command -v systemd-analyze >/dev/null 2>&1; then
    if systemd-analyze verify --user "$unit" > "$tmp/verify.txt" 2>&1; then ok "systemd-analyze verify --user"; else bad "systemd-analyze verify --user"; fi
    sed 's/^/      /' "$tmp/verify.txt"
else
    echo "SKIP  systemd-analyze not found"
fi
check "not enabled at boot (the package ships no .wants link)" none_found "$root" -name '*.wants'
for k in 'PYTHONPATH=/opt/reticulum/bridge:/opt/reticulum/py' 'SuccessExitStatus=3' 'CPUQuota=200%' 'MemoryMax=' 'MemoryHigh=' \
         'Nice=10' 'TasksMax=' 'MALLOC_ARENA_MAX=2' 'XDG_RUNTIME_DIR=%t' 'ExecStart=/usr/bin/python3 -m reticulum_bridge'; do
    check "unit has $k" grep -q -- "$k" "$unit"
done

echo "== bridge from the installed layout"
home=$tmp/home; run=$tmp/run; mkdir -p "$home" "$run"; chmod 700 "$run"
chmod -R a-w "$root"
ENVV="HOME=$home XDG_RUNTIME_DIR=$run PYTHONPATH=$root/opt/reticulum/bridge:$root/opt/reticulum/py MALLOC_ARENA_MAX=2 PYTHONUNBUFFERED=1"
# shellcheck disable=SC2086
env $ENVV python3 -m reticulum_bridge > "$tmp/bridge.log" 2>&1 &
pid=$!
n=0
while [ ! -S "$run/reticulum/bridge.sock" ] && [ $n -lt 150 ] && kill -0 $pid 2>/dev/null; do sleep 0.2; n=$((n + 1)); done
if [ -S "$run/reticulum/bridge.sock" ]; then ok "socket after about $((n * 2)) tenths of a second"; else bad "no socket"; cat "$tmp/bridge.log"; fi
# shellcheck disable=SC2086
env $ENVV python3 -m reticulum_bridge.client hello > "$tmp/hello.txt" 2>&1
echo "      hello:  $(cut -c1-300 "$tmp/hello.txt")"
check "hello ok, protocol 1, rns 1.6.1, lxmf 1.3.1" has_all "$tmp/hello.txt" '"ok": true' '"protocol": 1' '"rns": "1.6.1"' '"lxmf": "1.3.1"'
# shellcheck disable=SC2086
env $ENVV python3 -m reticulum_bridge.client status > "$tmp/status.txt" 2>&1
echo "      status: $(cut -c1-500 "$tmp/status.txt")"
check "status ok, no interface up (first run)" has_all "$tmp/status.txt" '"ok": true' '"interfaces_up": []'
check "first-run config: one interface, AutoInterface, enabled = no, outgoing = False" first_run_config
# shellcheck disable=SC2086
env $ENVV python3 -m reticulum_bridge > "$tmp/second.log" 2>&1; rc=$?
if [ $rc -eq 3 ]; then ok "a second bridge exits 3 (already running)"; else bad "second bridge exit $rc"; fi
# shellcheck disable=SC2086
env $ENVV python3 -m reticulum_bridge.client service.stop > /dev/null 2>&1
wait $pid; rc=$?
if [ $rc -eq 0 ]; then ok "service.stop: clean exit 0"; else bad "bridge exit $rc"; fi
check "log mentions no passphrase or private key" sh -c '! grep -i -E "passphrase|private" "$1"' _ "$tmp/bridge.log"
check "nothing written under the installed tree (no bytecode)" none_found "$root" '(' -name __pycache__ -o -name '*.pyc' ')'
sed 's/^/      /' "$tmp/bridge.log" | head -20

echo "== maintainer scripts (with /opt remapped into the scratch root)"
chmod -R u+w "$root"
for s in postinst prerm postrm; do sed "s|/opt/reticulum|$root/opt/reticulum|g; s|/run/user|$tmp/no-run-user|g" "$tmp/ctl/$s" > "$tmp/$s.sh"; done
start=$(date +%s)
sh "$tmp/postinst.sh" configure; rc=$?
echo "      postinst took $(( $(date +%s) - start )) s (this machine)"
check "postinst exit 0" test $rc -eq 0
check "postinst compiled bytecode" sh -c 'find "$1" -name "*.pyc" | grep -q .' _ "$root"
sh "$tmp/prerm.sh" remove; rc=$?
check "prerm exit 0" test $rc -eq 0
check "prerm removed all bytecode" none_found "$root" -name __pycache__
sh "$tmp/postinst.sh" configure; sh "$tmp/postrm.sh" purge; rc=$?
check "postrm purge exit 0" test $rc -eq 0
check "postrm purge: bytecode gone" none_found "$root" -name '*.pyc'

if [ "$skip_tests" != --no-unit-tests ]; then
    echo "== apps/reticulum/bridge/tests against the installed rns and lxmf (tests unchanged, copied next to the installed bridge)"
    troot=$tmp/troot
    mkdir -p "$troot/opt/reticulum"
    cp -a "$root/opt/reticulum/py" "$root/opt/reticulum/bridge" "$troot/opt/reticulum/"
    cp -a "$app/bridge/tests" "$troot/opt/reticulum/bridge/tests"
    # a venv without pip, only so that sys.executable's folder holds the rnstatus the integration tests call
    python3 -m venv --without-pip --system-site-packages "$tmp/venv"
    printf '#!/bin/sh\nexec "%s/venv/bin/python" -m RNS.Utilities.rnstatus "$@"\n' "$tmp" > "$tmp/venv/bin/rnstatus"
    chmod 755 "$tmp/venv/bin/rnstatus"
    # the integration tests start the bridge with PYTHONPATH set to the bridge folder only: a .pth file in the venv
    # gives every process of the venv the installed rns and lxmf, so the tests themselves stay unchanged
    for sp in "$tmp"/venv/lib/python*/site-packages; do echo "$troot/opt/reticulum/py" > "$sp/reticulum_installed.pth"; done
    export PYTHONDONTWRITEBYTECODE=1
    (cd "$troot/opt/reticulum/bridge" \
        && "$tmp/venv/bin/python" -c 'import RNS, LXMF, sys; print("python", sys.version.split()[0], "| rns", RNS.__version__, "from", RNS.__file__, "| lxmf", LXMF.__version__)' \
        && "$tmp/venv/bin/python" -m unittest discover -s tests -p 'test_*.py' -v) > "$tmp/unit.log" 2>&1
    rc=$?
    head -1 "$tmp/unit.log"; tail -4 "$tmp/unit.log"
    if [ $rc -eq 0 ]; then ok "bridge/tests (installed layout)"; else bad "bridge/tests exit $rc"; grep -E -A30 "^(FAIL|ERROR):" "$tmp/unit.log" | head -90; fi
fi

echo
if [ $fail -eq 0 ]; then echo "ALL PASS"; else echo "FAILURES"; fi
exit $fail
