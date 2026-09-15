/* 收藏独立于 ROM 索引保存，重扫、临时移走游戏都不会抹掉已有选择。 */
#pragma once

#include "rom_store.h"

/* 在 TF 卡挂载后调用；成功后幂等。损坏且无有效备份时返回错误，不能把它当成
 * 空收藏覆盖。只由菜单任务初始化、修改和释放。 */
esp_err_t rom_favorites_init(void);

/* 只比较完整外层路径；ZIP 解压后的内部名字不参与收藏身份。查询不读卡。 */
bool rom_favorites_contains(const rom_store_entry_t *entry);

/* 返回 ESP_OK 才表示已落盘并改变查询结果。失败保留修改前的内存与有效旧文件。 */
esp_err_t rom_favorites_toggle(const rom_store_entry_t *entry);

/* 启动模拟器前归还 PSRAM，不删除卡上的收藏；之后允许重新 init。 */
void rom_favorites_deinit(void);
