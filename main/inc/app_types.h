#ifndef _APP_TYPES_H
#define _APP_TYPES_H

/* ============================================================================
 * 产品级共享数据类型（唯一数据契约）
 *
 * 设计约束（按需求）：加速度计的数据**只使用这一个结构体**承载，
 * 采样线程是唯一写入方，其它模块只能拿到拷贝（见 AccelServiceGet）。
 * ========================================================================== */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 加速度计快照：单位 mg（毫重力），来源 SC7A20 */
typedef struct AccelSample {
    int32_t  xMg;          /* X 轴，mg */
    int32_t  yMg;          /* Y 轴，mg */
    int32_t  zMg;          /* Z 轴，mg */
    uint32_t timestampMs;  /* 采样时刻（平台毫秒计时） */
    uint8_t  valid;        /* 1 = 本次采样有效；0 = 尚未采到或读取失败 */
} AccelSample;

/* 采样周期：SC7A20 ODR = 100 Hz，按 2:1 抽样取 50 Hz，
 * 既不会漏掉明显变化，也不会把 I2C 总线占满。 */
#define APP_ACCEL_POLL_PERIOD_MS   20u

/* debug 模式下信息服务的回传周期（需求：1 s） */
#define APP_INFO_PUSH_PERIOD_MS    1000u

#ifdef __cplusplus
}
#endif

#endif /* _APP_TYPES_H */
