#include "position_insert.h"
#include "freertos/FreeRTOS.h"

// ===== 模块级内存管理默认实现（可被外部覆盖）=====
void* (*positionInsertMalloc)(size_t size) = pvPortMalloc;
void  (*positionInsertFree)(void* p) = vPortFree;
