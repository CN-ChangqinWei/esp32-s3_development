/* ==================================================================
 * led_matrix_app.c —— led_matrix 产品装配（组合根 / Composition Root）
 *
 * 本文件是全工程【唯一】把具象实现拼装成产品的场所，装配链严格按序：
 *
 *   0  ESP_LOGI 板卡摘要                           纯日志，不改动任何状态
 *   1  NewGpioEsp32c3()                           -> Gpio*     抽象 GPIO
 *   2  GpioInit(INIT 灯输出) + 拉高                INIT 灯常亮（此后不再变）
 *   3  SpiDefaultConfig + NewSpiEsp32c3           -> Spi*      抽象 SPI 总线
 *   4  Max7219SpiConfig + NewMax7219Spi + Init    -> Max7219*  抽象驱动（经 SPI 具象）
 *   5  ScreenMax7219Config + NewScreenMax7219     -> Screen*   抽象屏幕
 *      + ScreenInit + ScreenClear                 屏幕侧上电 + 清帧缓冲
 *   6  边框 + 8x8 bitmap 图标 + ScreenFlush       唯一一次整帧下发
 *   7  I2c 注入 + NewSc7a20 + Init + 两次读数      加速度计自检（失败不影响点阵）
 *   8  ESP_LOGI 就绪摘要（任一环节失败则 ESP_LOGW 降级）
 *   9  while (1) vTaskDelay 常驻                  永不返回
 *
 * 依赖方向（严格单向，本文件只做装配，不含业务算法）：
 *   led_matrix_app.h     应用入口声明
 *   board_config.h       板级唯一引脚 / 时钟 / 地址口径
 *   gpio.h               GPIO 抽象父类          + gpio_esp32c3.h  具象构造
 *   spi.h                SPI 抽象父类           + spi_esp32c3.h   具象构造
 *   max7219.h            MAX7219 抽象父类        + max7219_spi.h   经 SPI 的具象
 *   screen.h             Screen 抽象父类         + screen_max7219.h 点阵具象
 *   bitmap.h             1 bit 位图纯数据模块（图标载体）
 *   i2c.h                I2C 抽象父类           + i2c_esp32c3.h   具象构造
 *   sc7a20.h             三轴加速度计驱动（依赖注入抽象 I2c）
 *
 * 运行期一切调用都面向抽象对象（Gpio* / Spi* / Max7219* / Screen* / I2c* /
 * Sc7a20*）：引脚时序、片选、帧格式、寄存器编解码全在具象 .c 内，本文件
 * 不出现任何平台类型；像素几何一律由 board_config.h 宏或 ScreenWidth /
 * ScreenHeight 推导，不出现任何尺寸字面量。
 *
 * 【所有权与寿命】
 *   gpio / spi / driver / screen / bus / accel 六者均为【嵌入式常驻对象】：
 *   设备上电后在整个产品生命周期内常驻，故意不析构（不调用 DeleteGpio /
 *   DeleteSpi / DeleteMax7219 / DeleteScreen / DeleteI2c / DeleteSc7a20），
 *   本文件因此没有任何对象释放动作；唯一的 DeleteBitmap 针对装配期临时
 *   创建的图标载体，画完立即释放。
 *
 * 【INIT 指示灯策略】
 *   只允许点亮：全文件只有一次电平写入（步骤 2），且固定为高电平；
 *   任何失败分支都只记录日志，绝不下发低电平，也不再次改写该脚。
 * ================================================================== */

#include <stdint.h>

#include "board_config.h"
#include "led_matrix_app.h"

#include "gpio.h"
#include "gpio_esp32c3.h"

#include "spi.h"
#include "spi_esp32c3.h"

#include "max7219.h"
#include "max7219_spi.h"

#include "screen.h"
#include "screen_max7219.h"

#include "bitmap.h"

