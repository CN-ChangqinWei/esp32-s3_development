#include <stdio.h>
#include "global.h"

void app_main(void)
{
    printf("hello world\n");
    
    while (1)
    {
        /* code */
        GlobalInit();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
}
