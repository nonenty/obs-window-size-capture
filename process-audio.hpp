/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <windows.h>
#include <obs.h>

struct pid_audio_capture;
#ifdef __cplusplus
extern "C" {
#endif
struct pid_audio_capture *pid_audio_start(obs_source_t *source, DWORD pid);
void pid_audio_request_stop(struct pid_audio_capture *capture);
/* Schedules cleanup on OBS_TASK_DESTROY. Never wait on the video thread. */
void pid_audio_stop(struct pid_audio_capture *capture);
bool pid_audio_finished(struct pid_audio_capture *capture);
#ifdef __cplusplus
}
#endif
