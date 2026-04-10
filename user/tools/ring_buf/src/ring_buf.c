#include "ring_buf.h"

#include <string.h>

RingBuf NewRingBuf(int size)
{
    RingBuf ring={0};
    ring.buffer = (char*)pvPortMalloc(size);
    if (ring.buffer == NULL) {
        ring.size = 0;
        ring.head = 0;
        ring.tail = 0;
        ring.len = 0;
        return ring;
    }
    ring.size = size;
    ring.head = 0;
    ring.tail = 0;
    ring.len = 0;
    return ring;
}
RingBuf NewRingBufByOther(int size,char* buf){
    RingBuf ring={0};
    ring.buffer = buf;
    if (ring.buffer == NULL) {
        ring.size = 0;
        ring.head = 0;
        ring.tail = 0;
        ring.len = 0;
        return ring;
    }
    ring.size = size;
    ring.head = 0;
    ring.tail = 0;
    ring.len = 0;
    return ring;
}
uint8_t RingBufAddByte(RingBuf* ring, char byte)
{
    if (ring == NULL || ring->buffer == NULL) {
        return 1;
    }
    ring->buffer[ring->tail] = byte;
    ring->tail = (ring->tail + 1) % ring->size;
    if (ring->len < ring->size) {
        ring->len++;
    } else {
        ring->head = (ring->head + 1) % ring->size;
    }
    return 0;
}

uint8_t RingBufAddData(RingBuf* ring, char* data, int len)
{
    if (ring == NULL || ring->buffer == NULL || data == NULL || len <= 0) {
        return 0;
    }
    for (int i = 0; i < len; i++) {
        ring->buffer[ring->tail] = data[i];
        ring->tail = (ring->tail + 1) % ring->size;
        if (ring->len < ring->size) {
            ring->len++;
        } else {
            ring->head = (ring->head + 1) % ring->size;
        }
    }
    return 1;
}

char RingBufPop(RingBuf* ring)
{
    if (ring == NULL || ring->buffer == NULL || ring->len == 0) {
        return 0;
    }
    char byte = ring->buffer[ring->head];
    ring->head = (ring->head + 1) % ring->size;
    ring->len--;
    return byte;
}

uint32_t RingBufRead(RingBuf* ring, char* buf, int len)
{
    if (ring == NULL || ring->buffer == NULL || buf == NULL || len <= 0) {
        return 0;
    }
    int readLen = (len > ring->len) ? ring->len : len;
    for (int i = 0; i < readLen; i++) {
        buf[i] = ring->buffer[(ring->head + i) % ring->size];
    }
    ring->head = (ring->head + readLen) % ring->size;
    ring->len -= readLen;
    return readLen;
}
