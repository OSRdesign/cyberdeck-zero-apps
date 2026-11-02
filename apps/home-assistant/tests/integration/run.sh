#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The integration test of the Home Assistant app: the real client of src/core (connection thread, libcurl WebSocket,
# HTTP token calls, pairing listener, files) against the fake Home Assistant tests/fake_ha/fake_ha.py on 127.0.0.1.
# Never talks to a real Home Assistant.
#
#   sh apps/home-assistant/tests/integration/run.sh
#
# Needs g++, python3 and a libcurl with WebSocket support (8.11 or newer, built with it): the PC's own libcurl often
# lacks it (Ubuntu 24.04's 8.5.0 does), so the test opens HA_LIBCURL, default $CURLROOT/lib/libcurl.so.4 with CURLROOT
# default ~/ha-spike/curlroot (curl 8.14.1, the boards' version, built from source for the spike of study 052; the
# headers there also serve the ABI check). ARM_CURL_INCLUDE=<sysroot>/usr/include[/aarch64-linux-gnu] and
# ARM_CXX=aarch64-linux-gnu-g++ add the same ABI check against the boards' headers.
#
# Steps: the ABI check (check_curl_abi.cpp: every libcurl constant the app declares equals the headers'), the test
# program (integration.cpp), the fake server (2,000 entities, 20 areas, 1 KB frames, 40 s tokens), the run, then every
# token the fake issued is searched for in every log: none may be there. Output in $OUT (default
# ${CDK_BUILD:-~/cdk-build/home-assistant}/integration).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/../.." && pwd)
CURLROOT=${CURLROOT:-$HOME/ha-spike/curlroot}
HA_LIBCURL=${HA_LIBCURL:-$CURLROOT/lib/libcurl.so.4}
out=${OUT:-${CDK_BUILD:-$HOME/cdk-build/home-assistant}/integration}
port=${PORT:-18765}
rm -rf "$out"
mkdir -p "$out/work"

echo "== ABI check against $CURLROOT/include"
g++ -std=c++17 -I"$CURLROOT/include" -I"$app/src/core" "$here/check_curl_abi.cpp" -o "$out/check-curl-abi"
"$out/check-curl-abi"
if [ -n "${ARM_CURL_INCLUDE:-}" ]; then
    echo "== ABI check against the boards' headers ($ARM_CURL_INCLUDE)"
    ${ARM_CXX:-aarch64-linux-gnu-g++} -std=c++17 -fsyntax-only -I"$ARM_CURL_INCLUDE" -I"$app/src/core" "$here/check_curl_abi.cpp"
    echo "aarch64: identical"
fi

echo "== build ha-integration"
g++ -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -I"$app/src/core" "$app"/src/core/*.cpp \
    "$here/integration.cpp" -lpthread -o "$out/ha-integration"

echo "== fake Home Assistant on 127.0.0.1:$port"
python3 -I "$app/tests/fake_ha/fake_ha.py" --port "$port" --entities 2000 --areas 20 --frag 1024 --expires 40 \
    --secrets "$out/work/secrets.txt" > "$out/fake.log" 2>&1 &
fake=$!
trap 'kill $fake 2>/dev/null || true' EXIT INT TERM
sleep 1

echo "== run (libcurl: $HA_LIBCURL)"
rc=0
HA_LIBCURL=$HA_LIBCURL "$out/ha-integration" "$port" "$out/work" > "$out/integration.log" 2> "$out/client.log" || rc=$?
cat "$out/integration.log"

echo "== tokens in the logs"
issued=0
leaks=0
while IFS= read -r tok; do
    [ -n "$tok" ] || continue
    issued=$((issued + 1))
    if grep -qF -- "$tok" "$out/integration.log" "$out/client.log" "$out/fake.log"; then
        leaks=$((leaks + 1))
    fi
done < "$out/work/secrets.txt"
echo "tokens and codes issued by the fake: $issued; found in a log: $leaks"
[ "$leaks" -eq 0 ] || rc=1
echo "client log: $out/client.log ($(wc -l < "$out/client.log") lines)"
exit "$rc"
