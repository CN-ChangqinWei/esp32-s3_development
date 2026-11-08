/* ==================================================================
 * led_matrix_app.c —— led_matrix 产品装配（组合根 / Composition Root）
 *
 * 本文件是全工程【唯一】把具象实现拼装成产品的场所，装配链严格按序：
 *
 *   0  ESP_LOGI 板卡摘要                         纯日志，不改动任何状态
 *   1  NewGpioEsp32c3()                         -> Gpio*     抽象 GPIO
 *   2  GpioInit(INIT 灯输出) + 拉高             -> INIT 灯常亮（此后不再变）
 *   3  Max7219BitBangConfig + NewMax7219BitBang -> Max7219*  抽象驱动
 *   4  Max7219Init(driver)                      驱动侧上电默认配置
 *   5  ScreenMax7219Config + NewScreenMax7219   -> Screen*   抽象屏幕
 *   6  ScreenInit(screen) + ScreenClear(screen) 屏幕侧上电 + 清帧缓冲
 *   7  ScreenSetPixel 画图案 + ScreenFlush      整帧下发到硬件
 *   8  ESP_LOGI 就绪摘要（任一步失败则降级为 ESP_LOGW）
 *   9  while (1) vTaskDelay 常驻                永不返回
 *
 * 依赖方向（严格单向，本文件只做装配，不含业务算法）：
 *   led_matrix_app.h     应用入口声明
 *   board_config.h       板级唯一引脚与几何口径
 *   gpio.h               GPIO 抽象父类（hal_ext）
 *   gpio_esp32c3.h       GPIO 具象派生类，只在装配期取构造
 *   max7219.h            MAX7219 抽象父类（hal_ext）
 *   max7219_bitbang.h    MAX7219 具象派生类：3 线位带（平台无关）
 *   screen.h             Screen 抽象父类：点阵显示统一口径
 *   screen_max7219.h     Screen 具象派生类：MAX7219 级联点阵
 *
 * 运行期一切调用都面向抽象对象（Gpio* / Max7219* / Screen*）：
 * 引脚时序、片选与帧格式全在具象 .c 内，本文件不出现任何 driver 层
 * 平台类型；像素几何一律由 board_config.h 宏或 ScreenWidth /
 * ScreenHeight 推导，不出现任何尺寸字面量。
 *
 * 【所有权与寿命】
 *   gpio / driver / screen 三者均为【嵌入式常驻对象】：设备上电后在
 *   整个产品生命周期内常驻，故意不析构（不调用 DeleteGpio /
 *   DeleteMax7219 / DeleteScreen），本文件因此没有任何释放动作。
 *
 * 【INIT 指示灯策略】
 *   只允许点亮：全文件只有一次电平写入（步骤 2），且固定为高电平；
 *   任何失败分支都只记录日志，绝不下发低电平。矩阵或屏幕初始化失败
 *   时该灯仍常亮，作为「MCU 已启动、GPIO 已就绪」的现场指示。
 * ================================================================== */

#include <stdint.h>

#include "board_config.h"
#include "led_matrix_app.h"

#include "gpio.h"
#include "gpio_esp32c3.h"

#include "max7219.h"
#include "max7219_bitbang.h"

#include "screen.h"
#include "screen_max7219.h"

#include "esp_log.h"
#include "esp_rom_sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------
 * 日志标签与装配参数
 *
 * 引脚与几何一律取自 board_config.h，此处只放与硬件无关的
 * 常驻周期与图案派生量。
 * ------------------------------------------------------------------ */
static const char* LED_MATRIX_APP_TAG = "led_matrix_app";

/* 常驻循环周期：无业务动作，仅保持本任务存活 */
#define LED_MATRIX_APP_IDLE_PERIOD_MS     (1000u)

/* 中心实心方块边长：取单片驱动板块的一半，由板级宏推导，
 * 板级几何变化时自动跟随，本文件不出现尺寸字面量 */
