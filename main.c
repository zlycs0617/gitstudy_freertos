#include <stdint.h>  /* 提供 uint32_t 等固定宽度整数类型，嵌入式代码常用。 */
#include <stdio.h>   /* 提供 puts、printf、snprintf、fflush 等控制台输出函数。 */
#include <conio.h>   /* Windows console: _kbhit/_getch for live demo commands. */

#include "FreeRTOS.h"     /* FreeRTOS 基础类型、宏和配置入口，例如 pdTRUE、pdMS_TO_TICKS。 */
#include "event_groups.h" /* 事件组 API：用多个 bit 表示多个事件是否发生。 */
#include "queue.h"        /* 队列 API：任务之间传递数据。 */
#include "semphr.h"       /* 信号量/互斥量 API：这里用互斥量保护控制台输出。 */
#include "task.h"         /* 任务 API：创建任务、延时、任务通知、启动调度器。 */
#include "timers.h"       /* 软件定时器 API：周期性触发心跳事件。 */

/*
 * FreeRTOS study demo
 * -------------------
 * 这个 demo 用 Windows 桌面版 FreeRTOS port 模拟一个小型“温湿度监控系统”，
 * 目的是在没有开发板的情况下学习 FreeRTOS 的常用内核对象和任务协作方式。
 *
 * 整体数据流：
 * 1. vSensorTask 周期性模拟采集温湿度数据。
 * 2. vSensorTask 通过队列 xSensorQueue 把数据发送给 vControlTask。
 * 3. vControlTask 消费数据，若温度过高，用任务通知提醒 vSupervisorTask。
 * 4. vHeartbeatTimer 每 2 秒产生一次心跳事件，用事件组通知 vSupervisorTask。
 * 5. vSupervisorTask 汇总观察传感器事件、心跳事件和高温通知。
 * 6. vStatsTask 周期性打印系统状态，辅助观察 FreeRTOS 运行情况。
 *
 * 本文件演示的 FreeRTOS 知识点：
 * - Task：多个任务并发运行，使用优先级和阻塞等待配合调度。
 * - Queue：任务间传递带数据的消息。
 * - Mutex：保护共享的控制台输出资源。
 * - Event Group：用 bit 位广播系统事件。
 * - Task Notification：轻量级的一对一任务提醒。
 * - Software Timer：周期性产生心跳，不占用一个普通业务任务。
 * - Hook：内存申请失败、任务栈溢出时进入错误处理。
 */

/* 事件组中的每一位都代表一个“系统事件”。
 * bit0：传感器任务已经产生了一条新数据。
 * bit1：软件定时器产生一次心跳。
 * bit2：控制任务请求传感器任务执行一次降温处理。
 */
#define EVENT_SENSOR_READY    ( 1UL << 0UL ) /* bit0：传感器任务产生了新数据。 */
#define EVENT_HEARTBEAT       ( 1UL << 1UL ) /* bit1：软件定时器产生了一次心跳。 */
#define EVENT_COOLING_REQUEST ( 1UL << 2UL ) /* bit2：控制任务请求降低下一次采样湿度。 */

#define LOG_GROUP_1           ( 1UL << 0UL )
#define LOG_GROUP_2           ( 1UL << 1UL )
#define LOG_GROUP_3           ( 1UL << 2UL )
#define LOG_GROUP_4           ( 1UL << 3UL )
#define LOG_GROUP_5           ( 1UL << 4UL )
/* 第 6 组专门给本次新增的任务通知 demo 使用，方便单独屏蔽输出。 */
#define LOG_GROUP_6           ( 1UL << 5UL )
/* 第 7 组用于低功耗/tickless idle 学习实验输出。 */
#define LOG_GROUP_7           ( 1UL << 6UL )
#define LOG_GROUP_ALL         ( LOG_GROUP_1 | LOG_GROUP_2 | LOG_GROUP_3 | LOG_GROUP_4 | LOG_GROUP_5 | LOG_GROUP_6 | LOG_GROUP_7 )

/* 计数信号量演示：最多可以累计多少次按键释放。 */
#define SEM_DEMO_COUNTING_MAX       5U
/* 队列集演示中的队列长度；创建队列集时容量要把这个长度算进去。 */
#define QUEUE_SET_DEMO_QUEUE_LENGTH 5U
/* xTaskNotifyWait() 演示用通知位：按 w/W 时设置这个 bit。 */
#define NOTIFY_WAIT_DEMO_BIT        ( 1UL << 0UL )

static TaskHandle_t xTaskApiWorkerTask;
static TaskHandle_t xTaskApiInspectorTask;

/* 传感器采样数据结构。
 * 队列 xSensorQueue 每次传递的就是一个 SensorSample_t 对象。
 */
typedef struct
{
    uint32_t sequence;    /* 样本序号：每产生一条数据递增一次，方便观察数据流。 */
    uint32_t temperature; /* 模拟温度值：本 demo 中单位可理解为摄氏度。 */
    uint32_t humidity;    /* 模拟湿度值：本 demo 中单位可理解为百分比。 */
} SensorSample_t;

/* 带参数任务使用的配置结构体。
 * xTaskCreate 只能传入一个 void * 参数，所以多个配置项通常打包进结构体。
 */
typedef struct
{
    const char * pcTaskLabel;       /* 参数任务打印时使用的标签。 */
    uint32_t warningTemperature;    /* 学习用阈值：温度达到该值可认为偏高。 */
    uint32_t coolingHumidityStep;   /* 学习用降湿步长：对应前面降温函数中的 5。 */
    TickType_t printPeriodTicks;    /* 参数任务的打印周期，单位是 tick。 */
} ParamTaskConfig_t;

/* FreeRTOS 资源句柄。
 * 句柄本质上是内核对象的引用，创建成功后由任务通过这些句柄访问队列、
 * 互斥量、事件组和软件定时器。
 */
static QueueHandle_t xSensorQueue;          /* 传感器数据队列：SensorTask 写入，ControlTask 读取。 */
static SemaphoreHandle_t xConsoleMutex;     /* 控制台互斥量：防止多个任务同时打印导致日志混乱。 */
static SemaphoreHandle_t xParamConfigMutex; /* 参数配置互斥量：保护可变任务参数，避免读写同时发生。 */
static EventGroupHandle_t xSystemEvents;    /* 系统事件组：保存 EVENT_SENSOR_READY、EVENT_HEARTBEAT 等事件位。 */
static TimerHandle_t xHeartbeatTimer;       /* 心跳软件定时器：周期性设置 EVENT_HEARTBEAT。 */
static TaskHandle_t xSupervisorTask;        /* 监督任务句柄：ControlTask 用它向监督任务发送任务通知。 */
static TaskHandle_t xSensorTask;            /* 传感器任务句柄：便于后续查询栈水位、挂起或恢复该任务。 */
static TaskHandle_t xControlTask;           /* 控制任务句柄：便于后续调试或直接控制该任务。 */
static TaskHandle_t xStatsTask;             /* 统计任务句柄：便于后续观察统计任务状态。 */
static TaskHandle_t xParamDemoTask;         /* 带参数演示任务句柄：便于观察参数任务状态。 */
static TaskHandle_t xParamUpdateTask;       /* 参数更新任务句柄：便于观察或控制参数更新任务。 */
static TaskHandle_t xSuspendWorkerTask;     /* 挂起/恢复演示中的工作任务。 */
static TaskHandle_t xSuspendDemoTask;       /* 自动调用 vTaskSuspend/vTaskResume 的演示任务。 */
static TaskHandle_t xConsoleCommandTask;    /* 键盘命令任务：运行时切换日志组输出。 */

static TaskHandle_t xRelativeDelayTask;        /* 相对延时演示任务句柄：用于观察 vTaskDelay()。 */
static TaskHandle_t xAbsoluteDelayTask;        /* 绝对延时演示任务句柄：用于观察 vTaskDelayUntil()。 */
static TaskHandle_t xQueueSetMonitorTask;      /* 队列集监视任务句柄：阻塞等待队列集中的任意成员就绪。 */
static SemaphoreHandle_t xBinaryDemoSemaphore; /* 二值信号量：按 b/B 释放，由队列集任务接收。 */
static SemaphoreHandle_t xCountingDemoSemaphore; /* 计数信号量：按 c/C 释放，可累计多个 token。 */
static QueueHandle_t xQueueSetDemoQueue;       /* 队列集中的队列成员：按 q/Q 发送 uint32_t 消息。 */
static QueueSetHandle_t xDemoQueueSet;         /* 队列集：同时包含二值信号量、计数信号量和队列。 */
static TaskHandle_t xNotifyTakeDemoTask;       /* 任务通知计数演示任务：按 n/N 后用 ulTaskNotifyTake(pdFALSE) 逐个消费。 */
static TaskHandle_t xNotifyWaitDemoTask;       /* 任务通知位等待演示任务：按 w/W 后用 xTaskNotifyWait() 等待通知位。 */
static TaskHandle_t xNotifyKeyValueDemoTask;   /* 任务通知值传输演示任务：按未占用的可打印按键后，用通知值传递按键 ASCII 码。 */
static TaskHandle_t xLowPowerMonitorTask;       /* 低功耗学习监视任务：周期打印 idle hook 和 tickless idle 统计信息。 */

static volatile uint32_t ulEnabledLogGroups = LOG_GROUP_ALL;
static volatile BaseType_t xManualSuspendWorker = pdFALSE;
static volatile BaseType_t xLowPowerDemoEnabled = pdFALSE; /* pdFALSE: 观察关闭状态；pdTRUE: 允许进入教学版 tickless 路径。 */
static volatile uint32_t ulIdleHookCount = 0;              /* vApplicationIdleHook() 被调用的累计次数。 */
static volatile uint32_t ulLowPowerPreventedCount = 0;     /* 低功耗关闭时，本来满足条件但被应用层阻止的次数。 */
static volatile uint32_t ulLowPowerAttemptCount = 0;       /* 低功耗开启后，进入 portSUPPRESS_TICKS_AND_SLEEP 路径的次数。 */
static volatile uint32_t ulLowPowerLastExpectedTicks = 0;  /* 最近一次预计可空闲的 tick 数。 */
static volatile uint32_t ulLowPowerTotalExpectedTicks = 0; /* 累计预计可空闲 tick 数，用来观察长期趋势。 */

