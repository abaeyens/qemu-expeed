#!/usr/bin/env bash
# Check the kernel-doc comments (/** ... */) in the QEMU sources with QEMU's own checker.
# Prints nothing and exits 0 when they are well-formed; any warning fails the check.
#
# Usage: scripts/check_qemu_docs.sh [file...]   (default: qemu/*.h)
# Env:   QEMU_SRC=<dir>   QEMU source tree holding scripts/kernel-doc (default /opt/qemu-9.1.0)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_DOC="${QEMU_SRC:-/opt/qemu-9.1.0}/scripts/kernel-doc"

if [ ! -x "$KERNEL_DOC" ]; then
  echo "error: no kernel-doc at $KERNEL_DOC; set QEMU_SRC to a QEMU source tree" >&2
  exit 1
fi

if [ $# -eq 0 ]; then
  set -- "$ROOT"/qemu/*.h
fi
# kernel-doc exits 0 on an error (e.g. a struct it cannot parse) when it is only checking,
# so any output fails too
if out="$("$KERNEL_DOC" -none -Werror "$@" 2>&1)" && [ -z "$out" ]; then
  exit 0
fi
printf '%s\n' "$out" >&2
exit 1
