#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* 断言失败时由应用层实现的处理函数，configASSERT 会调用它定位文件和行号。 */
extern void vAssertCalled( const char * pcFile, uint32_t ulLine );
extern void vLowPowerPreSuppressTicksAndSleep( void * pvExpectedIdleTime );
extern void vLowPowerSuppressTicksAndSleep( uint32_t ulExpectedIdleTicks );

/* 基础调度配置 ------------------------------------------------------------ */
/* CPU 主频，单位 Hz；这里表示模拟/目标平台按 100 MHz 配置。 */
#define configCPU_CLOCK_HZ                         ( ( unsigned long ) 100000000 )
/* 系统节拍频率，单位 Hz；1000 表示 1 ms 产生一次 tick。 */
#define configTICK_RATE_HZ                         ( ( TickType_t ) 1000 )
/* 使能抢占式调度；高优先级任务就绪后可以立即抢占低优先级任务。 */
#define configUSE_PREEMPTION                       1
/* 使能同优先级任务时间片轮转；同优先级任务会按 tick 轮流运行。 */
#define configUSE_TIME_SLICING                     1
/* 是否使用移植层优化的任务选择算法；0 表示使用通用 C 实现。 */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION    0
/* 是否启用 tickless idle 低功耗模式；1 表示空闲时间足够长时会尝试进入低功耗路径。 */
#define configUSE_TICKLESS_IDLE                    1
/* 进入 tickless idle 前期望至少空闲的 tick 数；当前关闭 tickless 时基本不起作用。 */
#define configEXPECTED_IDLE_TIME_BEFORE_SLEEP      2
/* 系统可用任务优先级数量，合法优先级为 0 到 configMAX_PRIORITIES - 1。 */
#define configMAX_PRIORITIES                       7
/* 空闲任务等内核任务使用的最小栈深度，单位是 StackType_t 个数而不是字节。 */
#define configMINIMAL_STACK_SIZE                   ( ( unsigned short ) 128 )
/* 任务名最大长度，包含字符串结尾的 '\0'。 */
#define configMAX_TASK_NAME_LEN                    16
/* TickType_t 的位宽；32 位 tick 在 1 ms tick 下约 49.7 天回绕一次。 */
#define configTICK_TYPE_WIDTH_IN_BITS              TICK_TYPE_WIDTH_32_BITS
/* 空闲任务是否主动让出 CPU 给同优先级的其它就绪任务。 */
#define configIDLE_SHOULD_YIELD                    1
/* 每个任务拥有的任务通知数组元素个数；1 表示只使用默认通知槽。 */
#define configTASK_NOTIFICATION_ARRAY_ENTRIES      1
/* 队列注册表大小，调试器可通过注册表给队列/信号量起可读名字。 */
#define configQUEUE_REGISTRY_SIZE                  8
/* 是否启用旧版 API/命名的兼容支持；0 表示关闭，便于发现过时代码。 */
#define configENABLE_BACKWARD_COMPATIBILITY        0
/* 每个任务的线程本地存储指针数量；0 表示不启用 TLS 指针。 */
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS    0
/* 使用较小的链表项结构，节省内存；通常保持为 1。 */
#define configUSE_MINI_LIST_ITEM                   1
/* 栈深度参数使用的类型；这里用 size_t，适合本工程的主机/仿真环境。 */
#define configSTACK_DEPTH_TYPE                     size_t
/* 消息缓冲区长度字段使用的类型；这里用 size_t 表示平台自然大小。 */
#define configMESSAGE_BUFFER_LENGTH_TYPE           size_t
/* 释放堆内存时是否清零，便于减少旧数据残留。 */
#define configHEAP_CLEAR_MEMORY_ON_FREE            1

/* 内存分配配置 ------------------------------------------------------------ */
/* 是否支持静态分配；0 表示不能用 xTaskCreateStatic 等静态创建接口。 */
#define configSUPPORT_STATIC_ALLOCATION            0
/* 是否支持动态分配；1 表示可以用 pvPortMalloc 以及动态创建对象接口。 */
#define configSUPPORT_DYNAMIC_ALLOCATION           1
/* FreeRTOS 堆大小，供 heap_x.c 管理；本 demo 任务较多，这里配置为 128 KB。 */
#define configTOTAL_HEAP_SIZE                      ( 128U * 1024U )
/* 是否由应用自己提供 ucHeap 数组；0 表示由 FreeRTOS 堆实现内部提供。 */
#define configAPPLICATION_ALLOCATED_HEAP           0
/* 任务栈是否从单独堆分配；0 表示任务控制块和栈都走 FreeRTOS 堆。 */
#define configSTACK_ALLOCATION_FROM_SEPARATE_HEAP  0
/* 是否启用堆保护机制；0 表示关闭额外保护检查。 */
#define configENABLE_HEAP_PROTECTOR                0