/* 传给 vParamDemoTask 的参数必须在任务运行期间一直有效，因此这里使用静态存储期对象。 */
static ParamTaskConfig_t xParamDemoConfig =
{
    "param-demo",
    28U,
    5U,
    pdMS_TO_TICKS( 7000 )
};

/*
 * 日志模块
 * -------
 * 本 demo 中多个任务都会打印日志。printf/puts 属于共享资源，如果多个任务
 * 同时写控制台，输出可能交叉在一起。因此这里统一通过 prvPrintLine()
 * 输出，并用 xConsoleMutex 做互斥保护。
 */
static void prvPrintLine( const char * pcLine ) /* pcLine：要打印的一整行日志字符串。 */
{
    uint32_t ulLineGroup = 0U;

    /* 空指针防护：传入 NULL 时直接返回，避免 pcLine[0] 解引用导致崩溃。 */
    if( pcLine == NULL )
    {
        return;
    }

    if( ( pcLine[ 0 ] == '[' ) && ( pcLine[ 1 ] != '\0' ) && ( pcLine[ 2 ] == ']' ) )
    {
        switch( pcLine[ 1 ] )
        {
            case '1':
                ulLineGroup = LOG_GROUP_1;
                break;

            case '2':
                ulLineGroup = LOG_GROUP_2;
                break;

            case '3':
                ulLineGroup = LOG_GROUP_3;
                break;

            case '4':
                ulLineGroup = LOG_GROUP_4;
                break;

            case '5':
                ulLineGroup = LOG_GROUP_5;
                break;

            case '6':
                ulLineGroup = LOG_GROUP_6;
                break;

            case '7':
                ulLineGroup = LOG_GROUP_7;
                break;

            default:
                break;
        }
    }

    if( ( ulLineGroup != 0U ) && ( ( ulEnabledLogGroups & ulLineGroup ) == 0U ) )
    {
        return;
    }

    /* 互斥量尚未创建时不能调用 xSemaphoreTake，避免对 NULL 句柄操作。 */
    if( xConsoleMutex == NULL )
    {
        return;
    }

    /* 多个任务都会打印日志，用互斥量保护 stdout，避免多任务输出互相穿插。 */
    if( xSemaphoreTake( xConsoleMutex, portMAX_DELAY ) == pdTRUE )
    {
        puts( pcLine );               /* puts 会追加换行，适合打印一整行日志。 */
        fflush( stdout );             /* 立即刷新输出，避免调试时日志滞留在缓冲区。 */
        xSemaphoreGive( xConsoleMutex ); /* 打印完成后释放互斥量，允许其他任务输出。 */
    }
}

static void prvPrintBlock( const char * pcTitle, const char * pcBlock )
{
    char line[ 256 ];
    uint32_t ulLineGroup = 0U;
    const char * pcLineStart;
    const char * pcLineEnd;

    /* 空指针防护：标题或正文为 NULL 时不访问字符串内容。 */
    if( ( pcTitle == NULL ) || ( pcBlock == NULL ) )
    {
        return;
    }

    pcLineStart = pcBlock;

    if( ( pcTitle[ 0 ] == '[' ) && ( pcTitle[ 1 ] != '\0' ) && ( pcTitle[ 2 ] == ']' ) )
    {
        switch( pcTitle[ 1 ] )
        {
            case '1':
                ulLineGroup = LOG_GROUP_1;
                break;

            case '2':
                ulLineGroup = LOG_GROUP_2;
                break;

            case '3':
                ulLineGroup = LOG_GROUP_3;
                break;

            case '4':
                ulLineGroup = LOG_GROUP_4;
                break;

            case '5':
                ulLineGroup = LOG_GROUP_5;
                break;

            case '6':
                ulLineGroup = LOG_GROUP_6;
                break;

            case '7':
                ulLineGroup = LOG_GROUP_7;
                break;

            default:
                break;
        }
    }

    if( ( ulLineGroup != 0U ) && ( ( ulEnabledLogGroups & ulLineGroup ) == 0U ) )
    {
        return;
    }

    /* 互斥量尚未创建时不能调用 xSemaphoreTake，避免对 NULL 句柄操作。 */
    if( xConsoleMutex == NULL )
    {
        return;
    }

    if( xSemaphoreTake( xConsoleMutex, portMAX_DELAY ) == pdTRUE )
    {
        puts( pcTitle );

        while( *pcLineStart != '\0' )
        {
            pcLineEnd = pcLineStart;

            while( ( *pcLineEnd != '\0' ) && ( *pcLineEnd != '\n' ) && ( *pcLineEnd != '\r' ) )
            {
                pcLineEnd++;
            }

            if( pcLineEnd > pcLineStart )
            {
                snprintf( line,
                          sizeof( line ),
                          "[%c][table] %.*s",
                          pcTitle[ 1 ],
                          ( int ) ( pcLineEnd - pcLineStart ),
                          pcLineStart );
                puts( line );
            }

            while( ( *pcLineEnd == '\n' ) || ( *pcLineEnd == '\r' ) )
            {
                pcLineEnd++;
            }

            pcLineStart = pcLineEnd;
        }

        fflush( stdout );
        xSemaphoreGive( xConsoleMutex );
    }
}

static void prvPrintLogMenu( void )
{
    prvPrintLine( "[0][menu] keys: 1 stats/param, 2 sensor/control, 3 supervisor, 4 suspend-worker, 5 delay/sem/queue-set, 6 task-notify, 7 low-power, l low-power on/off, a all on/off, b binary-sem, c count-sem, q queue-set-msg, n notifyGive, w notifyWait-bit, = assert-test, other-key notify-value, h help" );
}

static void prvPrintLogMask( void )
{
    char line[ 192 ];

    snprintf( line,
              sizeof( line ),
              "[0][menu] output mask: [1]=%s [2]=%s [3]=%s [4]=%s [5]=%s [6]=%s [7]=%s",
              ( ( ulEnabledLogGroups & LOG_GROUP_1 ) != 0U ) ? "on" : "off",
              ( ( ulEnabledLogGroups & LOG_GROUP_2 ) != 0U ) ? "on" : "off",
              ( ( ulEnabledLogGroups & LOG_GROUP_3 ) != 0U ) ? "on" : "off",
              ( ( ulEnabledLogGroups & LOG_GROUP_4 ) != 0U ) ? "on" : "off",
              ( ( ulEnabledLogGroups & LOG_GROUP_5 ) != 0U ) ? "on" : "off",
              ( ( ulEnabledLogGroups & LOG_GROUP_6 ) != 0U ) ? "on" : "off",
              ( ( ulEnabledLogGroups & LOG_GROUP_7 ) != 0U ) ? "on" : "off" );
    prvPrintLine( line );
}

static void prvToggleLogGroup( uint32_t ulGroupMask )
{
    ulEnabledLogGroups ^= ulGroupMask;
    prvPrintLogMask();
}

static void prvPrintSample( const char * pcPrefix, const SensorSample_t * pxSample )
{
    char line[ 128 ]; /* 临时日志缓冲区：把结构体字段格式化成一行字符串。 */

    /* 空指针防护：前缀或样本指针无效时跳过格式化，避免 snprintf 解引用 NULL。 */
    if( ( pcPrefix == NULL ) || ( pxSample == NULL ) )
    {
        return;
    }

    snprintf( line,
              sizeof( line ),                              /* 防止写出 line 数组边界。 */
              "%s seq=%lu temperature=%lu humidity=%lu",   /* 日志格式：前缀 + 采样字段。 */
              pcPrefix,                                    /* pcPrefix 用来区分生产者/消费者日志。 */
              ( unsigned long ) pxSample->sequence,        /* 强转为 unsigned long，匹配 %lu。 */
              ( unsigned long ) pxSample->temperature,
              ( unsigned long ) pxSample->humidity );
    prvPrintLine( line );
}

static void prvCoolDownSample( SensorSample_t * pxSample )
{
    if( pxSample->humidity >= 5U )
    {
        pxSample->humidity -= 5U;
    }
    else
    {
        pxSample->humidity = 0U;
    }

    prvPrintSample( "[2][sensor] cooling applied", pxSample );
}

/*
 * 传感器任务
 * --------
 * 角色：生产者。
 *
 * 每 1 秒模拟生成一条温湿度数据，并把数据发送到 xSensorQueue。
 * 发送成功后，再置位 EVENT_SENSOR_READY，让监督任务知道“新数据事件”发生了。
 *
 * 这里同时演示了：
 * - vTaskDelay()：让任务主动阻塞一段时间，避免一直占用 CPU。
 * - xQueueSend()：把结构体数据复制到队列中。
 * - xEventGroupSetBits()：设置事件位，通知其他任务。
 */
static void vSensorTask( void * pvParameters )
{
    ( void ) pvParameters; /* 本任务没有使用创建任务时传入的参数，显式丢弃避免编译警告。 */

    static SensorSample_t sample = { 0 }; /* 静态局部变量：只初始化一次，函数反复执行时会保留上次的值。 */

    for( ;; ) /* FreeRTOS 任务通常是无限循环，任务函数不能直接返回。 */
    {
        /* 模拟传感器采样：每秒产生一条温湿度数据。 */
        sample.sequence++;                                 /* 每轮循环产生一个新的样本序号。 */
        sample.temperature = 25U + ( sample.sequence % 5U ); /* 生成 25~29 之间循环变化的温度。 */
        sample.humidity = 50U + ( sample.sequence % 10U );   /* 生成 50~59 之间循环变化的湿度。 */

        if( ( xEventGroupClearBits( xSystemEvents, EVENT_COOLING_REQUEST ) & EVENT_COOLING_REQUEST ) != 0U )
        {
            prvCoolDownSample( &sample );
        }

        /* 队列用于传递“带数据”的消息。这里把 sample 拷贝进队列。 */
        if( xQueueSend( xSensorQueue, &sample, pdMS_TO_TICKS( 100 ) ) == pdPASS )
        {
            /* 事件组适合广播“某件事发生了”。这里通知监督任务：新数据已产生。 */
            xEventGroupSetBits( xSystemEvents, EVENT_SENSOR_READY );
            prvPrintSample( "[2][sensor] produced", &sample );
        }
        else
        {
            prvPrintLine( "[2][sensor] queue full" );
        }

        vTaskDelay( pdMS_TO_TICKS( 3000 ) ); /* 延时 1000ms，让采样周期约等于 1 秒。 */
    }
}

