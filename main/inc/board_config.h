#ifndef _BOARD_CONFIG_H
#define _BOARD_CONFIG_H

/* ============================================================================
 * led_matrix 产品板级配置 —— 唯一引脚口径
 *
 * 来源：Netlist_leds_matrix_2026-11-07.enet（336 器件，SHA256 C33FC4BE…F800B89）
 * 完整拓扑见 IMA《项目记忆 · esp32_development》→ `## 2026-11-07 硬件拓扑 · led_matrix`
 *
 * 该文件属于**产品分支**（product/led_matrix），不进 main。
 * 任何装配代码只允许引用这里的宏，不得在别处硬编码引脚号。
 * ========================================================================== */

/* ---------------- LED 点阵：4 × MAX7219 级联，16 × 16 单色 ---------------- */
#define BOARD_MATRIX_CHIP_COUNT   4u
#define BOARD_MATRIX_WIDTH        16u
#define BOARD_MATRIX_HEIGHT       16u
#define BOARD_MATRIX_BLOCK         8u   /* 单片 MAX7219 驱动 8×8 */
#define BOARD_MATRIX_PIXEL_COUNT  (BOARD_MATRIX_WIDTH * BOARD_MATRIX_HEIGHT)

/* 片阵列：2 × 2（chipIndex = chipY * CHIPS_X + chipX，chipIndex 0 为接 DIN 的首片） */
#define BOARD_MATRIX_CHIPS_X      (BOARD_MATRIX_WIDTH / BOARD_MATRIX_BLOCK)
#define BOARD_MATRIX_CHIPS_Y      (BOARD_MATRIX_HEIGHT / BOARD_MATRIX_BLOCK)

#define BOARD_MATRIX_PIN_DIN      7u    /* GPIO7  -> MAX1.DIN（只进第一片，SPI 的 MOSI） */
#define BOARD_MATRIX_PIN_CLK      6u    /* GPIO6  -> 4 片 CLK 共用（SPI 的 SCLK） */
#define BOARD_MATRIX_PIN_LOAD     10u   /* GPIO10 -> 4 片 LOAD/CS# 共用（SPI 的 CS） */
#define BOARD_MATRIX_PIN_MISO     0xFFFFFFFFu /* MAX7219 单向写，未接 MISO（= (uint32_t)-1） */
#define BOARD_MATRIX_SPI_CLOCK_HZ 1000000u    /* 5V 逻辑裕量为负，先跑 1 MHz；实测稳定后可升到 10 MHz */

/* 点阵 digit 极性：0 = 正相（位1亮），1 = 反相（位0亮）。
 * 本板实测为反相：ScreenFill(0) 全亮、ScreenFill(1) 全灭。换模块时只改这一处。 */
#define BOARD_MATRIX_INVERT_DIGIT   1u

/* ---------------- I2C 加速度计 SC7A20HTR ---------------- */
#define BOARD_I2C_PORT            0u
#define BOARD_I2C_PIN_SDA         0u    /* GPIO0 = SDx（该脚兼 XTAL_32K_P） */
#define BOARD_I2C_PIN_SCL         1u    /* GPIO1 = SCx（该脚兼 XTAL_32K_N） */
#define BOARD_I2C_CLOCK_HZ        400000u
#define BOARD_ACCEL_I2C_ADDRESS   0x19u /* 7-bit（SDO 悬空/接地，按实测确认） */

/* ---------------- 指示灯 / 按键 ---------------- */
#define BOARD_LED_INIT_PIN        3u    /* 高电平点亮：GPIO3 经 R18 10K 到阳极，阴极 GND */
#define BOARD_KEY_BOOT_PIN        9u    /* 按下接地，外部 R3 10K 上拉 */
#define BOARD_KEY_REST_PIN        (-1)  /* REST 直接拉 CHIP_EN，软件不可见 */
#define BOARD_KEY_WAKE_PIN        (-1)  /* WAKE 接 IP5306 KEY，软件不可见 */

/* ---------------- 专用 / 保留引脚（禁止当普通 IO 使用） ---------------- */
#define BOARD_PIN_FLASH_HOLD      12u   /* SPI Flash HOLD# */
#define BOARD_PIN_FLASH_WP        13u   /* SPI Flash WP# */
#define BOARD_USB_PIN_DM          18u   /* 原生 USB D- */
#define BOARD_USB_PIN_DP          19u   /* 原生 USB D+ */
#define BOARD_UART_CONSOLE_PORT   0u    /* UART0 <-> CH340N（CH340 的 TX/RX 与芯片脚名交叉） */

/* ---------------- 启动与电气约束（写代码前先读） ----------------
 * 1) Strapping：GPIO2 / GPIO8 / GPIO9，上电瞬间不得被外部强驱动；
 *    GPIO9 兼 BOOT 键（按下接地）。
 * 2) GPIO0 / GPIO1 已作 I2C，**禁止再启用 32K 晶振**。
 * 3) GPIO12 / GPIO13 接 Flash HOLD#/WP#；GPIO18/19 为 USB，均不可挪用。
 * 4) MAX7219 为 5V 逻辑（VIH 典型 3.5V），C3 输出 3.3V，**裕量为负**，
 *    属待硬件复核项；软件侧先按可用实现，必要时降 SPI 时钟。
 * 5) 电源链：VBUS -> VIN -> IP5306 -> VOUT -> SW1 -> +5V -> AMS1117 -> +3.3V。
 * ---------------------------------------------------------------------- */

#endif /* _BOARD_CONFIG_H */
