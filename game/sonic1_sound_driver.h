#ifndef SONIC1_SOUND_DRIVER_H
#define SONIC1_SOUND_DRIVER_H
#include <stddef.h>
#include <stdint.h>
struct RecompLauncherCModProvider;
enum { S1_SOUND_STATE_BYTES = 40 };
void s1_sound_load_settings(const char *settings);
void s1_sound_post_reset(void);
int s1_sound_hook(uint32_t pc);
int s1_sound_available(void);
int s1_sound_selected(void);
int s1_sound_active(void);
int s1_sound_select(int enabled);
int s1_sound_set_pack(const char *path);
const char *s1_sound_pack(void);
const char *s1_sound_error(void);
int s1_sound_verified(void);
int s1_sound_commit(void);
int s1_sound_netplay_allowed(void);
void s1_sound_config_image(char *out, size_t size);
void s1_sound_state_save(void *out);
int s1_sound_state_valid(const void *data);
const struct RecompLauncherCModProvider *s1_sound_mods(const struct RecompLauncherCModProvider *base);
#endif
