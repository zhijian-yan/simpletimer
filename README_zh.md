<h1 align="center">simpletimer</h1>

<p align="center">
<a href="README.md">English</a> | <a href="README_zh.md">简体中文</a>
</p>

<p align="center">
轻量级嵌入式软件定时器库
</p>

<p align="center">
<a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square"></a>
<img alt="Language" src="https://img.shields.io/badge/language-C99-blue.svg?style=flat-square">
<img alt="Dependencies" src="https://img.shields.io/badge/dependencies-none-brightgreen.svg?style=flat-square">
<img alt="Dynamic memory" src="https://img.shields.io/badge/dynamic_memory-none-brightgreen.svg?style=flat-square">
<img alt="Platform" src="https://img.shields.io/badge/platform-bare--metal_%7C_RTOS_%7C_Linux-lightgrey.svg?style=flat-square">
</p>

## 特性

* 分组式管理，组内定时器共享时基、回调与队列
* 定时周期以时基 tick 为单位，范围 1 到 2^31-1
* 有序双向链表调度，最早到期的定时器位于表头
* 到期检查为 O(1)，tick 比较溢出安全
* 定时器落后时按已过周期补齐到期事件
* 启动与停止通过命令队列异步完成，可在中断、任务或主循环中调用
* 支持立即回调与延迟回调
* 每个定时器带到期计数，可原子读写
* 到期事件队列缓冲区由用户提供，队列满时丢弃的事件会被计数并返回
* 到期回调可以省略，此时仅统计到期次数
* 无动态内存分配，不依赖操作系统，C99
* 平台相关的临界区与顺序屏障由钩子层隔离

## 移植

移植时直接在 `simpletimer_port.h` 中实现下列四个平台钩子即可；该文件属于应用，升级库时保留自己的版本

### 平台钩子

| 钩子 | 作用 |
| ---- | ---- |
| `int stim_lock(void)` | 进入临界区，保护共享状态（时基、队列索引、定时器到期计数），返回进入前的状态 |
| `void stim_unlock(int state)` | 退出临界区，并恢复 `stim_lock()` 返回的状态 |
| `STIM_ACQUIRE()` | 读取队列索引之后、读取该索引所发布的表项之前执行的顺序屏障 |
| `STIM_RELEASE()` | 写入发布表项的队列索引之前执行的顺序屏障 |

### 默认实现

库内提供的钩子默认实现全部为空操作。在 32 位单核平台上，或每个分组只由一个执行上下文使用时，可以直接使用默认实现，无需编写任何移植代码：时基、队列索引与到期计数都是自然对齐的 32 位及以下宽度，单上下文访问不会被撕裂，也不需要任何顺序屏障。

### 示例：裸机

时基通常由 SysTick 或定时器中断递增，而主循环在轮询，因此锁需要保存并恢复中断屏蔽状态。单核无需顺序屏障，`STIM_ACQUIRE()` / `STIM_RELEASE()` 保持库内默认实现即可。

```c
/* simpletimer_port.h */
#include "cmsis_compiler.h" /* CMSIS 5；更低版本请包含 core_cm*.h */

static inline int stim_lock(void) {
    int state = __get_PRIMASK();
    __disable_irq();
    return state;
}

static inline void stim_unlock(int stim_lock_state) {
    __set_PRIMASK(stim_lock_state);
}
```

### 示例：RTOS

应使用 RTOS 的临界区：仅关中断无法覆盖临界区内的任务切换。`taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` 支持嵌套，因此无需保存状态；RT-Thread 可用 `rt_enter_critical()` / `rt_exit_critical()`。这里不适合用互斥锁，因为 `stim_start_timer()` 与 `stim_stop_timer()` 也会取锁，而它们可能在中断中被调用。

```c
/* simpletimer_port.h */
#include "FreeRTOS.h"
#include "task.h"

static inline int stim_lock(void) {
    taskENTER_CRITICAL();
    return 0;
}

static inline void stim_unlock(int stim_lock_state) {
    (void)stim_lock_state;
    taskEXIT_CRITICAL();
}
```

