#!/bin/sh
# SPDX-License-Identifier: MIT
# A stand-in for `systemctl --user ...` in the app's scenes (RETICULUM_SYSTEMCTL points here): nothing is started or
# enabled on the machine. It logs its arguments, keeps "enabled" / "disabled" in a file per canvas size, and answers:
#   is-enabled reticulum   prints enabled or disabled (exit 0 / 1, as systemctl does)
#   enable / disable       writes the state; FAKE_ENABLE_RC (default 0) is the exit status, with a message when not 0
#   start reticulum        FAKE_START_RC (default 0) is the exit status, with FAKE_START_MSG on stderr when not 0
#   stop reticulum         exit 0 (the app asks for it with --no-block when it leaves)
# Environment: FAKE_SYSTEMCTL_LOG (a file; every call is one line "<canvas> <arguments>"), FAKE_UNIT_DIR (folder of the
# state files). The canvas comes from APPLAUNCH_CANVAS, which the scene harness sets for the app and its children.
canvas=${APPLAUNCH_CANVAS:-none}
log=${FAKE_SYSTEMCTL_LOG:-/dev/null}
state=${FAKE_UNIT_DIR:-/tmp}/unit-$canvas
echo "$canvas $*" >> "$log"
verb=
for a in "$@"; do
    case $a in
        -*) ;;
        *) verb=$a; break ;;
    esac
done
case $verb in
    is-enabled)
        s=$(cat "$state" 2>/dev/null || echo "${FAKE_UNIT_INITIAL:-disabled}")
        echo "$s"
        [ "$s" = enabled ]
        ;;
    enable | disable)
        if [ "${FAKE_ENABLE_RC:-0}" != 0 ]; then
            echo "Failed to $verb unit: Access denied" >&2
            exit "$FAKE_ENABLE_RC"
        fi
        [ "$verb" = enable ] && echo enabled > "$state" || echo disabled > "$state"
        ;;
    start)
        if [ "${FAKE_START_RC:-0}" != 0 ]; then
            echo "${FAKE_START_MSG:-Failed to start reticulum.service: Unit reticulum.service not found.}" >&2
            exit "$FAKE_START_RC"
        fi
        ;;
    stop) ;;
    *) exit 0 ;;
esac
