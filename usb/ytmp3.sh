#!/bin/sh

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <output.mp3> <YouTube URL or ID>"
    exit 1
fi

OUT="$1"
INPUT="$2"

# ถ้าเป็น YouTube ID ให้เติม URL
case "$INPUT" in
    http://*|https://*)
        URL="$INPUT"
        ;;
    *)
        URL="https://www.youtube.com/watch?v=$INPUT"
        ;;
esac

yt-dlp \
    --extract-audio \
    --audio-format mp3 \
    --audio-quality 0 \
     --no-playlist \
    --output "$OUT" \
    "$URL"
