/*
 * sonic1_spec.c — instantiates the GameSpec for Sonic the Hedgehog
 * (Genesis, 1991, JUE REV00).
 *
 * Runtime lifecycle and dispatch enter through g_game_spec. Sonic-specific
 * frame/debug helpers remain in sonic_extras.c until their compatibility API
 * is removed together with consumer game source lists.
 */
#include "game_spec.h"
#include "genesis_runtime.h"
#include "sonic1_video.h"
#include "sonic1_audio.h"
#include "sonic1_sound_driver.h"
#include "audio/event_queue.h"

#include <stdint.h>
#ifdef GENESIS_Z80_RECOMP
#include "s1z80_step.h"
#endif
#include <stddef.h>   /* NULL — not pulled in transitively under gcc/glibc */

/* ---- 68K RAM shadow (defined in glue.c) ---- */
extern uint8_t g_ram[0x10000];

/* ---- Recompiled entry trampolines ---- */
/* Routed through recomp_call_addr so the tail-call trampoline frame
 * is set up before the recompiled function runs. */
static void s1_call_entry_point(void) {
    s1_sound_post_reset();
    recomp_call_addr(0x000206u);
}
static void s1_call_vblank(void)      { recomp_call_addr(0x000B10u); }
static void s1_call_hblank(void)      { recomp_call_addr(0x001126u); }
static int s1_instruction_hook(uint32_t pc)
{
    if(s1_audio_hook(pc))return 1;
    return s1_video_hook(pc);
}

/* Mode-aware save-state resume (s1disasm sonic.asm; GameModeArray masks $1C:
 * $00 Sega, $04 Title, $08 Demo->GM_Level, $0C Level, $10 Special,
 * $14 Continue, $18 Ending, $1C Credits). Each target is that mode's
 * per-frame loop top, so a load continues moment-in-time. Unmapped / bit-7-
 * flagged modes fall back to resume_main_loop_pc (Level_MainLoop, whose
 * mode-check tail RTSes to the dispatcher and reinits — old behaviour). */
static uint32_t s1_save_resume_pc(uint8_t game_mode)
{
    switch (game_mode) {
        case 0x08:
        case 0x0C: return 0x003AE2u;   /* Level_MainLoop (handles demo)  */
        case 0x10: return 0x00472Au;   /* SS_MainLoop                    */
        case 0x14: return 0x004DC4u;   /* Cont_MainLoop                  */
        case 0x04: return 0x00317Cu;   /* Tit_MainLoop                   */
        case 0x18:
            /* EndSonic phase 8 waits in End_AllEmlds while the emeralds
             * brighten the palette. Its two f_restart handshakes enter and
             * leave that loop; resuming End_MainLoop here loses the fade. */
            if (g_ram[0xD000] == 0x87 &&
                ((g_ram[0xD025] == 8 && !(g_ram[0xFE02] | g_ram[0xFE03])) ||
                 (g_ram[0xD025] == 10 && (g_ram[0xFE02] | g_ram[0xFE03]))))
                return 0x0052F0u;    /* End_AllEmlds                   */
            return 0x005284u;        /* End_MainLoop                   */
        default:   return 0u;
    }
}

static void s1_call_resume(uint32_t resume_pc)
{
    /* GM_Ending calls PaletteWhiteIn after replacing the flower layout.
     * A snapshot inside that nested call must finish its remaining frames
     * before advancing actors again. REV00 $1EF4-$1F08: use the restored D4
     * DBF counter and the original fade/PLC routines, without reinitializing
     * the palette or restarting the whole 22-frame fade. */
    if (g_ram[0xF600] == 0x18 && g_ram[0xD000] == 0x87 &&
        g_ram[0xD025] == 10 && g_ram[0xF62A] == 0x12 &&
        !(g_ram[0xFE02] | g_ram[0xFE03]) && (uint16_t)g_cpu.D[4] < 22) {
        uint16_t remaining;
        do {
            m68k_write8(0xFFF62A, 0x12);
            recomp_call_addr(0x0029A8u); /* WaitForVBlank */
            recomp_call_addr(0x001F0Au); /* WhiteIn_FromWhite */
            recomp_call_addr(0x0015ECu); /* RunPLC */
            remaining = (uint16_t)(g_cpu.D[4] - 1u);
            g_cpu.D[4] = (g_cpu.D[4] & 0xFFFF0000u) | remaining;
        } while (remaining != 0xFFFFu);
    }
    recomp_call_addr(resume_pc);
}