/*
 * 控制任务
 * -------
 * 角色：消费者。
 *
 * 该任务一直阻塞等待 xSensorQueue 中的新数据。收到数据后进行简单业务判断：
 * 温度 >= 28 时认为需要告警，于是通过任务通知提醒 vSupervisorTask。
 *
 * 注意：
 * - 队列适合传递“具体数据”。
 * - 任务通知适合提醒“某个指定任务”，比二值信号量更轻量。
 */
static void vControlTask( void * pvParameters )
{
    ( void ) pvParameters; /* 本任务没有使用创建任务时传入的参数。 */

    SensorSample_t sample; /* 从队列中取出的传感器数据会放到这里。 */

    for( ;; )
    {
        /* portMAX_DELAY 表示一直阻塞，直到队列中有数据可取。 */
        if( xQueueReceive( xSensorQueue, &sample, portMAX_DELAY ) == pdPASS )
        {
            prvPrintSample( "[2][control] consumed", &sample );

            if( sample.temperature >= 28U ) /* 简化的业务规则：温度达到 28 就认为偏高。 */
            {
                prvPrintLine( "[2][control] high temperature, notify supervisor and request cooling" );
                xEventGroupSetBits( xSystemEvents, EVENT_COOLING_REQUEST );
                /* 任务句柄无效时跳过通知，避免 xTaskNotifyGive(NULL) 触发断言或异常。 */
                if( xSupervisorTask != NULL )
                {
                    xTaskNotifyGive( xSupervisorTask );
                }
            }
        }
    }
}

/*
 * 监督任务
 * -------
 * 角色：系统观察者/汇总者。
 *
 * 它同时处理两类信号：
 * 1. 事件组：观察传感器事件和心跳事件，属于“系统事件广播”。
 * 2. 任务通知：接收控制任务发来的高温告警，属于“一对一提醒”。
 *
 * 这个任务优先级最高，是为了让学习时更容易及时看到事件被处理。
 */
static void vSupervisorTask( void * pvParameters )
{
    ( void ) pvParameters; /* 本任务没有使用创建任务时传入的参数。 */

    EventBits_t events;      /* 保存 xEventGroupWaitBits 返回的事件位快照。 */
    uint32_t alertCount = 0; /* 统计收到过多少次高温任务通知。 */
    char line[ 128 ];        /* 打印 alertCount 时使用的临时日志缓冲区。 */

    for( ;; )
    {
        /* 同时等待“传感器事件”和“心跳事件”。
         * pdTRUE：返回前自动清除已经等到的事件位。
         * pdFALSE：任意一个事件位到达即可返回，不要求两个事件同时到达。
         */
        events = xEventGroupWaitBits( xSystemEvents,
                                      EVENT_SENSOR_READY | EVENT_HEARTBEAT,
                                      pdTRUE,
                                      pdFALSE,
                                      pdMS_TO_TICKS( 500 ) );

        if( ( events & EVENT_SENSOR_READY ) != 0U ) /* 判断本次返回是否包含“传感器就绪”事件。 */
        {
            prvPrintLine( "[3][supervisor] sensor event observed" );
        }

        if( ( events & EVENT_HEARTBEAT ) != 0U ) /* 判断本次返回是否包含“心跳”事件。 */
        {
            prvPrintLine( "[3][supervisor] heartbeat event observed" );
        }

        /* 非阻塞读取任务通知。若控制任务发过高温告警，这里会读到计数。 */
        if( ulTaskNotifyTake( pdTRUE, 0 ) > 0U )
        {
            alertCount++;
            snprintf( line,
                      sizeof( line ),
                      "[3][supervisor] notification count=%lu",
                      ( unsigned long ) alertCount );
            prvPrintLine( line );
        }
        UBaseType_t stackleft = uxTaskGetStackHighWaterMark( xSupervisorTask );

        /* 打印栈水位，单位为栈单元数量。 */
        snprintf( line,
                  sizeof( line ),
                  "[3][stack water] Current task stack high water mark: %lu",
                  ( unsigned long ) stackleft );
        prvPrintLine( line );

        /* 获取栈相关类型大小。 */
        snprintf( line,
                  sizeof( line ),
                  "[3][stack size test] sizeof(StackType_t):%zu sizeof(size_t):%zu sizeof(void*):%zu",
                  sizeof( StackType_t ),
                  sizeof( size_t ),
                  sizeof( void * ) );
        prvPrintLine( line );   
    }
}

/*
 * 统计任务
 * -------
 * 角色：运行状态观测。
 *
 * 每 3 秒打印一次：
 * - 当前 tick：观察系统节拍是否在前进。
 * - 队列中等待处理的消息数量：观察是否发生积压。
 * - 剩余堆空间：观察动态创建任务、队列、定时器后的内存情况。
 */
static void vStatsTask( void * pvParameters )
{
    ( void ) pvParameters; /* 本任务没有使用创建任务时传入的参数。 */

    char line[ 128 ]; /* 统计日志缓冲区。 */

    for( ;; )
    {
        /* 周期性打印系统状态，观察 tick、队列积压数量和剩余堆空间。 */
        snprintf( line,
                  sizeof( line ),
                  "[1][stats] tick=%lu queued=%lu free_heap=%lu",
                  ( unsigned long ) xTaskGetTickCount(),
                  ( unsigned long ) uxQueueMessagesWaiting( xSensorQueue ),
                  ( unsigned long ) xPortGetFreeHeapSize() );
        prvPrintLine( line );

        vTaskDelay( pdMS_TO_TICKS( 5000 ) );
    }
}

/*
 * 带参数任务
 * --------
 * 创建任务时，xTaskCreate 的第 4 个参数会原样传进任务函数的 pvParameters。
 * 任务函数内部需要把 void * 转回真实的参数类型，再读取里面的配置。
 */
static void vParamDemoTask( void * pvParameters )
{
    const ParamTaskConfig_t * pxConfig = ( const ParamTaskConfig_t * ) pvParameters;
    ParamTaskConfig_t configSnapshot;
    char line[ 160 ];

    configASSERT( pxConfig != NULL );
    configASSERT( xParamConfigMutex != NULL );

    for( ;; )
    {
        /* 参数结构体现在会被其他任务修改，所以读取时先加锁，再拷贝一份快照。 */
        if( xSemaphoreTake( xParamConfigMutex, portMAX_DELAY ) == pdTRUE )
        {
            configSnapshot = *pxConfig;
            xSemaphoreGive( xParamConfigMutex );

            snprintf( line,
                      sizeof( line ),
                      "[1][param] label=%s warn_temp=%lu humidity_step=%lu period_ticks=%lu",
                      ( configSnapshot.pcTaskLabel != NULL ) ? configSnapshot.pcTaskLabel : "(null)",
                      ( unsigned long ) configSnapshot.warningTemperature,
                      ( unsigned long ) configSnapshot.coolingHumidityStep,
                      ( unsigned long ) configSnapshot.printPeriodTicks );
            prvPrintLine( line );

            vTaskDelay( configSnapshot.printPeriodTicks );
        }
        else
        {
            /* 互斥量获取失败时让出 CPU，避免空转占满时间片。 */
            vTaskDelay( pdMS_TO_TICKS( 100 ) );
        }
    }
}

/*
 * 参数更新任务
 * ----------
 * 这个任务模拟“运行过程中参数发生变化”的场景。
 * 它和 vParamDemoTask 共享同一个参数结构体，因此写入时也要使用同一个互斥量保护。
 */
static void vParamUpdateTask( void * pvParameters )
{
    ParamTaskConfig_t * pxConfig = ( ParamTaskConfig_t * ) pvParameters;
    uint32_t updateCount = 0;
    char line[ 160 ];

    configASSERT( pxConfig != NULL );
    configASSERT( xParamConfigMutex != NULL );

    for( ;; )
    {
        vTaskDelay( pdMS_TO_TICKS( 15000 ) );
        updateCount++;

        if( xSemaphoreTake( xParamConfigMutex, portMAX_DELAY ) == pdTRUE )
        {
            if( ( updateCount & 1U ) == 0U )
            {
                pxConfig->warningTemperature = 28U;
                pxConfig->coolingHumidityStep = 5U;
                pxConfig->printPeriodTicks = pdMS_TO_TICKS( 7000 );
            }
            else
            {
                pxConfig->warningTemperature = 27U;
                pxConfig->coolingHumidityStep = 8U;
                pxConfig->printPeriodTicks = pdMS_TO_TICKS( 3000 );
            }

            snprintf( line,
                      sizeof( line ),
                      "[2][param-update] warn_temp=%lu humidity_step=%lu period_ticks=%lu",
                      ( unsigned long ) pxConfig->warningTemperature,
                      ( unsigned long ) pxConfig->coolingHumidityStep,
                      ( unsigned long ) pxConfig->printPeriodTicks );

            xSemaphoreGive( xParamConfigMutex );
            prvPrintLine( line );
        }
        else
        {
            /* 互斥量获取失败时让出 CPU，避免参数更新任务空转。 */
            vTaskDelay( pdMS_TO_TICKS( 100 ) );
        }
        
    }
}