/* 同步和通信对象配置 ------------------------------------------------------ */
/* 是否启用互斥量；互斥量带优先级继承，适合保护共享资源。 */
#define configUSE_MUTEXES                          1
/* 是否启用递归互斥量；同一任务可多次获取，需对应次数释放。 */
#define configUSE_RECURSIVE_MUTEXES                1
/* 是否启用计数信号量；常用于资源计数或事件累计。 */
#define configUSE_COUNTING_SEMAPHORES              1
/* 是否启用队列集；可让一个任务同时等待多个队列/信号量。 */
#define configUSE_QUEUE_SETS                       1
/* 是否启用事件组；适合用多个 bit 表示多个事件状态。 */
#define configUSE_EVENT_GROUPS                     1
/* 是否启用流缓冲区和消息缓冲区；用于字节流或变长消息传递。 */
#define configUSE_STREAM_BUFFERS                   1

/* 软件定时器配置 ---------------------------------------------------------- */
/* 是否启用软件定时器功能。 */
#define configUSE_TIMERS                           1
/* 定时器服务任务优先级；这里设为系统最高优先级。 */
#define configTIMER_TASK_PRIORITY                  ( configMAX_PRIORITIES - 1 )
/* 定时器命令队列长度，保存启动/停止/复位等定时器命令。 */
#define configTIMER_QUEUE_LENGTH                   10
/* 定时器服务任务栈深度，单位同样是 StackType_t 个数。 */
#define configTIMER_TASK_STACK_DEPTH               ( configMINIMAL_STACK_SIZE * 3 )

/* Hook、错误检查和调试辅助 ------------------------------------------------ */
/* 是否启用空闲钩子函数 vApplicationIdleHook。 */
#define configUSE_IDLE_HOOK                        1
/* 是否启用 tick 钩子函数 vApplicationTickHook。 */
#define configUSE_TICK_HOOK                        0
/* 是否启用 malloc 失败钩子 vApplicationMallocFailedHook。 */
#define configUSE_MALLOC_FAILED_HOOK               1
/* 是否启用定时器/daemon 任务启动钩子。 */
#define configUSE_DAEMON_TASK_STARTUP_HOOK         0
/* 是否启用流/消息缓冲区发送完成回调。 */
#define configUSE_SB_COMPLETED_CALLBACK            0
/* 栈溢出检查级别；2 表示切换任务时进行较完整的栈边界检查。 */
#define configCHECK_FOR_STACK_OVERFLOW             2

/* 运行统计和跟踪配置 ------------------------------------------------------ */
/* 是否生成运行时间统计；本 Windows 学习 demo 使用 clock() 做模拟计数器。 */
#define configGENERATE_RUN_TIME_STATS              1
/* 是否启用跟踪相关字段和接口，例如任务列表、调试信息等。 */
#define configUSE_TRACE_FACILITY                   1
/* 是否启用 vTaskList/vTaskGetRunTimeStats 等格式化输出函数。 */
#define configUSE_STATS_FORMATTING_FUNCTIONS       1
/* 统计输出缓冲区最大长度限制。 */
#define configSTATS_BUFFER_MAX_LENGTH              0xFFFF
/* 是否记录任务栈高地址，便于调试器或跟踪工具分析栈范围。 */
#define configRECORD_STACK_HIGH_ADDRESS            1
/* 是否在 List_t/ListItem_t 中加入完整性检查字节，用于发现链表破坏。 */
#define configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES  1

extern uint32_t ulGetRunTimeCounterValue( void );
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE()           ulGetRunTimeCounterValue()
/*
 * Windows 模拟端口没有真实 MCU 睡眠模式。这里接入教学版 tickless hook：
 * - 进入 portSUPPRESS_TICKS_AND_SLEEP 前，先让应用决定是否允许低功耗；
 * - 真正“睡眠”函数只记录统计信息，不关闭 Windows 定时器或硬件时钟。
 */
