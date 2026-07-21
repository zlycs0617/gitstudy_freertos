# freertos_study

这是一个 Windows + VS Code 下学习 FreeRTOS 的桌面仿真 demo。它使用 FreeRTOS-Kernel 自带的 `portable/MSVC-MingW` port，可以先在电脑上学习 FreeRTOS API，不需要开发板。

## 你当前依赖检查结果

`D:\freertos_study` 下已经发现两个 FreeRTOS-Kernel 目录：

```text
D:\freertos_study\FreeRTOS-KernelV11.3.0
D:\freertos_study\FreeRTOS-Kernel-11.3.0\FreeRTOS-Kernel-11.3.0
```

两份都包含关键文件：

```text
CMakeLists.txt
tasks.c
queue.c
timers.c
include/FreeRTOS.h
portable/MSVC-MingW/port.c
portable/MSVC-MingW/portmacro.h
portable/MemMang/heap_4.c
```

建议使用更干净的：

```text
D:\freertos_study\FreeRTOS-KernelV11.3.0
```

另一个 `FreeRTOS-Kernel-11.3.0` 多嵌套了一层，不是错，但更容易把路径写乱。

## 本 demo 包含的 FreeRTOS 知识点

- 任务：`xTaskCreate()`、`vTaskDelay()`
- 队列：`xQueueCreate()`、`xQueueSend()`、`xQueueReceive()`
- 互斥锁：`xSemaphoreCreateMutex()`、`xSemaphoreTake()`、`xSemaphoreGive()`
- 事件组：`xEventGroupSetBits()`、`xEventGroupWaitBits()`
- 任务通知：`xTaskNotifyGive()`、`ulTaskNotifyTake()`
- 软件定时器：`xTimerCreate()`、`xTimerStart()`
- 内存：`heap_4.c`、`xPortGetFreeHeapSize()`
- 配置：`include/FreeRTOSConfig.h`

## 目录结构

```text
freertos_study/
  .vscode/
  include/
    FreeRTOSConfig.h
  FreeRTOS-KernelV11.3.0/
  CMakeLists.txt
  app_assert.c
  main.c
  README.md
```

## 构建环境

需要安装：

1. VS Code
2. VS Code 插件：C/C++、CMake Tools
3. Visual Studio Build Tools 或 Visual Studio Community，勾选“使用 C++ 的桌面开发”
4. CMake
5. Git，可选；你已经下载 zip 的话不再必须

## 构建运行

在 `D:\freertos_study` 打开 PowerShell：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug
.\build\Debug\freertos_study.exe
```

或者在 VS Code 中：

1. 打开 `D:\freertos_study`
2. 安装推荐插件
3. 运行任务 `CMake: build debug`
4. 按 F5 调试

## 预期输出

```text
FreeRTOS study demo started.
[sensor] produced seq=1 temperature=26 humidity=51
[control] consumed seq=1 temperature=26 humidity=51
[supervisor] sensor event observed
[supervisor] heartbeat event observed
[stats] tick=3000 queued=0 free_heap=...
```

输出顺序可能会因任务调度略有变化，这是 RTOS 调度现象的一部分。

## 移植到开发板时要换什么

当前 demo 是 PC 学习版。换到真实 MCU 时，需要替换：

- `FREERTOS_PORT`，例如 Cortex-M4F + GCC 常用 `GCC_ARM_CM4F`
- 交叉编译器，例如 Arm GNU Toolchain
- 启动文件，例如 `startup_stm32xxxx.s`
- 链接脚本，例如 `STM32xxxx_FLASH.ld`
- CMSIS 和芯片厂商 HAL/LL/BSP
- 调试烧录工具，例如 OpenOCD、ST-LINK GDB Server、J-Link GDB Server

FreeRTOS API 学习代码可以保留，底层 port 和硬件初始化需要按芯片重配。