static void vSuspendWorkerTask( void * pvParameters )
{
    ( void ) pvParameters;

    uint32_t loopCount = 0;
    char line[ 160 ];

    for( ;; )
    {
        loopCount++;
        snprintf( line,
                  sizeof( line ),
                  "[4][suspend-worker] running loop=%lu tick=%lu",
                  ( unsigned long ) loopCount,
                  ( unsigned long ) xTaskGetTickCount() );
        prvPrintLine( line );

        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

static void vSuspendDemoTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ;; )
    {
        vTaskDelay( pdMS_TO_TICKS( 6000 ) );

        if( xManualSuspendWorker == pdTRUE )
        {
            prvPrintLine( "[0][suspend-demo] manual suspend is active; auto demo is waiting" );
            continue;
        }

        prvPrintLine( "[0][suspend-demo] vTaskSuspend(suspend-worker): [4] output will stop for 8 seconds" );
        if( xSuspendWorkerTask != NULL )
        {
            vTaskSuspend( xSuspendWorkerTask );
        }

        vTaskDelay( pdMS_TO_TICKS( 8000 ) );

        if( xManualSuspendWorker == pdTRUE )
        {
            prvPrintLine( "[0][suspend-demo] manual suspend took over; auto resume skipped" );
        }
        else
        {
            prvPrintLine( "[0][suspend-demo] vTaskResume(suspend-worker): [4] output will continue" );
            if( xSuspendWorkerTask != NULL )
            {
                vTaskResume( xSuspendWorkerTask );
            }
        }
    }
}

static void vConsoleCommandTask( void * pvParameters )
{
    ( void ) pvParameters;

    int key;
    uint32_t queueSetMessage = 0;
    /* 按 w/W 时递增一次，仅用于表示“用户又触发了一次通知位演示”。 */
    uint32_t notifyWaitValue = 0;
    char line[ 180 ];

    prvPrintLogMenu();
    prvPrintLogMask();

    for( ;; )
    {
        /* _kbhit() 只检查当前是否有按键输入，不会阻塞任务；没有按键时继续往下延时让出 CPU。 */
        if( _kbhit() != 0 )
        {
            /* _getch() 读取一个按键字符，按键本身不会自动回显到控制台。 */
            key = _getch();

            switch( key )
            {
                /* 按 1：开关第 1 组日志，主要包含统计任务、参数任务、任务 API 演示输出。 */
                case '1':
                    prvToggleLogGroup( LOG_GROUP_1 );
                    break;

                /* 按 2：开关第 2 组日志，主要包含传感器任务和控制任务输出。 */
                case '2':
                    prvToggleLogGroup( LOG_GROUP_2 );
                    break;

                /* 按 3：开关第 3 组日志，主要包含 supervisor 监督任务输出。 */
                case '3':
                    prvToggleLogGroup( LOG_GROUP_3 );
                    break;

                /* 按 4：开关第 4 组日志，主要包含任务挂起/恢复 demo 输出。 */
                case '4':
                    prvToggleLogGroup( LOG_GROUP_4 );
                    break;

                /* 按 5：开关第 5 组日志，主要包含延时、信号量、队列集 demo 输出。 */
                case '5':
                    prvToggleLogGroup( LOG_GROUP_5 );
                    break;

                /* 按 6：开关第 6 组日志，主要包含任务通知 demo 输出。 */
                case '6':
                    /* 单独开关任务通知 demo 的日志输出。 */
                    prvToggleLogGroup( LOG_GROUP_6 );
                    break;

                /* 按 7：开关第 7 组日志，主要包含低功耗/tickless idle 学习实验输出。 */
                case '7':
                    prvToggleLogGroup( LOG_GROUP_7 );
                    break;

                /* 按 a/A：所有日志组在“全部打开”和“全部关闭”之间切换。 */
                case 'a':
                case 'A':
                    if( ( ulEnabledLogGroups & LOG_GROUP_ALL ) == LOG_GROUP_ALL )
                    {
                        ulEnabledLogGroups = 0U;
                    }
                    else
                    {
                        ulEnabledLogGroups = LOG_GROUP_ALL;
                    }
                    prvPrintLogMask();
                    break;

                /* 按 b/B：释放一次二值信号量，队列集监控任务会收到并 take 掉它。 */
                case 'b':
                case 'B':
                    if( xSemaphoreGive( xBinaryDemoSemaphore ) == pdTRUE )
                    {
                        prvPrintLine( "[5][sem-demo] gave binary semaphore: one token is now available" );
                    }
                    else
                    {
                        prvPrintLine( "[5][sem-demo] binary semaphore already full; extra key press is not accumulated" );
                    }
                    break;

                /* 按 c/C：释放一次计数信号量，最多累计到 SEM_DEMO_COUNTING_MAX。 */
                case 'c':
                case 'C':
                    if( xSemaphoreGive( xCountingDemoSemaphore ) == pdTRUE )
                    {
                        prvPrintLine( "[5][sem-demo] gave counting semaphore: count increased by one" );
                    }
                    else
                    {
                        prvPrintLine( "[5][sem-demo] counting semaphore is full; max count reached" );
                    }
                    break;

                /* 按 q/Q：向队列集里的队列发送一个递增数字消息。 */
                case 'q':
                case 'Q':
                    queueSetMessage++;
                    if( xQueueSend( xQueueSetDemoQueue, &queueSetMessage, 0 ) == pdTRUE )
                    {
                        prvPrintLine( "[5][queue-set-demo] sent one message to the queue-set queue" );
                    }
                    else
                    {
                        prvPrintLine( "[5][queue-set-demo] queue is full; message was dropped" );
                    }
                    break;

                /* 按 l/L：开启或关闭“允许进入 tickless idle 低功耗路径”的学习开关。 */
                case 'l':
                case 'L':
                    xLowPowerDemoEnabled = ( xLowPowerDemoEnabled == pdFALSE ) ? pdTRUE : pdFALSE;
                    snprintf( line,
                              sizeof( line ),
                              "[7][low-power] runtime low-power demo is now %s",
                              ( xLowPowerDemoEnabled == pdTRUE ) ? "on" : "off" );
                    prvPrintLine( line );
                    break;

                /* 按 n/N：调用 xTaskNotifyGive()，给 notify_take 任务的通知计数加 1。 */
                case 'n':
                case 'N':
                    /*
                     * xTaskNotifyGive() 会把目标任务的通知值加 1。
                     * notify_take 任务用 ulTaskNotifyTake(pdFALSE) 接收，
                     * 因此连续按 n/N 可以观察通知值累积后被逐个减 1。
                     */
                    if( xNotifyTakeDemoTask != NULL )
                    {
                        xTaskNotifyGive( xNotifyTakeDemoTask );
                        prvPrintLine( "[6][notify-give] xTaskNotifyGive() sent one count to notify_take task" );
                    }
                    else
                    {
                        prvPrintLine( "[6][notify-give] notify_take task handle is NULL; notification skipped" );
                    }
                    break;

                /* 按 w/W：调用 xTaskNotify(..., eSetBits)，给 notify_wait 任务设置一个通知 bit。 */
                case 'w':
                case 'W':
                    /*
                     * xTaskNotify(..., eSetBits) 把通知值当作 bit 位集合使用。
                     * notify_wait 任务用 xTaskNotifyWait() 等待并读取这个 bit。
                     */
                    notifyWaitValue++;
                    if( xNotifyWaitDemoTask != NULL )
                    {
                        configASSERT( xTaskNotify( xNotifyWaitDemoTask,
                                                    NOTIFY_WAIT_DEMO_BIT,
                                                    eSetBits ) == pdPASS );
                        prvPrintLine( "[6][notify-wait] xTaskNotify(..., eSetBits) set NOTIFY_WAIT_DEMO_BIT for notify_wait task" );
                    }
                    else
                    {
                        prvPrintLine( "[6][notify-wait] notify_wait task handle is NULL; notification skipped" );
                    }
                    ( void ) notifyWaitValue;
                    break;

                /* 按 s/S：手动挂起 suspend-worker 任务，并暂时禁止自动恢复逻辑接管。 */
                case 's':
                case 'S':
                    xManualSuspendWorker = pdTRUE;
                    prvPrintLine( "[0][menu] manual vTaskSuspend(suspend-worker); auto resume is locked out" );
                    if( xSuspendWorkerTask != NULL )
                    {
                        vTaskSuspend( xSuspendWorkerTask );
                    }
                    break;

                /* 按 r/R：手动恢复 suspend-worker 任务，并重新允许自动挂起/恢复 demo 运行。 */
                case 'r':
                case 'R':
                    xManualSuspendWorker = pdFALSE;
                    prvPrintLine( "[0][menu] manual vTaskResume(suspend-worker); auto demo is enabled again" );
                    if( xSuspendWorkerTask != NULL )
                    {
                        vTaskResume( xSuspendWorkerTask );
                    }
                    break;

                /* 按 =：故意触发一次 configASSERT，用来学习断言报错时怎么看文件名和行号。 */
                case '=':
                    prvPrintLine( "[0][assert-demo] press =, about to call configASSERT(pdFALSE)" );
                    configASSERT( pdFALSE );
                    break;

                /* 按 h/H：重新打印按键菜单，方便运行时查看可用命令。 */
                case 'h':
                case 'H':
                    prvPrintLogMenu();
                    break;

                /* 其他可打印按键：把按键 ASCII 码作为任务通知值发送给 notify_key 任务。 */
                default:
                    /*
                     * 这里把“没有被菜单命令占用的可打印按键”作为任务通知值发送。
                     * eSetValueWithOverwrite 表示直接把目标任务的通知值改成按键 ASCII 码。
                     * 如果连续快速按多个未知键，而接收任务还没来得及读取，只保留最后一个值。
                     */
                    if( ( key >= 32 ) && ( key <= 126 ) )
                    {
                        if( xNotifyKeyValueDemoTask != NULL )
                        {
                            configASSERT( xTaskNotify( xNotifyKeyValueDemoTask,
                                                        ( uint32_t ) ( uint8_t ) key,
                                                        eSetValueWithOverwrite ) == pdPASS );
                            snprintf( line,
                                      sizeof( line ),
                                      "[6][notify-key-send] xTaskNotify(..., eSetValueWithOverwrite) key='%c' ascii=%lu",
                                      ( char ) key,
                                      ( unsigned long ) ( uint8_t ) key );
                            prvPrintLine( line );
                        }
                        else
                        {
                            prvPrintLine( "[6][notify-key-send] notify_key task handle is NULL; notification skipped" );
                        }
                    }
                    break;
            }
        }

        vTaskDelay( pdMS_TO_TICKS( 100 ) );
    }
}

