#include <stdio.h>
#include "global.h"

void app_main(void)
{
    printf("hello world\n");
    GlobalInit();
    while (1)
    {
        /* code */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
}
