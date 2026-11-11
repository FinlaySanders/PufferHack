#!/bin/bash
# stock/build.sh: the stock reference in stock/build/ref (libnethack.so, nethackdir/), rebuilt from pinned inputs (refs.toml
# [stock]): NLE 0.9.0's sdist, NetHack 3.7's attknum line (attknum.patch), CMake Release with gcc as pip builds NLE 0.9.0
# (target nethack), and the certified reference's __FILE__ prefix and build stamp. Needs gcc, g++, cmake, make, patch, python3.
# The sdist comes from $NLE_SDIST, else stock/build/dl/, else sdist_url (curl, or python3 when curl is missing).
# JOBS (default 8) parallel compile jobs.
set -e
V=$(cd "$(dirname "$0")/.." && pwd); O=$V/stock/build; J=${JOBS:-8}
pin() { awk -v s="[$1]" -v k="$2" '/^\[/ {sec = ($0 == s)} sec && $1 == k {sub(/^[^=]*= */, ""); gsub(/"/, ""); print; exit}' "$V/refs.toml"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
die() { echo "STOCK_FAIL $*" >&2; exit 1; }
SD=$(pin stock sdist); P=$V/$(pin stock patch); PFX=$(pin stock file_prefix); EP=$(pin stock build_epoch); CCV=$(pin stock compiler)
for t in gcc g++ cmake make patch python3; do command -v $t > /dev/null || die "$t not found"; done
mkdir -p $O/dl
T=${NLE_SDIST:-$O/dl/$SD}
if [ ! -s "$T" ]; then
  U=$(pin stock sdist_url); echo "fetching $U"
  if command -v curl > /dev/null; then curl -fsSL -o "$T.part" "$U" || die "download $U"
  else python3 -I -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$U" "$T.part" || die "download $U"; fi
  mv "$T.part" "$T"
fi
[ "$(sha "$T")" = "$(pin stock sdist_sha256)" ] || die "$T has sha256 $(sha "$T"), refs.toml pins $(pin stock sdist_sha256)"
[ "$(sha "$P")" = "$(pin stock patch_sha256)" ] || die "$P has sha256 $(sha "$P"), refs.toml pins $(pin stock patch_sha256)"
G=$(gcc --version | head -1)
[ "$G" = "$CCV" ] || echo "note: gcc here is '$G', the reference was built with '$CCV': the library will differ in bytes (certify.sh --ref-any)"

rm -rf $O/src $O/cmake $O/ref $O/date.mtime; mkdir -p $O/src
tar -xzf "$T" -C $O/src; S=$(cd $O/src/nle-0.9.0 && pwd -P)
patch -s -p1 -d $S < "$P"
# pybind11 looks Python up as pip would hand it over (no Python target is built, the headers need not exist)
PY=$(command -v python3); read PL PI < <($PY -I -c 'import sysconfig; print(sysconfig.get_config_var("LIBDIR"), sysconfig.get_paths()["include"])')
CC=gcc CXX=g++ cmake -S $S -B $O/cmake -DCMAKE_BUILD_TYPE=Release -DUSE_SEEDING=ON -DPYTHON_EXECUTABLE=$PY -DPYTHON_LIBRARY=$PL -DPYTHON_INCLUDE_DIR=$PI \
  "-DCMAKE_C_FLAGS=-fmacro-prefix-map=$S=$PFX" "-DCMAKE_CXX_FLAGS=-fmacro-prefix-map=$S=$PFX" > $O/cmake.log 2>&1 \
  || die "cmake configure ($O/cmake.log)"
cmake --build $O/cmake --target nethack -j$J > $O/build.log 2>&1 || die "build ($O/build.log)"

# the build stamp: makedefs wrote the current time; version.c alone includes date.h and is rebuilt with the pinned one
D=$O/cmake/include/date.h
touch -r $D $O/date.mtime
python3 -I - $D $EP <<'PY'
import re, sys, time
p, ep = sys.argv[1], int(sys.argv[2])
s = open(p).read()
od = re.search(r'#define BUILD_DATE "([^"]*)"', s).group(1); rd = time.asctime(time.gmtime(ep))
ot = re.search(r'#define BUILD_TIME \((\d+)UL\)', s).group(1)
open(p, 'w').write(s.replace(od, rd).replace(f'({ot}UL)', f'({ep}UL)').replace(od[4:], rd[4:]))
PY
touch -r $O/date.mtime $D
[ "$(sha $D)" = "$(pin stock date_h_sha256)" ] || die "$D has sha256 $(sha $D), refs.toml pins $(pin stock date_h_sha256)"
rm -f $O/cmake/CMakeFiles/nethack.dir/src/version.c.o
cmake --build $O/cmake --target nethack -j$J >> $O/build.log 2>&1 || die "relink ($O/build.log)"

mkdir -p $O/ref/nethackdir
cp $O/cmake/libnethack.so $O/ref/
for f in $O/cmake/dat/*; do case ${f##*/} in CMakeFiles | Makefile | cmake_install.cmake) ;; *) cp -p "$f" $O/ref/nethackdir/ ;; esac; done
L=$(sha $O/ref/libnethack.so); N=$(sha $O/ref/nethackdir/nhdat)
[ "$N" = "$(pin stock nhdat_sha256)" ] || die "nhdat has sha256 $N, refs.toml pins $(pin stock nhdat_sha256)"
echo "$L" > $O/ref/libnethack.so.sha256
if [ "$L" = "$(pin stock library_sha256)" ]; then echo "STOCK_OK $O/ref/libnethack.so $L (the pinned reference, byte for byte)"
else echo "STOCK_DIFFERS $O/ref/libnethack.so $L, refs.toml pins $(pin stock library_sha256) (compiler '$G')"; fi