/*
 * 软件定时器回调
 * -------------
 * 角色：周期性心跳源。
 *
 * 这个回调由 FreeRTOS 的 timer service task 调用，不是中断服务函数。
 * 本 demo 中它只做一件事：设置 EVENT_HEARTBEAT 事件位。
 */
static void vHeartbeatTimerCallback( TimerHandle_t xTimer )
{
    ( void ) xTimer; /* 本 demo 不需要通过 xTimer 读取定时器 ID 或修改定时器。 */

    /* 软件定时器回调运行在 FreeRTOS 的 timer service task 中。
     * 回调里应尽量短小，不要执行长时间阻塞操作。
     */
    xEventGroupSetBits( xSystemEvents, EVENT_HEARTBEAT );
}

/* Start 任务 */
/*
 * 任务 API 演示辅助函数
 * --------------------
 * eTaskGetState()/vTaskGetInfo()/uxTaskGetSystemState() 返回的是 eTaskState 枚举值。
 * 日志里直接打印数字不直观，所以这里把枚举值转换成人能看懂的字符串。
 */
/*
 * 相对延时演示任务
 * ----------------
 * vTaskDelay() 是“相对延时”：从调用这一刻开始，阻塞指定 tick 数。
 * 如果任务前面做了不同长度的工作，下一次唤醒时间也会跟着漂移。
 */
static void vRelativeDelayDemoTask( void * pvParameters )
{
    uint32_t loopCount = 0; /* 记录相对延时任务已经执行了多少轮，便于对照日志。 */
    char line[ 160 ];       /* 临时日志缓冲区。 */

    ( void ) pvParameters;

    for( ;; )
    {
        loopCount++;
        /* 打印调用 vTaskDelay() 之前的 tick，作为相对延时的起点。 */
        snprintf( line,
                  sizeof( line ),
                  "[5][delay-relative] loop=%lu before vTaskDelay tick=%lu",
                  ( unsigned long ) loopCount,
                  ( unsigned long ) xTaskGetTickCount() );
        prvPrintLine( line );

        /* 从“现在”开始阻塞 1500ms；如果前面代码耗时变化，下一次周期也会跟着变化。 */
        vTaskDelay( pdMS_TO_TICKS( 1500 ) );

        /* 打印醒来后的 tick，观察 after-before 约等于 1500ms。 */
        snprintf( line,
                  sizeof( line ),
                  "[5][delay-relative] loop=%lu after  vTaskDelay tick=%lu",
                  ( unsigned long ) loopCount,
                  ( unsigned long ) xTaskGetTickCount() );
        prvPrintLine( line );
    }
}

/*
 * 绝对延时演示任务
 * ----------------
 * vTaskDelayUntil() 是“绝对延时/周期延时”：按固定节拍唤醒任务。
 * xLastWakeTime 会被 FreeRTOS 更新为下一次周期基准，适合做稳定周期任务。
 */
static void vAbsoluteDelayDemoTask( void * pvParameters )
{
    const TickType_t xPeriod = pdMS_TO_TICKS( 2000 ); /* 固定周期：每 2000ms 唤醒一次。 */
    TickType_t xLastWakeTime = xTaskGetTickCount();   /* vTaskDelayUntil() 使用的周期基准。 */
    uint32_t loopCount = 0;                           /* 记录绝对延时任务执行轮次。 */
    char line[ 160 ];                                 /* 临时日志缓冲区。 */

    ( void ) pvParameters;

    for( ;; )
    {
        loopCount++;
        /* 打印本轮使用的周期基准，观察它按固定步长递增。 */
        snprintf( line,
                  sizeof( line ),
                  "[5][delay-absolute] loop=%lu wait-from=%lu period=%lu",
                  ( unsigned long ) loopCount,
                  ( unsigned long ) xLastWakeTime,
                  ( unsigned long ) xPeriod );
        prvPrintLine( line );

        /*
         * 阻塞到 xLastWakeTime + xPeriod。
         * 返回时 FreeRTOS 会自动更新 xLastWakeTime，下一轮继续基于固定节拍延时。
         */
        vTaskDelayUntil( &xLastWakeTime, xPeriod );

        /* 打印实际醒来 tick 和下一轮基准，观察周期稳定性。 */
        snprintf( line,
                  sizeof( line ),
                  "[5][delay-absolute] loop=%lu woke tick=%lu next-base=%lu",
                  ( unsigned long ) loopCount,
                  ( unsigned long ) xTaskGetTickCount(),
                  ( unsigned long ) xLastWakeTime );
        prvPrintLine( line );
    }
}

/*
 * 队列集演示任务
 * --------------
 * 一个队列集可以同时等待多个“队列类对象”：
 * - 队列；
 * - 二值信号量；
 * - 计数信号量。
 *
 * 本 demo 中按键任务负责投递事件：
 * - b/B: give 二值信号量；
 * - c/C: give 计数信号量；
 * - q/Q: 向队列发送一个整数消息。
 *
 * 本任务只阻塞在 xQueueSelectFromSet() 上，谁先就绪就处理谁。
 */
static void vQueueSetMonitorTask( void * pvParameters )
{
    QueueSetMemberHandle_t xActivatedMember; /* xQueueSelectFromSet() 返回的已就绪成员。 */
    uint32_t queueMessage;                   /* 从队列集中队列成员接收出来的消息值。 */
    uint32_t binaryCount = 0;                /* 统计二值信号量被处理的次数。 */
    uint32_t countingCount = 0;              /* 统计计数信号量被处理的次数。 */
    uint32_t queueCount = 0;                 /* 统计队列消息被处理的次数。 */
    char line[ 180 ];                        /* 临时日志缓冲区。 */

    ( void ) pvParameters;

    for( ;; )
    {
        /*
         * 阻塞等待队列集中的任意成员就绪。
         * 返回值不是数据本身，而是“哪个成员就绪了”的句柄。
         */
        xActivatedMember = xQueueSelectFromSet( xDemoQueueSet, portMAX_DELAY );

        if( xActivatedMember == ( QueueSetMemberHandle_t ) xBinaryDemoSemaphore )
        {
            /* 队列集只告诉我们“二值信号量可取”，真正消费还要调用 xSemaphoreTake()。 */
            if( xSemaphoreTake( xBinaryDemoSemaphore, 0 ) == pdTRUE )
            {
                binaryCount++;
                snprintf( line,
                          sizeof( line ),
                          "[5][queue-set] binary semaphore taken count=%lu tick=%lu",
                          ( unsigned long ) binaryCount,
                          ( unsigned long ) xTaskGetTickCount() );
                prvPrintLine( line );
            }
        }
        else if( xActivatedMember == ( QueueSetMemberHandle_t ) xCountingDemoSemaphore )
        {
            /* 计数信号量可能累计多个 token，本任务每次就绪处理一个 token。 */
            if( xSemaphoreTake( xCountingDemoSemaphore, 0 ) == pdTRUE )
            {
                countingCount++;
                snprintf( line,
                          sizeof( line ),
                          "[5][queue-set] counting semaphore taken total=%lu tick=%lu",
                          ( unsigned long ) countingCount,
                          ( unsigned long ) xTaskGetTickCount() );
                prvPrintLine( line );
            }
        }
        else if( xActivatedMember == ( QueueSetMemberHandle_t ) xQueueSetDemoQueue )
        {
            /* 队列成员就绪后，需要再用 xQueueReceive() 取出真正的消息内容。 */
            if( xQueueReceive( xQueueSetDemoQueue, &queueMessage, 0 ) == pdTRUE )
            {
                queueCount++;
                snprintf( line,
                          sizeof( line ),
                          "[5][queue-set] queue message=%lu received_count=%lu tick=%lu",
                          ( unsigned long ) queueMessage,
                          ( unsigned long ) queueCount,
                          ( unsigned long ) xTaskGetTickCount() );
                prvPrintLine( line );
            }
        }
        else
        {
            prvPrintLine( "[5][queue-set] unknown member selected" );
        }
    }
}

/*
 * 任务通知计数演示任务
 * --------------------
 * 按 n/N 时，键盘任务调用 xTaskNotifyGive()。
 * xTaskNotifyGive() 会让本任务的通知值 +1，效果类似一个轻量级计数信号量。
 *
 * 这里故意使用 ulTaskNotifyTake(pdFALSE, ...)，pdFALSE 表示：
 * - 退出等待时不把通知值直接清零；
 * - 而是只把通知值减 1。
 *
 * 所以你快速按多次 n/N 时，通知值会累积，本任务每次醒来只消费一个。
 */
static void vNotifyTakeDemoTask( void * pvParameters )
{
    uint32_t takeCount = 0;             /* 统计本任务已经成功取走多少次通知。 */
    uint32_t notifyValueBeforeTake;     /* ulTaskNotifyTake() 返回的取走前通知值。 */
    uint32_t notifyValueAfterTake;      /* 使用 pdFALSE 时，理论上取走后通知值会减少 1。 */
    char line[ 180 ];                   /* 临时日志缓冲区。 */

    ( void ) pvParameters;

    for( ;; )
    {
        /*
         * pdFALSE 是这个 demo 的重点：
         * 如果通知值大于 0，退出时只减 1，不把通知值全部清零。
         */
        notifyValueBeforeTake = ulTaskNotifyTake( pdFALSE, portMAX_DELAY );
        notifyValueAfterTake = ( notifyValueBeforeTake > 0U ) ? ( notifyValueBeforeTake - 1U ) : 0U;
        takeCount++;

        snprintf( line,
                  sizeof( line ),
                  "[6][notify-take] ulTaskNotifyTake(pdFALSE) take_count=%lu value_before=%lu value_after_minus_1=%lu",
                  ( unsigned long ) takeCount,
                  ( unsigned long ) notifyValueBeforeTake,
                  ( unsigned long ) notifyValueAfterTake );
        prvPrintLine( line );

        /*
         * 故意慢一点处理，方便你连续按 n/N 让通知值累积，
         * 从日志里看到 value_before 大于 1，然后每次只减 1。
         */
        vTaskDelay( pdMS_TO_TICKS( 700 ) );
    }
}

