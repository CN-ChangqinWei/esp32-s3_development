/* ==================================================================
 * led_matrix_app.c —— led_matrix 产品应用入口实现（最小可运行骨架）
 *
 * 依赖方向（严格单向）：
 *   led_matrix_app -> board_config.h（产品宏）
 *   led_matrix_app -> gpio.h（抽象父类，不含平台头）
 *   led_matrix_app -> gpio_esp32c3.h（具象派生类，仅取构造/虚表）
 *
 * 说明：抽象层只做装配与透传，具体 GPIO 行为全在 gpio_esp32c3.c 内，
 *       因此本文件不出现任何 driver/esp_err_t 平台类型。
 * ================================================================== */

#include <stdint.h>

#include "board_config.h"
#include "led_matrix_app.h"

#include "gpio.h"
#include "gpio_esp32c3.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------
 * 日志标签与流程参数（引脚号一律来自 board_config.h，此处只放
 * 与硬件无关的时序/次数常量）
 * ------------------------------------------------------------------ */
static const char *LED_MATRIX_APP_TAG = "led_matrix_app";

#define LED_MATRIX_APP_BLINK_TIMES     3u    /* 闪烁次数 */
#define LED_MATRIX_APP_BLINK_ON_MS     100u  /* 点亮保持 */
#define LED_MATRIX_APP_BLINK_OFF_MS    100u  /* 熄灭保持 */

/* 本模块内部返回码（与 hal_ext 的 GPIO_OK/GPIO_ERR_* 区分开） */
#define LED_MATRIX_APP_OK              ( 0)
#define LED_MATRIX_APP_ERR_PARAM       (-1)
#define LED_MATRIX_APP_ERR_STEP        (-2)

/* ------------------------------------------------------------------
 * 打印板卡摘要：矩阵尺寸 / 驱动片数 / 三根控制脚
 * ------------------------------------------------------------------ */
static void LedMatrixAppPrintBoardSummary(void)
{
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "board summary: matrix %ux%u, %u x MAX7219, DIN=GPIO%u CLK=GPIO%u LOAD=GPIO%u",
             (unsigned)BOARD_MATRIX_WIDTH,
             (unsigned)BOARD_MATRIX_HEIGHT,
             (unsigned)BOARD_MATRIX_CHIP_COUNT,
             (unsigned)BOARD_MATRIX_PIN_DIN,
             (unsigned)BOARD_MATRIX_PIN_CLK,
             (unsigned)BOARD_MATRIX_PIN_LOAD);
}

/* ------------------------------------------------------------------
 * INIT 指示灯闪烁：高 100ms / 低 100ms，共 LED_MATRIX_APP_BLINK_TIMES 次
 * 调用前置条件：self 非空且 BOARD_LED_INIT_PIN 已配置为输出
 * ------------------------------------------------------------------ */
static int LedMatrixAppBlinkInitLed(Gpio* self)
{
    uint32_t idx = 0;

    if (self == NULL) {
        return LED_MATRIX_APP_ERR_PARAM;
    }

    for (idx = 0; idx < LED_MATRIX_APP_BLINK_TIMES; ++idx) {
        int ret = GpioWrite(self, BOARD_LED_INIT_PIN, GpioLevelHigh);
        if (ret != GPIO_OK) {
            ESP_LOGE(LED_MATRIX_APP_TAG, "blink[%u] set high failed, ret=%d",
                     (unsigned)idx, ret);
            return LED_MATRIX_APP_ERR_STEP;
        }
        vTaskDelay(pdMS_TO_TICKS(LED_MATRIX_APP_BLINK_ON_MS));

        ret = GpioWrite(self, BOARD_LED_INIT_PIN, GpioLevelLow);
        if (ret != GPIO_OK) {
            ESP_LOGE(LED_MATRIX_APP_TAG, "blink[%u] set low failed, ret=%d",
                     (unsigned)idx, ret);
            return LED_MATRIX_APP_ERR_STEP;
        }
        vTaskDelay(pdMS_TO_TICKS(LED_MATRIX_APP_BLINK_OFF_MS));
    }

    return LED_MATRIX_APP_OK;
}

/* ------------------------------------------------------------------
 * 应用主流程
 *
 * 关键调用序列：
 *   NewGpioEsp32c3 -> GpioInit(输出/无上下拉)
 *   -> GpioWrite(Low) 预复位
 *   -> [高/低 各 100ms] x 3
 *   -> GpioWrite(Low) 保持熄灭
 *   -> DeleteGpio(&self)（析构后指针置空）
 * ------------------------------------------------------------------ */
void LedMatrixAppRun(void)
{
    Gpio* self = NULL;
    int   ret  = LED_MATRIX_APP_OK;

    LedMatrixAppPrintBoardSummary();

    /* 1) 创建具象 GPIO 对象（内部已调用抽象构造 NewGpio 完成装配） */
    self = NewGpioEsp32c3();
    if (self == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "NewGpioEsp32c3 failed (out of memory)");
        return;
    }

    /* 2) 配置 INIT 指示灯为推挽输出、无上下拉 */
    ret = GpioInit(self, BOARD_LED_INIT_PIN, GpioDirOutput, GpioPullNone);
    if (ret != GPIO_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "GpioInit(pin=GPIO%u) failed, ret=%d",
                 (unsigned)BOARD_LED_INIT_PIN, ret);
        DeleteGpio(&self);
        return;
    }

    /* 3) 先拉低，避免复位后残留点亮 */
    ret = GpioWrite(self, BOARD_LED_INIT_PIN, GpioLevelLow);
    if (ret != GPIO_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "initial set low failed, ret=%d", ret);
        DeleteGpio(&self);
        return;
    }

    /* 4) 闪烁 3 次（高 100ms / 低 100ms） */
    ret = LedMatrixAppBlinkInitLed(self);
    if (ret != LED_MATRIX_APP_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "blink INIT led failed, ret=%d", ret);
        /* 尽力恢复熄灭状态，失败也已记录，随后统一释放 */
        (void)GpioWrite(self, BOARD_LED_INIT_PIN, GpioLevelLow);
        DeleteGpio(&self);
        return;
    }

    /* 5) 最终保持熄灭 */
    ret = GpioWrite(self, BOARD_LED_INIT_PIN, GpioLevelLow);
    if (ret != GPIO_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "final set low failed, ret=%d", ret);
        DeleteGpio(&self);
        return;
    }

    /* 6) 释放具象实例与抽象对象，指针由 DeleteGpio 置空 */
    DeleteGpio(&self);

    ESP_LOGI(LED_MATRIX_APP_TAG, "led_matrix init done");
}