### 示例：多核

需要所有核共享的锁，以及真正的顺序屏障：`volatile` 本身不保证顺序，否则一个核可能先看到队列索引、后看到该索引发布的表项。取自旋锁前必须先屏蔽本地中断，否则中断一旦抢占持锁上下文就会死锁。

```c
/* simpletimer_port.h */
#include "cmsis_compiler.h"

/* stim_spinlock 与 spin_lock() / spin_unlock() 均由 SoC 提供 */
static inline int stim_lock(void) {
    int state = __get_PRIMASK();
    __disable_irq();
    spin_lock(&stim_spinlock);
    return state;
}

static inline void stim_unlock(int stim_lock_state) {
    spin_unlock(&stim_spinlock);
    __set_PRIMASK(stim_lock_state);
}

#define STIM_ACQUIRE() __DMB()
#define STIM_RELEASE() __DMB()
```

### 示例：宿主平台（Linux）

做宿主工具、单元测试、仿真或用户态 Linux 驱动时，只要同一个分组只由一个线程使用，直接沿用默认实现即可。多个线程共享一个分组时改用互斥锁；若宿主平台是弱内存模型，还需要真正的屏障。

```c
/* simpletimer_port.h */
#include <pthread.h>

static pthread_mutex_t stim_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline int stim_lock(void) {
    pthread_mutex_lock(&stim_mutex);
    return 0;
}

static inline void stim_unlock(int stim_lock_state) {
    (void)stim_lock_state;
    pthread_mutex_unlock(&stim_mutex);
}

#define STIM_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define STIM_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)
```

`simpletimer_port.h` 会被每个包含库头的编译单元各展开一份，因此上面的互斥锁也是每个编译单元一份：若多个模块都会调用本库，请把互斥锁放到其中一个 `.c` 文件中定义，这里改为 `extern`。

### 注意事项

* 钩子实现需保持无状态，嵌套的加解锁需正确恢复最外层状态：`static inline` 定义对每个编译单元都是私有的，函数内的静态变量会变成每个编译单元一份，而不是整个程序一份
* 所有生产者（`stim_timebase_inc()`、`stim_start_timer()`、`stim_stop_timer()`、`stim_set_event_count()`、`stim_get_event_count()`）以及 `stim_poll()` 都会加锁，后者保护其访问的时基、到期计数与队列索引

## 使用方法

### 1. 定义对象与回调

```c
#include "simpletimer.h"

#define TIMER_NUM       2
#define HEARTBEAT_TICKS 1000 /* 时基为 1ms 时即 1s */
#define SAMPLE_TICKS    100  /* 100ms */

static stim_t timers[TIMER_NUM];
static stim_group_t group;
static stim_message_t command_buffer[16]; /* 启动/停止命令 */
static stim_message_t expired_buffer[16]; /* 到期事件，延迟模式使用 */

/* 由回调写入，主循环自行处理 */
static volatile uint8_t heartbeat_flag;
static volatile uint8_t sample_flag;

static void timer_expired_cb(stim_t *timer) {
    if (timer == &timers[0])
        heartbeat_flag = 1;
    else
        sample_flag = 1;
}
```

回调参数即为到期的定时器，因此可直接与定时器对象做指针比较，也可通过 `stim_init_timer()` 存入的 `timer->user_data` 区分

### 2. 初始化分组与定时器

```c
stim_group_config_t config = {
    .expired_cb = timer_expired_cb,
    .callback_mode = STIM_CALLBACK_MODE_DEFERRED,
    .command_buffer = command_buffer,
    .command_queue_size = 16,
    .expired_buffer = expired_buffer,
    .expired_queue_size = 16,
};
stim_init_group(&group, &config);

stim_init_timer(&timers[0], HEARTBEAT_TICKS, 0);
stim_init_timer(&timers[1], SAMPLE_TICKS, 0);

(void)stim_start_timer(&timers[0], &group); /* 非 0 表示命令队列已满 */
(void)stim_start_timer(&timers[1], &group);
```

