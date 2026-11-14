/* ==================================================================
 * led_matrix_app.c —— led_matrix 产品组合根（Composition Root）
 *
 * 本文件是全工程【唯一】把具象实现拼装成产品的场所。装配完成后，
 * 产品由三条通讯 / 采样线程 + 一个组合根主循环共同驱动：
 *
 *   spi_task   （优先级 5，栈 4096）  LED 刷新专用线程
 *       由 EventLoop spi_loop 的 worker 承担线程体；
 *       MatrixDisplayShow / BlockTilerShowLetterT 只往该队列投递渲染任务，
 *       屏幕帧缓冲的读写与 ScreenFlush 永远只发生在这一条线程里。
 *
 *   uart_task  （优先级 4，栈 4096）  信息回传专用线程
 *       由 EventLoop uart_loop 的 worker 承担线程体；
 *       InfoService 只往该队列投递回传任务，
 *       因此串口输出（ESP_LOGI 的逐帧快照）只在这一条线程发生。
 *
 *   采样线程   （accel_service 内部创建，周期 APP_ACCEL_POLL_PERIOD_MS）
 *       只做一件事：按周期读 SC7A20 并在互斥量保护下整帧写入 AccelSample
 *       快照。它是该快照的唯一写入方，其它模块只能拿到拷贝。
 *
 * 装配顺序（严格按序，任一步失败都只降级，绝不崩溃、绝不关灯）：
 *
 *   步骤 1  NewGpioEsp32c3 + INIT 灯常亮   唯一一次电平写，固定高电平
 *   步骤 2  SpiDefaultConfig + NewSpiEsp32c3 + SpiInit
 *   步骤 3  NewMax7219Spi（注入 spi）+ Max7219Init
 *   步骤 4  NewScreenMax7219（注入 driver）+ ScreenInit + ScreenClear
 *   步骤 5  spi_task 线程对象 + spi_loop + start   LED 刷新线程就此运行
 *   步骤 6  uart_task 线程对象 + uart_loop + start 信息回传线程就此运行
 *   步骤 7  NewMatrixDisplay（注入 screen + spi_loop）
 *   步骤 7b NewBlockTiler（注入 screen + spi_loop）方向自检装配
 *   步骤 8  NewI2cEsp32c3
 *   步骤 9  NewAccelService（注入 bus）+ AccelServiceStart  采样线程就此运行
 *   步骤 10 NewInfoService（注入 uart_loop + provider）+ InfoServiceStart
 *   步骤 11 BlockTilerShowLetterT          异步出图，每个 8x8 块显示字母 T
 *   步骤 12 主循环：1 s 一拍；保持启动画面；每 5 拍主动回传
 *
 * 失败处理策略（全部只降级，绝不 return、绝不写低 INIT 灯）：
 *   - 步骤 1 GPIO 失败：INIT 灯点不亮，但显示 / 采样链路照常（两者不依赖 GPIO）；
 *   - 步骤 2 到 5 任一失败：goto 步骤 6，跳过后续显示链路，
 *     步骤 7 再按 spiLoopReady 判定是否建刷新服务，UART / 采样链路照常；
 *   - 步骤 6 UART 线程失败：goto 步骤 7，跳过信息服务，显示链路照常；
 *   - 步骤 7b 方向自检装配失败：只跳过字母 T 自检（屏幕停在清屏后的空白），
 *     采样链路 / 信息服务 / 主循环照常；
 *   - 步骤 8 / 9 失败：goto 步骤 10，accel 为 NULL 时 providerContext 传 NULL，
 *     provider 内部判空返回 valid = 0，信息服务仍可建立；
 *   - 步骤 10 失败：跳过信息服务，初始出图与主循环照常。
 *
 * 依赖方向（严格单向，只依赖抽象头 + 板级宏，不含任何业务算法）：
 *   led_matrix_app.h  应用入口
 *   board_config.h    板级唯一引脚 / 时钟 / 地址口径
 *   app_types.h       AccelSample 与采样 / 回传周期
 *   gpio / spi / max7219 / screen / i2c 抽象父类 + 各自具象构造
 *   accel_service / matrix_display / info_service 三个服务
 *   block_tiler       方向自检：把屏幕按 8x8 分块逐块贴字母 T
 *   platform / thread_object / event_loop 线程与队列抽象
 *
 * 所有权与寿命：
 *   gpio / spi / driver / screen / bus / accel / info / display / tiler
 *   以及两个线程对象、两个事件循环全部是【嵌入式常驻对象】，随设备上电常驻，
 *   故意不析构（本文件不出现任何 Delete 调用），主循环永不退出。
 * ================================================================== */

