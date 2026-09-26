#!/bin/sh
# Regenerates ../desk_fonts.c (needs FreeType and the Roboto fonts)
set -e
cd "$(dirname "$0")"
R=/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF
cc -O2 -Wall -o /tmp/mkfont.$$ mkfont.c $(pkg-config --cflags --libs freetype2)
/tmp/mkfont.$$ ui:$R/Roboto-Regular.ttf:13 uib:$R/Roboto-Medium.ttf:13 \
  small:$R/Roboto-Regular.ttf:11 title:$R/Roboto-Medium.ttf:20 \
  big:$R/Roboto-Light.ttf:44 > ../desk_fonts.c
rm -f /tmp/mkfont.$$