static size_t s1_state_size(void)
{
    return S1_SOUND_STATE_BYTES+s1_video_state_size()+audio_event_state_size();
}
static int s1_state_save(void *data,size_t size)
{
    size_t video=s1_video_state_size();
    if(!data || size!=s1_state_size())return 0;
    s1_sound_state_save(data);
    return s1_video_state_save((uint8_t*)data+S1_SOUND_STATE_BYTES,video) &&
        audio_event_state_save((uint8_t*)data+S1_SOUND_STATE_BYTES+video,size-S1_SOUND_STATE_BYTES-video);
}
static int s1_state_load(const void *data,size_t size,int apply)
{
    size_t video=s1_video_state_size();
    if(!data || size!=s1_state_size() || !s1_sound_state_valid(data))return 0;
    const uint8_t *v=(const uint8_t*)data+S1_SOUND_STATE_BYTES,*a=v+video;
    size_t audio=size-S1_SOUND_STATE_BYTES-video;
    if(!s1_video_state_load(v,video,0) || !audio_event_state_load(a,audio,0))return 0;
    return !apply || (s1_video_state_load(v,video,1) && audio_event_state_load(a,audio,1));
}
static size_t s1_rb_save(void *dst,size_t cap)
{
    size_t video=s1_video_rb_save(NULL,0),total=video+S1_SOUND_STATE_BYTES;
    if(!dst)return total;if(cap<total)return 0;
    s1_sound_state_save(dst);
    return s1_video_rb_save((uint8_t*)dst+S1_SOUND_STATE_BYTES,cap-S1_SOUND_STATE_BYTES)==video?total:0;
}
static int s1_rb_load(const void *src,size_t len)
{
    return src && len>=S1_SOUND_STATE_BYTES && s1_sound_state_valid(src) &&
        s1_video_rb_load((const uint8_t*)src+S1_SOUND_STATE_BYTES,len-S1_SOUND_STATE_BYTES);
}
static int s1_state_at_boundary(void)
{
    unsigned sp=g_cpu.A[7]&65535u;
    if(sp>65532)return 0;
    unsigned caller=((unsigned)g_ram[sp]<<24)|((unsigned)g_ram[sp+1]<<16)|
        ((unsigned)g_ram[sp+2]<<8)|g_ram[sp+3];
    /* REV00 WaitForVBlank callers with established cold-resume loops. */
    switch(g_ram[0xF600]) {
    case 8:case 12:return caller==0x3AF0 || caller==0x13D4;
    case 16:return caller==0x4738 || caller==0x13D4;
    case 4:return caller==0x3186;
    case 20:return caller==0x4DCE;
    case 24:return caller==0x5292 || caller==0x52FE || caller==0x13D4;
    default:return 0;
    }
}

/* ---- Sonic-specific debug-server handlers (sonic_extras.c) ---- */
extern void handle_sonic_state(int id);
extern void handle_object_table(int id, const char *json);

/* ---- Frame-record packer (sonic_extras.c) ---- */
extern void s1_fill_frame_record(uint8_t game_data[256]);


/* ---- Lifecycle hooks ---- */

/*
 * Sonic 1 post-reset: seed the SMPS sound-driver RAM so UpdateMusic
 * exits cleanly. The Z80 is stubbed and never runs the real driver
 * init sequence, so we set v_sound_id ($FFF009) to $80 ("silence")
 * which makes UpdateMusic skip PlaySoundID and fall through to
 * DoStartZ80 + rts. All PlaybackControl bytes stay 0 (tracks
 * stopped) so per-track update paths also short-circuit cleanly.
 */
static void s1_on_post_reset(void) {
    g_ram[0xF009] = 0x80;
}

/* ---- Debug-server command adapters ---- */
/* GameDebugCommand handlers take (id, json); handle_sonic_state
 * doesn't need the json so we wrap to match the spec signature. */

static void cmd_sonic_state(int id, const char *json) {
    (void)json;
    handle_sonic_state(id);
}

static void cmd_object_table(int id, const char *json) {
    handle_object_table(id, json);
}

static const GameDebugCommand s1_commands[] = {
    { "custom_video", s1_video_command },
    { "sonic_state",  cmd_sonic_state  },
    { "object_table", cmd_object_table },
};

/* ---- The spec ---- */

const GameSpec g_game_spec = {
    .video                  = &sonic1_video,
    .video_default          = "fit",
    .logical_players        = 1,
    .rb_state_save           = s1_rb_save,
    .rb_state_load           = s1_rb_load,
    .mods                   = s1_sound_mods,
    .load_settings          = s1_sound_load_settings,
    .netplay_allowed        = s1_sound_netplay_allowed,
    .netplay_config_image   = s1_sound_config_image,
    .state_build_id          = SONIC1_STATE_BUILD_ID,
    .state_size              = s1_state_size,
    .state_save              = s1_state_save,
    .state_load              = s1_state_load,
    .state_at_boundary       = s1_state_at_boundary,
    .display_name           = "Sonic the Hedgehog",
    .short_name             = "Sonic1",

    /* JUE REV00 — verified against s1disasm reference ROM */
    .expected_rom_crc32     = 0xF9394E97u,
    .expected_rom_size      = 0x80000u,
#ifdef GENESIS_Z80_RECOMP
    .z80_step               = s1z80_step,
#endif

    .call_entry_point       = s1_call_entry_point,
    .call_vblank            = s1_call_vblank,
    .call_hblank            = s1_call_hblank,
    .resume_main_loop_pc    = 0x003AE2u,
    .save_resume_pc         = s1_save_resume_pc,   /* mode -> loop top (moment-in-time) */
    .call_resume            = s1_call_resume,
    .dispatch_main_loop_pc  = 0x000388u,

    .on_post_reset          = s1_on_post_reset,
    .on_frame_pre           = NULL,
    .on_frame_post          = NULL,
    .on_hblank              = NULL,

    .handle_arg             = NULL,
    .arg_usage              = NULL,
    .dispatch_override      = s1_sound_dispatch_miss,
    .instruction_hook       = s1_instruction_hook,

    .fill_frame_record      = s1_fill_frame_record,
    .frame_record_version   = 2,                /* SONIC_GAME_DATA_VERSION */

    .commands               = s1_commands,
    .command_count          = (int)(sizeof(s1_commands) / sizeof(s1_commands[0])),

};
