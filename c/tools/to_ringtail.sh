#!/bin/sh
# Copy Barb's core and its RingTail platform into a RingTail checkout, where its Makefile builds
# them into the program BARB: tools/to_ringtail.sh PATH/TO/RINGTAIL
#
# This repository is where Barb is changed; vendor/barb in RingTail is a copy, replaced whole each
# time, with a note of the commit it came from. Build and test RingTail afterwards (make test there).
set -eu
rt=${1:?usage: tools/to_ringtail.sh PATH/TO/RINGTAIL}
here=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$rt/src/include/ringtail/abi.h" ] || { echo "$rt does not look like a RingTail checkout" >&2; exit 1; }
dst=$rt/vendor/barb
rm -rf "$dst/core" "$dst/ringtail" "$dst/include"
mkdir -p "$dst/core" "$dst/ringtail" "$dst/include"
cp "$here"/src/core/*.c "$here"/src/core/*.h "$dst/core/"
cp "$here"/src/ringtail/*.c "$here"/src/ringtail/*.h "$dst/ringtail/"
# The core includes <string.h>; on RingTail the string functions are in lib.h.
printf '%s\n' '// The core'"'"'s <string.h>, on RingTail: the string functions are in lib.h.' '#include "lib.h"' > "$dst/include/string.h"
rev=$(git -C "$here" rev-parse --short HEAD)
if [ -n "$(git -C "$here" status --porcelain -- src)" ]; then rev="$rev, with changes not yet committed"; fi
cat > "$dst/README.md" <<EOF
# Barb (vendored)

Barb is the coding agent from github.com/tdotlab/barb: a core that does no I/O (\`core/\`) and a
platform layer for RingTail (\`ringtail/\`). The Makefile builds them into the program \`barb\`.

This folder is a copy, made by \`tools/to_ringtail.sh\` in the barb repository from commit $rev.
Change Barb there and copy it again; changes made here are lost at the next copy.
EOF
echo "copied barb $rev into $dst"