#define configPRE_SUPPRESS_TICKS_AND_SLEEP_PROCESSING( xExpectedIdleTime ) \
    vLowPowerPreSuppressTicksAndSleep( &( xExpectedIdleTime ) )
#define portSUPPRESS_TICKS_AND_SLEEP( xExpectedIdleTime )                  \
    vLowPowerSuppressTicksAndSleep( ( uint32_t ) ( xExpectedIdleTime ) )

/* 协程配置 ---------------------------------------------------------------- */
/* 是否启用旧式 co-routine；现代 FreeRTOS 通常使用任务而不是协程。 */
#define configUSE_CO_ROUTINES                      0
/* 协程优先级数量；协程关闭时该值基本无影响。 */
#define configMAX_CO_ROUTINE_PRIORITIES            1

/* 中断优先级配置 ---------------------------------------------------------- */
/* 内核中断优先级；不同芯片/移植层含义不同，本工程配置为 0。 */
#define configKERNEL_INTERRUPT_PRIORITY            0
/* 允许调用 FreeRTOS FromISR API 的最高中断优先级门限。 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY       0
/* 某些移植层使用的 API 调用中断优先级门限，通常与上一个宏含义接近。 */
#define configMAX_API_CALL_INTERRUPT_PRIORITY      0

/* 可选 API 裁剪配置 -------------------------------------------------------- */
/* 是否编译 vTaskPrioritySet，用于修改任务优先级。 */
#define INCLUDE_vTaskPrioritySet                   1
/* 是否编译 uxTaskPriorityGet，用于读取任务优先级。 */
#define INCLUDE_uxTaskPriorityGet                  1
/* 是否编译 vTaskDelete，用于删除任务。 */
#define INCLUDE_vTaskDelete                        1
/* 是否编译 vTaskSuspend/vTaskResume，用于挂起和恢复任务。 */
#define INCLUDE_vTaskSuspend                       1
/* 是否编译 xTaskResumeFromISR，用于在中断中恢复任务。 */
#define INCLUDE_xResumeFromISR                     1
/* 是否编译 vTaskDelayUntil，用于周期性精确定时延时。 */
#define INCLUDE_vTaskDelayUntil                    1
/* 是否编译 vTaskDelay，用于相对 tick 延时。 */
#define INCLUDE_vTaskDelay                         1
/* 是否编译 xTaskGetSchedulerState，用于查询调度器状态。 */
#define INCLUDE_xTaskGetSchedulerState             1
/* 是否编译 xTaskGetCurrentTaskHandle，用于获取当前任务句柄。 */
#define INCLUDE_xTaskGetCurrentTaskHandle          1
/* 是否编译 uxTaskGetStackHighWaterMark，用于查看任务剩余栈水位。 */
#define INCLUDE_uxTaskGetStackHighWaterMark        1
/* 是否编译 xTaskGetIdleTaskHandle，用于获取空闲任务句柄。 */
#define INCLUDE_xTaskGetIdleTaskHandle             1
/* 是否编译 eTaskGetState，用于查询任务状态。 */
#define INCLUDE_eTaskGetState                      1
/* 是否编译 xTimerPendFunctionCall，用于把函数调用挂到定时器服务任务执行。 */
#define INCLUDE_xTimerPendFunctionCall             1
/* 是否编译 xTaskAbortDelay，用于打断任务的阻塞延时状态。 */
#define INCLUDE_xTaskAbortDelay                    1
/* 是否编译 xTaskGetHandle，用于按任务名查找任务句柄。 */
#define INCLUDE_xTaskGetHandle                     1
/* 是否编译 xTaskResumeFromISR，用于在中断中恢复任务。 */
#define INCLUDE_xTaskResumeFromISR                 1

/* 断言宏：表达式为假时调用 vAssertCalled，便于在调试时停住并定位问题。 */
#define configASSERT( x )                                      \
    do                                                         \
    {                                                          \
        if( ( x ) == 0 )                                       \
        {                                                      \
            vAssertCalled( __FILE__, ( uint32_t ) __LINE__ );   \
        }                                                      \
    } while( 0 )

#endif
