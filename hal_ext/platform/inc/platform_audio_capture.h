#ifndef _PLATFORM_AUDIO_CAPTURE_H
#define _PLATFORM_AUDIO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

/* 本头会被 C++ 编译单元（Qt 具象层 / demo）包含，故自带 extern "C" 包裹，
   且把本项目 C 头 platform.h 的包含置于包裹**之内**：这样无论本头与平台聚合头
   谁先被包含，platform.h 及其内部的全部 platform_*.h 都以 C 链接展开，
   消除「谁先包含」对语言链接的隐式耦合。platform.h 自身亦带包裹，
   此处出现的「extern "C" 嵌套」在 C++ 中是合法且幂等的。
   标准库头（<stddef.h> / <stdint.h>）留在包裹之外，与 platform_sync.h /
   platform_memory.h / platform_timestamp.h 一致。 */
#ifdef __cplusplus
extern "C" {
#endif

#include "platform.h"

/* =====================================================================
 * platform_audio_capture —— 跨平台「音频采集」能力门面（对象式）
 *
 * 职责：
 *   为「非 UI 层不得使用 Qt」提供纯 C 的音频采集能力：从「系统整体输出」
 *   （Windows: WASAPI loopback 环回）或「麦克风 / 录音设备」采集音频，
 *   在平台层内部完成转码，向调用方交付**统一格式**的字节流。
 *   调用方拿到的是「音频 PCM 字节流」，不感知任何平台 / COM API。
 *
 * ---------------------------------------------------------------------
 * 能力与平台差异（由 platform_audio_capture.c 的互斥分支选择，本头不感知平台）
 * ---------------------------------------------------------------------
 *   Windows : WASAPI 具象（IMMDeviceEnumerator + IAudioClient + IAudioCaptureClient，
 *             独立采集线程 + 环形缓冲；系统源走 loopback，麦克风走 eCapture）。
 *             需链接 ole32 / uuid / avrt。
 *   Linux / macOS / FreeRTOS : 暂无可移植实现，
 *             NewPlatformAudioCaptureNative() 返回 NULL（并 log_error，保证任何平台可链接）。
 *
 * 【平台隔离 · 重要】本头**绝不**包含 <windows.h> / <mmdeviceapi.h> / <audioclient.h>
 *   或任何平台 / COM 头。本头会被整个工程（含 Qt 的 C++ 编译单元）包含，一旦引入
 *   <windows.h> 一类平台头，其 COM 头里的 `#define interface struct` 与
 *   IN / OUT / TRUE / FALSE 等宏会污染整个工程，并可能与本头中形如 `interfaces`
 *   的字段名冲突，造成 C++ 编译期难以定位的错误。具体实现结构体只出现在
 *   platform_audio_capture.c 的文件作用域内（与 platform_screencapture.h /
 *   platform_network.h 的「实现结构体刻意不进头文件」约定一致）。
 *
 * ---------------------------------------------------------------------
 * 字节流格式契约（消费方务必遵守）
 * ---------------------------------------------------------------------
 *   - 平台层**保证**交付的字节流恒为 PlatformAudioCaptureFormat 所描述的格式，
 *     与源设备原生格式无关（转码在平台层内部完成）：
 *       sampleRate    : 目标采样率 Hz（> 0）
 *       channels      : 目标声道数（1 或 2），多声道交织存放
 *       bitsPerSample : 目标位深 16 或 32（PCM）——
 *                       16 = 有符号 16 位小端；32 = 有符号 32 位小端
 *   - 多字节采样值一律**小端**，多声道按「帧内相邻」顺序交织
 *     （帧 = channels 个采样值，帧与帧首尾相接，无填充 / 无对齐字节）。
 *   - minChunkBytes 为「最小可读交付字节数」：> 0 时可用字节数不足则 read() 返回 0
 *     （暂无可读，**不阻塞**）；<= 0 表示无最小限制（有多少给多少）。
 *
 * ---------------------------------------------------------------------
 * 缓冲所有权与读取语义（重要）
 * ---------------------------------------------------------------------
 *   - read() 为**拷贝语义**：把环形缓冲中的数据拷贝进调用方提供的 out 缓冲，
 *     **绝不**向调用方暴露内部回绕；调用方拿到的是线性、连续的一段字节。
 *   - read() 一次调用**只返回连续的一段**；若本次可用字节数不足 minChunkBytes，
 *     返回 0，**绝不阻塞等待**。
 *   - 环形缓冲由采集实例内部持有：容量在构造时按 config.ringBytes 一次性分配，
 *     运行期零 malloc；写入溢出时**覆盖最旧数据**（并去抖记一条 warn）。
 *
 * ---------------------------------------------------------------------
 * 可观测性契约（本轮新增：**不改变任何行为与返回值**，只把「静默失败」变可见）
 * ---------------------------------------------------------------------
 *   背景：链路上「采到 0 字节」与「采到静音」两种情况在设计上都走**合法返回码 0**
 *         （read() 返回 0 = 暂无可读，按契约**不是**错误），全链路没有任何一环
 *         会把它升级成日志，现场表现为「既无识别输出、也无一条 error」。
 *         故本模块内建以下观测点，**全部经节流**（实现内 AUDIOCAP_LOG_INTERVAL_MS /
 *         AUDIOCAP_IDLE_WARN_MS），控制流与返回值逐字未改：
 *   - 采集线程周期打「写入统计」：写入字节 / 包数 / 静音包数 / 环形缓冲水位；
 *   - 连续 AUDIOCAP_IDLE_WARN_MS 取不到任何包时打一条 warn，并附
 *     IAudioClient_GetCurrentState() 的端点状态，用于区分
 *     「系统真的没出声音」与「端点挂起 / 全是静音包」：
 *       0 = AUDIOCLIENT_STATE_UNINITIALIZED（未初始化）
 *       1 = AUDIOCLIENT_STATE_ACTIVE（运行中）
 *       2 = AUDIOCLIENT_STATE_INACTIVE（设备无活动 / 禁用）
 *       3 = AUDIOCLIENT_STATE_SUSPENDED（已挂起）
 *   - read() 因「可用字节数 < minChunkBytes」返回 0 时，**严格节流**地打一条 info
 *     （消费者高频调用，绝不刷屏），说明「交不出货」的原因：环形缓冲水位 / 运行态 /
 *     暂停态 / minChunkBytes 折合的毫秒数。
 *   - 转码链路上几处「本该产出数据却产出 0 字节」的分支补齐告警（见实现内
 *     audioCapConvert / audioCapHandlePacket），以便直接定位丢包位置。
 *
 * ---------------------------------------------------------------------
 * lastError 契约
 * ---------------------------------------------------------------------
 *   lastError() 返回**指向实例内部只读缓冲**的 NUL 结尾字符串；无错误时返回空串 ""，
 *   **绝不返回 NULL**；调用方只读、**不得 free、不得改写**。
 *   契约与 platform_websocket.h 的同名槽完全一致（便于上层统一处理）。
 *   ⚠️ deleteInstance 之后**绝不可**再调用任何接口（**含 lastError**）——
 *      具体实例已释放，调用即为未定义行为。
 * ===================================================================== */

/* 音频源类型 */
typedef enum PlatformAudioSourceType{
    PLATFORM_AUDIO_SOURCE_SYSTEM = 0,   /* 系统整体输出（Windows: WASAPI loopback 环回采集） */
    PLATFORM_AUDIO_SOURCE_MIC    = 1    /* 麦克风 / 录音设备 */
} PlatformAudioSourceType;

/* 采集输出格式：平台层保证交付的字节流**恒为**本格式（转码在平台层内部完成） */
typedef struct PlatformAudioCaptureFormat{
    int sampleRate;     /* 目标采样率 Hz，必须 > 0（例：16000） */
    int channels;       /* 目标声道数，必须为 1 或 2 */
    int bitsPerSample;  /* 目标位深，必须为 16 或 32（PCM）；16 = 有符号 16 位小端，多声道交织 */
    int minChunkBytes;  /* 最小可读交付字节数；<= 0 = 无最小限制（有多少给多少）；
                         * > 0 时可用字节数不足则 read() 返回 0（暂无可读，不阻塞） */
} PlatformAudioCaptureFormat;

/* 采集配置（全部信息外部注入；库内零默认值） */
typedef struct PlatformAudioCaptureConfig{
    PlatformAudioSourceType sourceType;  /* 必填 */
    const char*             deviceId;    /* 可选：NULL / 空串 = 使用该类型的默认设备；
                                          * 非空 = 设备 id 字符串（由 listDevices 给出） */
    PlatformAudioCaptureFormat format;   /* 必填：目标输出格式（平台层负责转码） */
    int ringBytes;                       /* 环形缓冲容量（字节），必须 > 0；由调用方按「秒数 x 格式」算出 */
    int startOnCreate;                   /* 1 = 构造后立即启动采集线程；0 = 由调用方自行 start() */
} PlatformAudioCaptureConfig;

/* 设备信息（供 UI 下拉选择；纯数据、平凡类型、可 memset） */
#define PLATFORM_AUDIO_DEVICE_ID_CAP    256
#define PLATFORM_AUDIO_DEVICE_NAME_CAP  256

typedef struct PlatformAudioDeviceInfo{
    char                    id[PLATFORM_AUDIO_DEVICE_ID_CAP];     /* 平台设备 id（稳定标识，用于回填配置） */
    char                    name[PLATFORM_AUDIO_DEVICE_NAME_CAP]; /* 面向用户的友好名 */
    PlatformAudioSourceType sourceType;
    int                     isDefault;                            /* 1 = 该类型的默认设备 */
} PlatformAudioDeviceInfo;

/* 【装配约定 · 工程硬约定】deleteInstance 必须保持为**最后一项**：
 *   所有具象实现均以「集中返回虚表」宏按**名字**（指定初始值）装配，故新增槽一律
 *   插在 deleteInstance **之前**，绝不追加到它后面。 */
typedef struct PlatformAudioCaptureInterface{
    /* 枚举某类型可用设备：写入 out[0,outCap)，返回写入条数；out 为 NULL 或 outCap <= 0 ⇒ <0。
     * 该函数**不依赖 start()**，可在未启动时调用（供配置页列出设备）。 */
    int  (*listDevices)(void* instance, PlatformAudioSourceType type,
                        PlatformAudioDeviceInfo* out, int outCap);
    /* 启动采集（幂等：已启动再调返回 0）。0 = 成功；<0 = 失败。 */
    int  (*start)(void* instance);
    /* 暂停采集（幂等）：采集线程继续存活但不再写入环形缓冲；**保留**缓冲内已有数据。0 / <0。 */
    int  (*pause)(void* instance);
    /* 恢复采集（幂等）。0 / <0。 */
    int  (*resume)(void* instance);
    /* 清空环形缓冲（丢弃全部未读数据；不影响采集状态）。0 / <0。 */
    int  (*clear)(void* instance);
    /* 停止采集并回收采集线程（幂等；**保留**缓冲内已采集数据）。0 / <0。 */
    int  (*stop)(void* instance);
    /* 当前环形缓冲中可读字节数；<0 = 入参非法。 */
    int  (*availableBytes)(void* instance);
    /* 运行期修改目标采样率（Hz，必须 > 0）。
     * 成功 0，并在内部**重建采集管线**（停流 → 按新格式重新初始化 → 恢复原有运行状态）；
     * <0 = 入参非法或平台不支持，此时**既有格式保持不变**。 */
    int  (*setSampleRate)(void* instance, int sampleRateHz);
    /* 取当前**实际生效**的格式（含实际采样率）。0 = 成功；<0 = 入参非法。 */
    int  (*getFormat)(void* instance, PlatformAudioCaptureFormat* outFormat);
    /* 读取至多 outCap 字节到 out（**拷贝语义**，绝不向调用方暴露环形回绕）。
     * 返回 >0 = 实际拷贝字节数；0 = 当前暂无可读（可用字节数 < minChunkBytes）；
     * <0 = 入参非法或采集出错。
     * 【一次调用只返回连续的一段；不足 minChunkBytes 时返回 0，绝不阻塞等待。】 */
    int  (*read)(void* instance, void* out, int outCap);
    /* 最近一次失败的可读原因：必须返回**指向实例内部只读缓冲**的 NUL 结尾字符串；
     * 无错误时返回空串 ""；**不得返回 NULL**；调用方只读、不得 free。
     * 契约与 platform_websocket.h 的同名槽一致。
     * ⚠️ deleteInstance 之后不得再调用本槽（实例已释放）。 */
    const char* (*lastError)(void* instance);
    void (*deleteInstance)(void* instance);   /* 停止采集、回收线程与缓冲、释放实例（幂等）。
                                                * 必须保持为虚表最后一项。 */
} PlatformAudioCaptureInterface;

typedef struct PlatformAudioCapture{
    void*                          instance;      /* 指向具体实现实例（多态核心） */
    PlatformAudioCaptureInterface  interfaces;    /* 虚表，字段名固定为 interfaces */
} PlatformAudioCapture;

PlatformAudioCapture* NewPlatformAudioCapture(void* instance, PlatformAudioCaptureInterface interfaces);
int                   InitPlatformAudioCapture(PlatformAudioCapture* capture, void* instance,
                                               PlatformAudioCaptureInterface interfaces);
void                  DeletePlatformAudioCapture(PlatformAudioCapture** pCapture);

#if PLATFORM_USE_AUDIO_CAPTURE
/* 平台中立具象工厂：Windows → WASAPI；其余平台返回 NULL（并 log_error，保证可链接）。 */
PlatformAudioCapture* NewPlatformAudioCaptureNative(const PlatformAudioCaptureConfig* config);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_AUDIO_CAPTURE_H */
