/* ============================================================================
 * hal 抽象层单元测试入口
 *
 * 每个模块的用例文件导出一个 RunXxxTests()，内部用 RUN_TEST 串联用例；
 * 本文件只负责按固定顺序执行，不包含任何断言逻辑。
 * ========================================================================== */

#include "unity.h"

void RunGpioTests(void);
void RunUartTests(void);
void RunI2cTests(void);
void RunSpiTests(void);
void RunPwmTests(void);
void RunAdcTests(void);

void app_main(void)
{
    UNITY_BEGIN();

    RunGpioTests();
    RunUartTests();
    RunI2cTests();
    RunSpiTests();
    RunPwmTests();
    RunAdcTests();

    UNITY_END();
}
