<h1 align="center">softimer</h1>

<p align="center">
<a href="README.md">English</a> | <a href="README_zh.md">简体中文</a>
</p>

<p align="center">
Lightweight Embedded Software Timer Library
</p>

## Features

* O(1) expiration checking
* Ordered linked-list scheduler
* Overflow-safe tick comparison
* No dynamic memory allocation
* Platform-independent lock abstraction
* Supports deferred and immediate callbacks
* MPSC (Multi-Producer Single-Consumer) asynchronous control
* Event counting support

## Installation

### Git Submodule

```bash
git submodule add https://github.com/zhijian-yan/softimer.git
```

### Direct Integration

Add the following files to your project:

* `softimer.c`
* `softimer.h`

## Quick Start

### 1. Create a Timer

```c
stim_t timer;
```

### 2. Initialize the Timer

```c
stim_init(&timer, 100, STIM_CB_MODE_DEFERRED, timer_callback, NULL);
```

### 3. Start the Timer

```c
stim_start(&timer);
```

### 4. Update the System Tick

```c
void systick_handler(void) {
    stim_tick_inc();
}
```

### 5. Poll Timer State

```c
stim_poll();
```

### 6. Dispatch Timer Events

```c
while (1) {
    stim_dispatch(8);
}
```

### 7. Complete Example

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

## Design Overview

### Architecture

softimer uses an **MPSC (Multi-Producer Single-Consumer)** architecture for asynchronous timer control.

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
        │            Execute Deferred
        │              Callbacks
        │
        ├── Process Commands
        │
        ├── Check Expiration
        │
        ├── Update Timer State
        │
        └── Generate Events
               │
       ┌───────┴────────┐
       │                │
       ▼                ▼
 Immediate Callback  Event Queue
```

All timer management logic is performed inside `stim_poll()`.

Functions such as `stim_start()` and `stim_stop()` do not modify the timer list directly. Instead, they enqueue commands which are later processed by `stim_poll()`.

This design avoids concurrent modifications to the timer list from multiple execution contexts.

---

### Ordered Linked-List Scheduling

All active timers are maintained in ascending order of expiration time.

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
stim_start(timer);
```

The timer is inserted into the appropriate position to keep the list ordered.

As a result, the earliest expiring timer is always located at the head of the list.

---

### O(1) Expiration Check

Because timers are sorted by expiration time:

```text
Head
 │
 ▼
TimerA(100)
TimerB(200)
TimerC(500)
```

Only the head node needs to be checked:

```c
if ((int32_t)(timer->expire_ticks - now) <= 0)
```

If the head timer has not expired, all subsequent timers must also be unexpired.

Therefore, expiration checking is O(1) and does not require traversing the entire timer list.

---

### Overflow-Safe Tick Comparison

softimer compares time using signed subtraction:

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

the comparison remains valid.

To guarantee correctness:

```text
period_ticks <= INT32_MAX
             = STIM_MAX_TICKS
             = 2147483647
```

---

### Asynchronous Start and Stop

Starting and stopping timers does not immediately modify the timer list:

```c
stim_start(timer);
stim_stop(timer);
```

Instead, commands are posted to a command queue:

```text
Producer
    │
    ▼
Command Queue
    │
    ▼
stim_poll()
```

The actual operation is performed later by `stim_poll()`.

This allows these APIs to be safely called from:

* Main loop
* Interrupt service routines
* RTOS tasks

---

### Callback Execution Model

softimer supports two callback execution modes.

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
 Event Queue
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

### Concurrency Model

softimer internally uses an **MPSC (Multi-Producer Single-Consumer)** model.

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

The following APIs may be called from any execution context:

* `stim_start()`
* `stim_stop()`
* `stim_set_count()`
* `stim_get_count()`

The following APIs must follow the single-consumer rule:

* `stim_poll()`
* `stim_dispatch()`

Only one execution context may call them at a time.

## API Reference

### stim_tick_inc

```c
void stim_tick_inc(void);
```

Increment the system tick.

This function should be called periodically, typically from a SysTick interrupt handler.

---

### stim_init

```c
int stim_init(stim_t *timer,
              uint32_t period_ticks,
              stim_cb_mode_t cb_mode,
              stim_cb_t cb,
              void *user_data);
```

Initialize a timer.

**Parameters**

* `timer` - Timer object
* `period_ticks` - Timer period in ticks, range `[1, 2147483647]`
* `cb_mode` - Callback execution mode
* `cb` - Callback function, may be `NULL`
* `user_data` - User-defined callback parameter

**Returns**

* `0` - Success
* `-STIM_EINVAL` - Invalid parameter

---

### stim_start

```c
int stim_start(stim_t *timer);
```

Start a timer.

The timer is inserted into the internal ordered timer list.

**Returns**

* `0` - Success
* `-STIM_EINVAL` - Invalid parameter
* `-STIM_EAGAIN` - Command queue full

---

### stim_stop

```c
int stim_stop(stim_t *timer);
```

Stop a timer.

This operation is asynchronous and takes effect when processed by `stim_poll()`.

**Returns**

* `0` - Success
* `-STIM_EINVAL` - Invalid parameter
* `-STIM_EAGAIN` - Command queue full

---

### stim_poll

```c
int stim_poll(void);
```

Process pending commands and check timer expiration.

* Generates expiration events for `STIM_CB_MODE_DEFERRED`
* Executes callbacks directly for `STIM_CB_MODE_IMMEDIATE`

**Returns**

* `0` - Success
* `-STIM_EAGAIN` - Event queue full

---

### stim_dispatch

```c
void stim_dispatch(uint8_t max_event_num);
```

Process expiration events and execute callbacks.

Only applicable to `STIM_CB_MODE_DEFERRED`.

**Parameters**

* `max_event_num` - maximum number of events processed in a single call

---

### stim_set_count

```c
int stim_set_count(stim_t *timer, uint32_t count);
```

Set the timer event count.

**Parameters**

* `timer` - Timer object
* `count` - Event count, pass `0` to reset

**Returns**

* `0` - Success
* `-STIM_EINVAL` - Invalid parameter

---

### stim_get_count

```c
int stim_get_count(const stim_t *timer, uint32_t *count);
```

Get the timer event count.

**Parameters**

* `timer` - Timer object
* `count` - Pointer to store the event count

**Returns**

* `0` - Success
* `-STIM_EINVAL` - Invalid parameter

## Macros

### STIM_ATOMIC_TICKS

Indicates whether system tick reads and writes are atomic.

Typically enabled on 32-bit and 64-bit platforms.

For 8-bit and 16-bit platforms, this macro should be undefined.

### STIM_QUEUE_SIZE

Length of both the command queue and event queue.

Requirements:

* Must be a power of two
* Must not exceed 256

Default value: `16`

### STIM_MAX_TICKS

Maximum allowed timer period.

### STIM_EINVAL

Invalid parameter error code.

### STIM_EAGAIN

Queue full error code.