#include <stdint.h>

#include "board_config.h"
#include "app_types.h"
#include "led_matrix_app.h"

#include "gpio.h"
#include "gpio_esp32c3.h"

#include "spi.h"
#include "spi_esp32c3.h"

#include "max7219.h"
#include "max7219_spi.h"

#include "screen.h"
#include "screen_max7219.h"

#include "i2c.h"
#include "i2c_esp32c3.h"

#include "block_tiler.h"   /* 方向自检：按 8x8 分块逐块贴字母 T（显示扩展，非业务） */

#include "accel_service.h"
#include "matrix_display.h"
#include "info_service.h"

#include "platform.h"
#include "thread_object.h"
#include "event_loop.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------
 * 日志标签
 * ------------------------------------------------------------------ */
static const char* LED_MATRIX_APP_TAG = "led_matrix_app";

/* ------------------------------------------------------------------
 * 线程参数与队列名：只属于本文件的装配常量（栈 / 优先级 / 线程名），
 * 引脚、时钟、地址、周期一律取自 board_config.h 与 app_types.h。
 * ------------------------------------------------------------------ */
#define APP_SPI_THREAD_STACK_SIZE     (4096)
#define APP_SPI_THREAD_PRIORITY       (5)
#define APP_SPI_THREAD_NAME           "spi_task"
#define APP_SPI_LOOP_NAME             "spi_loop"

#define APP_UART_THREAD_STACK_SIZE    (4096)
#define APP_UART_THREAD_PRIORITY      (4)
#define APP_UART_THREAD_NAME          "uart_task"
#define APP_UART_LOOP_NAME            "uart_loop"

/* MAX7219 单帧传输超时（毫秒） */
#define APP_MAX7219_TIMEOUT_MS        (100u)

/* 主循环节奏：1 s 一拍；屏幕固定启动画面；每 5 拍主动回传一次 */
#define APP_MAIN_TICK_PERIOD_MS       (1000u)
#define APP_PUSH_TICKS                (5u)

/* ==================================================================
 * 快照供给（依赖反转的注入点）
 *
 * info_service 不认识 accel_service，只按 InfoSampleProvider 回调取数；
 * 本函数即那条包装：把 AccelServiceGet 的返回值翻译成 AccelSample.valid。
 * 它运行在 uart_task 线程内（由 info_service 的 post 任务调用），
 * 因此这里的读取与采样线程的写入经 accel_service 的互斥量串行化。
 *
 * 失败语义：拿不到快照（未启动 / 尚无有效样本 / context 为空）时
 * 只把 valid 置 0，坐标字段保留服务给出的副本或置 0，绝不阻塞、绝不崩溃。
 * ================================================================== */
static void AppAccelProvider(void* context, AccelSample* outSample)
{
    AccelService* service = (AccelService*)context;
    int           ret     = 0;

    if (outSample == NULL) {
        return;
    }

    /* 先给出确定的无效快照：即使下面提前返回，字段也不会是栈上杂值 */
    outSample->xMg         = 0;
    outSample->yMg         = 0;
    outSample->zMg         = 0;
    outSample->timestampMs = 0u;
    outSample->valid       = 0u;

    /* accel 装配失败时 providerContext 就是 NULL：直接给出 valid = 0 */
    if (service == NULL) {
        ESP_LOGD(LED_MATRIX_APP_TAG, "accel service absent, sample marked invalid");
        return;
    }

    ret = AccelServiceGet(service, outSample);
    if (ret != 0) {
        /* 未启动 / 尚无有效样本 / 最近一次读取失败：统一降级为无效帧 */
        outSample->valid = 0u;
        ESP_LOGD(LED_MATRIX_APP_TAG, "accel snapshot unavailable, ret=%d", ret);
    }
}

/* ==================================================================
 * 应用主流程（阻塞式常驻，正常路径永不返回）
 * ================================================================== */
