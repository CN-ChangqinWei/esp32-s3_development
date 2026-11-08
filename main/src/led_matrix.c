/* ==================================================================
 * led_matrix.c —— 16x16 单色点阵帧缓冲 + 4 片 MAX7219 组帧下发
 *
 * 【几何映射】本板约定（网表未给出面板丝印方向，故属假设项）
 *   4 片按 2x2 排布，下标即 chipIndex（等价于 Max7219 抽象层的级联编号）：
 *     chipIndex 0 = 左上 (x 0..7,  y 0..7 )
 *     chipIndex 1 = 右上 (x 8..15, y 0..7 )
 *     chipIndex 2 = 左下 (x 0..7,  y 8..15)
 *     chipIndex 3 = 右下 (x 8..15, y 8..15)
 *   若实物方向不同（旋转 180 度、上下翻转、左右翻转、行列互换等），
 *   只改下面 LED_MATRIX_CHIP_ORIGIN 这一处常量即可，其余代码不依赖方向。
 *
 * 【单片行位序】MAX7219 一个 digit 寄存器对应点阵的一行：
 *   寄存器地址 0x01 是第 0 行 …… 0x08 是第 7 行；
 *   数据字节 bit7 = 该块最左列（x 最小），bit0 = 该块最右列。
 *
 * 【一次下发 8 字节的归属】Max7219 抽象约定 writeFrame「先发最远端」，
 *   故字节对与 chipIndex 的对应关系为：
 *     frame[0]、frame[1] -> chipIndex 3（寄存器地址、该行 8 位位图）
 *     frame[2]、frame[3] -> chipIndex 2
 *     frame[4]、frame[5] -> chipIndex 1
 *     frame[6]、frame[7] -> chipIndex 0
 *   每片有 8 行、一帧只能放一行，故 Flush 内部按行循环 8 次、每次一帧。
 *
 * 返回值约定（与 led_matrix.h 一致）：
 *    0  成功
 *   -1  参数非法（句柄为空、driver 或 driver->instance 为空、坐标越界）
 *   负值原样上抛  Flush 透传 Max7219WriteFrame 的错误码
 * ================================================================== */

#include "led_matrix.h"
#include "max7219.h"
#include "board_config.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 本模块返回码
 * ------------------------------------------------------------------ */
#define LED_MATRIX_OK           0
#define LED_MATRIX_ERR_PARAM    (-1)

/* MAX7219 行寄存器基址：第 0 行 = 0x01 …… 第 7 行 = 0x08。
 * 这是芯片协议常量，与板级几何无关，故不取自 board_config.h。 */
#define LED_MATRIX_REG_ROW_BASE 0x01u

/* 一帧的字节数：每片两个字节（寄存器地址 + 数据） */
#define LED_MATRIX_FRAME_LEN    (2u * BOARD_MATRIX_CHIP_COUNT)

/* ------------------------------------------------------------------
 * 编译期自检：2x2 块划分必须与板级几何、片数自洽
 * ------------------------------------------------------------------ */
#if (BOARD_MATRIX_WIDTH % BOARD_MATRIX_BLOCK) != 0u
#error "BOARD_MATRIX_WIDTH 必须能被 BOARD_MATRIX_BLOCK 整除"
#endif

#if (BOARD_MATRIX_HEIGHT % BOARD_MATRIX_BLOCK) != 0u
#error "BOARD_MATRIX_HEIGHT 必须能被 BOARD_MATRIX_BLOCK 整除"
#endif

#if BOARD_MATRIX_CHIP_COUNT != \
    ((BOARD_MATRIX_WIDTH / BOARD_MATRIX_BLOCK) * (BOARD_MATRIX_HEIGHT / BOARD_MATRIX_BLOCK))
#error "片数与 2x2 块划分不一致，请先核对 board_config.h"
#endif

/* ------------------------------------------------------------------
 * 每片在 16x16 全局坐标中的起始位置
 *
 * 【若实物方向不同，只改这里】下标即 chipIndex，与上面文件头注释一致。
 * 注意：chipIndex 同时被 Max7219 抽象层当作级联编号使用
 * （0 = 直接接 DIN 的第一片），因此本表隐含「面板左上片接在链首」
 * 这一假设；若实际上链顺序与物理位置不同，改本表即可，无需改逻辑。
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t originY;   /* 该片左上角在全局坐标中的行 */
    uint8_t originX;   /* 该片左上角在全局坐标中的列 */
} LedMatrixChipOrigin;

