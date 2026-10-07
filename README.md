<h1 align="center">simpletimer</h1>

<p align="center">
<a href="README.md">English</a> | <a href="README_zh.md">简体中文</a>
</p>

<p align="center">
Lightweight Embedded Software Timer Library
</p>

<p align="center">
<a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square"></a>
<img alt="Language" src="https://img.shields.io/badge/language-C99-blue.svg?style=flat-square">
<img alt="Dependencies" src="https://img.shields.io/badge/dependencies-none-brightgreen.svg?style=flat-square">
<img alt="Dynamic memory" src="https://img.shields.io/badge/dynamic_memory-none-brightgreen.svg?style=flat-square">
<img alt="Platform" src="https://img.shields.io/badge/platform-bare--metal_%7C_RTOS_%7C_Linux-lightgrey.svg?style=flat-square">
</p>

## Features

* Group-based management: the timers of a group share the timebase, the callback and the queues
* Periods configurable from 1 to 2^31-1 ticks of the group timebase
* Ordered intrusive doubly-linked list scheduler; the timer that expires first is at the head
* O(1) expiration check and overflow-safe tick comparison
* Catch-up: a timer that is behind reports one expiration per elapsed period
* Start and stop carried out asynchronously through a command queue, callable from an interrupt handler, a task or the main loop
* Immediate and deferred callbacks
* A per-timer expiration count, readable and writable atomically
* Expiration event queue buffer provided by the application; dropped events are counted and reported
* The expiration callback may be omitted, in which case expirations are only counted
* No dynamic memory allocation, no OS dependency, C99
* The platform-specific critical section and ordering barriers are isolated behind porting hooks

## Porting

Porting simpletimer means implementing the four platform hooks directly in `simpletimer_port.h`; that file belongs to the application, so keep your own version when the library is updated.

### Hooks

| Hook | Role |
| ---- | ---- |
| `int stim_lock(void)` | Enters the critical section around the shared state (timebase, queue indices, timer event count) and returns the previous state |
| `void stim_unlock(int state)` | Leaves the critical section and restores the state returned by `stim_lock()` |
| `STIM_ACQUIRE()` | Ordering barrier taken after a queue index has been loaded and before the entry it publishes is read |
| `STIM_RELEASE()` | Ordering barrier taken right before the queue index that publishes an entry is stored |

### Default implementation

The shipped hooks are all no-ops. They can be used as they are, without writing any porting code, on a 32-bit single-core target or while a group is used by a single execution context: the timebase, the queue indices and the expiration count are all naturally aligned values of 32 bits or less, so a single-context access cannot be torn and no ordering barrier is needed.

### Example: bare metal

The timebase is normally incremented from a SysTick or timer interrupt handler while the main loop polls, so the lock has to save and restore the interrupt mask. A single core needs no ordering barrier, so `STIM_ACQUIRE()` / `STIM_RELEASE()` keep their shipped definition.

```c
/* simpletimer_port.h */
#include "cmsis_compiler.h" /* CMSIS 5; include core_cm*.h on older versions */

static inline int stim_lock(void) {
    int state = __get_PRIMASK();
    __disable_irq();
    return state;
}

static inline void stim_unlock(int stim_lock_state) {
    __set_PRIMASK(stim_lock_state);
}
```

### Example: RTOS

Use the critical section of the RTOS: disabling interrupts alone does not cover a task switch inside the critical section. `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` are nestable, so the saved state is unused; with RT-Thread use `rt_enter_critical()` / `rt_exit_critical()`. A mutex is not an option here, because the hooks are also taken by `stim_start_timer()` and `stim_stop_timer()`, which may be called from an interrupt handler.

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

### Example: multi-core

A lock shared by every core and real ordering barriers are both required: `volatile` alone provides no ordering, so one core could otherwise observe a queue index before the entry it publishes. Mask the local interrupts before taking the spinlock, because a spinlock that does not mask them deadlocks as soon as an ISR preempts the context holding it.

