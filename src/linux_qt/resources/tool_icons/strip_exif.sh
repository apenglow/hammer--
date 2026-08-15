#!/usr/bin/env bash
# Strip all metadata (EXIF, XMP, tEXt, etc.) from every PNG in a folder.
# Usage: ./strip_exif.sh [folder]   (defaults to current directory)
set -euo pipefail

dir="${1:-.}"
[[ -d "$dir" ]] || { echo "Not a directory: $dir" >&2; exit 1; }

shopt -s nullglob nocaseglob
files=("$dir"/*.png)
(( ${#files[@]} )) || { echo "No PNG files found in $dir"; exit 0; }

if command -v exiftool >/dev/null 2>&1; then
    # -all= removes all metadata; -overwrite_original avoids _original backups
    exiftool -all= -overwrite_original "${files[@]}"
elif command -v mogrify >/dev/null 2>&1; then
    # ImageMagick fallback: -strip removes profiles/comments/metadata
    mogrify -strip "${files[@]}"
else
    echo "Need exiftool or ImageMagick (mogrify) installed." >&2
    exit 1
fi

echo "Stripped metadata from ${#files[@]} PNG file(s) in $dir"
