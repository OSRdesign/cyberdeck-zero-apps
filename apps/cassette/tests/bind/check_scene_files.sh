#!/bin/sh
# SPDX-License-Identifier: MIT
#
# After the bound scenes (build.sh host): the files each one must have written in its demo home, per screen size
# (tests/bind/scenes/expect-files.txt). The scenes run the real controller on the scripted sound system, so these are real
# WAV files of a generated tone.
#   sh tests/bind/check_scene_files.sh <expect-files.txt> <harness out dir>
set -u
list=$1
out=$2
fail=0
checked=0
while read -r scene globs; do
    case $scene in '' | '#'*) continue ;; esac
    found_size=0
    for dir in "$out/$scene"/*x*/; do
        [ -d "$dir" ] || continue
        found_size=1
        rec="${dir}runtime/applaunch/demo-home/Recordings"
        size=$(basename "$dir")
        for g in $globs; do
            checked=$((checked + 1))
            hit=
            # a|b: either pattern
            for alt in $(echo "$g" | tr '|' ' '); do
                # shellcheck disable=SC2086
                set -- $rec/$alt
                if [ -f "$1" ]; then hit=$1; break; fi
            done
            if [ -n "$hit" ]; then
                echo "PASS $scene $size: $(basename "$hit")"
            else
                echo "FAIL $scene $size: no $g in $rec"
                fail=1
            fi
        done
    done
    if [ $found_size = 0 ]; then
        echo "FAIL $scene: no output in $out/$scene"
        fail=1
    fi
done < "$list"
echo "scene files: $checked checked, $([ $fail = 0 ] && echo all present || echo SOME MISSING)"
exit $fail