void LedMatrixAppRun(void)
{
    /* -------- 抽象对象与常驻句柄（全部在顶部声明，失败分支只跳转不返回） -------- */
    Gpio*    gpio   = NULL;
    Spi*     spi    = NULL;
    Max7219* driver = NULL;
    Screen*  screen = NULL;
    I2c*     bus    = NULL;

    ThreadObject* spiThread  = NULL;   /* spi_task：LED 刷新线程的线程对象 */
    EventLoop*    spiLoop    = NULL;   /* spi_loop：LED 刷新任务队列 */
    ThreadObject* uartThread = NULL;   /* uart_task：信息回传线程的线程对象 */
    EventLoop*    uartLoop   = NULL;   /* uart_loop：信息回传任务队列 */

    MatrixDisplay* display = NULL;
    BlockTiler*    tiler   = NULL;   /* 方向自检：每个 8x8 块显示字母 T */
    AccelService*  accel   = NULL;
    InfoService*   info    = NULL;

    PlatformThreadConfig spiThreadCfg;
    PlatformThreadConfig uartThreadCfg;
    SpiConfig            spiCfg;

    int      ret           = 0;
    int      spiLoopReady  = 0;   /* 1 = spi_loop 已 start，可安全 post */
    int      uartLoopReady = 0;   /* 1 = uart_loop 已 start，可安全 post */
    uint32_t tick          = 0;

    /* ---------------- 步骤 0 板卡摘要（纯日志，不触碰任何硬件） ---------------- */
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "board: matrix %ux%u, %u x MAX7219, chips %ux%u, block=%u",
             (unsigned)BOARD_MATRIX_WIDTH,
             (unsigned)BOARD_MATRIX_HEIGHT,
             (unsigned)BOARD_MATRIX_CHIP_COUNT,
             (unsigned)BOARD_MATRIX_CHIPS_X,
             (unsigned)BOARD_MATRIX_CHIPS_Y,
             (unsigned)BOARD_MATRIX_BLOCK);
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "wiring: SCLK=GPIO%u MOSI=GPIO%u CS=GPIO%u @%u Hz, "
             "SDA=GPIO%u SCL=GPIO%u @%u Hz, accel addr7=0x%02x, init-led=GPIO%u",
             (unsigned)BOARD_MATRIX_PIN_CLK,
             (unsigned)BOARD_MATRIX_PIN_DIN,
             (unsigned)BOARD_MATRIX_PIN_LOAD,
             (unsigned)BOARD_MATRIX_SPI_CLOCK_HZ,
             (unsigned)BOARD_I2C_PIN_SDA,
             (unsigned)BOARD_I2C_PIN_SCL,
             (unsigned)BOARD_I2C_CLOCK_HZ,
             (unsigned)BOARD_ACCEL_I2C_ADDRESS,
             (unsigned)BOARD_LED_INIT_PIN);

    /* ---------------- 步骤 1 抽象 GPIO + INIT 指示灯常亮 ----------------
     * INIT 灯是整机唯一的「已装配」指示：本文件只写一次电平，固定高电平。
     * 点亮失败只记录日志：显示链路与采样链路都不依赖 GPIO，因此继续装配；
     * 任何失败分支都【不写低电平】，也不再次改写该脚。
     * -------------------------------------------------------------------- */
    gpio = NewGpioEsp32c3();
    if (gpio == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "NewGpioEsp32c3 failed (out of memory): init-led cannot be driven, "
                 "display and sampling chains continue");
    } else {
        ret = GpioInit(gpio, BOARD_LED_INIT_PIN, GpioDirOutput, GpioPullNone);
        if (ret != GPIO_OK) {
            ESP_LOGE(LED_MATRIX_APP_TAG,
                     "GpioInit(init-led GPIO%u) failed, ret=%d (led level untouched)",
                     (unsigned)BOARD_LED_INIT_PIN, ret);
        } else {
            ret = GpioWrite(gpio, BOARD_LED_INIT_PIN, GpioLevelHigh);
            if (ret != GPIO_OK) {
                ESP_LOGE(LED_MATRIX_APP_TAG,
                         "init-led high failed, ret=%d (led level untouched)", ret);
            } else {
                ESP_LOGI(LED_MATRIX_APP_TAG,
                         "init-led ON (GPIO%u high, written once for the whole runtime)",
                         (unsigned)BOARD_LED_INIT_PIN);
            }
        }
    }

    /* ---------------- 步骤 2 SPI 总线（抽象） ----------------
     * mode0 / 8 bit 由 SpiDefaultConfig 给出，时钟取板级宏；
     * MISO 宏为 0xFFFFFFFF 表示未接（MAX7219 单向写），具象层自行归一。
     * -------------------------------------------------------- */
    spiCfg = SpiDefaultConfig(BOARD_MATRIX_SPI_CLOCK_HZ);
    spi = NewSpiEsp32c3(BOARD_MATRIX_PIN_CLK,
                        BOARD_MATRIX_PIN_DIN,
                        BOARD_MATRIX_PIN_MISO,
                        BOARD_MATRIX_PIN_LOAD,
                        &spiCfg);
    if (spi == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "NewSpiEsp32c3 failed (pins / bus / memory): display chain skipped");
        goto app_step6_uart_thread;
    }

    ret = SpiInit(spi);
    if (ret != 0) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "SpiInit failed, ret=%d: display chain skipped", ret);
        goto app_step6_uart_thread;
    }

    /* ---------------- 步骤 3 MAX7219 驱动（经抽象 SPI 具象） ----------------
     * 驱动只借用 spi，不拥有总线；4 片级联由板级宏给出。
     * ---------------------------------------------------------------------- */
    driver = NewMax7219Spi(&(Max7219SpiConfig){
        .spi         = spi,
        .chainLength = BOARD_MATRIX_CHIP_COUNT,
        .timeoutMs   = APP_MAX7219_TIMEOUT_MS,
    });
    if (driver == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "NewMax7219Spi failed (bad config / memory): display chain skipped");
        goto app_step6_uart_thread;
    }

    ret = Max7219Init(driver);
    if (ret != MAX7219_SPI_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "Max7219Init failed, ret=%d: display chain skipped (init-led kept ON)", ret);
        goto app_step6_uart_thread;
    }

    /* ---------------- 步骤 4 Screen 抽象（注入驱动 + 片阵列几何） ----------------
     * 宽高由 chipsX / chipsY 与 blockSize 推导，后续显示操作全经 Screen*。
     * ---------------------------------------------------------------------------- */
    screen = NewScreenMax7219(&(ScreenMax7219Config){
        .driver      = driver,
        .blockSize   = BOARD_MATRIX_BLOCK,
        .chipsX      = BOARD_MATRIX_CHIPS_X,
        .chipsY      = BOARD_MATRIX_CHIPS_Y,
        .invertDigit = (uint8_t)BOARD_MATRIX_INVERT_DIGIT,
    });
    if (screen == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "NewScreenMax7219 failed (bad config / memory): display chain skipped");
        goto app_step6_uart_thread;
    }

    ret = ScreenInit(screen);
    if (ret != SCREEN_OK) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "ScreenInit failed, ret=%d: display chain skipped (init-led kept ON)", ret);
        goto app_step6_uart_thread;
    }

    ret = ScreenClear(screen);
    if (ret != SCREEN_OK) {
        ESP_LOGW(LED_MATRIX_APP_TAG, "ScreenClear failed, ret=%d (continue)", ret);
    }

    /* ---------------- 步骤 5 SPI 通讯线程（LED 刷新专用） ----------------
     * 线程体由 EventLoop 提供（workFunc 留空），本文件只提供线程对象；
     * loop 启动后，渲染任务一律在其 worker 线程内执行。
     * -------------------------------------------------------------------- */
    spiThreadCfg.stackSize = APP_SPI_THREAD_STACK_SIZE;
    spiThreadCfg.priority  = APP_SPI_THREAD_PRIORITY;
    spiThreadCfg.name      = APP_SPI_THREAD_NAME;
    spiThreadCfg.workFunc  = NULL;
    spiThreadCfg.workParam = NULL;

    spiThread = NewPlatformThreadObject(&spiThreadCfg);
    if (spiThread == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "create %s thread object failed: display chain skipped",
                 APP_SPI_THREAD_NAME);
        goto app_step6_uart_thread;
    }

    spiLoop = NewEventLoopDefault(APP_SPI_LOOP_NAME);
    if (spiLoop == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "create %s failed: display chain skipped", APP_SPI_LOOP_NAME);
        goto app_step6_uart_thread;
    }

    if ((spiLoop->instance == NULL) || (spiLoop->interfaces.start == NULL)) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "%s is not usable (instance / start slot empty): display chain skipped",
                 APP_SPI_LOOP_NAME);
        goto app_step6_uart_thread;
    }

    ret = spiLoop->interfaces.start(spiLoop->instance, spiThread);
    if (ret != 0) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "%s start failed, ret=%d: display chain skipped", APP_SPI_LOOP_NAME, ret);
        goto app_step6_uart_thread;
    }
    spiLoopReady = 1;
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "%s running: every LED refresh runs on the spi worker thread only",
             APP_SPI_THREAD_NAME);