#include "i2c.h"
#include "i2c_esp32c3.h"
#include "sc7a20.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------
 * 日志标签与装配参数
 *
 * 引脚 / 时钟 / 器件地址一律取自 board_config.h；此处只放与硬件接线
 * 无关的语义常量（图标尺寸、超时、器件量程与速率、常驻周期）。
 * ------------------------------------------------------------------ */
static const char* LED_MATRIX_APP_TAG = "led_matrix_app";

/* 本文件内部步骤返回码（与 hal_ext 的 0 成功、负值失败口径一致） */
#define LED_MATRIX_APP_OK                  ( 0)
#define LED_MATRIX_APP_ERR_PARAM           (-1)
#define LED_MATRIX_APP_ERR_STEP            (-2)

/* 图标边长（像素）：8x8 位图，与单片 MAX7219 驱动板块同尺寸 */
#define LED_MATRIX_APP_ICON_SIZE           (8u)

/* 边框线宽：1 像素（图案语义常量，非矩阵尺寸） */
#define LED_MATRIX_APP_BORDER_ON           (1)

/* MAX7219 单帧传输超时（毫秒） */
#define LED_MATRIX_APP_MAX7219_TIMEOUT_MS  (100u)

/* 加速度计装配参数：首版不校验身份（0 表示不校验），量程与速率按器件手册取值 */
#define LED_MATRIX_APP_ACCEL_WHOAMI_EXPECT (0u)
#define LED_MATRIX_APP_ACCEL_RANGE_G       (2u)
#define LED_MATRIX_APP_ACCEL_ODR_HZ        (100u)

/* 常驻循环周期：无业务动作，仅保持本任务存活 */
#define LED_MATRIX_APP_IDLE_PERIOD_MS      (1000u)

/* MISO 接线状态文本：引脚宏为 0xFFFFFFFF 即「未接」，日志里直接说明，
 * 避免把 0xFFFFFFFF 当成真实 GPIO 号打印出去 */
#if (BOARD_MATRIX_PIN_MISO == 0xFFFFFFFFu)
#define LED_MATRIX_APP_MISO_TEXT           "none"
#else
#define LED_MATRIX_APP_MISO_TEXT           "wired"
#endif

/* ------------------------------------------------------------------
 * 编译期自检：图标必须能落在点阵范围内
 * ------------------------------------------------------------------ */
#if (LED_MATRIX_APP_ICON_SIZE == 0u) || \
    (LED_MATRIX_APP_ICON_SIZE > BOARD_MATRIX_WIDTH) || \
    (LED_MATRIX_APP_ICON_SIZE > BOARD_MATRIX_HEIGHT)
#error "图标尺寸必须大于 0 且不超过点阵宽高，请核对 board_config.h"
#endif

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

/* 8x8 菱形图标：经 bitmap 载体组装，再交给 ScreenDrawBitmap 一次落帧
 *
 * 位置推导（不出现尺寸字面量）：
 *   marginX = (width  - iconSize) / 2   居中时左右两侧的留白
 *   marginY = (height - iconSize) / 2   居中时上下两侧的留白
 *   originX = marginX + marginX / 2     居中基础上再右移半个留白 = 居中偏右
 *   originY = marginY                   垂直方向保持居中
 * 整数除法保证了 marginX + marginX / 2 不超过 width - iconSize，
 * 因此无论宽高如何变化，图标右 / 下边界都不会越出屏幕，无需额外夹取。
 */
