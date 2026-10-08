// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Zhijian Yan

/**
 * @file
 * @brief Public interface of simpletimer.
 *
 * Declares the timer object, the timer group that schedules it, and the queues
 * a group is configured with. The platform hooks live in simpletimer_port.h.
 *
 * A group is set up with stim_init_group() and its timers with
 * stim_init_timer(); the timers are then driven by stim_timebase_inc() and
 * stim_poll().
 */

#ifndef SIMPLETIMER_H
#define SIMPLETIMER_H

#include <stdint.h>

/**
 * @brief Platform port header.
 *
 * Define this macro to the name of the header providing the platform hooks
 * (stim_lock(), stim_unlock(), STIM_ACQUIRE() and STIM_RELEASE()) when the port
 * file lives outside the library tree, for example
 * -DSTIM_PORT_HEADER='"my_port.h"'. When it is not defined, the shipped
 * "simpletimer_port.h" next to this header is used.
 */
#ifdef STIM_PORT_HEADER
#include STIM_PORT_HEADER
#else
#include "simpletimer_port.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum timer period in ticks.
 *
 * A period must be in [1, STIM_MAX_PERIOD_TICKS]. Keeping it below 2^31 lets
 * stim_poll() compare ticks with a plain signed difference, so expiration stays
 * correct when the timebase wraps around.
 */
#define STIM_MAX_PERIOD_TICKS (((uint32_t)(-1)) >> 1)
/**
 * @brief Maximum number of elements of a queue buffer.
 *
 * @note The ring buffer always leaves one slot free, so a queue created with
 *       this size holds at most 255 messages.
 */
#define STIM_MAX_QUEUE_SIZE   (256)
/**
 * @brief Minimum number of elements of a queue buffer.
 *
 * @note A queue created with this size holds at most one message.
 */
#define STIM_MIN_QUEUE_SIZE   (2)

/**
 * @brief Callback execution mode.
 *
 * Selects when the expiration callback runs.
 */
typedef enum {
    /** Expiration events are queued and executed later by stim_dispatch(). */
    STIM_CALLBACK_MODE_DEFERRED = 0,
    /** The callback is executed in stim_poll() as soon as a timer expires. */
    STIM_CALLBACK_MODE_IMMEDIATE,
} stim_callback_mode_t;

/**
 * @brief Intrusive list node.
 *
 * Links a stim_t into the ordered list of the group it runs in. Reserved for
 * internal use: the node is managed by the library and must not be modified by
 * the application.
 */
struct stim_node {
    /** Next node; points to itself while the timer is not linked. */
    struct stim_node *next;
    /** Previous node; points to itself while the timer is not linked. */
    struct stim_node *prev;
};

/**
 * @brief Timer object.
 *
 * Create it with stim_init_timer(). A timer may be linked into one group at a
 * time, and because the group keeps a pointer to its node field, a timer that
 * is running must not be copied or moved in memory.
 */
typedef struct {
    /** Internal ordered-list node linking the timer into its group. */
    struct stim_node node;
    /** User data, available to the expired callback as timer->user_data. */
    void *user_data;
    /**
     * Absolute timebase tick of the next expiration; managed by the library.
     */
    uint32_t expire_ticks;
    /** Timer period in ticks, set by stim_init_timer(). */
    uint32_t period_ticks;
    /**
     * Number of expirations so far, incremented once per elapsed period; read
     * and written through stim_get_event_count() and stim_set_event_count(),
     * which guard it with the lock abstraction.
     */
    volatile uint16_t event_count;
    /** Internal state of the timer: 0 = stopped, 1 = running. */
    uint8_t state;
} stim_t;

/**
 * @brief Queue message.
 *
 * One entry of the command queue or of the expired-event queue. Applications
 * only need this type to provide queue storage; message contents are managed by
 * the library.
 */
typedef struct {
    /** Timer the message refers to. */
    stim_t *timer;
    /** Command to apply to the timer; only used in the command queue. */
    uint8_t command;
} stim_message_t;

