/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "capture-ipc.h"
#include <obs.h>
struct capture_host;
struct capture_host *capture_host_create(const char *label);
void capture_host_configure(struct capture_host *, const struct capture_config *);
/* Graphics-thread calls: never wait for the worker, IPC ownership or supervisor. */
void capture_host_tick(struct capture_host *, bool showing);
void capture_host_render(struct capture_host *);
void capture_host_clear(struct capture_host *);
uint32_t capture_host_width(struct capture_host *);
uint32_t capture_host_height(struct capture_host *);
DWORD capture_host_target_pid(struct capture_host *);
/* Signals shutdown; graphics resources are freed now, joins happen asynchronously. */
void capture_host_destroy(struct capture_host *);
