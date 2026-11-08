#include "freertos_extra.h"
#include <string.h>
#include <esp_heap_caps.h>

void *pvPortRealloc(void *pv, size_t xWantedSize)
{
    if (pv == NULL) {
        return pvPortMalloc(xWantedSize);
    }
    
    if (xWantedSize == 0) {
        vPortFree(pv);
        return NULL;
    }
    
    size_t xOldSize = heap_caps_get_allocated_size(pv);
    
    void *pvNew = pvPortMalloc(xWantedSize);
    if (pvNew == NULL) {
        return NULL;
    }
    
    size_t xCopySize = (xOldSize < xWantedSize) ? xOldSize : xWantedSize;
    memcpy(pvNew, pv, xCopySize);
    
    vPortFree(pv);
    
    return pvNew;
}
