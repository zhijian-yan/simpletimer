// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Zhijian Yan

#include "softimer.h"
#include <stddef.h>
#include <string.h>

#define STIM_TICK_OUT_OF_RANGE(tick) (tick > STIM_MAX_TICKS || tick == 0)
#define container_of(ptr, type, member)                                        \
    ((type *)((char *)(ptr) - offsetof(type, member)))

typedef enum {
    STIM_COMMAND_STOP = 0,
    STIM_COMMAND_START,
} stim_command_t;

typedef struct {
    stim_t *timer;
    stim_command_t command;
} stim_message_t;

typedef struct {
    stim_message_t buffer[STIM_QUEUE_SIZE];
    volatile uint8_t write_index;
    volatile uint8_t read_index;
} stim_queue_t;

static stim_node_t list = {
    .next = &list,
    .prev = &list,
};

static volatile uint32_t stim_ticks = 0;
static stim_queue_t stim_command_queue;
static stim_queue_t stim_expired_queue;

void stim_tick_inc(void) {
#ifdef STIM_ATOMIC_TICKS
    ++stim_ticks;
#else
    int stim_lock_state;
    stim_lock_state = stim_lock();
    ++stim_ticks;
    stim_unlock(stim_lock_state);
#endif
}

static uint32_t stim_get_ticks(void) {
#ifdef STIM_ATOMIC_TICKS
    return stim_ticks;
#else
    uint32_t ticks;
    int stim_lock_state;
    stim_lock_state = stim_lock();
    ticks = stim_ticks;
    stim_unlock(stim_lock_state);
    return ticks;
#endif
}

static int stim_queue_send(stim_queue_t *queue, const stim_message_t *message) {
    int stim_lock_state;
    int ret = 0;
    uint8_t w;
    uint8_t next;
    stim_lock_state = stim_lock();
    w = queue->write_index;
    next = (w + 1) & (STIM_QUEUE_SIZE - 1);
    if (next == queue->read_index) {
        ret = -STIM_EAGAIN;
    } else {
        queue->buffer[w] = *message;
        queue->write_index = next;
    }
    stim_unlock(stim_lock_state);
    return ret;
}

static int stim_queue_receive(stim_queue_t *queue, stim_message_t *message) {
    int ret = 0;
    uint8_t r;
    r = queue->read_index;
    if (r == queue->write_index) {
        ret = -STIM_EAGAIN;
    } else {
        *message = queue->buffer[r];
        queue->read_index = (r + 1) & (STIM_QUEUE_SIZE - 1);
    }
    return ret;
}

static void stim_list_add(stim_t *timer, uint32_t now) {
    stim_t *entry;
    stim_node_t *pos;
    stim_node_t *node = &timer->node;
    if (node->next == node) {
        for (pos = list.next; pos != &list; pos = pos->next) {
            entry = container_of(pos, stim_t, node);
            if ((int32_t)(timer->expire_ticks - now) <
                (int32_t)(entry->expire_ticks - now)) {
                break;
            }
        }
        node->next = pos;
        node->prev = pos->prev;
        pos->prev->next = node;
        pos->prev = node;
    }
}

static void stim_list_del(stim_t *timer) {
    stim_node_t *node = &timer->node;
    if (node->next != node) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
        node->next = node;
        node->prev = node;
    }
}

int stim_init(stim_t *timer, uint32_t period_ticks, stim_cb_mode_t cb_mode,
              stim_cb_t cb, void *user_data) {
    int ret = 0;
    if (!timer || STIM_TICK_OUT_OF_RANGE(period_ticks)) {
        ret = -STIM_EINVAL;
    } else {
        memset(timer, 0, sizeof(stim_t));
        timer->period_ticks = period_ticks;
        timer->cb = cb;
        timer->user_data = user_data;
        timer->cb_mode = cb_mode;
        timer->state = STIM_STATE_STOPPED;
        timer->node.next = &timer->node;
        timer->node.prev = &timer->node;
    }
    return ret;
}

int stim_start(stim_t *timer) {
    int ret = 0;
    stim_message_t message;
    if (!timer) {
        ret = -STIM_EINVAL;
    } else {
        message.timer = timer;
        message.command = STIM_COMMAND_START;
        ret = stim_queue_send(&stim_command_queue, &message);
    }
    return ret;
}

int stim_stop(stim_t *timer) {
    int ret = 0;
    stim_message_t message;
    if (!timer) {
        ret = -STIM_EINVAL;
    } else {
        message.timer = timer;
        message.command = STIM_COMMAND_STOP;
        ret = stim_queue_send(&stim_command_queue, &message);
    }
    return ret;
}

static void stim_process_commands(uint32_t now) {
    stim_message_t message;
    while (!stim_queue_receive(&stim_command_queue, &message)) {
        if (message.command == STIM_COMMAND_START &&
            message.timer->state == STIM_STATE_STOPPED) {
            message.timer->state = STIM_STATE_RUNNING;
            message.timer->expire_ticks = message.timer->period_ticks + now;
            stim_list_add(message.timer, now);
        } else if (message.command == STIM_COMMAND_STOP &&
                   message.timer->state == STIM_STATE_RUNNING) {
            message.timer->state = STIM_STATE_STOPPED;
            stim_list_del(message.timer);
        }
    }
}

int stim_poll(void) {
    int stim_lock_state;
    int ret = 0;
    stim_t *timer;
    stim_message_t message;
    uint32_t now = stim_get_ticks();
    stim_process_commands(now);
    while (list.next != &list) {
        timer = container_of(list.next, stim_t, node);
        if ((int32_t)(timer->expire_ticks - now) <= 0) {
            stim_list_del(timer);
            timer->expire_ticks += timer->period_ticks;
            stim_lock_state = stim_lock();
            ++timer->count;
            stim_unlock(stim_lock_state);
            stim_list_add(timer, now);
            if (timer->cb) {
                if (timer->cb_mode == STIM_CB_MODE_IMMEDIATE) {
                    timer->cb(timer, timer->user_data);
                } else {
                    message.timer = timer;
                    ret |= stim_queue_send(&stim_expired_queue, &message);
                }
            }
        } else {
            break;
        }
    }
    return ret;
}

void stim_dispatch(uint8_t max_event_num) {
    stim_message_t message;
    while (max_event_num-- &&
           !stim_queue_receive(&stim_expired_queue, &message))
        if (message.timer->cb) {
            message.timer->cb(message.timer, message.timer->user_data);
        }
}

int stim_set_count(stim_t *timer, uint32_t count) {
    int stim_lock_state;
    int ret = 0;
    if (!timer) {
        ret = -STIM_EINVAL;
    } else {
        stim_lock_state = stim_lock();
        timer->count = count;
        stim_unlock(stim_lock_state);
    }
    return ret;
}

int stim_get_count(const stim_t *timer, uint32_t *count) {
    int stim_lock_state;
    int ret = 0;
    if (!timer || !count) {
        ret = -STIM_EINVAL;
    } else {
        stim_lock_state = stim_lock();
        *count = timer->count;
        stim_unlock(stim_lock_state);
    }
    return ret;
}
