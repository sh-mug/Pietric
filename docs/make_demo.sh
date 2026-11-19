#!/usr/bin/env bash
# Makes docs/demo.gif: tests/cases/pietric.png on the left, and on the right a terminal that
# compiles and runs it and a benchmark program, recorded with VHS from docs/demo.tape.
#
# Usage: docs/make_demo.sh <pietric>   (needs vhs, ttyd, ffmpeg and Python 3 with Pillow)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
pietric=$(realpath "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp "$pietric" "$work/Pietric"
cp "$root/tests/cases/pietric.png" "$work/pietric.png"
python3 "$root/bench/programs.py" "$work/programs"
cp "$work/programs/summod.png" "$work/summod.png"
cp "$root/docs/demo.tape" "$work/demo.tape"
# A plain prompt, on a new line when the output before it did not end with one.
cat > "$work/demo.rc" <<'RC'
PS1='$ '
newline_if_needed() {
    local row col
    IFS='[;' read -rs -d R -p $'\e[6n' _ row col
    [ "$col" -gt 1 ] && echo
}
PROMPT_COMMAND=newline_if_needed
RC
(cd "$work" && vhs demo.tape)

# The left panel: the program itself, enlarged, on the terminal's background.
python3 - "$root/tests/cases/pietric.png" "$work/panel.png" <<'PY'
import sys
from PIL import Image, ImageDraw, ImageFont
program = Image.open(sys.argv[1]).convert("RGB")
panel = Image.new("RGB", (400, 460), (23, 23, 23))
art = program.resize((program.width * 3, program.height * 3), Image.NEAREST)
panel.paste(art, ((400 - art.width) // 2, 50))
font = ImageFont.truetype("DejaVuSansMono.ttf", 20)
draw = ImageDraw.Draw(panel)
draw.text((200, 415), "pietric.png", fill=(200, 200, 200), font=font, anchor="mm")
panel.save(sys.argv[2])
PY

ffmpeg -loglevel error -y -i "$work/panel.png" -i "$work/terminal.gif" -filter_complex \
    "[0:v][1:v]hstack=inputs=2,split[a][b];[a]palettegen=max_colors=64[p];[b][p]paletteuse" \
    "$root/docs/demo.gif"
echo "wrote $root/docs/demo.gif"
