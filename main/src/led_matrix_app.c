/* ==================================================================
 * led_matrix_app.c —— led_matrix 产品装配（组合根 / Composition Root）
 *
 * 本文件是全工程【唯一】把具象实现拼装成产品的场所，装配链严格按序：
 *
 *   ① NewGpioEsp32c3()                        -> Gpio*     抽象 GPIO
 *   ② GpioInit(INIT 灯输出) + 拉高            -> INIT 灯常亮（此后不再变）
 *   ③ Max7219BitBangConfig + NewMax7219BitBang() -> Max7219* 抽象驱动
 *   ④ Max7219Init(driver)                     -> 上电默认配置 + 清屏
 *   ⑤ Max7219SetIntensity / Max7219Clear      -> 亮度 + 清屏（失败只告警）
 *   ⑥ NewLedMatrix(driver) + LedMatrixClear   -> LedMatrix* 帧缓冲
 *   ⑦ 画边框 + 中心方块，LedMatrixFlush       -> 点亮图案
 *   ⑧ ESP_LOGI 就绪摘要
 *   ⑨ while (1) vTaskDelay(1000ms)            -> 常驻，永不返回
 *
 * 依赖方向（严格单向，本文件只做装配，不含业务算法）：
 *   led_matrix_app.h   应用入口声明
 *   board_config.h     板级唯一引脚 / 几何口径
 *   gpio.h             GPIO 抽象父类（hal_ext）
 *   gpio_esp32c3.h     GPIO 具象派生类（只在装配期取构造与虚表）
 *   max7219.h          MAX7219 抽象父类（hal_ext）
 *   max7219_bitbang.h  MAX7219 具象派生类：3 线位带（平台无关）
 *   led_matrix.h       产品层 16x16 帧缓冲（借用 Max7219，不拥有）
 *
 * 运行期一切调用都面向抽象对象（Gpio* / Max7219*），具体引脚时序
 * 全在具象 .c 内，因此本文件不出现任何 driver 层平台类型。
 *
 * 【所有权与寿命】
 *   gpio / driver / display 三者均为【嵌入式常驻对象】：设备上电后
 *   整个生命周期内常驻，故意不析构（不调用 DeleteGpio / DeleteMax7219 /
 *   DeleteLedMatrix），本文件因此没有任何释放与指针置空动作。
 *
 * 【INIT 指示灯策略】
 *   只允许点亮：本文件只有一次电平写入（②），且固定为高电平；
 *   任何失败分支都只记录日志，绝不下发低电平。矩阵初始化失败时
 *   该灯仍常亮，作为「MCU 已启动、GPIO 已就绪」的现场指示。
 * ================================================================== */

#include <stdint.h>

#include "board_config.h"
#include "led_matrix_app.h"

#include "gpio.h"
#include "gpio_esp32c3.h"

#include "max7219.h"
#include "max7219_bitbang.h"

#include "led_matrix.h"

#include "esp_log.h"
#include "esp_rom_sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------
 * 日志标签与装配参数（引脚一律取自 board_config.h，此处只放
 * 与硬件无关的亮度 / 时序 / 图案常量）
 * ------------------------------------------------------------------ */
static const char* LED_MATRIX_APP_TAG = "led_matrix_app";

/* MAX7219 亮度寄存器取值 0 到 15，取中间值，兼顾亮度与电源裕量
 * （网表约束：MAX7219 为 5V 逻辑，C3 输出 3.3V，裕量为负） */
#define LED_MATRIX_APP_INTENSITY          (8u)

/* 常驻循环周期：无业务动作，仅保持本任务存活 */
#define LED_MATRIX_APP_IDLE_PERIOD_MS     (1000u)

/* 中心实心方块边长：取单片 MAX7219 驱动板块的一半（8 / 2 = 4），
 * 由板级宏推导而非硬编码，板级几何变化时自动跟随 */
#define LED_MATRIX_APP_SQUARE_SIZE        (BOARD_MATRIX_BLOCK / 2u)

