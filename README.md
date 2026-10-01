<h1 align="center">simpletimer</h1>

<p align="center">
<a href="README.md">English</a> | <a href="README_zh.md">简体中文</a>
</p>

<p align="center">
Lightweight Embedded Software Timer Library
</p>

## Features

* Group-based timer management: timers in the same group share a timebase, callback, and queues
* O(1) expiration checking
* Ordered doubly-linked-list scheduler
* Overflow-safe tick comparison
* No dynamic memory allocation
* Platform-independent lock abstraction
* Immediate and deferred callback execution modes
* MPSC (Multi-Producer Single-Consumer) asynchronous start/stop
* Event counting support

## Installation

### Git Submodule

```bash
git submodule add https://github.com/zhijian-yan/simpletimer.git
```

### Direct Integration

Add the following files to your project:

* `simpletimer.c`
* `simpletimer.h`

## Quick Start

### 1. Create Timers, a Group, and Queue Buffers

```c
#define TIMER_NUM 2

stim_t timers[TIMER_NUM];
stim_group_t group;
stim_message_t command_buffer[16];
stim_message_t expired_buffer[16];
```

`stim_t` and `stim_group_t` are initialized by `stim_init_timer()` and `stim_init_group()` respectively.

### 2. Configure and Initialize the Group

```c
stim_group_config_t config = {
    .cb = stim_callback,
    .cb_mode = STIM_CB_MODE_DEFERRED,
    .command_buffer = command_buffer,
    .command_length = 16,
    .expired_buffer = expired_buffer,
    .expired_length = 16,
};
stim_init_group(&group, &config);
```

`command_buffer` and `command_length` are required; `expired_buffer` / `expired_length` are only needed in deferred mode.

### 3. Initialize the Timers

```c
stim_init_timer(&timers[0], 1000, (void *)1);
stim_init_timer(&timers[1], 100, (void *)2);
```

`stim_init_timer()` clears the timer state, so user data must be passed here.

### 4. Implement the Callback

```c
void stim_callback(stim_t *timer) {
    switch ((int)timer->user_data) {
    case 1:
        printf("timer1 count:%u\r\n", stim_get_count(timer));
        break;
    case 2:
        led_toggle();
        break;
    }
}
```

### 5. Start the Timers

```c
stim_start_timer(&timers[0], &group);
stim_start_timer(&timers[1], &group);
```

### 6. Update the System Timebase

```c
void systick_handler(void) {
    stim_timebase_inc(&group);
}
```

### 7. Poll and Dispatch

```c
while (1) {
    stim_poll(&group);
    stim_dispatch(8, &group);
}
```

`stim_dispatch()` is only required in `STIM_CB_MODE_DEFERRED` mode.

### 8. Complete Example

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
        printf("timer1 count:%u\r\n", stim_get_count(timer));
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
        .cb = stim_callback,
        .cb_mode = STIM_CB_MODE_DEFERRED,
        .command_buffer = command_buffer,
        .command_length = 16,
        .expired_buffer = expired_buffer,
        .expired_length = 16,
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

## Design

### Group Model

Timers are managed in units of "groups":

* `stim_t` holds the period, expiration time, count, and list node of a single timer
* `stim_group_t` holds the timebase, callback, callback mode, and the two queues shared by a group of timers

Benefits of the group model:

* The timebase is maintained per group (`group->timebase_ticks`), so different groups may use different timebases
* The callback and callback mode are configured once per group, reducing per-timer storage
* The command queue and expired-event queue are embedded in the group, avoiding global state

### Architecture

simpletimer uses an **MPSC (Multi-Producer Single-Consumer)** architecture for asynchronous timer control.

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
   stim_poll(group)   stim_dispatch(num, group)
        │                     │
        │                     ▼
        │            Execute Deferred
        │              Callbacks
        │
        ├── Process Commands
        │
        ├── Check Head Expiration
        │
        ├── Update Expiration / Count
        │   and Re-insert
        │
        └── Generate Events
               │
       ┌───────┴────────┐
       │                │
       ▼                ▼
 Immediate Callback  Event Queue
