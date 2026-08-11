# gnuboy 来源

本目录从 Retro-Go 的 `retro-core/components/gnuboy` 原样引入，基线提交：

`4ced120669750ca7228fd0414211430c1d923166`

上游地址：https://github.com/ducalex/retro-go

只对 `gnuboy.h` 做了一处 ESP-IDF 适配：在 ESP 平台包含 `esp_attr.h`，让 CPU
热点函数继续使用真正的 `IRAM_ATTR`。宿主显示、输入和音频适配全部放在
`main/gbc_emu.c`，以后更新上游时不需要反复修改模拟器核心。