/* 方块左上角 = 全局居中：(边长 = 4 时即原定的 (6,6)) */
#define LED_MATRIX_APP_SQUARE_ORIGIN_X    ((BOARD_MATRIX_WIDTH  - LED_MATRIX_APP_SQUARE_SIZE) / 2u)
#define LED_MATRIX_APP_SQUARE_ORIGIN_Y    ((BOARD_MATRIX_HEIGHT - LED_MATRIX_APP_SQUARE_SIZE) / 2u)

/* 本文件内部返回码（与 hal_ext 的 0 表示成功、负值表示错误的口径一致） */
#define LED_MATRIX_APP_OK                 ( 0)
#define LED_MATRIX_APP_ERR_PARAM          (-1)
#define LED_MATRIX_APP_ERR_STEP           (-2)

/* ------------------------------------------------------------------
 * 编译期自检：图案几何必须落在矩阵范围内
 * ------------------------------------------------------------------ */
#if (BOARD_MATRIX_BLOCK < 2u)
#error "BOARD_MATRIX_BLOCK 过小，无法推导中心实心方块边长"
#endif

#if (LED_MATRIX_APP_SQUARE_SIZE == 0u) || \
    (LED_MATRIX_APP_SQUARE_SIZE >= BOARD_MATRIX_WIDTH) || \
    (LED_MATRIX_APP_SQUARE_SIZE >= BOARD_MATRIX_HEIGHT)
#error "中心实心方块边长必须大于 0 且小于矩阵尺寸，请核对 board_config.h"
#endif

/* ==================================================================
 * 延时钩子：注入给 MAX7219 位带驱动的边沿延时
 *
 * esp_rom_delay_us 为忙等实现，不依赖 FreeRTOS 节拍，可在任意上下文调用；
 * 位带移出时每个电平边沿后调用一次，保证 MAX7219 的建立 / 保持时间。
 * ================================================================== */
static void LedMatrixAppDelayUs(uint32_t microseconds)
{
    esp_rom_delay_us(microseconds);
}

/* ==================================================================
 * 图案绘制（只改帧缓冲，完全不碰硬件）
 * ================================================================== */

/* 画一个点并统一打日志：坐标由常量推导，一旦失败必为参数类错误 */
static int LedMatrixAppSetPixel(LedMatrix* self, uint32_t x, uint32_t y)
{
    int ret = LED_MATRIX_APP_OK;

    if (self == NULL) {
        return LED_MATRIX_APP_ERR_PARAM;
    }

    ret = LedMatrixSetPixel(self, x, y, 1);
    if (ret != LED_MATRIX_APP_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "set pixel (%u,%u) failed, ret=%d",
                 (unsigned)x, (unsigned)y, ret);
        return LED_MATRIX_APP_ERR_STEP;
    }

    return LED_MATRIX_APP_OK;
}

/* 1 像素边框：首末两行整行 + 首末两列整列 */
static int LedMatrixAppDrawBorder(LedMatrix* self)
{
    uint32_t x    = 0u;
    uint32_t y    = 0u;
    uint32_t maxX = (uint32_t)BOARD_MATRIX_WIDTH  - 1u;   /* 最右列 */
    uint32_t maxY = (uint32_t)BOARD_MATRIX_HEIGHT - 1u;   /* 最下行 */

    if (self == NULL) {
        return LED_MATRIX_APP_ERR_PARAM;
    }

    /* 上下两条横边 */
    for (x = 0u; x < (uint32_t)BOARD_MATRIX_WIDTH; ++x) {
        int ret = LedMatrixAppSetPixel(self, x, 0u);
        if (ret != LED_MATRIX_APP_OK) {
            return ret;
        }
        ret = LedMatrixAppSetPixel(self, x, maxY);
        if (ret != LED_MATRIX_APP_OK) {
            return ret;
        }
    }

    /* 左右两条竖边（角点重复写入同一值，帧缓冲幂等，无副作用） */
    for (y = 0u; y < (uint32_t)BOARD_MATRIX_HEIGHT; ++y) {
        int ret = LedMatrixAppSetPixel(self, 0u, y);
        if (ret != LED_MATRIX_APP_OK) {
            return ret;
        }
        ret = LedMatrixAppSetPixel(self, maxX, y);
        if (ret != LED_MATRIX_APP_OK) {
            return ret;
        }
    }

    return LED_MATRIX_APP_OK;
}