/*
 * 任务通知位等待演示任务
 * ----------------------
 * 按 w/W 时，键盘任务使用 xTaskNotify(..., eSetBits) 设置 NOTIFY_WAIT_DEMO_BIT。
 * 本任务使用 xTaskNotifyWait() 等待通知值发生变化，并读取通知值中的 bit。
 *
 * xTaskNotifyWait() 更适合“事件位/状态位”式通知；
 * ulTaskNotifyTake() 更适合“计数”式通知。
 */
static void vNotifyWaitDemoTask( void * pvParameters )
{
    uint32_t waitCount = 0;     /* 统计 xTaskNotifyWait() 成功返回次数。 */
    uint32_t notifiedValue = 0; /* 保存 xTaskNotifyWait() 读到的通知值快照。 */
    char line[ 180 ];           /* 临时日志缓冲区。 */

    ( void ) pvParameters;

    for( ;; )
    {
        /*
         * 第一个参数 0UL：进入等待前不清除任何 bit。
         * 第二个参数 UINT32_MAX：退出等待时清除所有 bit，避免同一事件反复触发。
         * 第三个参数 &notifiedValue：接收本次通知值快照。
         */
        if( xTaskNotifyWait( 0UL,
                             UINT32_MAX,
                             &notifiedValue,
                             portMAX_DELAY ) == pdTRUE )
        {
            waitCount++;

            snprintf( line,
                      sizeof( line ),
                      "[6][notify-wait] xTaskNotifyWait() wait_count=%lu notified_value=0x%08lx bit0=%s",
                      ( unsigned long ) waitCount,
                      ( unsigned long ) notifiedValue,
                      ( ( notifiedValue & NOTIFY_WAIT_DEMO_BIT ) != 0U ) ? "set" : "clear" );
            prvPrintLine( line );
        }
    }
}

/*
 * 任务通知值传输演示任务
 * --------------------
 * 键盘任务收到“没有被菜单命令占用的可打印按键”后，会调用：
 * xTaskNotify(xNotifyKeyValueDemoTask, 按键ASCII码, eSetValueWithOverwrite)。
 *
 * 本任务用 xTaskNotifyWait() 阻塞等待通知，并把通知值当作普通 uint32_t 数据读取。
 * 这个 demo 用来体现：任务通知不只可以当信号量/事件位，也可以传递一个 32 位数值。
 */
static void vNotifyKeyValueDemoTask( void * pvParameters )
{
    uint32_t receiveCount = 0; /* 统计本任务已经收到多少次按键通知值。 */
    uint32_t notifiedValue = 0; /* 保存 xTaskNotifyWait() 读到的通知值，也就是按键 ASCII 码。 */
    char line[ 180 ];           /* 临时日志缓冲区。 */

    ( void ) pvParameters;

    for( ;; )
    {
        /*
         * 第 1 个参数 0UL：进入等待前不清除任何 bit。
         * 第 2 个参数 UINT32_MAX：退出等待时清除整个通知值，避免旧按键值残留。
         * 第 3 个参数 &notifiedValue：读取发送方写入的 32 位通知值。
         */
        if( xTaskNotifyWait( 0UL,
                             UINT32_MAX,
                             &notifiedValue,
                             portMAX_DELAY ) == pdTRUE )
        {
            receiveCount++;

            snprintf( line,
                      sizeof( line ),
                      "[6][notify-key-recv] xTaskNotifyWait() recv_count=%lu notify_value=0x%08lx ascii=%lu key='%c'",
                      ( unsigned long ) receiveCount,
                      ( unsigned long ) notifiedValue,
                      ( unsigned long ) notifiedValue,
                      ( char ) notifiedValue );
            prvPrintLine( line );
        }
    }
}

/*
 * 低功耗学习监视任务
 * ----------------
 * 这个任务周期性打印两个层面的信息：
 * 1. idle_hook_delta：空闲任务的 hook 被调用了多少次，说明系统确实有空闲时间。
 * 2. sleep_attempt_delta：tickless idle 路径被允许进入了多少次，说明 FreeRTOS 认为可以跳过一段 tick。
 *
 * Windows 模拟端口不会真的让 MCU 进入睡眠，所以这里观察的是“调度器低功耗路径是否被触发”，
 * 不是真实硬件电流变化。
 */
static void vLowPowerMonitorTask( void * pvParameters )
{
    uint32_t lastIdleHookCount = 0;
    uint32_t lastPreventedCount = 0;
    uint32_t lastAttemptCount = 0;
    uint32_t idleDelta;
    uint32_t preventedDelta;
    uint32_t attemptDelta;
    char line[ 220 ];

    ( void ) pvParameters;

    for( ;; )
    {
        vTaskDelay( pdMS_TO_TICKS( 3000 ) );

        idleDelta = ulIdleHookCount - lastIdleHookCount;
        preventedDelta = ulLowPowerPreventedCount - lastPreventedCount;
        attemptDelta = ulLowPowerAttemptCount - lastAttemptCount;

        lastIdleHookCount = ulIdleHookCount;
        lastPreventedCount = ulLowPowerPreventedCount;
        lastAttemptCount = ulLowPowerAttemptCount;

        snprintf( line,
                  sizeof( line ),
                  "[7][low-power] mode=%s idle_hook_delta=%lu sleep_prevented_delta=%lu sleep_attempt_delta=%lu last_expected_idle_ticks=%lu total_expected_idle_ticks=%lu",
                  ( xLowPowerDemoEnabled == pdTRUE ) ? "on" : "off",
                  ( unsigned long ) idleDelta,
                  ( unsigned long ) preventedDelta,
                  ( unsigned long ) attemptDelta,
                  ( unsigned long ) ulLowPowerLastExpectedTicks,
                  ( unsigned long ) ulLowPowerTotalExpectedTicks );
        prvPrintLine( line );
    }
}

/*
 * tickless idle 前置处理 hook
 * --------------------------
 * FreeRTOS 在准备调用 portSUPPRESS_TICKS_AND_SLEEP() 前会先调用这个宏。
 * 如果把 *pxExpectedIdleTime 改成 0，就等于告诉内核“这次不要进入低功耗路径”。
 */
void vLowPowerPreSuppressTicksAndSleep( void * pvExpectedIdleTime )
{
    TickType_t * pxExpectedIdleTime = ( TickType_t * ) pvExpectedIdleTime;

    /* 参数无效时不写指针，避免 tickless hook 在异常入参下解引用崩溃。 */
    if( pxExpectedIdleTime == NULL )
    {
        return;
    }

    if( xLowPowerDemoEnabled == pdFALSE )
    {
        ulLowPowerPreventedCount++;
        *pxExpectedIdleTime = 0;
    }
}

/*
 * 教学版 tickless idle 执行 hook
 * -----------------------------
 * 真实 MCU 这里通常会关外设、停 SysTick、执行 WFI/WFE，然后被中断唤醒。
 * Windows 模拟端口没有真实低功耗硬件，所以这里只记录“预计能空闲多久”和“进入了几次”。
 */
void vLowPowerSuppressTicksAndSleep( uint32_t ulExpectedIdleTicks )
{
    ulLowPowerAttemptCount++;
    ulLowPowerLastExpectedTicks = ulExpectedIdleTicks;
    ulLowPowerTotalExpectedTicks += ulExpectedIdleTicks;
}

static const char * prvTaskStateName( eTaskState eState )
{
    switch( eState )
    {
        case eRunning:
            return "Running";

        case eReady:
            return "Ready";

        case eBlocked:
            return "Blocked";

        case eSuspended:
            return "Suspended";

        case eDeleted:
            return "Deleted";

        default:
            return "Invalid";
    }
}

/*
 * Task API worker
 * ---------------
 * 这个任务本身不负责查询 API，它是给 inspector 观察和控制的“样本任务”。
 * inspector 会通过任务名找到它、读取它的优先级/状态/栈水位，并周期性修改它的优先级。
 */
