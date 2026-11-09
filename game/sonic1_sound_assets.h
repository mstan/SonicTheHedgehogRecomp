#ifndef SONIC1_SOUND_ASSETS_H
#define SONIC1_SOUND_ASSETS_H
#include <stddef.h>
#include <stdint.h>

/* Guest addresses reserved for locally converted assets, outside stock ROM. */
enum {
    S1_SOUND_MUSIC_BASE=0x100000, S1_SOUND_MUSIC_STRIDE=0x4000,
    S1_SOUND_SFX_BASE=0x150000, S1_SOUND_SFX_STRIDE=0x400,
    S1_SOUND_PSG_BASE=0x15F000, S1_SOUND_PCM_BASE=0x160000,
    S1_SOUND_SEGA_BASE=0x170000, S1_SOUND_ASSETS_END=0x180000
};
typedef struct S1SoundAssetLayout {
    uint32_t music_index, sfx_index, special_index, psg_index;
} S1SoundAssetLayout;

/* Pure local transformation of verified JUE REV00 bytes. No I/O, network,
 * toolchain, game execution or source assets. Writes only extension banks. */
int s1_sound_build_assets(const uint8_t *rom, size_t rom_size,
                          uint8_t *memory, size_t memory_size,
                          const S1SoundAssetLayout *layout,
                          char *error, size_t error_size);
#endif
