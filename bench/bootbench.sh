#!/bin/bash
# Usage: bootbench.sh <temu-binary> <cfg> <tag>
# Boots, runs 3 awk integer-loop samples, prints "TAG real ...".
# stdbuf keeps grep line-buffered so samples stream out live.
bin="$1"; cfg="$2"; tag="$3"
( printf 'time awk "BEGIN{s=0;for(i=0;i<1500000;i++)s+=i;print s}"\n'
  printf 'time awk "BEGIN{s=0;for(i=0;i<1500000;i++)s+=i;print s}"\n'
  printf 'time awk "BEGIN{s=0;for(i=0;i<1500000;i++)s+=i;print s}"\n'
  printf 'poweroff -f\n'
  sleep 300 ) | timeout 360 "$bin" "$cfg" 2>/dev/null \
  | stdbuf -oL grep -a -E 'real' | stdbuf -oL sed "s/^/$tag /"
