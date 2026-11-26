#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prints one field of a bridge reply line read from stdin: real_field.py result.lxmf_address (used by run_real_chat.sh)."""
import json
import sys

v = json.loads(sys.stdin.readline())
for key in sys.argv[1].split("."):
    v = v[key]
print(v)