static int LedMatrixAppDrawIcon(Screen* screen, int32_t width, int32_t height)
{
    /* 菱形图案：逐行 8 bit，最高位对应最左列 */
    static const uint8_t kLedMatrixAppIcon[LED_MATRIX_APP_ICON_SIZE] = {
        0x18u, 0x24u, 0x42u, 0x81u, 0x81u, 0x42u, 0x24u, 0x18u
    };

    const int32_t iconSize = (int32_t)LED_MATRIX_APP_ICON_SIZE;
    Bitmap*       icon     = NULL;
    int32_t       marginX  = 0;
    int32_t       marginY  = 0;
    int32_t       originX  = 0;
    int32_t       originY  = 0;
    int32_t       row      = 0;
    int32_t       col      = 0;
    int           ret      = LED_MATRIX_APP_OK;

    if ((screen == NULL) || (width <= 0) || (height <= 0)) {
        return LED_MATRIX_APP_ERR_PARAM;
    }

    /* 屏幕装不下图标：不是装配失败，跳过图标即可（边框照旧绘制） */
    if ((width < iconSize) || (height < iconSize)) {
        ESP_LOGW(LED_MATRIX_APP_TAG,
                 "skip icon: screen %dx%d smaller than icon %d",
                 (int)width, (int)height, (int)iconSize);
        return LED_MATRIX_APP_OK;
    }

    marginX = (width  - iconSize) / 2;
    marginY = (height - iconSize) / 2;
    originX = marginX + (marginX / 2);   /* 居中偏右 */
    originY = marginY;                   /* 垂直居中 */

    /* 临时位图载体：只活在本函数内，画完立即释放 */
    icon = NewBitmap((int)LED_MATRIX_APP_ICON_SIZE, (int)LED_MATRIX_APP_ICON_SIZE);
    if ((icon == NULL) || (icon->set == NULL)) {
        ESP_LOGW(LED_MATRIX_APP_TAG,
                 "NewBitmap %ux%u failed, skip icon (border kept)",
                 (unsigned)LED_MATRIX_APP_ICON_SIZE,
                 (unsigned)LED_MATRIX_APP_ICON_SIZE);
        return LED_MATRIX_APP_OK;
    }

    for (row = 0; row < iconSize; ++row) {
        for (col = 0; col < iconSize; ++col) {
            const uint32_t mask = (uint32_t)1u << (uint32_t)(iconSize - 1 - col);

            if (0u != ((uint32_t)kLedMatrixAppIcon[row] & mask)) {
                (void)icon->set(icon, (int)row, (int)col, (Bit)1);
            }
        }
    }

    ret = ScreenDrawBitmap(screen, originX, originY, icon);
    DeleteBitmap(icon);
    icon = NULL;

    if (ret != SCREEN_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "draw icon at (%d,%d) failed, ret=%d",
                 (int)originX, (int)originY, ret);
        return LED_MATRIX_APP_ERR_STEP;
    }

    ESP_LOGI(LED_MATRIX_APP_TAG, "icon %dx%d drawn at (%d,%d) on %dx%d screen",
             (int)iconSize, (int)iconSize, (int)originX, (int)originY,
             (int)width, (int)height);

    return LED_MATRIX_APP_OK;
}

/* 完整图案 = 1 像素边框 + 居中偏右的 bitmap 图标；边界来自 Screen 抽象 */
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

    /* 图标失败（载体分配不到 / 绘制失败）不阻止后续刷新：边框仍需上屏 */
    ret = LedMatrixAppDrawIcon(screen, (int32_t)width, (int32_t)height);
    if (ret != LED_MATRIX_APP_OK) {
        ESP_LOGW(LED_MATRIX_APP_TAG, "icon skipped, border still flushed, ret=%d", ret);
    }

    return LED_MATRIX_APP_OK;
}

/* ==================================================================
 * 加速度计自检：抽象 I2C 注入 + 器件装配 + 两条实测日志
 *
 * 全部失败只降级为 ESP_LOGW，绝不影响点阵链路（点阵已在步骤 6 上屏）。
 * WHO_AM_I 打印实测值，供后续把 LED_MATRIX_APP_ACCEL_WHOAMI_EXPECT
 * 固化为期望值使用。
 * ================================================================== */
