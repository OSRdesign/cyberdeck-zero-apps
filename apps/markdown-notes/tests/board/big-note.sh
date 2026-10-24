#!/bin/sh
# Markdown Notes, board test B4 (tests/BOARD-TESTS.md): writes a note of about 100 KB into the vault for the typing-lag
# test of report 037 (lv_textarea does O(n) work per key: this decides between the 128 KB cap and the custom editor E2).
#   sh big-note.sh [KB] [vault]
#   KB default 100 (keep it under 128 to stay editable; 200 shows the read-only path); vault default $NOTES_VAULT, else
#   the vault= of ~/.config/markdown-notes/settings.conf, else ~/Notes.
# Writes "<vault>/Big note <KB> KB.md" (a temp file, then a rename: the app never sees half of it). Remove it by hand,
# or with Delete in the app (it goes to <vault>/.trash).
set -e
kb=${1:-100}
vault=${2:-${NOTES_VAULT:-}}
if [ -z "$vault" ]; then
    conf=${XDG_CONFIG_HOME:-$HOME/.config}/markdown-notes/settings.conf
    v=$(sed -n 's/^vault=//p' "$conf" 2> /dev/null | tail -n 1)
    case $v in
        "") vault=$HOME/Notes ;;
        "~") vault=$HOME ;;
        "~/"*) vault=$HOME/${v#\~/} ;;
        /*) vault=$v ;;
        *) vault=$HOME/$v ;;
    esac
fi
mkdir -p "$vault"
out="$vault/Big note $kb KB.md"
tmp="$vault/.big-note.~tmp"
want=$((kb * 1024))
{
    printf '# A big note (%s KB)\n\nWritten by big-note.sh for the typing test: type at the end and in the middle.\n\n' "$kb"
    n=0
    size=0
    while [ "$size" -lt "$want" ]; do
        n=$((n + 1))
        line="## Section $n

Some **bold** text, some *italic* text and a [[Link $n]] in a paragraph that goes on for a while so that it wraps over a few lines on the deck's screen.

- [ ] task $n one
- [x] task $n two
- a plain item

"
        printf '%s' "$line"
        size=$((size + ${#line}))
    done
} > "$tmp"
mv "$tmp" "$out"
echo "wrote $out ($(wc -c < "$out") bytes)"
