<h1 align="center">simpletimer</h1>

<p align="center">
<a href="README.md">English</a> | <a href="README_zh.md">简体中文</a>
</p>

<p align="center">
轻量级嵌入式软件定时器库
</p>

## 特性

* 分组式定时器管理，同一分组内的定时器共享时基、回调与队列
* O(1) 到期检查
* 有序双向链表调度
* 溢出安全的 Tick 比较
* 无动态内存分配
* 平台无关的锁抽象
* 支持延迟回调与立即回调
* MPSC（多生产者单消费者）异步启动/停止
* 支持事件计数

## 安装

### Git Submodule

```bash
git submodule add https://github.com/zhijian-yan/simpletimer.git
```

### 直接集成

将以下文件加入工程：

* `simpletimer.c`
* `simpletimer.h`

## 快速开始

### 1. 定义定时器、分组与队列缓冲区

```c
#define TIMER_NUM 2

stim_t timers[TIMER_NUM];
stim_group_t group;
stim_message_t command_buffer[16];
stim_message_t expired_buffer[16];
```

`stim_t` 与 `stim_group_t` 需分别通过 `stim_init_timer()` 与 `stim_init_group()` 初始化

### 2. 配置并初始化分组

```c
stim_group_config_t config = {
    .expired_cb = stim_callback,
    .callback_mode = STIM_CALLBACK_MODE_DEFERRED,
    .command_buffer = command_buffer,
    .command_queue_size = 16,
    .expired_buffer = expired_buffer,
    .expired_queue_size = 16,
};
stim_init_group(&group, &config);
```

`command_buffer` 与 `command_queue_size` 为必需项；`expired_buffer` / `expired_queue_size` 在 `STIM_CALLBACK_MODE_DEFERRED` 模式下必需

### 3. 初始化定时器

```c
stim_init_timer(&timers[0], 1000, (void *)1);
stim_init_timer(&timers[1], 100, (void *)2);
```

`stim_init_timer()` 会清零定时器状态，因此用户数据需在此处传入

### 4. 实现回调

```c
void stim_callback(stim_t *timer) {
    switch ((int)timer->user_data) {
    case 1:
        printf("timer1 count:%u\r\n", stim_get_event_count(timer));
        break;
    case 2:
        led_toggle();
        break;
    }
}
```

### 5. 启动定时器

```c
stim_start_timer(&timers[0], &group);
stim_start_timer(&timers[1], &group);
```

### 6. 更新系统时基

```c
void systick_handler(void) {
    stim_timebase_inc(&group);
}
```

### 7. 轮询与分发

```c
while (1) {
    stim_poll(&group);
    stim_dispatch(8, &group);
}
```

`stim_dispatch()` 仅对 `STIM_CALLBACK_MODE_DEFERRED` 模式有效

### 8. 完整示例

```c
#include "simpletimer.h"
#include <stdio.h>

stim_t timers[2];
stim_group_t group;
stim_message_t command_buffer[16];
stim_message_t expired_buffer[16];

static void stim_callback(stim_t *timer) {
    switch ((int)timer->user_data) {
    case 1:
        printf("timer1 count:%u\r\n", stim_get_event_count(timer));
        break;
    case 2:
        led_toggle();
        break;
    }
}

void systick_handler(void) {
    stim_timebase_inc(&group);
}

int main(void) {
    hardware_init();

    stim_group_config_t config = {
        .expired_cb = stim_callback,
        .callback_mode = STIM_CALLBACK_MODE_DEFERRED,
        .command_buffer = command_buffer,
        .command_queue_size = 16,
        .expired_buffer = expired_buffer,
        .expired_queue_size = 16,
    };
    stim_init_group(&group, &config);

    stim_init_timer(&timers[0], 1000, (void *)1);
    stim_init_timer(&timers[1], 100, (void *)2);

    stim_start_timer(&timers[0], &group);
    stim_start_timer(&timers[1], &group);

    while (1) {
        stim_poll(&group);
        stim_dispatch(8, &group);
    }
    return 0;
}
```

