#ifndef _FREERTOS_EXTRA_H
#define _FREERTOS_EXTRA_H
#include "freertos/FreeRTOS.h"

/* C++ 兼容：本模块的 C 声明统一使用 C 链接规范，避免被 C++ 编译单元包含时
 * 名字修饰与 C 实现不匹配而链接失败。第三方平台头 "freertos/FreeRTOS.h"
 * 自带 C++ 链接处理，保留在包裹之外以维持其包含顺序与语义不变。 */
#ifdef __cplusplus
extern "C" {
#endif

void* pvPortRealloc(void* pv,size_t xWantedSize);

#ifdef __cplusplus
}
#endif

#endif