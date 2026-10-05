// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Zhijian Yan

#ifndef SIMPLETIMER_PORT_H
#define SIMPLETIMER_PORT_H

static inline int stim_lock(void) {
    return 0;
}

static inline void stim_unlock(int stim_lock_state) {
    (void)stim_lock_state;
}

#define STIM_ACQUIRE() ((void)0)
#define STIM_RELEASE() ((void)0)

#endif