## 设计原理

### 分组模型

定时器以“分组（group）”为单位进行管理

* `stim_t` 描述单个定时器的周期、到期时间、事件计数与链表节点
* `stim_group_t` 描述一组定时器共享的时基、回调、回调模式与两个队列

分组模型带来的好处：

* 时基由分组统一维护（`group->timebase_ticks`），不同分组可使用不同的时基
* 回调与回调模式为分组级别配置，减少每个定时器的存储开销
* 命令队列与到期事件队列均内嵌于分组，无需全局状态

### 整体架构

simpletimer 采用 **MPSC（Multi-Producer Single-Consumer）** 架构实现异步控制

```text
              SysTick ISR
                   │
                   ▼
         stim_timebase_inc(group)
                   │
                   ▼
               Main Loop
                   │
        ┌──────────┴──────────┐
        │                     │
        ▼                     ▼
   stim_poll(group)   stim_dispatch(max_event_count, group)
        │                     │
        │                     ▼
        │               执行延迟回调
        │
        ├── 处理命令队列（启动 / 停止）
        │
        ├── 检查表头定时器是否到期
        │
        ├── 更新到期时间与事件计数并重新入表
        │
        └── 生成到期事件
               │
       ┌───────┴────────┐
       │                │
       ▼                ▼
   立即回调        事件队列
```

所有定时器管理逻辑均在 `stim_poll()` 中完成，`stim_start_timer()` 和 `stim_stop_timer()` 不会直接修改定时器链表，而是向命令队列发送请求，由 `stim_poll()` 统一处理

这种设计避免了多个执行上下文同时修改链表的问题

---

### 有序双向链表调度

所有运行中的定时器通过内嵌的 `struct stim_node` 按照到期时间升序排列

```text
Head
 │
 ▼
TimerA(100)
 │
 ▼
TimerB(200)
 │
 ▼
TimerC(500)
```

启动定时器时：

```c
stim_start_timer(timer, group);
```

命令在 `stim_poll()` 中被处理，定时器会被插入到合适的位置以保证链表始终有序，因此最早到期的定时器始终位于链表表头

使用双向链表与内嵌节点（`stim_container_of`）意味着**无需任何动态内存分配**

---

### O(1) 到期检查

由于链表按到期时间排序，每次轮询仅需检查表头节点：

```c
if ((int32_t)(timer->expire_ticks - now) <= 0)
```

如果表头尚未到期，则后续节点一定也未到期，因此到期检查复杂度为 O(1)，而无需遍历整个定时器链表

定时器到期后会被重新插入链表，以保证后续检查仍然成立

---

### 溢出安全 Tick 比较

simpletimer 使用有符号差值比较时间：

```c
(int32_t)(expire_ticks - now)
```

例如：

```text
expire = 0x00000010
now    = 0xFFFFFFF0
```

即使系统 Tick 已发生回绕：

```text
0xFFFFFFFF → 0x00000000
```

比较结果仍然正确，因此无需额外处理 Tick 溢出问题

为了保证比较结果有效需满足：

```text
period_ticks <= INT32_MAX == STIM_MAX_PERIOD_TICKS == 2147483647
```

---

### 异步启动与停止

启动和停止操作不会立即修改链表：

```c
stim_start_timer(timer, group);
stim_stop_timer(timer, group);
```

上述调用仅向命令队列发送请求：

```text
Producer
    │
    ▼
Command Queue
    │
    ▼
stim_poll()
```

随后由 `stim_poll()` 完成实际处理，这样可以安全地在：

* 主循环
* 中断服务函数
* RTOS 任务

中调用启动和停止接口

---

### 到期处理流程

`stim_poll()` 每次调用会先处理命令队列，再循环检查表头定时器是否到期：