app_step6_uart_thread:
    /* ---------------- 步骤 6 UART 通讯线程（信息回传专用） ----------------
     * 与步骤 5 同构：串口输出只允许发生在 uart_task 线程内，
     * 信息服务只负责把回传任务 post 到这个队列。
     * -------------------------------------------------------------------- */
    uartThreadCfg.stackSize = APP_UART_THREAD_STACK_SIZE;
    uartThreadCfg.priority  = APP_UART_THREAD_PRIORITY;
    uartThreadCfg.name      = APP_UART_THREAD_NAME;
    uartThreadCfg.workFunc  = NULL;
    uartThreadCfg.workParam = NULL;

    uartThread = NewPlatformThreadObject(&uartThreadCfg);
    if (uartThread == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "create %s thread object failed: info service skipped",
                 APP_UART_THREAD_NAME);
        goto app_step7_display;
    }

    uartLoop = NewEventLoopDefault(APP_UART_LOOP_NAME);
    if (uartLoop == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "create %s failed: info service skipped", APP_UART_LOOP_NAME);
        goto app_step7_display;
    }

    if ((uartLoop->instance == NULL) || (uartLoop->interfaces.start == NULL)) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "%s is not usable (instance / start slot empty): info service skipped",
                 APP_UART_LOOP_NAME);
        goto app_step7_display;
    }

    ret = uartLoop->interfaces.start(uartLoop->instance, uartThread);
    if (ret != 0) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "%s start failed, ret=%d: info service skipped", APP_UART_LOOP_NAME, ret);
        goto app_step7_display;
    }
    uartLoopReady = 1;
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "%s running: serial output runs on the uart worker thread only",
             APP_UART_THREAD_NAME);