/**
 * @brief Single-producer single-consumer ring buffer.
 *
 * The storage comes from the application through stim_group_config_t and is
 * referenced rather than copied, so it must stay valid for the lifetime of the
 * group.
 */
typedef struct {
    /** Message storage; points to the buffer passed in the configuration. */
    stim_message_t *buffer;
    /**
     * Index mask derived from the configured queue size: buffer holds capacity
     * + 1 elements and at most capacity messages can be queued.
     */
    uint8_t capacity;
    /** Index of the next message to write; managed by the producers. */
    volatile uint8_t write_index;
    /** Index of the next message to read; managed by the consumer. */
    volatile uint8_t read_index;
} stim_queue_t;

/**
 * @brief Timer group.
 *
 * Holds the timebase, the callbacks and the queues shared by a set of timers,
 * together with the ordered list of the timers currently running. Create it
 * with stim_init_group() rather than filling it in by hand.
 */
typedef struct {
    /** Free-running tick counter, incremented by stim_timebase_inc(). */
    volatile uint32_t timebase_ticks;
    /**
     * Expiration callback, invoked with the expired timer; may be NULL, in
     * which case expirations are only counted.
     */
    void (*expired_cb)(stim_t *timer);
    /** Callback execution mode, copied from the configuration. */
    stim_callback_mode_t callback_mode;
    /** Start and stop command queue. */
    stim_queue_t command_queue;
    /** Expired-event queue, used in STIM_CALLBACK_MODE_DEFERRED mode. */
    stim_queue_t expired_queue;
    /** Internal head of the ordered list of running timers. */
    struct stim_node head;
} stim_group_t;

/**
 * @brief Group initialization configuration.
 *
 * Passed to stim_init_group(), which copies every field; the queue buffers it
 * points to are referenced rather than copied and must stay valid for the
 * lifetime of the group.
 */
typedef struct {
    /**
     * Expiration callback; may be NULL, in which case expirations are only
     * counted.
     */
    void (*expired_cb)(stim_t *timer);
    /** Callback execution mode. */
    stim_callback_mode_t callback_mode;
    /** Storage for the command queue; must not be NULL. */
    stim_message_t *command_buffer;
    /** Storage for the expired-event queue; required in deferred mode. */
    stim_message_t *expired_buffer;
    /**
     * Number of elements of command_buffer: a power of two in
     * [STIM_MIN_QUEUE_SIZE, STIM_MAX_QUEUE_SIZE].
     */
    uint16_t command_queue_size;
    /**
     * Number of elements of expired_buffer: same rule as command_queue_size,
     * and required in deferred mode.
     */
    uint16_t expired_queue_size;
} stim_group_config_t;

/**
 * @brief Increment the group timebase by one tick.
 *
 * Call this periodically, typically from a SysTick interrupt handler, to drive
 * every timer of the group.
 *
 * @param group Group the timers belong to; must not be NULL.
 */
void stim_timebase_inc(stim_group_t *group);
/**
 * @brief Initialize a timer.
 *
 * Clears the timer state, stores the period and the user data, and leaves the
 * timer in the stopped state.
 *
 * @param timer Timer object; must not be NULL.
 * @param period_ticks Timer period in ticks; must be in the range
 *                     [1, STIM_MAX_PERIOD_TICKS].
 * @param user_data User data, available to the callback as timer->user_data;
 *                  may be NULL.
 *
 * @note The timer must not be linked in a group's timer list when this function
 *       is called, so stop it with stim_stop_timer() and let stim_poll()
 *       process the stop command first.
 */
void stim_init_timer(stim_t *timer, uint32_t period_ticks, void *user_data);
/**
 * @brief Initialize a group.
 *
 * Copies the configuration into the group and empties its command queue, its
 * expired-event queue and its timer list.
 *
 * @param group Group object; must not be NULL.
 * @param config Group configuration; must not be NULL and is only read.
 *
 * @note config->command_buffer must not be NULL, and
 *       config->command_queue_size must be a power of two between
 *       STIM_MIN_QUEUE_SIZE and STIM_MAX_QUEUE_SIZE.
 * @note config->expired_buffer and config->expired_queue_size follow the same
 *       rule and are required in STIM_CALLBACK_MODE_DEFERRED mode.
 * @note The queue buffers are referenced, not copied, so both must stay valid
 *       for the lifetime of the group.
 */
