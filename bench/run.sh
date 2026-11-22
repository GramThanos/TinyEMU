#!/bin/bash
# Guest-side timing: the guest's clock is driven by the host PIT, so `time`
# inside the guest measures host wall-clock spent emulating that work.
#
# The awk loop is the signal (a tight integer/branch loop, i.e. the flag-heavy
# path); the dd runs are I/O-bound noise kept as a sanity check. Two awk
# samples per binary, and the trailing sleep must outlast them or the last
# sample is lost to the timeout -- size it against the *slower* binary.
run_one() {
  bin="$1"; tag="$2"
  cp "$bin" /tmp/t && chmod +x /tmp/t
  ( printf 'time dd if=/dev/zero of=/dev/null bs=1M count=64\n'
    printf 'time awk "BEGIN{s=0;for(i=0;i<1500000;i++)s+=i;print s}"\n'
    printf 'time awk "BEGIN{s=0;for(i=0;i<1500000;i++)s+=i;print s}"\n'
    sleep 600 ) | timeout 900 /tmp/t /workspace/tests/alpine-x86.cfg 2>/dev/null \
    | sed -n 's/^real[^0-9]*/'"$tag"' real /p'
}
run_one /workspace/bench/temu-base BASE
run_one /workspace/bench/temu-new  NEW