static const LedMatrixChipOrigin LED_MATRIX_CHIP_ORIGIN[BOARD_MATRIX_CHIP_COUNT] = {
    { 0u,                 0u                 },  /* chipIndex 0 -> 左上 */
    { 0u,                 BOARD_MATRIX_BLOCK },  /* chipIndex 1 -> 右上 */
    { BOARD_MATRIX_BLOCK, 0u                 },  /* chipIndex 2 -> 左下 */
    { BOARD_MATRIX_BLOCK, BOARD_MATRIX_BLOCK }   /* chipIndex 3 -> 右下 */
};

/* ------------------------------------------------------------------
 * 内部结构体（不透明，仅本文件可见）
 *   pixels 为帧缓冲，每个元素取 0 或 1；Set/Get/Clear/Fill 只改内存
 *   driver 为注入的抽象父类，本模块不拥有、不销毁
 * ------------------------------------------------------------------ */
struct LedMatrix {
    Max7219* driver;
    uint8_t  pixels[BOARD_MATRIX_HEIGHT][BOARD_MATRIX_WIDTH];
};

/* ------------------------------------------------------------------
 * 内存钩子默认实现：外部可在创建对象之前替换为平台专用分配器
 * ------------------------------------------------------------------ */
void* (*ledMatrixMalloc)(size_t size) = malloc;
void  (*ledMatrixFree)(void* p)       = free;

/* ------------------------------------------------------------------
 * 内部工具：句柄与注入的驱动都必须有效
 * ------------------------------------------------------------------ */
static int ledMatrixCheckHandle(const LedMatrix* matrix)
{
    if (matrix == NULL) {
        return LED_MATRIX_ERR_PARAM;
    }
    if (matrix->driver == NULL) {
        return LED_MATRIX_ERR_PARAM;
    }
    if (matrix->driver->instance == NULL) {
        return LED_MATRIX_ERR_PARAM;
    }
    return LED_MATRIX_OK;
}

/* 内部工具：坐标范围检查（x 列、y 行） */
static int ledMatrixCheckCoord(uint32_t x, uint32_t y)
{
    if (x >= (uint32_t)BOARD_MATRIX_WIDTH) {
        return LED_MATRIX_ERR_PARAM;
    }
    if (y >= (uint32_t)BOARD_MATRIX_HEIGHT) {
        return LED_MATRIX_ERR_PARAM;
    }
    return LED_MATRIX_OK;
}

/* ------------------------------------------------------------------
 * 内部工具：取某一片的第 localRow 行，压成一个 8 位字节
 *   bit7 = 该块最左列（x 最小），bit0 = 该块最右列（逐列向低位排布）
 * 前置条件：chipIndex 在 0 到 BOARD_MATRIX_CHIP_COUNT - 1 之间、
 *           localRow 在 0 到 BOARD_MATRIX_BLOCK - 1 之间（由调用方保证）
 * ------------------------------------------------------------------ */
static uint8_t ledMatrixBuildRowByte(const LedMatrix* matrix,
                                    uint32_t chipIndex,
                                    uint32_t localRow)
{
    uint8_t  data    = 0u;
    uint32_t originY = (uint32_t)LED_MATRIX_CHIP_ORIGIN[chipIndex].originY;
    uint32_t originX = (uint32_t)LED_MATRIX_CHIP_ORIGIN[chipIndex].originX;
    uint32_t row     = originY + localRow;
    uint32_t col     = 0u;

    for (col = 0u; col < (uint32_t)BOARD_MATRIX_BLOCK; ++col) {
        /* 最左列落在最高位：col 0 -> bit7，col 7 -> bit0 */
        uint32_t bitPos = ((uint32_t)BOARD_MATRIX_BLOCK - 1u) - col;

        if (0u != matrix->pixels[row][originX + col]) {
            data = (uint8_t)(data | (uint8_t)(1u << bitPos));
        }
    }

    return data;
}

/* ------------------------------------------------------------------
 * 构造：不访问硬件，只分配 + 记录 driver + 帧缓冲清零（初始全灭）
 * ------------------------------------------------------------------ */
LedMatrix* NewLedMatrix(Max7219* driver)
{
    LedMatrix* self = NULL;

    /* driver 是外部注入的抽象父类，必须可用；本模块不负责创建它 */
    if (driver == NULL) {
        return NULL;
    }
    if (driver->instance == NULL) {
        return NULL;
    }
    if (ledMatrixMalloc == NULL) {
        return NULL;
    }

    self = (LedMatrix*)ledMatrixMalloc(sizeof(LedMatrix));
    if (self == NULL) {
        return NULL;
    }

    memset(self, 0, sizeof(LedMatrix));   /* pixels 全 0 即全灭 */
    self->driver = driver;                /* 只持有引用，不拥有 */

    return self;
}

