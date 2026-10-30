#!/bin/sh
# fetch.sh <name> <url> : download a datasheet into its own new dir /tmp/ds/parts/<name>/ and make ds.txt
# follows LCSC's JS redirect (window.top.location.href=...)
d=/tmp/ds/parts/$1; mkdir -p "$d"; UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/120 Safari/537.36"
curl -sSL -A "$UA" -o "$d/ds.pdf" "$2"
if ! head -c 5 "$d/ds.pdf" | grep -q '%PDF'; then
  u=$(grep -o "href='[^']*'" "$d/ds.pdf" | head -1 | sed "s/href='//;s/'$//")
  [ -n "$u" ] && curl -sSL -A "$UA" -o "$d/ds.pdf" "$u"
fi
head -c 5 "$d/ds.pdf" | grep -q '%PDF' || { echo "$1: NOT A PDF"; exit 1; }
pdftotext -layout "$d/ds.pdf" "$d/ds.txt" 2>/dev/null; echo "$1: $(pdfinfo "$d/ds.pdf" 2>/dev/null | grep Pages) $(wc -l < "$d/ds.txt") lines"
