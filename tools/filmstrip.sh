#!/bin/sh
# filmstrip — render a range of frames and lay them out as one image.
#
# This exists because every throwaway one-liner written to "just check something" got the
# crop box wrong, or ate its own exit code through a pipe, or reached for a gawk builtin
# that BSD awk doesn't have. Measurement tools that live in the repo get reviewed and get
# reused; measurement tools improvised in the moment are disposable, and so is their
# correctness.
#
#   tools/filmstrip.sh <game> <from> <to> <step> [out.png]
#
# It deliberately does not crop. A crop is a guess about where to look, and a guess about
# where to look is how you end up measuring the wrong part of the frame with great
# precision.
set -e
GAME=${1:-title}
FROM=${2:-0}
TO=${3:-840}
STEP=${4:-60}
OUT=${5:-/tmp/filmstrip.png}
W=${W:-400}
H=${H:-240}

[ -x ./cvertex ] || ./build.sh >/dev/null

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
N=0
F=$FROM
while [ "$F" -le "$TO" ]; do
  ./cvertex --game "$GAME" --res "$W" "$H" --ppm "$F" > "$TMP/$(printf '%05d' $N).ppm" 2>/dev/null
  N=$((N + 1))
  F=$((F + STEP))
done

python3 - "$TMP" "$OUT" "$FROM" "$STEP" <<'PY'
import sys, glob
from PIL import Image, ImageDraw
tmp, out, start, step = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
fs = sorted(glob.glob(tmp + "/*.ppm"))
ims = [Image.open(f) for f in fs]
w, h = ims[0].size
cols = min(6, len(ims))
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (w * cols + 2 * (cols - 1), (h + 12) * rows), (18, 18, 22))
d = ImageDraw.Draw(sheet)
for i, im in enumerate(ims):
    x, y = (i % cols) * (w + 2), (i // cols) * (h + 12)
    sheet.paste(im, (x, y))
    d.text((x + 3, y + h + 1), f"{start + i*step}", fill=(140, 140, 150))
sheet.save(out)
print(f"{len(ims)} frames -> {out}")
PY
