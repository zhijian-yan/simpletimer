// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Zhijian Yan

/**
 * @file
 * @brief Platform hooks of simpletimer.
 *
 * Provides the critical section and the queue ordering barriers that
 * simpletimer needs from the target.
 *
 * This file belongs to the application rather than to the library: adapt it to
 * the target and keep it when simpletimer is updated. To keep it outside the
 * library tree, define STIM_PORT_HEADER to its name instead of editing the
 * shipped file.
 *
 * The defaults below are all no-ops, which is correct only while a single
 * execution context uses each group.
 */

#ifndef SIMPLETIMER_PORT_H
#define SIMPLETIMER_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enter the critical section.
 *
 * The library calls this around every access to the state that execution
 * contexts share: the group timebase, the indices of the command queue and of
 * the expired-event queue, and the event count of a timer. It is taken by the
 * producers (stim_timebase_inc(), stim_start_timer(), stim_stop_timer(),
 * stim_set_event_count() and stim_get_event_count()) and also by the consumer
 * stim_poll(), which locks the timebase and the event count and re-enters
 * through stim_queue_send() when it queues an expired event in deferred mode.
 *
 * @return Opaque state describing the situation before the critical section was
 *         entered; it is passed back to stim_unlock(), which must restore it
 *         exactly.
 *
 * @note The default implementation does nothing, which is correct only while a
 *       single execution context uses the group.
 * @note Keep the implementation stateless: a static inline definition is
 *       private to each translation unit, so a function-local static variable
 *       would exist once per translation unit instead of once per program.
 * @note If the library may be called from an interrupt handler, the lock must
 *       disable interrupts, or use the ISR-safe entry point of the RTOS
 *       critical section. A plain spinlock that does not mask local interrupts
 *       deadlocks as soon as an ISR preempts the context holding it.
 * @note If the library may be called from RTOS tasks, use the RTOS critical
 *       section or a mutex: disabling interrupts alone does not cover a task
 *       switch inside the critical section.
 */
static inline int stim_lock(void) {
    return 0;
}

/**
 * @brief Leave the critical section entered by stim_lock().
 *
 * @param stim_lock_state Value returned by the matching stim_lock() call.
 *
 * @note Nested lock/unlock pairs must restore the state of the outermost pair
 *       correctly, so save and restore the interrupt mask instead of
 *       unconditionally enabling interrupts.
 */
static inline void stim_unlock(int stim_lock_state) {
    (void)stim_lock_state;
}

/**
 * @brief Ordering barrier between a queue entry and its index.
 *
 * Called after a queue index has been loaded and before the data that index
 * protects is touched.
 *
 * @note The default is a no-op, which is enough on a single core. A multi-core
 *       target needs a real acquire barrier here, for example the CMSIS
 *       __DMB(): volatile alone provides no ordering, so a core could otherwise
 *       observe an index before the entry it publishes.
 */
#define STIM_ACQUIRE() ((void)0)

/**
 * @brief Ordering barrier taken just before a queue index is stored.
 *
 * Called right before the index that publishes a queue entry is written, so
 * that the entry itself is visible first.
 *
 * @note The default is a no-op, which is enough on a single core; a multi-core
 *       target needs a real release barrier here. Keep it consistent with
 *       STIM_ACQUIRE().
 */
#define STIM_RELEASE() ((void)0)

#ifdef __cplusplus
}
#endif

#endif