/* 中心实心方块：边长 LED_MATRIX_APP_SQUARE_SIZE，左上角居中 */
static int LedMatrixAppDrawSquare(LedMatrix* self)
{
    uint32_t row = 0u;
    uint32_t col = 0u;

    if (self == NULL) {
        return LED_MATRIX_APP_ERR_PARAM;
    }

    for (row = 0u; row < (uint32_t)LED_MATRIX_APP_SQUARE_SIZE; ++row) {
        for (col = 0u; col < (uint32_t)LED_MATRIX_APP_SQUARE_SIZE; ++col) {
            int ret = LedMatrixAppSetPixel(self,
                                          (uint32_t)LED_MATRIX_APP_SQUARE_ORIGIN_X + col,
                                          (uint32_t)LED_MATRIX_APP_SQUARE_ORIGIN_Y + row);
            if (ret != LED_MATRIX_APP_OK) {
                return ret;
            }
        }
    }

    return LED_MATRIX_APP_OK;
}

/* 完整图案 = 1 像素边框 + 中心实心方块 */
static int LedMatrixAppDrawPattern(LedMatrix* self)
{
    int ret = LED_MATRIX_APP_OK;

    ret = LedMatrixAppDrawBorder(self);
    if (ret != LED_MATRIX_APP_OK) {
        return ret;
    }

    return LedMatrixAppDrawSquare(self);
}

/* ==================================================================
 * 应用主流程（阻塞式常驻，正常路径永不返回）
 * ================================================================== */