static void LedMatrixAppProbeAccel(I2c* bus, int* pAccelReady,
                                   int32_t* pAccelX, int32_t* pAccelY, int32_t* pAccelZ)
{
    Sc7a20Config accCfg;
    Sc7a20*       accel  = NULL;
    uint8_t       whoAmI = 0u;
    int           ret    = 0;

    if ((bus == NULL) || (pAccelReady == NULL) ||
        (pAccelX == NULL) || (pAccelY == NULL) || (pAccelZ == NULL)) {
        return;
    }

    /* 依赖注入：总线与 7bit 地址全部来自板级宏 */
    accCfg.bus            = bus;
    accCfg.address7       = (uint16_t)BOARD_ACCEL_I2C_ADDRESS;
    accCfg.expectedWhoAmI = (uint8_t)LED_MATRIX_APP_ACCEL_WHOAMI_EXPECT;
    accCfg.rangeG         = (uint8_t)LED_MATRIX_APP_ACCEL_RANGE_G;
    accCfg.odrHz          = (uint16_t)LED_MATRIX_APP_ACCEL_ODR_HZ;  /* 16 位：400Hz 档不被截断 */

    accel = NewSc7a20(&accCfg);
    if (accel == NULL) {
        ESP_LOGW(LED_MATRIX_APP_TAG, "NewSc7a20 failed, accel skipped");
        return;
    }

    /* 上电配置：失败仅告警，后面的实测读数照常尝试（便于定位接线问题） */
    ret = Sc7a20Init(accel);
    if (ret != 0) {
        ESP_LOGW(LED_MATRIX_APP_TAG,
                 "Sc7a20Init failed, ret=%d (matrix unaffected)", ret);
    }

    /* 实测 WHO_AM_I：0 校验模式下唯一可用于固化期望值的依据 */
    ret = Sc7a20ReadWhoAmI(accel, &whoAmI);
    if (ret == 0) {
        ESP_LOGI(LED_MATRIX_APP_TAG, "accel WHO_AM_I=0x%02x (address7=0x%02x)",
                 (unsigned)whoAmI, (unsigned)BOARD_ACCEL_I2C_ADDRESS);
    } else {
        ESP_LOGW(LED_MATRIX_APP_TAG,
                 "Sc7a20ReadWhoAmI failed, ret=%d (check SDA=GPIO%u SCL=GPIO%u)",
                 ret, (unsigned)BOARD_I2C_PIN_SDA, (unsigned)BOARD_I2C_PIN_SCL);
    }

    /* 三轴实测加速度：读成功才算加速度计可用 */
    ret = Sc7a20ReadMilliG(accel, pAccelX, pAccelY, pAccelZ);
    if (ret == 0) {
        ESP_LOGI(LED_MATRIX_APP_TAG, "accel x=%d y=%d z=%d mg",
                 (int)(*pAccelX), (int)(*pAccelY), (int)(*pAccelZ));
        *pAccelReady = 1;
    } else {
        ESP_LOGW(LED_MATRIX_APP_TAG, "Sc7a20ReadMilliG failed, ret=%d", ret);
    }
}

/* ==================================================================
 * 应用主流程（阻塞式常驻，正常路径永不返回）
 * ================================================================== */
