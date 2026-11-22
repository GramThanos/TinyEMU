#!/bin/sh
# Round-trip inflate.c against gzip(1): empty, text, random (stored blocks),
# fixed-Huffman and long-match inputs at several levels.
#   make inflate-test && sh tests/inflate_test.sh ./inflate-test
set -e
bin=${1:-./inflate-test}
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

: >"$d/empty"
printf 'a' >"$d/one"
cat "$(dirname "$0")"/../*.c >"$d/text"
head -c 300000 /dev/urandom >"$d/random"
head -c 200000 /dev/zero >"$d/zeros"
{ head -c 70000 /dev/urandom; cat "$d/text"; head -c 70000 /dev/zero; } >"$d/mixed"

rc=0
for f in empty one text random zeros mixed; do
    for lvl in 1 6 9; do
        gzip -"$lvl" -n -c "$d/$f" >"$d/$f.$lvl.gz"
        "$bin" "$d/$f.$lvl.gz" "$d/$f" || rc=1
    done
done
# with a stored file name in the header (FNAME)
gzip -c "$d/text" >"$d/named.gz"
"$bin" "$d/named.gz" "$d/text" || rc=1
[ $rc = 0 ] && echo "INFLATE TEST PASSED" || echo "INFLATE TEST FAILED"
exit $rc