```text
stim_poll(group)
    │
    ├── 处理命令（启动 / 停止）
    │
    └── while (表头定时器已到期)
            ├── 从链表移除
            ├── expire_ticks += period_ticks
            ├── event_count += 1
            ├── 重新插入链表
            └── 立即模式 ? 执行回调 : 事件入队
```

只有当表头定时器未到期时才跳出循环，从而保持 O(1) 的到期检查

---

### 回调执行模型

simpletimer 支持两种回调执行模式

#### Immediate Mode

```text
Timer Expired
      │
      ▼
 stim_poll()
      │
      ▼
 Callback
```

回调在定时器到期后立即执行

**特点**

* 延迟最小
* 不丢失事件
* 适用于短时间操作

**限制**

* 不可调用阻塞式 API
* 不适合耗时操作

---

#### Deferred Mode

```text
Timer Expired
      │
      ▼
Event Queue
      │
      ▼
stim_dispatch()
      │
      ▼
 Callback
```

回调会先进入到期事件队列，随后由 `stim_dispatch()` 执行

**特点**

* 可执行耗时操作
* 可调用阻塞式 API
* 支持 printf()/malloc() 等函数

**限制**

* 回调执行存在一定延迟
* 延迟取决于 `stim_dispatch()` 的调用频率
* 队列满时事件可能被丢弃

---

### 命令队列与事件队列

分组内包含两个环形队列，缓冲区均由用户提供：

| 队列             | 生产者                          | 消费者             | 用途               |
| -------------- | ---------------------------- | --------------- | ---------------- |
| `command_queue` | `stim_start_timer()` / `stim_stop_timer()` | `stim_poll()`   | 异步启动 / 停止命令      |
| `expired_queue` | `stim_poll()`                | `stim_dispatch()` | 延迟模式下的到期事件       |

```c
typedef struct {
    stim_message_t *buffer;       /* 用户提供的缓冲区 */
    uint8_t capacity;             /* 索引掩码 = 缓冲区元素个数 - 1，由 stim_init_group() 设置 */
    volatile uint8_t write_index;
    volatile uint8_t read_index;
} stim_queue_t;
```

要求：

* `command_queue_size` 是 `command_buffer` 的元素个数，必须为 2 的幂，且介于 `STIM_MIN_QUEUE_SIZE`（`2`）与 `STIM_MAX_QUEUE_SIZE`（`256`）之间
* `expired_queue_size` 与 `expired_buffer` 同理，仅在 `STIM_CALLBACK_MODE_DEFERRED` 模式下使用
* 环形队列始终空出一个槽位来区分"满"与"空"，因此 `N` 个元素的队列最多容纳 `N - 1` 条消息
* 命令队列缓冲区为必需项
* 到期事件队列缓冲区在 `STIM_CALLBACK_MODE_DEFERRED` 模式下为必需项
* 队列满时事件将被丢弃，`stim_poll()` 的返回值会累加丢弃数量

---

### 并发模型

simpletimer 内部采用 `MPSC（Multi Producer Single Consumer）` 模型

**生产者**

* Main Loop
* ISR
* RTOS Task

**消费者**

* `stim_poll()`

命令队列与事件队列均通过锁抽象保护

simpletimer 通过两个接口抽象平台相关的临界区实现：

```c
static inline int stim_lock(void)
{
    /* Disable interrupts if needed */
    return 0;
}

static inline void stim_unlock(int stim_lock_state)
{
    /* Restore interrupt state */
    (void)stim_lock_state;
}
```

默认实现为空操作，需要中断安全的平台可自行实现这两个接口

以下 API 可在任意执行上下文中调用：

* `stim_timebase_inc()`
* `stim_start_timer()`
* `stim_stop_timer()`
* `stim_set_event_count()`
* `stim_get_event_count()`

以下 API 必须遵循单消费者模型：

* `stim_poll()`
* `stim_dispatch()`

即同一时刻只能由一个执行上下文调用

## API 参考