void stim_init_group(stim_group_t *group, stim_group_config_t *config);
/**
 * @brief Start a timer.
 *
 * Posts a start command to the group's command queue; the timer is started when
 * stim_poll() processes that command.
 *
 * @param timer Timer object; must have been initialized with stim_init_timer()
 *              and must belong to group.
 * @param group Group the timer belongs to; must not be NULL.
 * @return 0 on success, non-zero if the command queue is full.
 *
 * @note Starting a timer that is already running has no effect.
 * @note A timer may be used with one group at a time.
 */
int stim_start_timer(stim_t *timer, stim_group_t *group);
/**
 * @brief Stop a timer.
 *
 * Posts a stop command to the group's command queue; the timer is stopped when
 * stim_poll() processes that command.
 *
 * @param timer Timer object; must be the timer that was started with group.
 * @param group Group the timer belongs to; must not be NULL.
 * @return 0 on success, non-zero if the command queue is full.
 *
 * @note This operation is asynchronous: it takes effect only once stim_poll()
 *       has run.
 * @note Stopping a timer that is not running has no effect.
 */
int stim_stop_timer(stim_t *timer, stim_group_t *group);
/**
 * @brief Process pending commands and check timer expiration.
 *
 * Applies every queued start/stop command, then handles each timer whose
 * expiration has been reached: the timer is rescheduled by adding its period,
 * its event count is incremented, and either the expiration callback is invoked
 * right away (STIM_CALLBACK_MODE_IMMEDIATE) or an expiration event is queued
 * for stim_dispatch() (STIM_CALLBACK_MODE_DEFERRED). A timer that is behind by
 * several periods produces one event per elapsed period.
 *
 * @param group Group the timers belong to; must not be NULL.
 * @return Number of expiration events dropped because the expired-event queue
 *         was full; always 0 in immediate mode.
 *
 * @note For a given group, only one execution context may call this function at
 *       a time, and it must not be called from a callback.
 * @note Different groups are independent of each other and may be polled from
 *       different execution contexts.
 * @note The interval between two calls must stay below 2^31 ticks so that the
 *       overflow-safe tick comparison remains valid.
 */
int stim_poll(stim_group_t *group);
/**
 * @brief Process expiration events and run the expiration callback.
 *
 * Drains at most max_event_count events from the group's expired-event queue.
 * Only applicable to STIM_CALLBACK_MODE_DEFERRED.
 *
 * @param max_event_count Maximum number of events processed by this call.
 * @param group Group the timers belong to; must not be NULL.
 *
 * @note For a given group, only one execution context may call this function at
 *       a time: it is the single-consumer side of the group's expired-event
 *       queue, while stim_poll() is the producer.
 * @note Different groups are independent of each other and may be dispatched
 *       from different execution contexts.
 */
void stim_dispatch(uint8_t max_event_count, stim_group_t *group);
/**
 * @brief Set the timer event count.
 *
 * The write is protected by the lock abstraction, so this function may be
 * called from any execution context.
 *
 * @param timer Timer object; must not be NULL.
 * @param event_count New event count; pass 0 to reset the counter.
 *
 * @note The count is a uint16_t and wraps around after 65535 expirations.
 */
void stim_set_event_count(stim_t *timer, uint16_t event_count);
/**
 * @brief Get the timer event count.
 *
 * The read is protected by the lock abstraction, so this function may be called
 * from any execution context.
 *
 * @param timer Timer object; must not be NULL.
 * @return Current event count, that is the number of expirations so far.
 */
uint16_t stim_get_event_count(const stim_t *timer);

#ifdef __cplusplus
}
#endif

#endif
