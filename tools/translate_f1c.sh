#!/bin/sh
# Translates the readable F1 Challenge exe into out_all/ (then ./build_web.sh builds it):
#   tools/translate_f1c.sh [exe] [functions.json]
# The CRT setjmp/longjmp entries are the game's own (see README); host/f1c.patches holds the preferred defaults.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
EXE=${1:-$R/work/exe_candidate/f1c.exe}
FUNCS=${2:-$R/work/functions.json}
PATCHES=$(sed -e 's/[[:space:]]*#.*//' -e '/^$/d' "$R/host/f1c.patches" | tr '\n' ' ')
cd "$R"
${PY:-.venv/bin/python} -I translator/x86toc.py "$EXE" "$FUNCS" out_all --all --per-file 300 \
    --setjmp 0x58a7ec --longjmp 0x58a770 $PATCHES | head -1