```

All timer management logic is performed inside `stim_poll()`. `stim_start_timer()` and `stim_stop_timer()` do not modify the timer list directly; they enqueue commands that `stim_poll()` processes later.

This design avoids concurrent modifications to the timer list from multiple execution contexts.

---

### Ordered Doubly-Linked-List Scheduling

All active timers are linked through an embedded `struct stim_node` in ascending order of expiration time.

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

When starting a timer:

```c
stim_start_timer(timer, group);
```

the command is processed inside `stim_poll()`, which inserts the timer at the correct position to keep the list ordered. As a result, the earliest expiring timer is always at the head of the list.

Using a doubly-linked list with embedded nodes (`stim_container_of`) means **no dynamic memory allocation is required**.

---

### O(1) Expiration Check

Because the list is sorted by expiration time, only the head node needs to be checked on each poll:

```c
if ((int32_t)(timer->expire_ticks - now) <= 0)
```

If the head timer has not expired, all subsequent timers must also be unexpired. Therefore, expiration checking is O(1) and does not require traversing the list.

After a timer expires, it is re-inserted into the list so that subsequent checks remain valid.

---

### Overflow-Safe Tick Comparison

simpletimer compares time using signed subtraction:

```c
(int32_t)(expire_ticks - now)
```

Example:

```text
expire = 0x00000010
now    = 0xFFFFFFF0
```

Even when the system tick wraps around:

```text
0xFFFFFFFF → 0x00000000
```

the comparison remains valid. To guarantee correctness:

```text
period_ticks <= INT32_MAX
             = STIM_MAX_TICKS
             = 2147483647
```

---

### Asynchronous Start and Stop

Starting and stopping timers does not immediately modify the timer list:

```c
stim_start_timer(timer, group);
stim_stop_timer(timer, group);
```

These calls only post a request to the command queue:

```text
Producer
    │
    ▼
Command Queue
    │
    ▼
stim_poll()
```

The actual operation is performed later by `stim_poll()`. This allows these APIs to be safely called from:

* Main loop
* Interrupt service routines
* RTOS tasks

---

### Expiration Processing

Each `stim_poll()` call first processes the command queue and then loops over the head of the list:

```text
stim_poll(group)
    │
    ├── Process commands (start / stop)
    │
    └── while (head timer expired)
            ├── remove from list
            ├── expire_ticks += period_ticks
            ├── count += 1
            ├── re-insert into list
            └── immediate mode ? invoke callback : enqueue event
