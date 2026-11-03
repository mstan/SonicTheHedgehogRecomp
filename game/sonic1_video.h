#pragma once
#include "game_video.h"
extern const GameVideo sonic1_video;
void s1_video_command(int id, const char *json);
int s1_video_hook(uint32_t pc);
size_t s1_video_rb_save(void *dst, size_t cap);
int s1_video_rb_load(const void *src, size_t len);