### stim_timebase_inc

```c
void stim_timebase_inc(stim_group_t *group);
```

递增分组时基

该函数必须以固定周期调用，通常在 SysTick 中断服务函数中执行

**参数**

* `group`：定时器所属分组，不可为空

---

### stim_init_group

```c
void stim_init_group(stim_group_t *group, stim_group_config_t *config);
```

初始化分组

**参数**

* `group`：分组对象
* `config`：分组配置，不可为空，且 `config->command_buffer` 不可为空

**说明**

* `config->command_queue_size` 必须为 2 的幂，且介于 `STIM_MIN_QUEUE_SIZE`（`2`）与 `STIM_MAX_QUEUE_SIZE`（`256`）之间
* `config->expired_buffer` / `config->expired_queue_size` 遵循同样的规则，在 `STIM_CALLBACK_MODE_DEFERRED` 模式下必需

---

### stim_init_timer

```c
void stim_init_timer(stim_t *timer, uint32_t period_ticks, void *user_data);
```

初始化定时器

该函数会清零定时器状态并设置周期与用户数据，定时器初始为停止状态

**参数**

* `timer`：定时器对象
* `period_ticks`：定时器周期（单位：Tick），有效范围 `[1, 2147483647]`
* `user_data`：用户数据，可在回调中通过 `timer->user_data` 访问

---

### stim_start_timer

```c
int stim_start_timer(stim_t *timer, stim_group_t *group);
```

启动定时器

向分组的命令队列投递启动命令，实际启动由 `stim_poll()` 完成

**参数**

* `timer`：定时器对象
* `group`：定时器所属分组

**返回值**

* `0`：成功
* 非 `0`：命令队列已满

---

### stim_stop_timer

```c
int stim_stop_timer(stim_t *timer, stim_group_t *group);
```

停止定时器

向分组的命令队列投递停止命令，该操作为异步操作，调用后不会立即生效，而是在 `stim_poll()` 处理命令时完成

**参数**

* `timer`：定时器对象
* `group`：定时器所属分组

**返回值**

* `0`：成功
* 非 `0`：命令队列已满

---

### stim_poll

```c
int stim_poll(stim_group_t *group);
```

处理待执行命令并检查定时器是否到期

* 对于 `STIM_CALLBACK_MODE_IMMEDIATE`，直接执行回调
* 对于 `STIM_CALLBACK_MODE_DEFERRED`，产生到期事件并放入队列

**参数**

* `group`：定时器所属分组

**返回值**

* 因到期事件队列已满而丢弃的事件数量（立即模式恒为 `0`）

---

### stim_dispatch

```c
void stim_dispatch(uint8_t max_event_count, stim_group_t *group);
```

处理到期事件队列并执行回调

仅对 `STIM_CALLBACK_MODE_DEFERRED` 模式有效

**参数**

* `max_event_count`：单次调用处理事件的最大数量
* `group`：定时器所属分组

---

### stim_set_event_count

```c
void stim_set_event_count(stim_t *timer, uint16_t event_count);
```

设置定时器事件计数值

该函数内部受锁保护，可在任意执行上下文中调用

**参数**

* `timer`：定时器对象
* `event_count`：事件计数值，传 `0` 可清零

**说明**

`stim_t.event_count` 的实际类型为 `uint16_t`，计数超过 65535 后会回绕

---

### stim_get_event_count

```c
uint16_t stim_get_event_count(const stim_t *timer);
```

获取定时器事件计数值

该函数内部受锁保护，可在任意执行上下文中调用

**参数**

* `timer`：定时器对象

**返回值**

* 当前事件计数值

---

### stim_lock / stim_unlock

```c
static inline int stim_lock(void);
static inline void stim_unlock(int stim_lock_state);
```

平台相关的锁抽象

`stim_lock()` 返回的锁状态会传给 `stim_unlock()`，用于恢复临界区

默认实现为空操作，需要中断安全的平台可自行实现

