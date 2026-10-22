#!/usr/bin/env bash
# Compile report sources only; never run experiments or regenerate plots.
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
command -v tectonic >/dev/null 2>&1 || { echo 'tectonic is required.' >&2; exit 1; }
# Use already installed TeX fonts when the Tectonic cache lacks font files.
font_args=()
for font_dir in /usr/share/fonts/texlive-*; do
  [[ -d "$font_dir" ]] && font_args+=(-Z "search-path=$font_dir")
done
shopt -s nullglob
sources=("$repo_dir"/lab[0-9]*/lab[0-9]*.tex)
((${#sources[@]})) || { echo 'No lab report sources found.' >&2; exit 1; }
mkdir -p "$repo_dir/build"
for source in "${sources[@]}"; do
  # Paths inside reports are relative to their own laboratory directory.
  (cd -- "$(dirname -- "$source")" && tectonic "${font_args[@]}" --outdir "$repo_dir/build" "$(basename -- "$source")")
done