app_step7_display:
    /* ---------------- 步骤 7 刷新服务（注入 screen + spi_loop） ----------------
     * 没有可用的刷新线程或屏幕时直接跳过：本对象只是异步入口，
     * 缺它不影响采样线程与信息服务。
     * -------------------------------------------------------------------------- */
    if ((screen != NULL) && (spiLoop != NULL) && (0 != spiLoopReady)) {
        display = NewMatrixDisplay(&(MatrixDisplayConfig){
            .screen = screen,
            .loop   = spiLoop,
        });
        if (display == NULL) {
            ESP_LOGE(LED_MATRIX_APP_TAG,
                     "NewMatrixDisplay failed: pattern switching disabled");
        } else {
            ESP_LOGI(LED_MATRIX_APP_TAG, "matrix display ready (async refresh on %s)",
                     APP_SPI_THREAD_NAME);
        }
    } else {
        ESP_LOGW(LED_MATRIX_APP_TAG,
                 "matrix display skipped: screen / spi refresh loop not ready");
    }

    /* ---------------- 步骤 7b 方向自检装配（注入 screen + spi_loop） ----------------
     * 每个 8x8 分块显示字母 T，用于现场确认片阵列方向与分块顺序是否正确。
     * 注入沿用显示链路的同一对 screen / spiLoop，本文件不硬编码屏幕对象；
     * screen 或已 start 的 spi_loop 任一缺失就整段跳过（不把空指针交给构造），
     * 构造失败只记录日志，绝不中断装配。
     * --------------------------------------------------------------------------------- */
    if ((screen != NULL) && (spiLoop != NULL) && (0 != spiLoopReady)) {
        tiler = NewBlockTiler(&(BlockTilerConfig){
            .screen = screen,
            .loop   = spiLoop,
        });
        if (tiler == NULL) {
            ESP_LOGE(LED_MATRIX_APP_TAG,
                     "NewBlockTiler failed: orientation self-check disabled");
        } else {
            ESP_LOGI(LED_MATRIX_APP_TAG,
                     "block tiler ready: letter-T shown per 8x8 block at startup");
        }
    } else {
        ESP_LOGW(LED_MATRIX_APP_TAG,
                 "block tiler skipped: screen / spi refresh loop not ready");
    }

    /* ---------------- 步骤 8 I2C 总线（抽象） ----------------
     * 总线失败即没有采样服务：直接跳到步骤 10，信息服务仍以 NULL 上下文建立。
     * -------------------------------------------------------- */
    bus = NewI2cEsp32c3(BOARD_I2C_PORT,
                        BOARD_I2C_PIN_SDA,
                        BOARD_I2C_PIN_SCL,
                        BOARD_I2C_CLOCK_HZ);
    if (bus == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "NewI2cEsp32c3 failed (port / pins / memory): sampling chain skipped");
        goto app_step10_info;
    }

    /* ---------------- 步骤 9 采样服务（注入抽象总线） ----------------
     * AccelServiceStart 拉起采样线程；线程起来后它是 AccelSample 的唯一写入方。
     * ------------------------------------------------------------------ */
    accel = NewAccelService(&(AccelServiceConfig){
        .bus      = bus,
        .address7 = (uint16_t)BOARD_ACCEL_I2C_ADDRESS,
        .periodMs = (uint32_t)APP_ACCEL_POLL_PERIOD_MS,
    });
    if (accel == NULL) {
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "NewAccelService failed: sampling chain skipped (info push reports invalid)");
        goto app_step10_info;
    }

    ret = AccelServiceStart(accel);
    if (ret != 0) {
        /* 器件无应答 / 线程未就绪：服务对象保留，快照维持 valid = 0 便于现场观察 */
        ESP_LOGE(LED_MATRIX_APP_TAG,
                 "AccelServiceStart failed, ret=%d: samples stay invalid", ret);
    } else {
        ESP_LOGI(LED_MATRIX_APP_TAG, "accel sampling thread running, period=%u ms",
                 (unsigned)APP_ACCEL_POLL_PERIOD_MS);
    }