## 数据结构

### stim_t

```c
typedef struct {
    struct stim_node node;
    void *user_data;
    uint32_t expire_ticks;
    uint32_t period_ticks;
    volatile uint16_t event_count;
    uint8_t state;
} stim_t;
```

* `node`：内嵌的有序链表节点
* `user_data`：用户数据，通过 `stim_init_timer()` 设置
* `expire_ticks`：下一次到期的绝对 Tick
* `period_ticks`：定时器周期
* `event_count`：到期事件计数，每次到期自动加一
* `state`：运行状态（`0` = 停止，`1` = 运行）

必须通过 `stim_init_timer()` 初始化

### stim_message_t

```c
typedef struct {
    stim_t *timer;
    uint8_t command;
} stim_message_t;
```

命令队列或到期事件队列中的消息单元。`command` 字段仅在命令队列中有意义

### stim_queue_t

```c
typedef struct {
    stim_message_t *buffer;
    uint8_t capacity;
    volatile uint8_t write_index;
    volatile uint8_t read_index;
} stim_queue_t;
```

环形队列。`capacity` 是 `stim_init_group()` 维护的索引掩码：`buffer` 的元素个数为 `capacity + 1`，最多可排队 `capacity` 条消息

### stim_group_t

```c
typedef struct {
    volatile uint32_t timebase_ticks;
    void (*expired_cb)(stim_t *timer);
    stim_callback_mode_t callback_mode;
    stim_queue_t command_queue;
    stim_queue_t expired_queue;
    struct stim_node head;
} stim_group_t;
```

定时器分组

* `timebase_ticks`：分组时基，由 `stim_timebase_inc()` 递增
* `expired_cb`：到期回调，回调参数为定时器指针
* `callback_mode`：回调执行模式
* `command_queue`：启动 / 停止命令队列
* `expired_queue`：延迟模式下的到期事件队列
* `head`：有序链表头节点

必须通过 `stim_init_group()` 初始化

### stim_group_config_t

```c
typedef struct {
    void (*expired_cb)(stim_t *timer);
    stim_callback_mode_t callback_mode;
    stim_message_t *command_buffer;
    stim_message_t *expired_buffer;
    uint16_t command_queue_size;
    uint16_t expired_queue_size;
} stim_group_config_t;
```

分组初始化配置

* `expired_cb`：到期回调
* `callback_mode`：回调执行模式
* `command_buffer` / `command_queue_size`：命令队列缓冲区与其元素个数（必需；须为 2 的幂，且介于 `STIM_MIN_QUEUE_SIZE` 与 `STIM_MAX_QUEUE_SIZE` 之间）
* `expired_buffer` / `expired_queue_size`：到期事件队列缓冲区与其元素个数（规则同上；延迟模式下必需）

## 宏与枚举

### stim_callback_mode_t

```c
typedef enum {
    STIM_CALLBACK_MODE_DEFERRED = 0,
    STIM_CALLBACK_MODE_IMMEDIATE,
} stim_callback_mode_t;
```

回调执行模式

* `STIM_CALLBACK_MODE_DEFERRED`：到期事件先入队，由 `stim_dispatch()` 执行
* `STIM_CALLBACK_MODE_IMMEDIATE`：到期时在 `stim_poll()` 中立即执行

### STIM_MAX_PERIOD_TICKS

允许设置的最大定时器周期，值为 `((uint32_t)(-1)) >> 1`（`0x7FFFFFFF`，即 `INT32_MAX`）

该值保证有符号差值比较在 Tick 回绕时仍然正确

### STIM_MAX_QUEUE_SIZE

队列缓冲区元素个数的上限（`256`）。

由于环形队列始终空出一个槽位，最多可排队 255 条消息。

### STIM_MIN_QUEUE_SIZE

队列缓冲区元素个数的下限（`2`）。

`2` 个元素的队列最多容纳 1 条消息。