void LedMatrixAppRun(void)
{
    Gpio*    gpio           = NULL;
    Max7219* driver         = NULL;
    LedMatrix* display      = NULL;
    int      ret            = LED_MATRIX_APP_OK;
    int      patternFlushed = 0;

    /* ---------------- ⓪ 板卡摘要（纯日志，便于现场核对接线） ---------------- */
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "board: matrix %ux%u, %u x MAX7219, DIN=GPIO%u CLK=GPIO%u LOAD=GPIO%u, init-led=GPIO%u",
             (unsigned)BOARD_MATRIX_WIDTH,
             (unsigned)BOARD_MATRIX_HEIGHT,
             (unsigned)BOARD_MATRIX_CHIP_COUNT,
             (unsigned)BOARD_MATRIX_PIN_DIN,
             (unsigned)BOARD_MATRIX_PIN_CLK,
             (unsigned)BOARD_MATRIX_PIN_LOAD,
             (unsigned)BOARD_LED_INIT_PIN);

    /* ---------------- ① 抽象 GPIO：具象对象内部已调 NewGpio 装配 ---------------- */
    gpio = NewGpioEsp32c3();
    if (gpio == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "NewGpioEsp32c3 failed (out of memory), abort");
        return;
    }

    /* ---------------- ② INIT 指示灯：推挽输出 + 常亮 ---------------- */
    ret = GpioInit(gpio, BOARD_LED_INIT_PIN, GpioDirOutput, GpioPullNone);
    if (ret != GPIO_OK) {
        /* 只记录不中断：矩阵三脚在 Max7219Init 内独立配置，仍可尝试点亮；
         * 此处【不得】写低电平去"修正"该灯 */
        ESP_LOGE(LED_MATRIX_APP_TAG, "GpioInit(init-led GPIO%u) failed, ret=%d (led stays as-is)",
                 (unsigned)BOARD_LED_INIT_PIN, ret);
    } else {
        ret = GpioWrite(gpio, BOARD_LED_INIT_PIN, GpioLevelHigh);
        if (ret != GPIO_OK) {
            ESP_LOGE(LED_MATRIX_APP_TAG, "init-led set high failed, ret=%d", ret);
        }
    }

    /* ---------------- ③ 组装位带配置 + 构造 MAX7219 抽象驱动 ----------------
     * gpio 由本文件持有（常驻），位带驱动只借用不销毁；
     * delayUs 注入上方 LedMatrixAppDelayUs，保证边沿时序。
     * ---------------------------------------------------------------------- */
    Max7219BitBangConfig cfg = {
        .gpio        = gpio,
        .dinPin      = BOARD_MATRIX_PIN_DIN,
        .clkPin      = BOARD_MATRIX_PIN_CLK,
        .loadPin     = BOARD_MATRIX_PIN_LOAD,
        .chainLength = BOARD_MATRIX_CHIP_COUNT,
        .delayUs     = LedMatrixAppDelayUs,
    };

    driver = NewMax7219BitBang(&cfg);
    if (driver == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "NewMax7219BitBang failed (bad config or out of memory)");
        goto led_matrix_app_hold;
    }

    /* ---------------- ④ 上电初始化：失败则跳过 ⑤ 到 ⑦，直接进入常驻 ---------------- */
    ret = Max7219Init(driver);
    if (ret != MAX7219_BITBANG_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "Max7219Init failed, ret=%d (init-led kept ON)", ret);
        goto led_matrix_app_hold;
    }

    /* ---------------- ⑤ 亮度 + 清屏：失败只告警，不中断装配 ---------------- */
    ret = Max7219SetIntensity(driver, (uint8_t)LED_MATRIX_APP_INTENSITY);
    if (ret != MAX7219_BITBANG_OK) {
        ESP_LOGW(LED_MATRIX_APP_TAG, "Max7219SetIntensity(%u) failed, ret=%d",
                 (unsigned)LED_MATRIX_APP_INTENSITY, ret);
    }

    ret = Max7219Clear(driver);
    if (ret != MAX7219_BITBANG_OK) {
        ESP_LOGW(LED_MATRIX_APP_TAG, "Max7219Clear failed, ret=%d", ret);
    }

    /* ---------------- ⑥ 帧缓冲对象：LedMatrix 借用 driver，不拥有 ---------------- */
    display = NewLedMatrix(driver);
    if (display == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "NewLedMatrix failed (driver invalid or out of memory)");
        goto led_matrix_app_hold;
    }

    ret = LedMatrixClear(display);
    if (ret != LED_MATRIX_APP_OK) {
        ESP_LOGW(LED_MATRIX_APP_TAG, "LedMatrixClear failed, ret=%d", ret);
    }

    /* ---------------- ⑦ 画图案并整帧下发 ---------------- */
    ret = LedMatrixAppDrawPattern(display);
    if (ret != LED_MATRIX_APP_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "draw pattern failed, ret=%d", ret);
        goto led_matrix_app_hold;
    }

    ret = LedMatrixFlush(display);
    if (ret != LED_MATRIX_APP_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "LedMatrixFlush failed, ret=%d", ret);
        goto led_matrix_app_hold;
    }
    patternFlushed = 1;

    /* ---------------- ⑧ 就绪摘要：以上所有失败分支在此汇合 ---------------- */
led_matrix_app_hold:
    if (0 != patternFlushed) {
        ESP_LOGI(LED_MATRIX_APP_TAG, "led_matrix ready: init-led=ON, pattern flushed");
    } else {
        ESP_LOGW(LED_MATRIX_APP_TAG, "led_matrix ready: init-led=ON, pattern NOT flushed (degraded)");
    }

    /* ---------------- ⑨ 常驻保持 ----------------
     * gpio / driver / display 均为嵌入式常驻对象，故意不析构：
     * 不调用 DeleteGpio / DeleteMax7219 / DeleteLedMatrix，
     * 让它们在设备整个上电周期内保持有效。本循环永不退出。
     * ------------------------------------------------------------------- */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_MATRIX_APP_IDLE_PERIOD_MS));
    }
}
