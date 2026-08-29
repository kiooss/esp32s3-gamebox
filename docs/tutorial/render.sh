#!/bin/sh
set -eu

cd "$(dirname "$0")"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

rsvg-convert -w 1080 -h 1920 -o "$tmp_dir/materials-base.png" 01-materials.svg
rsvg-convert -w 1080 -h 1920 -o "$tmp_dir/hero-overlay.png" assets/01-hero-overlay.svg

magick assets/parts-flatlay.png \
  -resize '1000x690^' -gravity center -extent 1000x690 \
  \( -size 1000x690 xc:none -fill white -draw 'roundrectangle 0,0 999,689 34,34' \) \
  -alpha off -compose CopyOpacity -composite "$tmp_dir/parts-hero.png"

magick "$tmp_dir/materials-base.png" \
  "$tmp_dir/parts-hero.png" -geometry +40+210 -compose Over -composite \
  "$tmp_dir/hero-overlay.png" -compose Over -composite \
  01-materials.png

rsvg-convert -w 1080 -h 1920 -o 02-wiring.png 02-wiring.svg
rsvg-convert -w 1080 -h 1920 -o 03-flash-tool.png 03-flash-tool.svg
rsvg-convert -w 1080 -h 1920 -o 04-assembly-acceptance.png 04-assembly-acceptance.svg