app_step10_info:
    /* ---------------- 步骤 10 信息服务（注入 uart_loop + provider） ----------------
     * 回传任务在 uart_task 内执行；accel 为 NULL 时上下文传 NULL，
     * provider 内部判空返回 valid = 0，信息服务仍可建立。
     * ------------------------------------------------------------------------------ */
    if ((uartLoop != NULL) && (0 != uartLoopReady)) {
        info = NewInfoService(&(InfoServiceConfig){
            .loop            = uartLoop,
            .provider        = AppAccelProvider,
            .providerContext = (void*)accel,
            .pushPeriodMs    = (uint32_t)APP_INFO_PUSH_PERIOD_MS,
        });
        if (info == NULL) {
            ESP_LOGE(LED_MATRIX_APP_TAG, "NewInfoService failed: info push disabled");
        } else {
            ret = InfoServiceStart(info);
            if (ret != 0) {
                /* debug 定时器起不来时仍可被动回传；release 构建本就是空实现 */
                ESP_LOGE(LED_MATRIX_APP_TAG,
                         "InfoServiceStart failed, ret=%d: on-demand push still available", ret);
            } else {
                ESP_LOGI(LED_MATRIX_APP_TAG,
                         "info service ready, default push period=%u ms on %s",
                         (unsigned)APP_INFO_PUSH_PERIOD_MS, APP_UART_THREAD_NAME);
            }
        }
    } else {
        ESP_LOGW(LED_MATRIX_APP_TAG,
                 "info service skipped: uart refresh loop not ready");
    }

    /* ---------------- 步骤 11 启动出图（异步，立即返回） ----------------
     * 启动即在每个 8x8 分块显示字母 T（方向自检）：真正清屏 / 逐块贴 T /
     * flush 全部在 spi_task 内完成。post 失败只记录日志，不中断装配。
     * display 对象保留装配（本次不再出图案），供日后恢复图案轮播使用。
     * ------------------------------------------------------------------ */
    if (tiler != NULL) {
        ret = BlockTilerShowLetterT(tiler);
        if (ret != 0) {
            ESP_LOGE(LED_MATRIX_APP_TAG,
                     "letter-T post failed, ret=%d (screen may stay blank)", ret);
        }
    } else {
        ESP_LOGW(LED_MATRIX_APP_TAG,
                 "letter-T self-check skipped: block tiler not ready");
    }

    /* ---------------- 步骤 12 主循环（常驻，永不返回） ----------------
     * 组合根线程只做一件事：
     *   每 5 拍主动请求一次回传，覆盖 release 构建下没有定时器的路径。
     * 屏幕保持启动时的字母 T 自检画面，不再切换图案；tick 保留以便日后恢复轮播。
     * 所有常驻对象在此不析构，随设备上电周期常驻。
     * ------------------------------------------------------------------ */
    ESP_LOGI(LED_MATRIX_APP_TAG,
             "enter resident loop: display kept at startup letter-T frame, "
             "info push every %u s",
             (unsigned)APP_PUSH_TICKS);

    tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(APP_MAIN_TICK_PERIOD_MS));
        tick++;

        if (0u == (tick % APP_PUSH_TICKS)) {
            if (info != NULL) {
                ret = InfoServiceRequestPush(info);
                if (ret != 0) {
                    ESP_LOGW(LED_MATRIX_APP_TAG,
                             "info push request failed, ret=%d (skipped for the tick)", ret);
                }
            }
        }
    }
}
