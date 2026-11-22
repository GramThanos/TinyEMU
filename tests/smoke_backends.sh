#!/bin/sh
# Build-artifact smoke using prebuilt dist artifacts.
#
# The software x86 CPU (x86_cpu.c) is still a work in progress, so this
# script does not yet attempt
# to boot an x86 guest. It verifies the expected binaries were produced and are
# executable. RISC-V guests are functional and can be run manually via the
# `run` compose service.
set -e

if [ ! -x ./dist/linux/temu-soft ] || [ ! -x ./dist/linux/temu-kvm ]; then
  echo "Missing dist/linux binaries. Run: docker compose run --rm build"
  exit 1
fi

if [ ! -f ./dist/windows/temu.exe ]; then
  echo "Missing dist/windows/temu.exe. Run: docker compose run --rm build"
  exit 1
fi

echo "Build artifacts present:"
echo "  - dist/linux/temu-soft   (software CPU, WASM-targetable path)"
echo "  - dist/linux/temu-kvm    (native KVM, Linux-host debugging)"
echo "  - dist/windows/temu.exe"
echo "Smoke complete."