```

The loop stops as soon as the head timer is not expired, keeping the expiration check O(1).

---

### Callback Execution Model

simpletimer supports two callback execution modes.

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

The callback is executed immediately when the timer expires.

**Advantages**

* Minimum latency
* No event loss
* Suitable for short operations

**Limitations**

* Blocking APIs should not be called
* Not suitable for time-consuming tasks

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

Expiration events are first queued and later executed by `stim_dispatch()`.

**Advantages**

* Supports long-running operations
* Blocking APIs are allowed
* Safe to use functions such as `printf()` and `malloc()`

**Limitations**

* Callback execution is delayed
* Latency depends on the frequency of `stim_dispatch()`
* Events may be dropped when the queue is full

---

### Command Queue and Event Queue

Each group embeds two ring buffers, both backed by user-provided storage:

| Queue           | Producer                                        | Consumer          | Purpose                       |
| --------------- | ----------------------------------------------- | ----------------- | ----------------------------- |
| `command_queue` | `stim_start_timer()` / `stim_stop_timer()`      | `stim_poll()`     | asynchronous start/stop       |
| `expired_queue` | `stim_poll()`                                   | `stim_dispatch()` | expiration events (deferred)  |

```c
typedef struct {
    stim_message_t *buffer;       /* user-provided buffer */
    uint8_t length;               /* must be a power of two (max 128 for uint8_t) */
    volatile uint8_t write_index;
    volatile uint8_t read_index;
} stim_queue_t;
```

Requirements:

* When `buffer` is not `NULL`, `length` must be a power of two
* The command queue buffer is mandatory
* The expired-event queue is only needed in `STIM_CB_MODE_DEFERRED` mode
* When a queue is full, events are dropped and the return value of `stim_poll()` accumulates the number of drops

---

### Concurrency Model

simpletimer internally uses an **MPSC (Multi-Producer Single-Consumer)** model.

**Producers**

* Main Loop
* ISR
* RTOS Tasks

**Consumer**

* `stim_poll()`

Both command and event queues are protected by a lock abstraction.

Platform-specific critical sections are abstracted through:

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

The default implementation is a no-op; platforms that require interrupt safety can provide their own implementation.

The following APIs may be called from any execution context:

* `stim_timebase_inc()`
* `stim_start_timer()`
* `stim_stop_timer()`
* `stim_set_count()`
* `stim_get_count()`

The following APIs must follow the single-consumer rule:

* `stim_poll()`
* `stim_dispatch()`

Only one execution context may call them at a time.

## API Reference

### stim_timebase_inc

```c
void stim_timebase_inc(stim_group_t *group);
```

Increment the group timebase.

This function should be called periodically, typically from a SysTick interrupt handler.

**Parameters**

* `group` - Group the timers belong to; must not be `NULL`

---

### stim_init_group

```c
void stim_init_group(stim_group_t *group, stim_group_config_t *config);
```

Initialize a group.

**Parameters**

* `group` - Group object
* `config` - Group configuration; must not be `NULL`, and `config->command_buffer` must not be `NULL`

**Notes**

* `config->command_length` must be a power of two
* `config->expired_buffer` / `config->expired_length` are used in deferred mode

---

### stim_init_timer

```c
void stim_init_timer(stim_t *timer, uint32_t period_ticks, void *user_data);
```

Initialize a timer.

This function clears the timer state and sets its period and user data; the timer starts in the stopped state.

**Parameters**

* `timer` - Timer object
* `period_ticks` - Timer period in ticks, range `[1, 2147483647]`
* `user_data` - User data, accessible in the callback via `timer->user_data`

---

### stim_start_timer

```c
int stim_start_timer(stim_t *timer, stim_group_t *group);
```

Start a timer.

Posts a start command to the group's command queue; the timer is actually started by `stim_poll()`.

**Parameters**

* `timer` - Timer object
* `group` - Group the timer belongs to

**Returns**

* `0` - Success
* non-zero - Command queue full

---

### stim_stop_timer

```c
int stim_stop_timer(stim_t *timer, stim_group_t *group);
```

Stop a timer.

Posts a stop command to the group's command queue. This operation is asynchronous and takes effect when processed by `stim_poll()`.

**Parameters**

* `timer` - Timer object
* `group` - Group the timer belongs to

**Returns**

* `0` - Success
* non-zero - Command queue full

---

### stim_poll

```c
int stim_poll(stim_group_t *group);
```

Process pending commands and check timer expiration.

* Executes callbacks directly for `STIM_CB_MODE_IMMEDIATE`
* Generates expiration events for `STIM_CB_MODE_DEFERRED`

**Parameters**

* `group` - Group the timers belong to

**Returns**

* Number of events dropped because the expired-event queue was full (always `0` in immediate mode)

---

### stim_dispatch

```c
void stim_dispatch(uint8_t max_event_num, stim_group_t *group);
```

Process expiration events and execute callbacks.

Only applicable to `STIM_CB_MODE_DEFERRED`.

**Parameters**

* `max_event_num` - Maximum number of events processed in a single call
* `group` - Group the timers belong to

---

### stim_set_count

```c
void stim_set_count(stim_t *timer, uint32_t count);
```

Set the timer event count.

This function is internally protected by the lock abstraction and may be called from any execution context.

**Parameters**

* `timer` - Timer object
* `count` - Event count, pass `0` to reset

**Notes**

`stim_t.count` is actually of type `uint16_t`; out-of-range values are truncated.

---

### stim_get_count

```c
uint16_t stim_get_count(const stim_t *timer);
```

Get the timer event count.

This function is internally protected by the lock abstraction and may be called from any execution context.

**Parameters**

* `timer` - Timer object

**Returns**

* Current event count

---

### stim_lock / stim_unlock

```c
static inline int stim_lock(void);
static inline void stim_unlock(int stim_lock_state);
```

Platform-specific lock abstraction.

The lock state returned by `stim_lock()` is passed back to `stim_unlock()` to restore the critical section.

The default implementation is a no-op; platforms that require interrupt safety can provide their own implementation.

## Data Structures

### stim_t

```c
typedef struct {
    struct stim_node node;
    void *user_data;
    uint32_t expire_ticks;
    uint32_t period_ticks;
    volatile uint16_t count;
    uint8_t state;
} stim_t;
```

* `node` - embedded ordered-list node
* `user_data` - user data, set via `stim_init_timer()`
* `expire_ticks` - absolute tick of the next expiration
* `period_ticks` - timer period
* `count` - expiration count, incremented on each expiration
* `state` - running state (stopped / running)

Must be initialized via `stim_init_timer()`.

### stim_message_t

```c
typedef struct {
    stim_t *timer;
    uint8_t command;
} stim_message_t;
```

A single message in the command and event queues.

### stim_queue_t

```c
typedef struct {
    stim_message_t *buffer;
    uint8_t length;
    volatile uint8_t write_index;
    volatile uint8_t read_index;
} stim_queue_t;
```

Ring buffer.

### stim_group_t

```c
typedef struct {
    volatile uint32_t timebase_ticks;
    void (*cb)(stim_t *timer);
    stim_cb_mode_t cb_mode;
    stim_queue_t command_queue;
    stim_queue_t expired_queue;
    struct stim_node head;
} stim_group_t;
```

Timer group.

* `timebase_ticks` - group timebase, incremented by `stim_timebase_inc()`
* `cb` - expiration callback, called with the timer pointer
* `cb_mode` - callback execution mode
* `command_queue` - start/stop command queue
* `expired_queue` - expiration event queue used in deferred mode
* `head` - ordered-list head node

Must be initialized via `stim_init_group()`.

### stim_group_config_t

```c
typedef struct {
    void (*cb)(stim_t *timer);
    stim_cb_mode_t cb_mode;
    stim_message_t *command_buffer;
    stim_message_t *expired_buffer;
    uint8_t command_length;
    uint8_t expired_length;
} stim_group_config_t;
```

Group initialization configuration.

* `cb` - expiration callback
* `cb_mode` - callback execution mode
* `command_buffer` / `command_length` - command queue buffer and length (required)
* `expired_buffer` / `expired_length` - expiration queue buffer and length (needed in deferred mode)

## Macros and Enums

### stim_cb_mode_t

```c
typedef enum {
    STIM_CB_MODE_DEFERRED = 0,
    STIM_CB_MODE_IMMEDIATE,
} stim_cb_mode_t;
```

Callback execution mode.

* `STIM_CB_MODE_DEFERRED` - expiration events are queued and executed by `stim_dispatch()`
* `STIM_CB_MODE_IMMEDIATE` - the callback is executed in `stim_poll()` on expiration

### STIM_MAX_TICKS

Maximum allowed timer period, `((uint32_t)(-1)) >> 1` (`0x7FFFFFFF`, i.e. `INT32_MAX`).

This value keeps the signed-difference comparison correct even when the tick wraps around.
