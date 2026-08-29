#!/bin/sh
set -eu

cd "$(dirname "$0")"
mkdir -p tiktok

# TikTok 仍用 1080×1920 画布。主体缩到 80%，放在 x=30、y=100：
# 右侧预留约 186 px 给头像/点赞/评论按钮，底部预留约 284 px 给文案和导航，
# 标题基线则落在 y≈224 以下，避开顶部导航。背景沿用 gruvbox dark0。
for name in \
  01-materials \
  02-wiring \
  03-flash-tool \
  04-assembly-acceptance
do
  magick -size 1080x1920 xc:'#282828' \
    \( "${name}.png" -resize 80% \) \
    -geometry +30+100 -compose Over -composite \
    -depth 8 \
    "tiktok/${name}-tiktok.png"
done
