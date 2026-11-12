/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private, same-build protocol. No named objects or cross-process locks. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#define CAPTURE_IPC_VERSION 1
#define CAPTURE_IPC_SLOTS 3
#define CAPTURE_MAX_FRAME_BYTES (128u * 1024u * 1024u)
#define CAPTURE_HEARTBEAT_TIMEOUT_MS 5000
#define CAPTURE_STARTUP_TIMEOUT_MS 15000
#define CAPTURE_EXE_CHARS 512

enum capture_slot_state { CAPTURE_FREE, CAPTURE_WRITING, CAPTURE_READY, CAPTURE_READING };
struct capture_config {
    wchar_t exe[CAPTURE_EXE_CHARS];
    uint32_t width, height, tolerance;
    bool client_area, cursor, include_children;
    uint64_t resume_window;
    DWORD resume_pid;
};
struct capture_slot {
    volatile LONG state;
    uint32_t width, height, stride;
    uint64_t sequence;
};
struct capture_ipc {
    uint32_t version, capacity;
    LONG revision;
    struct capture_config config;
    volatile LONG enabled;
    volatile LONG target_pid;
    volatile LONG status; /* 0 starting, 1 running, negative worker error. */
    volatile LONG test_command; /* Reserved; only test worker acts on it. */
    volatile LONG test_stage;
    volatile LONG64 target_window;
    volatile LONG64 heartbeat;
    volatile LONG64 required_bytes;
    struct capture_slot slots[CAPTURE_IPC_SLOTS];
};
static inline size_t capture_mapping_size(uint32_t capacity)
{
    return sizeof(struct capture_ipc) + (size_t)CAPTURE_IPC_SLOTS * capacity;
}
static inline uint8_t *capture_slot_pixels(struct capture_ipc *ipc, unsigned slot)
{
    return (uint8_t *)(ipc + 1) + (size_t)slot * ipc->capacity;
}
static inline bool capture_frame_valid(const struct capture_slot *slot, uint32_t capacity)
{
    return slot->width > 0 && slot->height > 0 && slot->width <= 16384 && slot->height <= 16384 &&
           slot->stride == slot->width * 4 && (uint64_t)slot->stride * slot->height <= capacity;
}
