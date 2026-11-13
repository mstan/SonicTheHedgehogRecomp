#pragma once
#include <stdint.h>
int s1_debug_hook(uint32_t pc);
void s1_debug_load_settings(const char *settings);
void s1_debug_post_reset(void);
int s1_debug_select(int enabled);
int s1_debug_selected(void);
int s1_debug_active(void);
int s1_debug_effective(void);
int s1_debug_commit(void);
enum { S1_ACCESS_LEVEL_SELECT = 1, S1_ACCESS_DEBUG = 2 };
int s1_access_select(unsigned feature, int enabled);
unsigned s1_access_selected(void);
unsigned s1_access_active(void);
unsigned s1_access_effective(void);
