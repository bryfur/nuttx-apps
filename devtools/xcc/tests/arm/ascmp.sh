#!/bin/sh
# ascmp.sh FILE.s: assemble with GNU as and xcc's as, compare the instructions
. "$(dirname "$0")/env.sh"
f=$1; t=$(mktemp -d)
arm-none-eabi-as -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard -o $t/g.o $f || exit 1
$A/as -o $t/x.o $f || exit 1
norm() {
  arm-none-eabi-objdump -d --no-show-raw-insn $1 | grep -P '^\s+[0-9a-f]+:\t' | cut -f2- |
    sed -e 's/\.[wn]\b//; s/\s*@.*//; s/;.*//' |
    awk '{ if ($1 ~ /^(b|bl|b[a-z][a-z]|cbz|cbnz)$/) print $1; else print }'
}
norm $t/g.o > $t/g.txt; norm $t/x.o > $t/x.txt
diff $t/g.txt $t/x.txt | head -${N:-30}
rm -rf $t
