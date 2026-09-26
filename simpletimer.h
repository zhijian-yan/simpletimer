// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Zhijian Yan

#ifndef SIMPLETIMER_H
#define SIMPLETIMER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

static inline int stim_lock(void) {
    /* Disable interrupts if needed */
    return 0;
}

static inline void stim_unlock(int stim_lock_state) {
    /* Restore interrupt state */
    (void)stim_lock_state;
}

#define STIM_MAX_TICKS (((uint32_t)(-1)) >> 1)

typedef enum {
    STIM_CB_MODE_DEFERRED = 0,
    STIM_CB_MODE_IMMEDIATE,
} stim_cb_mode_t;

struct stim_node {
    struct stim_node *next;
    struct stim_node *prev;
};

typedef struct {
    struct stim_node node;
    void *user_data;
    uint32_t expire_ticks;
    uint32_t period_ticks;
    volatile uint16_t count;
    uint8_t state;
} stim_t;

typedef struct {
    stim_t *timer;
    uint8_t command;
} stim_message_t;

typedef struct {
    stim_message_t *buffer;
    uint8_t length;
    volatile uint8_t write_index;
    volatile uint8_t read_index;
} stim_queue_t;

typedef struct {
    volatile uint32_t timebase_ticks;
    void (*cb)(stim_t *timer);
    stim_cb_mode_t cb_mode;
    stim_queue_t command_queue;
    stim_queue_t expired_queue;
    struct stim_node head;
} stim_group_t;

typedef struct {
    void (*cb)(stim_t *timer);
    stim_cb_mode_t cb_mode;
    stim_message_t *command_buffer;
    stim_message_t *expired_buffer;
    uint8_t command_length;
    uint8_t expired_length;
} stim_group_config_t;

void stim_timebase_inc(stim_group_t *group);
void stim_init_timer(stim_t *timer, uint32_t period_ticks, void *user_data);
void stim_init_group(stim_group_t *group, stim_group_config_t *config);
int stim_start_timer(stim_t *timer, stim_group_t *group);
int stim_stop_timer(stim_t *timer, stim_group_t *group);
int stim_poll(stim_group_t *group);
void stim_dispatch(uint8_t max_event_num, stim_group_t *group);
void stim_set_count(stim_t *timer, uint32_t count);
uint16_t stim_get_count(const stim_t *timer);

#ifdef __cplusplus
}
#endif

#endif
