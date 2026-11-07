#ifndef _LED_MATRIX_APP_H
#define _LED_MATRIX_APP_H

/* ==================================================================
 * led_matrix_app.h —— led_matrix 产品应用入口（最小可运行骨架）
 *
 * 职责：
 *   仅做「板级配置 + hal_ext 抽象」链路的落地验证：
 *     1) 打印板卡摘要（16x16 / 4 片 MAX7219 / DIN CLK LOAD）
 *     2) 经 hal_ext 抽象创建 GPIO 具象对象
 *     3) 闪烁 INIT 指示灯 3 次并最终熄灭
 *
 * 约束：
 *   本模块只依赖抽象头 gpio.h 与产品宏 board_config.h，
 *   不包含任何 user/services、motor、robot_* 头文件；
 *   所有引脚一律取自 board_config.h，禁止硬编码。
 * ================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 应用主入口：阻塞式执行一轮自检流程，返回即表示流程结束
 * （本骨架不留常驻循环，便于后续按模块逐步挂接）
 * ------------------------------------------------------------------ */
void LedMatrixAppRun(void);

#ifdef __cplusplus
}
#endif

#endif /* _LED_MATRIX_APP_H */
