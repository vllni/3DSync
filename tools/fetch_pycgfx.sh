#!/bin/sh
# Fetches pycgfx (https://github.com/skyfloogle/pycgfx), which
# tools/make_banner.py uses to turn the banner scene into a CGFX, at a pinned
# commit into a directory of your choosing, and checks it against a known
# hash.  pycgfx has no licence file, so it is fetched for each use and never
# stored in this repository.  It needs Python 3.12 or newer and gltflib
# (pip install gltflib).
#
#   tools/fetch_pycgfx.sh /tmp/pycgfx
#   tools/make_banner.py cgfx <bold font> /tmp/pycgfx meta/banner_3ds.cgfx
set -eu

COMMIT=1f78850086f3a77c41e07162e842f97a5bf3c18a
# sha256sum of "sha256sum" over main.py and cgfx/*.py, sorted by path.
SOURCES_SHA256=7360d3d7bbb5b5c5e62fcf615130ba9a6e499e3d29bf922d6301e113d7f1ce96

dir=${1:?usage: $0 <directory>}
mkdir -p "$dir"
curl -fsSL "https://codeload.github.com/skyfloogle/pycgfx/tar.gz/$COMMIT" |
    tar xz -C "$dir" --strip-components=1
actual=$(cd "$dir" && find main.py cgfx -name '*.py' | sort | xargs sha256sum | sha256sum | cut -d' ' -f1)
if [ "$actual" != "$SOURCES_SHA256" ]; then
    echo "pycgfx at $COMMIT does not match the expected hash" >&2
    exit 1
fi
echo "pycgfx $COMMIT in $dir"