#define LED_MATRIX_APP_SQUARE_SIZE        (BOARD_MATRIX_BLOCK / 2u)

/* 边框线宽：1 像素（图案语义常量，非矩阵尺寸） */
#define LED_MATRIX_APP_BORDER_ON          (1)

/* 本文件内部步骤返回码（与 hal_ext 的 0 成功、负值失败口径一致） */
#define LED_MATRIX_APP_OK                 ( 0)
#define LED_MATRIX_APP_ERR_PARAM          (-1)
#define LED_MATRIX_APP_ERR_STEP           (-2)

/* ------------------------------------------------------------------
 * 编译期自检：图案几何必须能由板级宏推导且落在矩阵范围内
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
 * esp_rom_delay_us 为忙等实现，不依赖 FreeRTOS 节拍，可在任意上下文
 * 调用；位带移出时每个电平边沿后调用一次，保证 MAX7219 的建立与
 * 保持时间。
 * ================================================================== */
static void LedMatrixAppDelayUs(uint32_t microseconds)
{
    esp_rom_delay_us(microseconds);
}

/* ==================================================================
 * 图案绘制（只改屏幕抽象的内存帧，不直接碰硬件）
 * ================================================================== */

/* 画一个点并统一打日志：坐标由屏幕实际宽高推导，越界必为逻辑错误 */
static int LedMatrixAppSetPixel(Screen* screen, int32_t x, int32_t y)
{
    int ret = LED_MATRIX_APP_OK;

    if (screen == NULL) {
        return LED_MATRIX_APP_ERR_PARAM;
    }

    ret = ScreenSetPixel(screen, x, y, LED_MATRIX_APP_BORDER_ON);
    if (ret != SCREEN_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "set pixel (%d,%d) failed, ret=%d",
                 (int)x, (int)y, ret);
        return LED_MATRIX_APP_ERR_STEP;
    }

    return LED_MATRIX_APP_OK;
}

/* 1 像素边框：首末两行整行 + 首末两列整列 */
static int LedMatrixAppDrawBorder(Screen* screen, int32_t width, int32_t height)
{
    int32_t x   = 0;
    int32_t y   = 0;
    int     ret = LED_MATRIX_APP_OK;

    if ((screen == NULL) || (width <= 0) || (height <= 0)) {
        return LED_MATRIX_APP_ERR_PARAM;
    }

    /* 上下两条横边：首行与末行整行点亮 */
    for (x = 0; x < width; ++x) {
        ret = LedMatrixAppSetPixel(screen, x, 0);
        if (ret != LED_MATRIX_APP_OK) {
            return ret;
        }
        ret = LedMatrixAppSetPixel(screen, x, height - 1);
        if (ret != LED_MATRIX_APP_OK) {
            return ret;
        }
    }

    /* 左右两条竖边：角点重复写入同一值，内存帧幂等，无副作用 */
    for (y = 0; y < height; ++y) {
        ret = LedMatrixAppSetPixel(screen, 0, y);
        if (ret != LED_MATRIX_APP_OK) {
            return ret;
        }
        ret = LedMatrixAppSetPixel(screen, width - 1, y);
        if (ret != LED_MATRIX_APP_OK) {
            return ret;
        }
    }

    return LED_MATRIX_APP_OK;
}

/* 中心实心方块：边长取单片板块的一半，原点由屏幕宽高居中推导 */
static int LedMatrixAppDrawSquare(Screen* screen, int32_t width, int32_t height)
{
    const int32_t size    = (int32_t)LED_MATRIX_APP_SQUARE_SIZE;
    int32_t       originX = 0;
    int32_t       originY = 0;
    int32_t       row     = 0;
    int32_t       col     = 0;
    int           ret     = LED_MATRIX_APP_OK;

    if ((screen == NULL) || (width <= 0) || (height <= 0)) {
        return LED_MATRIX_APP_ERR_PARAM;
    }

    if ((size <= 0) || (size > width) || (size > height)) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "square size %d exceeds screen %dx%d",
                 (int)size, (int)width, (int)height);
        return LED_MATRIX_APP_ERR_PARAM;
    }

    originX = (width  - size) / 2;
    originY = (height - size) / 2;

    for (row = 0; row < size; ++row) {
        for (col = 0; col < size; ++col) {
            ret = LedMatrixAppSetPixel(screen, originX + col, originY + row);
            if (ret != LED_MATRIX_APP_OK) {
                return ret;
            }
        }
    }

    return LED_MATRIX_APP_OK;
}

