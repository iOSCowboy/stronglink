#!/bin/sh
set -e

# HACK: This lets us work offline. Run update.sh manually if you want it.
#./update.sh
mkdir -p m4
autoreconf -i -f

# Patch libtool 2.4.2 to pass -fstack-protector as a linker argument
sed 's/-fuse-linker-plugin)/-fuse-linker-plugin|-fstack-protector*)/' \
  ltmain.sh > ltmain.sh.fixed
mv -f ltmain.sh.fixed ltmain.sh
