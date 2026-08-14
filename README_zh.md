<h1 align="center">softimer</h1>

<p align="center">
<a href="README.md">English</a> | <a href="README_zh.md">简体中文</a>
</p>

<p align="center">
轻量级嵌入式软件定时器库
</p>

## 特性

* O(1) 到期检查
* 有序链表调度
* 溢出安全的 Tick 比较
* 无动态内存分配
* 平台无关的锁抽象
* 支持延迟回调与立即回调
* MPSC（多生产者单消费者）异步控制
* 支持事件计数

## 安装

### Git Submodule

```bash
git submodule add https://github.com/zhijian-yan/softimer.git
```

### 直接集成

将以下文件加入工程：

* `softimer.c`
* `softimer.h`

## 快速开始

### 1. 创建定时器

```c
stim_t timer;
```

### 2. 初始化定时器

```c
stim_init(&timer, 100, STIM_CB_MODE_DEFERRED, timer_callback, NULL);
```

### 3. 启动定时器

```c
stim_start(&timer);
```

### 4. 更新系统 Tick

```c
void systick_handler(void) {
    stim_tick_inc();
}
```

### 5. 轮询定时器状态

```c
stim_poll();
```

### 6. 分发定时器事件

```c
while (1) {
    stim_dispatch(8);
}
```

### 7. 完整示例

```c
#include "softimer.h"
#include <stdio.h>

stim_t timer1, timer2;

static void stim_callback(stim_t *timer, void *user_data) {
    uint32_t count;
    switch ((int)user_data) {
    case 1:
        stim_get_count(timer, &count);
        printf("timer1 count:%lu\r\n", count);
        break;
    case 2:
        led_toggle();
        break;
    }
}

void systick_handler(void) {
    stim_tick_inc();
}

int main(void) {
    hardware_init();
    stim_init(&timer1, 1000, STIM_CB_MODE_DEFERRED, stim_callback, (void *)1);
    stim_init(&timer2, 100, STIM_CB_MODE_IMMEDIATE, stim_callback, (void *)2);
    stim_start(&timer1);
    stim_start(&timer2);
    while (1) {
        stim_poll();
        stim_dispatch(8);
    }
}
```

## 设计原理

### 整体架构

softimer 采用 **MPSC（Multi-Producer Single-Consumer）** 架构实现异步控制

```text
              SysTick ISR
                   │
                   ▼
            stim_tick_inc()
                   │
                   ▼
               Main Loop
                   │
        ┌──────────┴──────────┐
        │                     │
        ▼                     ▼
    stim_poll()        stim_dispatch()
        │                     │
        │                     ▼
        │               执行延迟回调
        │
        ├── 处理启动/停止命令
        │
        ├── 检查定时器到期
        │
        ├── 更新定时器状态
        │
        └── 生成到期事件
               │
       ┌───────┴────────┐
       │                │
       ▼                ▼
   立即回调        到期事件队列
```

所有定时器管理逻辑均在 `stim_poll()` 中完成，`stim_start()` 和 `stim_stop()` 不会直接修改定时器链表，而是向命令队列发送请求，由 `stim_poll()` 统一处理

这种设计避免了多个执行上下文同时修改链表的问题

---

### 有序链表调度

所有运行中的定时器按照到期时间升序排列

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
stim_start(timer);
```

定时器会被插入到合适的位置，以保证链表始终有序，因此最早到期的定时器始终位于链表表头

---

### O(1) 到期检查

由于链表按到期时间排序：

```text
Head
 │
 ▼
TimerA(100)
TimerB(200)
TimerC(500)
```

每次轮询时仅需检查表头节点：

```c
if ((int32_t)(timer->expire_ticks - now) <= 0)
```

如果表头尚未到期，则后续节点一定也未到期，因此到期检查复杂度为O(1)，而无需遍历整个定时器链表

---

### 溢出安全 Tick 比较

softimer 使用有符号差值比较时间：

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
period_ticks <= INT32_MAX == STIM_MAX_TICKS == 2147483647
```

---

### 异步启动与停止

启动和停止操作不会立即修改链表：

```c
stim_start(timer);
stim_stop(timer);
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
* RTOS任务

中调用启动和停止接口

---

### 回调执行模型

softimer 支持两种回调执行模式

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
Expired Queue
      │
      ▼
stim_dispatch()
      │
      ▼
 Callback
```

