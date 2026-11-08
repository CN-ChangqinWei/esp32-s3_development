#ifndef _ASSERT_MARCO_H
#define _ASSERT_MARCO_H

/* 本头会被 C++ 编译单元（Qt 具象层、demo）包含，故自带 extern "C" 包裹，
   保证 C++ 单元看到的是 C 名字修饰（避免链接期 undefined reference）。
   包裹内只放本项目 C 头与断言宏；log.h 置于包裹内，即使其自身漏加包裹
   也能获得 C 链接；若 log.h 已自带包裹，则该嵌套在 C++ 中同样合法。 */
#ifdef __cplusplus
extern "C" {
#endif

#include"log.h"


#define ASSERT_NOT_NULL(val,msg,...) do{\
    if(NULL==(val)){\
        log_error("assert: except not null but null; msg:%s",msg);\
        return __VA_ARGS__;\
    }\
}while(0)

#define ASSERT_NULL(val,msg,...) do{\
    if(NULL!=(val)){\
        log_error("assert: except  null but not null; msg:%s",msg);\
        return __VA_ARGS__;\
    }\
}while(0)

#define ASSERT_GREATER(val,flag,msg,...) do{\
    if(!((val)>flag)){\
        log_error("assert: except greater than flag; msg:%s",msg);\
        return __VA_ARGS__;\
    }\
}while(0)

#define ASSERT_LESS(val,flag,msg,...) do{\
    if(!((val)<flag)){\
        log_error("assert: except less than flag; msg:%s",msg);\
        return __VA_ARGS__;\
    }\
}while(0)

#define ASSERT_EQ(val,flag,msg,...) do{\
    if((val)!=flag){\
        log_error("assert: except equal to flag; msg:%s",msg);\
        return __VA_ARGS__;\
    }\
}while(0)

#define ASSERT_NOT_EQ(val,flag,msg,...) do{\
    if((val)==flag){\
        log_error("assert: except not equal to flag; msg:%s",msg);\
        return __VA_ARGS__;\
    }\
}while(0)

#ifdef __cplusplus
}
#endif

#endif