```c
/* simpletimer_port.h */
#include "cmsis_compiler.h"

/* stim_spinlock and spin_lock() / spin_unlock() come from the SoC */
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

### Example: hosted target (Linux)

For host tools, unit tests, simulations and user-space Linux drivers, the default hooks are enough while a single thread uses the group. When several threads share a group, use a mutex and real fences if the host has a weak memory model.

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

`simpletimer_port.h` is expanded once per translation unit that includes the library header, so the mutex above exists once per translation unit: when several modules use the library, define the mutex in one `.c` file and declare it `extern` here instead.

### Notes

* Keep the hooks stateless and let a nested lock/unlock pair restore the state of the outermost pair: a `static inline` definition is private to each translation unit, so a function-local static variable would exist once per translation unit instead of once per program
* The lock is taken by every producer (`stim_timebase_inc()`, `stim_start_timer()`, `stim_stop_timer()`, `stim_set_event_count()` and `stim_get_event_count()`) and by `stim_poll()`, which locks the timebase, the event count and the queue indices it touches

## Usage

### 1. Define the objects and the callback

```c
#include "simpletimer.h"

#define TIMER_NUM       2
#define HEARTBEAT_TICKS 1000 /* 1 s with a 1 ms timebase */
#define SAMPLE_TICKS    100  /* 100 ms */

static stim_t timers[TIMER_NUM];
static stim_group_t group;
static stim_message_t command_buffer[16]; /* start/stop commands */
static stim_message_t expired_buffer[16]; /* expiration events, deferred mode */

/* Set by the callback, handled by the main loop */
static volatile uint8_t heartbeat_flag;
static volatile uint8_t sample_flag;

static void timer_expired_cb(stim_t *timer) {
    if (timer == &timers[0])
        heartbeat_flag = 1;
    else
        sample_flag = 1;
}
```

The callback receives the timer, so the timers are told apart by comparing that pointer, or through `timer->user_data`, which is stored by `stim_init_timer()`.

### 2. Initialize the group and the timers

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

(void)stim_start_timer(&timers[0], &group); /* non-zero: command queue full */
(void)stim_start_timer(&timers[1], &group);
```

* `command_buffer` / `command_queue_size` are mandatory; `expired_buffer` / `expired_queue_size` are required in `STIM_CALLBACK_MODE_DEFERRED` only
* Both sizes must be a power of two in `[STIM_MIN_QUEUE_SIZE, STIM_MAX_QUEUE_SIZE]`. One slot of each ring stays free, so 16 elements hold at most 15 pending commands or events
* `expired_cb` may be a null pointer (`0`), in which case expirations are only counted
* Both buffers are referenced rather than copied and must stay valid for the lifetime of the group
* The period must be in `[1, STIM_MAX_PERIOD_TICKS]`; `stim_init_timer()` clears the timer state, so the period and the user data are passed there
* Start and stop are asynchronous: they only post a command, and the timer really starts or stops when `stim_poll()` processes it
* A running timer must not be copied or moved, because the group keeps a pointer to the node embedded in it, and `stim_init_timer()` must not be called on a timer that is still linked in a group

### 3. Update the timebase, poll and dispatch

```c
/* Called by the 1 ms SysTick interrupt */
void systick_handler(void) {
    stim_timebase_inc(&group);
}

for (;;) {
    stim_poll(&group);        /* applies the commands, checks the expirations */
    stim_dispatch(8, &group); /* deferred mode only */
    /* handle heartbeat_flag and sample_flag here */
}
```

Call `stim_timebase_inc()` at a fixed rate for every group: one tick of the timebase is the resolution of every period. `stim_poll()` either calls the expiration callback (`STIM_CALLBACK_MODE_IMMEDIATE`) or queues the events (`STIM_CALLBACK_MODE_DEFERRED`), and returns the number of events dropped because the expired-event queue was full. Keep an immediate callback short, because it runs inside `stim_poll()`. Within a group, `stim_poll()` and `stim_dispatch()` each accept a single execution context, `stim_poll()` must not be called from a callback, and the interval between two `stim_poll()` calls must stay below 2^31 ticks.

### 4. Complete example

```c
#include "simpletimer.h"

#define TIMER_NUM       2
#define HEARTBEAT_TICKS 1000 /* 1 s with a 1 ms timebase */
#define SAMPLE_TICKS    100  /* 100 ms */

static stim_t timers[TIMER_NUM];
static stim_group_t group;
static stim_message_t command_buffer[16];
static stim_message_t expired_buffer[16];

/* Set by the callback, handled by the main loop */
static volatile uint8_t heartbeat_flag;
static volatile uint8_t sample_flag;

static void timer_expired_cb(stim_t *timer) {
    if (timer == &timers[0])
        heartbeat_flag = 1;
    else
        sample_flag = 1;
}

/* Called by the 1 ms SysTick interrupt */
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
        /* handle heartbeat_flag and sample_flag here */
    }
}
```
