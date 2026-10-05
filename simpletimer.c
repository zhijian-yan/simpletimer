// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Zhijian Yan

#include "simpletimer.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

#define STIM_STATE_STOPPED           0
#define STIM_STATE_RUNNING           1

#define STIM_COMMAND_STOP            0
#define STIM_COMMAND_START           1

#define stim_check_param(param)      assert((param) != 0)
#define stim_is_pow2(val)            (!(val == 0 || val & (val - 1)))
#define stim_tick_out_of_range(tick) (tick > STIM_MAX_TICKS || tick == 0)
#define stim_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

void stim_timebase_inc(stim_group_t *group) {
    stim_check_param(group);
    int stim_lock_state;
    stim_lock_state = stim_lock();
    group->timebase_ticks += 1;
    stim_unlock(stim_lock_state);
}

static uint32_t stim_get_timebase(stim_group_t *group) {
    stim_check_param(group);
    int stim_lock_state;
    stim_lock_state = stim_lock();
    uint32_t ticks = group->timebase_ticks;
    stim_unlock(stim_lock_state);
    return ticks;
}

static int stim_queue_send(stim_queue_t *queue, const stim_message_t *message) {
    int stim_lock_state = stim_lock();
    uint8_t w = queue->write_index;
    uint8_t next = (w + 1) & (queue->length - 1);
    if (next == queue->read_index) {
        stim_unlock(stim_lock_state);
        return 1;
    }
    queue->buffer[w] = *message;
    queue->write_index = next;
    stim_unlock(stim_lock_state);
    return 0;
}

static int stim_queue_receive(stim_queue_t *queue, stim_message_t *message) {
    uint8_t r = queue->read_index;
    if (r == queue->write_index)
        return 1;
    *message = queue->buffer[r];
    queue->read_index = (r + 1) & (queue->length - 1);
    return 0;
}

static void stim_list_add(stim_t *timer, struct stim_node *head, uint32_t now) {
    struct stim_node *posi;
    struct stim_node *node = &timer->node;
    if (node->next == node) {
        for (posi = head->next; posi != head; posi = posi->next) {
            stim_t *entry = stim_container_of(posi, stim_t, node);
            if ((int32_t)(timer->expire_ticks - now) <
                (int32_t)(entry->expire_ticks - now)) {
                break;
            }
        }
        node->next = posi;
        node->prev = posi->prev;
        posi->prev->next = node;
        posi->prev = node;
    }
}

static void stim_list_del(stim_t *timer) {
    struct stim_node *node = &timer->node;
    if (node->next != node) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
        node->next = node;
        node->prev = node;
    }
}

void stim_init_timer(stim_t *timer, uint32_t period_ticks, void *user_data) {
    stim_check_param(timer);
    stim_check_param(!stim_tick_out_of_range(period_ticks));
    memset(timer, 0, sizeof(stim_t));
    timer->period_ticks = period_ticks;
    timer->user_data = user_data;
    timer->state = STIM_STATE_STOPPED;
    timer->node.next = &timer->node;
    timer->node.prev = &timer->node;
}

void stim_init_group(stim_group_t *group, stim_group_config_t *config) {
    stim_check_param(group);
    stim_check_param(config);
    stim_check_param(config->command_buffer);
    stim_check_param(stim_is_pow2(config->command_length));
    memset(group, 0, sizeof(stim_group_t));
    group->cb = config->cb;
    group->cb_mode = config->cb_mode;
    group->command_queue.buffer = config->command_buffer;
    group->command_queue.length = config->command_length;
    group->expired_queue.buffer = config->expired_buffer;
    group->expired_queue.length = config->expired_length;
    group->head.next = &group->head;
    group->head.prev = &group->head;
}

int stim_start_timer(stim_t *timer, stim_group_t *group) {
    stim_check_param(timer);
    stim_check_param(group);
    stim_message_t message;
    message.timer = timer;
    message.command = STIM_COMMAND_START;
    return stim_queue_send(&group->command_queue, &message);
}

int stim_stop_timer(stim_t *timer, stim_group_t *group) {
    stim_check_param(timer);
    stim_check_param(group);
    stim_message_t message;
    message.timer = timer;
    message.command = STIM_COMMAND_STOP;
    return stim_queue_send(&group->command_queue, &message);
}

static void stim_process_commands(uint32_t now, stim_group_t *group) {
    stim_message_t message;
    while (!stim_queue_receive(&group->command_queue, &message)) {
        if (message.command == STIM_COMMAND_START &&
            message.timer->state == STIM_STATE_STOPPED) {
            message.timer->state = STIM_STATE_RUNNING;
            message.timer->expire_ticks = message.timer->period_ticks + now;
            stim_list_add(message.timer, &group->head, now);
        } else if (message.command == STIM_COMMAND_STOP &&
                   message.timer->state == STIM_STATE_RUNNING) {
            message.timer->state = STIM_STATE_STOPPED;
            stim_list_del(message.timer);
        }
    }
}

int stim_poll(stim_group_t *group) {
    stim_check_param(group);
    int ret = 0;
    stim_message_t message;
    message.command = 0;
    uint32_t now = stim_get_timebase(group);
    stim_process_commands(now, group);
    while (group->head.next != &group->head) {
        stim_t *timer = stim_container_of(group->head.next, stim_t, node);
        if ((int32_t)(timer->expire_ticks - now) <= 0) {
            stim_list_del(timer);
            timer->expire_ticks += timer->period_ticks;
            int stim_lock_state = stim_lock();
            timer->count += 1;
            stim_unlock(stim_lock_state);
            stim_list_add(timer, &group->head, now);
            if (group->cb) {
                if (group->cb_mode == STIM_CB_MODE_IMMEDIATE) {
                    group->cb(timer);
                } else if (group->expired_queue.buffer) {
                    stim_check_param(stim_is_pow2(group->expired_queue.length));
                    message.timer = timer;
                    ret += stim_queue_send(&group->expired_queue, &message);
                }
            }
        } else
            break;
    }
    return ret;
}

void stim_dispatch(uint8_t max_event_num, stim_group_t *group) {
    stim_check_param(group);
    stim_check_param(group->cb);
    stim_message_t message;
    while (max_event_num > 0 &&
           !stim_queue_receive(&group->expired_queue, &message)) {
        max_event_num -= 1;
        group->cb(message.timer);
    }
}

void stim_set_count(stim_t *timer, uint32_t count) {
    stim_check_param(timer);
    int stim_lock_state = stim_lock();
    timer->count = count;
    stim_unlock(stim_lock_state);
}

uint16_t stim_get_count(const stim_t *timer) {
    stim_check_param(timer);
    int stim_lock_state = stim_lock();
    uint16_t count = timer->count;
    stim_unlock(stim_lock_state);
    return count;
}