* `command_buffer` / `command_queue_size` 为必需项；`expired_buffer` / `expired_queue_size` 仅在 `STIM_CALLBACK_MODE_DEFERRED` 模式下需要
* 两个长度都必须为 2 的幂且位于 `[STIM_MIN_QUEUE_SIZE, STIM_MAX_QUEUE_SIZE]` 之间；每个环形队列会空出一个槽位，因此 16 个元素最多缓存 15 条命令或事件
* `expired_cb` 可以传空指针（`0`），此时仅统计到期次数
* 两个缓冲区均为引用而非拷贝，其生命周期必须覆盖整个分组
* 周期必须位于 `[1, STIM_MAX_PERIOD_TICKS]` 之间；`stim_init_timer()` 会清零定时器状态，周期与用户数据在此传入
* 启动与停止均为异步操作：仅向命令队列投递命令，真正的启动或停止发生在 `stim_poll()` 处理该命令时
* 运行中的定时器不可拷贝或移动，因为分组持有其内嵌节点的指针；也不可对仍挂载在分组链表中的定时器调用 `stim_init_timer()`

### 3. 更新时基、轮询与分发

```c
/* 由 1ms 的 SysTick 中断调用 */
void systick_handler(void) {
    stim_timebase_inc(&group);
}

for (;;) {
    stim_poll(&group);        /* 处理命令并检查到期 */
    stim_dispatch(8, &group); /* 仅延迟模式需要 */
    /* 在此处理 heartbeat_flag 与 sample_flag */
}
```

每个分组都必须以固定频率调用 `stim_timebase_inc()`，时基的一个 tick 即所有周期的分辨率。`stim_poll()` 在立即模式（`STIM_CALLBACK_MODE_IMMEDIATE`）下直接执行到期回调，在延迟模式（`STIM_CALLBACK_MODE_DEFERRED`）下将到期事件入队，其返回值为因到期事件队列已满而丢弃的事件数。立即模式的回调在 `stim_poll()` 中执行，应保持简短。同一分组中，`stim_poll()` 与 `stim_dispatch()` 各自只接受一个执行上下文，且不可在回调中调用 `stim_poll()`，两次调用的间隔需小于 2^31 个 tick

### 4. 完整示例

```c
#include "simpletimer.h"

#define TIMER_NUM       2
#define HEARTBEAT_TICKS 1000 /* 时基为 1ms 时即 1s */
#define SAMPLE_TICKS    100  /* 100ms */

static stim_t timers[TIMER_NUM];
static stim_group_t group;
static stim_message_t command_buffer[16];
static stim_message_t expired_buffer[16];

/* 由回调写入，主循环自行处理 */
static volatile uint8_t heartbeat_flag;
static volatile uint8_t sample_flag;

static void timer_expired_cb(stim_t *timer) {
    if (timer == &timers[0])
        heartbeat_flag = 1;
    else
        sample_flag = 1;
}

/* 由 1ms 的 SysTick 中断调用 */
void systick_handler(void) {
    stim_timebase_inc(&group);
}

int main(void) {
    stim_group_config_t config = {
        .expired_cb = timer_expired_cb,
        .callback_mode = STIM_CALLBACK_MODE_DEFERRED,
        .command_buffer = command_buffer,
        .command_queue_size = 16,
        .expired_buffer = expired_buffer,
        .expired_queue_size = 16,
    };
    stim_init_group(&group, &config);

    stim_init_timer(&timers[0], HEARTBEAT_TICKS, 0);
    stim_init_timer(&timers[1], SAMPLE_TICKS, 0);

    (void)stim_start_timer(&timers[0], &group);
    (void)stim_start_timer(&timers[1], &group);

    for (;;) {
        stim_poll(&group);
        stim_dispatch(8, &group);
        /* 在此处理 heartbeat_flag 与 sample_flag */
    }
}
```