static void vTaskApiWorkerTask( void * pvParameters )
{
    uint32_t loopCount = 0;
    volatile uint32_t busyValue = 0;
    char line[ 160 ];

    ( void ) pvParameters;

    for( ;; )
    {
        loopCount++;

        /*
         * 制造一点点 CPU 运行时间，方便 vTaskGetRunTimeStats() 看到该任务的运行时间。
         * volatile 防止编译器把这个空转计算优化掉。
         */
        for( uint32_t i = 0; i < 120000UL; i++ )
        {
            busyValue += i;
        }

        /* uxTaskPriorityGet(NULL) 表示查询“当前正在运行的任务”，也就是本 worker 自己。 */
        snprintf( line,
                  sizeof( line ),
                  "[1][task-api-worker] loop=%lu self_prio=%lu busy=%lu",
                  ( unsigned long ) loopCount,
                  ( unsigned long ) uxTaskPriorityGet( NULL ),
                  ( unsigned long ) busyValue );
        prvPrintLine( line );

        /* 主动阻塞 1 秒，让出 CPU，也让 eTaskGetState() 经常能观察到 Blocked 状态。 */
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

/*
 * Task API inspector
 * ------------------
 * 这个任务集中演示“任务相关的其他 API”：
 * - 通过名字找任务句柄；
 * - 查询/修改任务优先级；
 * - 查询任务状态、栈水位、系统任务数量；
 * - 获取单个任务和全部任务的状态快照；
 * - 打印 vTaskList() 和 vTaskGetRunTimeStats() 的表格输出。
 */
static void vTaskApiInspectorTask( void * pvParameters )
{
    enum { taskAPI_STATUS_ARRAY_SIZE = 24 };

    /*
     * vTaskList()/vTaskGetRunTimeStats() 的输出缓冲区比较大。
     * 这里用 static 放到静态存储区，避免占用 inspector 任务自己的栈。
     */
    static TaskStatus_t xSystemState[ taskAPI_STATUS_ARRAY_SIZE ];
    static char taskListBuffer[ 2048 ];
    static char runTimeStatsBuffer[ 2048 ];
    TaskStatus_t xInfo;
    configRUN_TIME_COUNTER_TYPE ulTotalRunTime = 0;
    uint32_t loopCount = 0;
    char line[ 256 ];

    ( void ) pvParameters;

    for( ;; )
    {
        /* xTaskGetCurrentTaskHandle()：拿到当前任务，也就是 inspector 自己的句柄。 */
        TaskHandle_t xCurrentTask = xTaskGetCurrentTaskHandle();

        /* xTaskGetHandle()：通过任务名查找 api_worker 的任务句柄。 */
        TaskHandle_t xWorkerByName = xTaskGetHandle( "api_worker" );
        UBaseType_t uxOldPriority;
        UBaseType_t uxNewPriority;
        UBaseType_t uxTaskCount;
        UBaseType_t uxCapturedTasks;
        UBaseType_t uxWorkerStackFree;
        eTaskState eWorkerState;

        vTaskDelay( pdMS_TO_TICKS( 5000 ) );
        loopCount++;

        /* 确认“按名字查到的句柄”和创建任务时保存的句柄是同一个任务。 */
        if( xWorkerByName == NULL )
        {
            prvPrintLine( "[1][task-api] api_worker handle not found; skip this round" );
            continue;
        }

        configASSERT( xWorkerByName == xTaskApiWorkerTask );

        /*
         * uxTaskPriorityGet() 查询 worker 当前优先级；
         * vTaskPrioritySet() 把 worker 的优先级在 1 和 3 之间来回切换。
         * 这样日志里可以直观看到任务优先级确实被动态修改了。
         */
        uxOldPriority = uxTaskPriorityGet( xWorkerByName );
        uxNewPriority = ( uxOldPriority == ( tskIDLE_PRIORITY + 1U ) ) ? ( tskIDLE_PRIORITY + 3U ) : ( tskIDLE_PRIORITY + 1U );
        vTaskPrioritySet( xWorkerByName, uxNewPriority );

        /*
         * uxTaskGetNumberOfTasks()：当前系统任务总数；
         * eTaskGetState()：worker 当前运行状态；
         * uxTaskGetStackHighWaterMark()：worker 历史最小剩余栈空间。
         */
        uxTaskCount = uxTaskGetNumberOfTasks();
        eWorkerState = eTaskGetState( xWorkerByName );
        uxWorkerStackFree = uxTaskGetStackHighWaterMark( xWorkerByName );

        /*
         * vTaskGetInfo() 获取单个任务的详细快照。
         * 第三个参数 pdTRUE 表示同时计算栈高水位；
         * 第四个参数 eInvalid 表示让 FreeRTOS 自己判断任务当前状态。
         */
        vTaskGetInfo( xWorkerByName, &xInfo, pdTRUE, eInvalid );

        /* 汇总打印上面几个轻量查询 API 的结果。 */
        snprintf( line,
                  sizeof( line ),
                  "[1][task-api] current=%p worker_by_name=%p task_count=%lu prio:%lu->%lu state=%s stack_free=%lu",
                  ( void * ) xCurrentTask,
                  ( void * ) xWorkerByName,
                  ( unsigned long ) uxTaskCount,
                  ( unsigned long ) uxOldPriority,
                  ( unsigned long ) uxNewPriority,
                  prvTaskStateName( eWorkerState ),
                  ( unsigned long ) uxWorkerStackFree );
        prvPrintLine( line );

        /* 打印 vTaskGetInfo() 拿到的单任务详细信息。 */
        snprintf( line,
                  sizeof( line ),
                  "[1][task-api] vTaskGetInfo name=%s number=%lu current_prio=%lu base_prio=%lu info_state=%s info_stack_free=%lu run_time=%lu",
                  ( xInfo.pcTaskName != NULL ) ? xInfo.pcTaskName : "(null)",
                  ( unsigned long ) xInfo.xTaskNumber,
                  ( unsigned long ) xInfo.uxCurrentPriority,
                  ( unsigned long ) xInfo.uxBasePriority,
                  prvTaskStateName( xInfo.eCurrentState ),
                  ( unsigned long ) xInfo.usStackHighWaterMark,
                  ( unsigned long ) xInfo.ulRunTimeCounter );
        prvPrintLine( line );

        /*
         * uxTaskGetSystemState() 获取“所有任务”的状态快照。
         * xSystemState 是输出数组，taskAPI_STATUS_ARRAY_SIZE 是数组容量；
         * ulTotalRunTime 用来接收系统累计运行时间计数。
         */
        uxCapturedTasks = uxTaskGetSystemState( xSystemState,
                                                taskAPI_STATUS_ARRAY_SIZE,
                                                &ulTotalRunTime );
        /* 任务数超过数组容量时 FreeRTOS 会截断快照，这里给出提示便于调大数组。 */
        if( uxCapturedTasks >= taskAPI_STATUS_ARRAY_SIZE )
        {
            prvPrintLine( "[1][task-api] warning: task count exceeds state array; snapshot truncated" );
        }

        snprintf( line,
                  sizeof( line ),
                  "[1][task-api] uxTaskGetSystemState captured=%lu total_run_time=%lu first=%s/%s/prio%lu",
                  ( unsigned long ) uxCapturedTasks,
                  ( unsigned long ) ulTotalRunTime,
                  ( uxCapturedTasks > 0U ) ? xSystemState[ 0 ].pcTaskName : "none",
                  ( uxCapturedTasks > 0U ) ? prvTaskStateName( xSystemState[ 0 ].eCurrentState ) : "Invalid",
                  ( uxCapturedTasks > 0U ) ? ( unsigned long ) xSystemState[ 0 ].uxCurrentPriority : 0UL );
        prvPrintLine( line );

        if( ( loopCount % 2U ) == 0U )
        {
            /*
             * vTaskList() 生成任务列表表格：任务名、状态、优先级、栈水位、任务编号。
             * 它适合学习和调试观察，正式产品里通常更推荐直接用 uxTaskGetSystemState()。
             */
            vTaskList( taskListBuffer );
            prvPrintBlock( "[1][task-api] vTaskList output:", taskListBuffer );

            /*
             * vTaskGetRunTimeStats() 生成运行时间统计表格。
             * 它依赖 FreeRTOSConfig.h 中打开 configGENERATE_RUN_TIME_STATS，
             * 并提供 portGET_RUN_TIME_COUNTER_VALUE() 作为统计时基。
             */
            vTaskGetRunTimeStats( runTimeStatsBuffer );
            prvPrintBlock( "[1][task-api] vTaskGetRunTimeStats output:", runTimeStatsBuffer );
        }
    }
}

static void vStartTask( void * pvParameters )
{
    ( void ) pvParameters; /* 本 demo 不需要通过 pvParameters 读取参数。 */
    char line[128];

    /* 在 StartTask 中批量创建任务时进入临界区，避免创建过程被调度或中断打断。 */
    taskENTER_CRITICAL();

    /* 监督任务优先级最高，方便及时观察事件和告警。 */
    configASSERT( xTaskCreate( vSupervisorTask,
                               "supervisor",                 /* 任务名：调试/统计时用于识别任务。 */
                               configMINIMAL_STACK_SIZE * 3U, /* 任务栈大小：桌面 demo 给到最小栈的 3 倍。 */
                               NULL,                         /* 任务参数：本任务不需要参数。 */
                               tskIDLE_PRIORITY + 3U,        /* 任务优先级：高于其他业务任务。 */
                               &xSupervisorTask ) == pdPASS ); /* 输出任务句柄，供任务通知使用。 */

    /* 传感器任务负责生产数据，并通过队列发送给控制任务。 */
    configASSERT( xTaskCreate( vSensorTask,
                               "sensor",                    /* 任务名。 */
                               configMINIMAL_STACK_SIZE * 3U, /* 任务栈大小。 */
                               NULL,                        /* 任务参数：不使用。 */
                               tskIDLE_PRIORITY + 2U,       /* 任务优先级：中等。 */
                               &xSensorTask ) == pdPASS );  /* 输出任务句柄，便于后续调试或控制。 */

    /* 控制任务负责消费队列数据，高温时用任务通知提醒监督任务。 */
    configASSERT( xTaskCreate( vControlTask,
                               "control",                  /* 任务名。 */
                               configMINIMAL_STACK_SIZE * 3U, /* 任务栈大小。 */
                               NULL,                       /* 任务参数：不使用。 */
                               tskIDLE_PRIORITY + 2U,      /* 与传感器任务同优先级，二者可时间片轮转。 */
                               &xControlTask ) == pdPASS ); /* 输出任务句柄，便于后续调试或控制。 */

    /* 统计任务优先级较低，只做周期性观测，不影响主业务任务。 */
    configASSERT( xTaskCreate( vStatsTask,
                               "stats",                    /* 任务名。 */
                               configMINIMAL_STACK_SIZE * 3U, /* 任务栈大小。 */
                               NULL,                       /* 任务参数：不使用。 */
                               tskIDLE_PRIORITY + 1U,      /* 较低优先级，避免影响主要业务任务。 */
                               &xStatsTask ) == pdPASS );  /* 输出任务句柄，便于后续调试或控制。 */

    /* 带参数任务：第 4 个参数传入配置结构体地址，任务内部通过 pvParameters 读取。 */
    configASSERT( xTaskCreate( vParamDemoTask,
                               "param",                    /* 任务名。 */
                               configMINIMAL_STACK_SIZE * 3U, /* 任务栈大小。 */
                               &xParamDemoConfig,          /* 任务参数：传入静态配置结构体地址。 */
                               tskIDLE_PRIORITY + 1U,      /* 与统计任务同级，只做学习观察。 */
                               &xParamDemoTask ) == pdPASS ); /* 输出任务句柄，便于后续调试或控制。 */

    /* 参数更新任务：演示其他任务如何安全地修改同一个参数结构体。 */
    configASSERT( xTaskCreate( vParamUpdateTask,
                               "param_up",                 /* 任务名。 */
                               configMINIMAL_STACK_SIZE * 3U, /* 任务栈大小。 */
                               &xParamDemoConfig,          /* 任务参数：和 vParamDemoTask 共用同一个配置结构体。 */
                               tskIDLE_PRIORITY + 2U,      /* 学习观察任务，保持较低优先级。 */
                               &xParamUpdateTask ) == pdPASS ); /* 输出任务句柄，便于后续调试或控制。 */

    configASSERT( xTaskCreate( vSuspendWorkerTask,
                               "susp_worker",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 1U,
                               &xSuspendWorkerTask ) == pdPASS );

    configASSERT( xTaskCreate( vSuspendDemoTask,
                               "susp_demo",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 2U,
                               &xSuspendDemoTask ) == pdPASS );

    configASSERT( xTaskCreate( vConsoleCommandTask,
                               "console_cmd",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 1U,
                               &xConsoleCommandTask ) == pdPASS );

    /*
     * 任务 API 演示 worker：作为被观察对象运行。
     * 它会周期性打印自己的优先级，inspector 会动态修改这个优先级。
     */
    configASSERT( xTaskCreate( vTaskApiWorkerTask,
                               "api_worker",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 1U,
                               &xTaskApiWorkerTask ) == pdPASS );

    /*
     * 任务 API 演示 inspector：集中调用任务查询/统计类 API。
     * 优先级略高于 worker，便于周期性醒来后及时采集和打印状态。
     */
    configASSERT( xTaskCreate( vTaskApiInspectorTask,
                               "api_inspector",
                               configMINIMAL_STACK_SIZE * 4U,
                               NULL,
                               tskIDLE_PRIORITY + 2U,
                               &xTaskApiInspectorTask ) == pdPASS );

    /*
     * 相对延时任务：演示 vTaskDelay()。
     * 它的延时从调用 vTaskDelay() 的那一刻开始计算。
     */
    configASSERT( xTaskCreate( vRelativeDelayDemoTask,
                               "delay_rel",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 1U,
                               &xRelativeDelayTask ) == pdPASS );

    /*
     * 绝对延时任务：演示 vTaskDelayUntil()。
     * 它基于 xLastWakeTime 按固定 2 秒周期唤醒。
     */
    configASSERT( xTaskCreate( vAbsoluteDelayDemoTask,
                               "delay_abs",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 1U,
                               &xAbsoluteDelayTask ) == pdPASS );

    /*
     * 队列集监视任务：用一次 xQueueSelectFromSet() 阻塞等待
     * 二值信号量、计数信号量或队列消息。
     */
    configASSERT( xTaskCreate( vQueueSetMonitorTask,
                               "queue_set",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 2U,
                               &xQueueSetMonitorTask ) == pdPASS );

    /*
     * 任务通知计数演示：按 n/N 后由键盘任务调用 xTaskNotifyGive()，
     * 本任务用 ulTaskNotifyTake(pdFALSE) 一次只把通知值减 1。
     */
    configASSERT( xTaskCreate( vNotifyTakeDemoTask,
                               "notify_take",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 2U,
                               &xNotifyTakeDemoTask ) == pdPASS );

    /*
     * 任务通知位等待演示：按 w/W 后设置通知 bit，
     * 本任务用 xTaskNotifyWait() 等待并读取通知值。
     */
    configASSERT( xTaskCreate( vNotifyWaitDemoTask,
                               "notify_wait",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 2U,
                               &xNotifyWaitDemoTask ) == pdPASS );

    /*
     * 任务通知值传输演示：按任意未被菜单占用的可打印按键后，
     * 键盘任务把该按键 ASCII 码作为通知值发送给本任务。
     */
    configASSERT( xTaskCreate( vNotifyKeyValueDemoTask,
                               "notify_key",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 2U,
                               &xNotifyKeyValueDemoTask ) == pdPASS );

    /*
     * 低功耗学习监视任务：周期打印 idle hook 和 tickless idle 统计值。
     * 按 l/L 可以开启或关闭“允许进入低功耗路径”的运行时开关。
     */
    configASSERT( xTaskCreate( vLowPowerMonitorTask,
                               "low_power",
                               configMINIMAL_STACK_SIZE * 3U,
                               NULL,
                               tskIDLE_PRIORITY + 1U,
                               &xLowPowerMonitorTask ) == pdPASS );

    taskEXIT_CRITICAL();

    snprintf( line,
              sizeof( line ),
              "[0][start] All Task Created. free_heap=%lu",
              ( unsigned long ) xPortGetFreeHeapSize() );
    prvPrintLine( line );      
    vTaskDelete( NULL );

}

/*
 * main 初始化流程
 * --------------
 * main() 只负责创建 FreeRTOS 内核对象和任务，然后启动调度器。
 * 调度器启动后，应用逻辑都在各个任务和软件定时器回调中执行。
 */
int main( void )
{
    puts( "[0][main] FreeRTOS study demo started." );
    fflush( stdout );

    /* 先创建内核对象，再创建使用这些对象的任务。 */
    xConsoleMutex = xSemaphoreCreateMutex();                  /* 创建互斥量，用于保护控制台输出。 */
    xParamConfigMutex = xSemaphoreCreateMutex();              /* 创建互斥量，用于保护可变任务参数。 */
    xSensorQueue = xQueueCreate( 5, sizeof( SensorSample_t ) ); /* 创建队列：最多缓存 5 条传感器数据。 */
    xSystemEvents = xEventGroupCreate();                      /* 创建事件组：用 bit 位记录系统事件。 */
    /* 二值信号量初始为空；按 b/B 会释放一个 token 给队列集监视任务。 */
    xBinaryDemoSemaphore = xSemaphoreCreateBinary();
    /* 计数信号量初始计数为 0，最多累计 SEM_DEMO_COUNTING_MAX 次按键释放。 */
    xCountingDemoSemaphore = xSemaphoreCreateCounting( SEM_DEMO_COUNTING_MAX, 0 );
    /* 队列集里的队列成员；按 q/Q 会发送一个递增的 uint32_t 消息。 */
    xQueueSetDemoQueue = xQueueCreate( QUEUE_SET_DEMO_QUEUE_LENGTH, sizeof( uint32_t ) );
    /*
     * 队列集容量至少要等于所有成员容量之和：
     * 二值信号量 1 + 计数信号量最大计数 + 队列长度。
     */
    xDemoQueueSet = xQueueCreateSet( 1U + SEM_DEMO_COUNTING_MAX + QUEUE_SET_DEMO_QUEUE_LENGTH );
    xHeartbeatTimer = xTimerCreate( "heartbeat",
                                    pdMS_TO_TICKS( 2000 ),      /* 定时周期：2000ms。 */
                                    pdTRUE,                     /* 自动重载：到期后继续下一轮计时。 */
                                    NULL,                       /* 定时器 ID：本 demo 不需要额外上下文。 */
                                    vHeartbeatTimerCallback );  /* 到期时调用的回调函数。 */

    /* configASSERT 用于尽早发现创建失败，常见原因是 FreeRTOS 堆空间不足。 */
    configASSERT( xConsoleMutex != NULL );
    configASSERT( xParamConfigMutex != NULL );
    configASSERT( xSensorQueue != NULL );
    configASSERT( xSystemEvents != NULL );
    configASSERT( xBinaryDemoSemaphore != NULL );
    configASSERT( xCountingDemoSemaphore != NULL );
    configASSERT( xQueueSetDemoQueue != NULL );
    configASSERT( xDemoQueueSet != NULL );
    /* 队列、信号量这类“队列型对象”要在使用前加入队列集；这里放在调度器启动前完成。 */
    configASSERT( xQueueAddToSet( xBinaryDemoSemaphore, xDemoQueueSet ) == pdPASS );
    configASSERT( xQueueAddToSet( xCountingDemoSemaphore, xDemoQueueSet ) == pdPASS );
    configASSERT( xQueueAddToSet( xQueueSetDemoQueue, xDemoQueueSet ) == pdPASS );
    configASSERT( xHeartbeatTimer != NULL );

    configASSERT( xTaskCreate( vStartTask,
                               "start",                 /* 任务名：调试/统计时用于识别任务。 */
                               configMINIMAL_STACK_SIZE * 3U, /* 任务栈大小：桌面 demo 给到最小栈的 3 倍。 */
                               NULL,                         /* 任务参数：本任务不需要参数。 */
                               tskIDLE_PRIORITY + 4U,        /* 任务优先级：高于其他业务任务。 */
                               NULL ) == pdPASS );
    /* 启动自动重载软件定时器，每 2 秒置一次心跳事件位。 */
    configASSERT( xTimerStart( xHeartbeatTimer, 0 ) == pdPASS );

    /* 启动调度器后，FreeRTOS 开始按优先级调度上面创建的任务。 */
    vTaskStartScheduler();

    /* 正常情况下不会运行到这里；若到这里，通常说明调度器启动失败。 */
    for( ;; )
    {
    }
}

/*
 * 空闲任务钩子
 * ----------
 * 当所有普通任务都阻塞/延时，只有 Idle 任务能运行时，FreeRTOS 会调用这里。
 * 低功耗学习实验只在这里做计数，不能 printf，也不能调用会阻塞的 API。
 */
void vApplicationIdleHook( void )
{
    ulIdleHookCount++;
}

/*
 * 内存申请失败 Hook
 * ----------------
 * 当 FreeRTOS 动态创建任务、队列、互斥量、事件组、定时器等对象时，
 * 都可能从 FreeRTOS heap 中申请内存。若申请失败，并且
 * configUSE_MALLOC_FAILED_HOOK 为 1，就会进入这个函数。
 */
void vApplicationMallocFailedHook( void )
{
    /* 动态内存申请失败会进入这里，学习阶段直接停住便于调试。 */
    taskDISABLE_INTERRUPTS();

    for( ;; )
    {
    }
}

/*
 * 栈溢出 Hook
 * ----------
 * 当 configCHECK_FOR_STACK_OVERFLOW 打开后，FreeRTOS 会在任务切换时检查
 * 任务栈是否异常。若发现栈溢出，就会进入这个函数。
 *
 * 学习时如果停在这里，通常需要增大对应任务的栈大小，或减少该任务中的
 * 大型局部变量/深层函数调用。
 */
void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    ( void ) xTask;      /* 出问题的任务句柄；本 demo 中暂不打印，只停住方便调试。 */
    ( void ) pcTaskName; /* 出问题的任务名；真实项目可打印它来定位哪个任务栈溢出。 */

    /* 栈溢出说明某个任务栈给小了，或函数调用/局部变量占用过大。 */
    taskDISABLE_INTERRUPTS();

    for( ;; )
    {
    }
}