/* 完整图案 = 1 像素边框 + 中心实心方块；边界来自 Screen 抽象 */
static int LedMatrixAppDrawPattern(Screen* screen)
{
    int width  = 0;
    int height = 0;
    int ret    = LED_MATRIX_APP_OK;

    if (screen == NULL) {
        return LED_MATRIX_APP_ERR_PARAM;
    }

    /* 宽高由抽象层提供，不使用任何板级尺寸字面量 */
    width  = ScreenWidth(screen);
    height = ScreenHeight(screen);
    if ((width <= 0) || (height <= 0)) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "screen geometry invalid: width=%d height=%d",
                 width, height);
        return LED_MATRIX_APP_ERR_STEP;
    }

    ret = LedMatrixAppDrawBorder(screen, (int32_t)width, (int32_t)height);
    if (ret != LED_MATRIX_APP_OK) {
        return ret;
    }

    return LedMatrixAppDrawSquare(screen, (int32_t)width, (int32_t)height);
}

/* ==================================================================
 * 应用主流程（阻塞式常驻，正常路径永不返回）
 * ================================================================== */
void LedMatrixAppRun(void)
{
    Gpio*    gpio          = NULL;
    Max7219* driver        = NULL;
    Screen*  screen        = NULL;
    int      ret           = LED_MATRIX_APP_OK;
    int      screenFlushed = 0;

    /* ---------------- 步骤 0 板卡摘要（纯日志，便于现场核对接线） ---------------- */
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "board: matrix %ux%u, %u x MAX7219, chips %ux%u, block=%u, "
             "DIN=GPIO%u CLK=GPIO%u LOAD=GPIO%u, init-led=GPIO%u",
             (unsigned)BOARD_MATRIX_WIDTH,
             (unsigned)BOARD_MATRIX_HEIGHT,
             (unsigned)BOARD_MATRIX_CHIP_COUNT,
             (unsigned)BOARD_MATRIX_CHIPS_X,
             (unsigned)BOARD_MATRIX_CHIPS_Y,
             (unsigned)BOARD_MATRIX_BLOCK,
             (unsigned)BOARD_MATRIX_PIN_DIN,
             (unsigned)BOARD_MATRIX_PIN_CLK,
             (unsigned)BOARD_MATRIX_PIN_LOAD,
             (unsigned)BOARD_LED_INIT_PIN);

    /* ---------------- 步骤 1 抽象 GPIO：具象对象内部已调 NewGpio 装配 ---------------- */
    gpio = NewGpioEsp32c3();
    if (gpio == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "NewGpioEsp32c3 failed (out of memory), abort");
        return;
    }

    /* ---------------- 步骤 2 INIT 指示灯：推挽输出 + 常亮 ----------------
     * 全文件唯一一次电平写入，固定高电平；失败只记录日志，
     * 【不得】写低电平去「修正」该灯，矩阵三脚在 Max7219Init 内独立配置。
     * -------------------------------------------------------------------- */
    ret = GpioInit(gpio, BOARD_LED_INIT_PIN, GpioDirOutput, GpioPullNone);
    if (ret != GPIO_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "GpioInit(init-led GPIO%u) failed, ret=%d (led stays as-is)",
                 (unsigned)BOARD_LED_INIT_PIN, ret);
    } else {
        ret = GpioWrite(gpio, BOARD_LED_INIT_PIN, GpioLevelHigh);
        if (ret != GPIO_OK) {
            ESP_LOGE(LED_MATRIX_APP_TAG, "init-led set high failed, ret=%d", ret);
        }
    }

    /* ---------------- 步骤 3 组装位带配置 + 构造 MAX7219 抽象驱动 ----------------
     * gpio 由本文件持有并常驻，位带驱动只借用不销毁；
     * delayUs 注入 LedMatrixAppDelayUs，保证边沿时序。
     * -------------------------------------------------------------------------- */
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

    /* ---------------- 步骤 4 驱动侧上电配置：失败跳过 5 到 7，直接进入常驻 ---------------- */
    ret = Max7219Init(driver);
    if (ret != MAX7219_BITBANG_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "Max7219Init failed, ret=%d (init-led kept ON)", ret);
        goto led_matrix_app_hold;
    }

    /* ---------------- 步骤 5 改用 Screen 抽象：注入驱动 + 片阵列几何 ----------------
     * Screen 具象类只借用 driver，不销毁；驱动与屏幕共用同一条级联链，
     * 后续所有显示操作都经 Screen* 抽象，不再直接触达 Max7219*。
     * ---------------------------------------------------------------------------- */
    ScreenMax7219Config screenCfg = {
        .driver    = driver,
        .blockSize = BOARD_MATRIX_BLOCK,
        .chipsX    = BOARD_MATRIX_CHIPS_X,
        .chipsY    = BOARD_MATRIX_CHIPS_Y,
    };

    screen = NewScreenMax7219(&screenCfg);
    if (screen == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "NewScreenMax7219 failed (bad config or out of memory)");
        goto led_matrix_app_hold;
    }

    /* ---------------- 步骤 6 屏幕侧上电 + 清帧：清屏失败仅告警，不中断装配 ---------------- */
    ret = ScreenInit(screen);
    if (ret != SCREEN_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "ScreenInit failed, ret=%d (init-led kept ON)", ret);
        goto led_matrix_app_hold;
    }

    ret = ScreenClear(screen);
    if (ret != SCREEN_OK) {
        ESP_LOGW(LED_MATRIX_APP_TAG, "ScreenClear failed, ret=%d", ret);
    }

    /* ---------------- 步骤 7 画图案并整帧刷新 ----------------
     * 图案只写内存帧；ScreenFlush 才是唯一的下发动作。
     * 绘制失败视为装配降级（跳 8），刷新失败只记录错误，仍进入 8 打降级摘要。
     * ---------------------------------------------------------- */
    ret = LedMatrixAppDrawPattern(screen);
    if (ret != LED_MATRIX_APP_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "draw pattern failed, ret=%d", ret);
        goto led_matrix_app_hold;
    }

    ret = ScreenFlush(screen);
    if (ret != SCREEN_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "ScreenFlush failed, ret=%d (screen degraded)", ret);
    } else {
        screenFlushed = 1;
    }

    /* ---------------- 步骤 8 就绪摘要：以上所有失败分支在此汇合 ----------------
     * 两条路径都只打日志，绝不改动 INIT 灯电平（保持常亮）。
     * -------------------------------------------------------------------------- */
led_matrix_app_hold:
    if (0 != screenFlushed) {
        ESP_LOGI(LED_MATRIX_APP_TAG, "led_matrix ready: init-led=ON, screen flushed");
    } else {
        ESP_LOGW(LED_MATRIX_APP_TAG, "led_matrix ready: init-led=ON, screen flushed=NO (degraded)");
    }

    /* ---------------- 步骤 9 常驻保持 ----------------
     * gpio / driver / screen 为常驻对象，故意不析构：
     * 不调用 DeleteGpio / DeleteMax7219 / DeleteScreen，
     * 让它们在设备整个上电周期内保持有效。本循环永不退出。
     * ------------------------------------------------- */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_MATRIX_APP_IDLE_PERIOD_MS));
    }
}