void LedMatrixAppRun(void)
{
    Gpio*    gpio          = NULL;
    Spi*     spi           = NULL;
    Max7219* driver        = NULL;
    Screen*  screen        = NULL;
    I2c*     bus           = NULL;
    int      ret           = LED_MATRIX_APP_OK;
    int      screenFlushed = 0;
    int      accelReady    = 0;
    int32_t  accelX        = 0;
    int32_t  accelY        = 0;
    int32_t  accelZ        = 0;

    /* ---------------- 步骤 0 板卡摘要（纯日志，便于现场核对接线） ---------------- */
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "board: matrix %ux%u, %u x MAX7219, chips %ux%u, block=%u",
             (unsigned)BOARD_MATRIX_WIDTH,
             (unsigned)BOARD_MATRIX_HEIGHT,
             (unsigned)BOARD_MATRIX_CHIP_COUNT,
             (unsigned)BOARD_MATRIX_CHIPS_X,
             (unsigned)BOARD_MATRIX_CHIPS_Y,
             (unsigned)BOARD_MATRIX_BLOCK);
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "matrix spi: SCLK=GPIO%u MOSI=GPIO%u CS=GPIO%u MISO=%s @%u Hz, "
             "i2c: SDA=GPIO%u SCL=GPIO%u @%u Hz, accel addr7=0x%02x, init-led=GPIO%u",
             (unsigned)BOARD_MATRIX_PIN_CLK,
             (unsigned)BOARD_MATRIX_PIN_DIN,
             (unsigned)BOARD_MATRIX_PIN_LOAD,
             LED_MATRIX_APP_MISO_TEXT,
             (unsigned)BOARD_MATRIX_SPI_CLOCK_HZ,
             (unsigned)BOARD_I2C_PIN_SDA,
             (unsigned)BOARD_I2C_PIN_SCL,
             (unsigned)BOARD_I2C_CLOCK_HZ,
             (unsigned)BOARD_ACCEL_I2C_ADDRESS,
             (unsigned)BOARD_LED_INIT_PIN);

    /* ---------------- 步骤 1 抽象 GPIO：具象对象内部已调 NewGpio 装配 ----------------
     * 这是唯一一处失败即 return 的分支：GPIO 抽象都拿不到时，
     * 后续任何引脚操作都无从谈起，也就没有 INIT 灯可点。
     * ------------------------------------------------------------------------------ */
    gpio = NewGpioEsp32c3();
    if (gpio == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "NewGpioEsp32c3 failed (out of memory), abort");
        return;
    }

    /* ---------------- 步骤 2 INIT 指示灯：推挽输出 + 常亮 ----------------
     * 全文件唯一一次电平写入，固定高电平；失败只记录日志，
     * 【不得】写低电平去「修正」该灯，后续失败分支也不得再改写该脚。
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

    /* ---------------- 步骤 3 SPI 总线（抽象）----------------
     * mode0 / 8 bit 由 SpiDefaultConfig 给出，时钟取板级宏；
     * MISO 宏为 0xFFFFFFFF 表示未接（MAX7219 单向写），具象层归一化为 -1。
     * ------------------------------------------------------- */
    {
        SpiConfig spiCfg = SpiDefaultConfig(BOARD_MATRIX_SPI_CLOCK_HZ);

        spi = NewSpiEsp32c3(BOARD_MATRIX_PIN_CLK,
                            BOARD_MATRIX_PIN_DIN,
                            BOARD_MATRIX_PIN_MISO,
                            BOARD_MATRIX_PIN_LOAD,
                            &spiCfg);
    }

    if (spi == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "NewSpiEsp32c3 failed (bad pin / bus error / out of memory)");
        goto led_matrix_app_hold;
    }

    /* 幂等补装：满足 Max7219Spi 对「注入的 Spi 已 Init」这一前置约定 */
    ret = SpiInit(spi);
    if (ret != 0) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "SpiInit failed, ret=%d", ret);
        goto led_matrix_app_hold;
    }

    /* ---------------- 步骤 4 MAX7219 走 SPI 具象 ----------------
     * 驱动只借用 spi，不拥有总线；gpio / spi 由本文件常驻持有。
     * ---------------------------------------------------------- */
    {
        Max7219SpiConfig drvCfg = {
            .spi         = spi,
            .chainLength = BOARD_MATRIX_CHIP_COUNT,
            .timeoutMs   = LED_MATRIX_APP_MAX7219_TIMEOUT_MS,
        };

        driver = NewMax7219Spi(&drvCfg);
    }

    if (driver == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "NewMax7219Spi failed (bad config or out of memory)");
        goto led_matrix_app_hold;
    }

    /* 驱动侧上电默认配置（decode / scanLimit / intensity / 退出 shutdown） */
    ret = Max7219Init(driver);
    if (ret != MAX7219_SPI_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "Max7219Init failed, ret=%d (init-led kept ON)", ret);
        goto led_matrix_app_hold;
    }

    /* ---------------- 步骤 5 Screen 抽象：注入驱动 + 片阵列几何 ----------------
     * Screen 具象类只借用 driver，不销毁；后续显示操作全经 Screen* 抽象。
     * ------------------------------------------------------------------------ */
    {
        ScreenMax7219Config screenCfg = {
            .driver    = driver,
            .blockSize = BOARD_MATRIX_BLOCK,
            .chipsX    = BOARD_MATRIX_CHIPS_X,
            .chipsY    = BOARD_MATRIX_CHIPS_Y,
        };

        screen = NewScreenMax7219(&screenCfg);
    }

    if (screen == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "NewScreenMax7219 failed (bad config or out of memory)");
        goto led_matrix_app_hold;
    }

    /* 屏幕侧上电：失败即跳过绘制；清屏失败仅告警 */
    ret = ScreenInit(screen);
    if (ret != SCREEN_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG, "ScreenInit failed, ret=%d (init-led kept ON)", ret);
        goto led_matrix_app_hold;
    }

    ret = ScreenClear(screen);
    if (ret != SCREEN_OK) {
        ESP_LOGW(LED_MATRIX_APP_TAG, "ScreenClear failed, ret=%d (continue)", ret);
    }

    /* ---------------- 步骤 6 绘制：边框 + 8x8 bitmap 图标 + 整帧刷新 ----------------
     * 图案只写内存帧；ScreenFlush 才是唯一的下发动作。
     * 图标载体（Bitmap）在本步内部创建并立即释放，不留常驻对象。
     * ------------------------------------------------------------------------------ */
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

    /* ---------------- 步骤 7 加速度计：抽象 I2C 注入 + 装配 + 实测打印 ----------------
     * 总线构造失败 / 器件构造失败 / Init 失败 / 读数失败都只告警，
     * 点阵已上屏，此步纯属自检，不改变 LED 电平、不中断装配。
     * ------------------------------------------------------------------------------ */
    bus = NewI2cEsp32c3(BOARD_I2C_PORT,
                        BOARD_I2C_PIN_SDA,
                        BOARD_I2C_PIN_SCL,
                        BOARD_I2C_CLOCK_HZ);
    if (bus == NULL) {
        ESP_LOGW(LED_MATRIX_APP_TAG, "NewI2cEsp32c3 failed, accel skipped");
    } else {
        LedMatrixAppProbeAccel(bus, &accelReady, &accelX, &accelY, &accelZ);
    }

    /* ---------------- 步骤 8 就绪摘要：以上所有失败分支在此汇合 ----------------
     * 两条路径都只打日志，绝不改动 INIT 灯电平（保持常亮）。
     * -------------------------------------------------------------------------- */
led_matrix_app_hold:
    if ((0 != screenFlushed) && (0 != accelReady)) {
        ESP_LOGI(LED_MATRIX_APP_TAG,
                 "led_matrix ready: init-led=ON, screen flushed, accel %d/%d/%d mg",
                 (int)accelX, (int)accelY, (int)accelZ);
    } else {
        ESP_LOGW(LED_MATRIX_APP_TAG,
                 "led_matrix ready: init-led=ON, screen flushed=%s, "
                 "accel=%d/%d/%d mg (degraded)",
                 (0 != screenFlushed) ? "YES" : "NO",
                 (int)accelX, (int)accelY, (int)accelZ);
    }

    /* ---------------- 步骤 9 常驻保持 ----------------
     * gpio / spi / driver / screen / bus / accel 均为常驻对象，故意不析构：
     * 不调用 DeleteGpio / DeleteSpi / DeleteMax7219 / DeleteScreen /
     * DeleteI2c / DeleteSc7a20，让它们在设备整个上电周期内保持有效。
     * 本循环永不退出。
     * ------------------------------------------------- */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_MATRIX_APP_IDLE_PERIOD_MS));
    }
}