回调会先进入到期事件队列，随后由`stim_dispatch()`执行

**特点**

* 可执行耗时操作
* 可调用阻塞式 API
* 支持 printf()/malloc() 等函数

**限制**

* 回调执行存在一定延迟
* 延迟取决于 `stim_dispatch()` 的调用频率
* 队列满时事件可能被丢弃

---

### 并发模型

softimer 内部采用`MPSC(Multi Producer Single Consumer)`模型

**生产者**

* Main Loop
* ISR
* RTOS Task

**消费者**

* stim_poll()

命令队列与事件队列均通过锁抽象保护

softimer 通过两个接口抽象平台相关的临界区实现：

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

以下 API 可在任意执行上下文中调用：

* `stim_start()`
* `stim_stop()`
* `stim_set_count()`
* `stim_get_count()`

以下 API 必须遵循单消费者模型：

* `stim_poll()`
* `stim_dispatch()`

即同一时刻只能由一个执行上下文调用

## API 参考

### stim_tick_inc

```c
void stim_tick_inc(void);
```

递增系统 Tick

该函数必须以固定周期调用，通常在 SysTick 中断服务函数中执行

---

### stim_init

```c
int stim_init(stim_t *timer,
              uint32_t period_ticks,
              stim_cb_mode_t cb_mode,
              stim_cb_t cb,
              void *user_data);
```

初始化定时器

**参数**

* `timer`：定时器对象
* `period_ticks`：定时器周期（单位：Tick），有效范围 `[1, 2147483647]`
* `cb_mode`：回调执行模式
* `cb`：回调函数，可为 `NULL`
* `user_data`：传递给回调函数的用户数据

**返回值**

* `0`：成功
* `-STIM_EINVAL`：参数非法

---

### stim_start

```c
int stim_start(stim_t *timer);
```

启动定时器

定时器会被插入内部有序链表，并按照到期时间自动排序

**返回值**

* `0`：成功
* `-STIM_EINVAL`：参数非法
* `-STIM_EAGAIN`：命令队列已满

---

### stim_stop

```c
int stim_stop(stim_t *timer);
```

停止定时器

定时器会从内部有序链表移出，该操作为异步操作，调用后不会立即生效，而是在 `stim_poll()` 处理命令时完成

**返回值**

* `0`：成功
* `-STIM_EINVAL`：参数非法
* `-STIM_EAGAIN`：命令队列已满

---

### stim_poll

```c
int stim_poll(void);
```

处理待执行命令并检查定时器是否到期

* 对于 `STIM_CB_MODE_DEFERRED`，产生到期事件并放入队列
* 对于 `STIM_CB_MODE_IMMEDIATE`，直接执行回调

**返回值**

* `0`：成功
* `-STIM_EAGAIN`：到期事件队列已满

---

### stim_dispatch

```c
void stim_dispatch(uint8_t max_event_num);
```

处理到期事件队列并执行回调

仅对 `STIM_CB_MODE_DEFERRED` 模式有效

**参数**

* `max_event_num`：单次调用处理事件的最大数量

---

### stim_set_count

```c
int stim_set_count(stim_t *timer, uint32_t count);
```

设置定时器事件计数值

**参数**

* `timer`：定时器对象
* `count`：计数值，传 `0` 可清零

**返回值**

* `0`：成功
* `-STIM_EINVAL`：参数非法

---

### stim_get_count

```c
int stim_get_count(const stim_t *timer, uint32_t *count);
```

获取定时器事件计数值

**参数**

* `timer`：定时器对象
* `count`：用于接收计数值的指针

**返回值**

* `0`：成功
* `-STIM_EINVAL`：参数非法

## 宏

### STIM_ATOMIC_TICKS

指定系统 Tick 是否支持原子读写

通常在 32 位和 64 位平台上可保持定义状态，对于 8 位和 16 位平台，必须取消定义该宏

### STIM_QUEUE_SIZE

命令队列与事件队列长度

要求：

* 必须为 2 的幂
* 不得超过 256

默认值：`16`

### STIM_MAX_TICKS

允许设置的最大定时器周期

### STIM_EINVAL

参数非法错误码

### STIM_EAGAIN

队列已满错误码