/* ------------------------------------------------------------------
 * 析构：释放本模块对象并置空外部指针；不销毁 driver
 * ------------------------------------------------------------------ */
void DeleteLedMatrix(LedMatrix** pMatrix)
{
    LedMatrix* self = NULL;

    if (pMatrix == NULL) {
        return;
    }

    self = *pMatrix;
    if (self == NULL) {
        return;
    }

    /* 先断开引用，避免误用；driver 归调用方所有，此处绝不 DeleteMax7219 */
    self->driver = NULL;

    if (ledMatrixFree != NULL) {
        ledMatrixFree(self);
    }
    self = NULL;

    *pMatrix = NULL;   /* 外部指针置空，防悬空 */
}

/* ------------------------------------------------------------------
 * 帧缓冲操作：只改内存，不碰硬件
 * ------------------------------------------------------------------ */
int LedMatrixClear(LedMatrix* matrix)
{
    if (matrix == NULL) {
        return LED_MATRIX_ERR_PARAM;
    }

    memset(matrix->pixels, 0, sizeof(matrix->pixels));

    return LED_MATRIX_OK;
}

int LedMatrixFill(LedMatrix* matrix, int on)
{
    if (matrix == NULL) {
        return LED_MATRIX_ERR_PARAM;
    }

    memset(matrix->pixels, (0 != on) ? 1 : 0, sizeof(matrix->pixels));

    return LED_MATRIX_OK;
}

int LedMatrixSetPixel(LedMatrix* matrix, uint32_t x, uint32_t y, int on)
{
    if (matrix == NULL) {
        return LED_MATRIX_ERR_PARAM;
    }
    if (ledMatrixCheckCoord(x, y) != LED_MATRIX_OK) {
        return LED_MATRIX_ERR_PARAM;
    }

    /* on 归一化为 0 / 1，帧缓冲里只允许存 0 或 1 */
    matrix->pixels[y][x] = (uint8_t)((0 != on) ? 1u : 0u);

    return LED_MATRIX_OK;
}

int LedMatrixGetPixel(LedMatrix* matrix, uint32_t x, uint32_t y)
{
    if (matrix == NULL) {
        return LED_MATRIX_ERR_PARAM;
    }
    if (ledMatrixCheckCoord(x, y) != LED_MATRIX_OK) {
        return LED_MATRIX_ERR_PARAM;
    }

    return (0u != matrix->pixels[y][x]) ? 1 : 0;
}

/* ------------------------------------------------------------------
 * 组帧并下发：按行循环 BOARD_MATRIX_BLOCK 次，每次一帧
 *   第 row 帧内，第 pair 对字节对应 chipIndex = 总片数 - 1 - pair
 *   （先发最远端），寄存器地址 = 0x01 + row，数据 = 该片第 row 行位图
 * ------------------------------------------------------------------ */
int LedMatrixFlush(LedMatrix* matrix)
{
    uint8_t  frame[LED_MATRIX_FRAME_LEN];
    uint32_t row = 0u;

    if (ledMatrixCheckHandle(matrix) != LED_MATRIX_OK) {
        return LED_MATRIX_ERR_PARAM;
    }

    for (row = 0u; row < (uint32_t)BOARD_MATRIX_BLOCK; ++row) {
        uint32_t pair = 0u;
        int      ret  = LED_MATRIX_OK;

        for (pair = 0u; pair < (uint32_t)BOARD_MATRIX_CHIP_COUNT; ++pair) {
            /* 级联移位寄存器的固有顺序：先入者被推到链尾，故先发最远端 */
            uint32_t chipIndex = ((uint32_t)BOARD_MATRIX_CHIP_COUNT - 1u) - pair;
            uint32_t offset    = 2u * pair;

            frame[offset]      = (uint8_t)(LED_MATRIX_REG_ROW_BASE + row);
            frame[offset + 1u] = ledMatrixBuildRowByte(matrix, chipIndex, row);
        }

        ret = Max7219WriteFrame(matrix->driver, frame, (uint32_t)LED_MATRIX_FRAME_LEN);
        if (ret < 0) {
            return ret;   /* 底层错误原样上抛 -1 参数类 / -2 槽位缺失 */
        }
    }

    return LED_MATRIX_OK;
}
