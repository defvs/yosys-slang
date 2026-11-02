#!/usr/bin/env bash
# Copyright (c) 2026 The yosys-slang contributors
# SPDX-License-Identifier: ISC

# Check out the OpenTitan revision the stress tests are written against

set -euo pipefail

OT="$1"
OT_URL=https://github.com/lowRISC/opentitan.git
OT_REVISION=98a020b74d02c316fc2cc682bb212214b5c27e0e

if [ -e "$OT/.git" ] && [ "$(git -C "$OT" rev-parse HEAD)" = "$OT_REVISION" ]; then
  exit 0
fi

mkdir -p "$OT"
git -C "$OT" init -q
git -C "$OT" fetch -q --depth 1 "$OT_URL" "$OT_REVISION"
git -C "$OT" checkout -q --detach FETCH_HEAD
