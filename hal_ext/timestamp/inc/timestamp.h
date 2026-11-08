#ifndef TIMESTAMP_H
#define TIMESTAMP_H



#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
/**
 * 获取当前毫秒级时间戳
 * @return 自 Unix 纪元（1970-01-01 00:00:00 UTC）以来的毫秒数
 */
uint64_t get_timestamp_ms(void);

/**
 * 获取当前微秒级时间戳
 * @return 自 Unix 纪元以来的微秒数
 */
uint64_t get_timestamp_us(void);

/**
 * 获取当前纳秒级时间戳
 * @return 自 Unix 纪元以来的纳秒数
 */
uint64_t get_timestamp_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* TIMESTAMP_H */