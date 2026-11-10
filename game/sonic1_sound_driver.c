#include "sonic1_sound_driver.h"
#include "sonic1_debug.h"
#include "genesis_runtime.h"
#include "host_atomic_file.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SONIC1_CLONE_DRIVER
#include "sonic1_clone_metadata.h"
#ifdef S1_CLONE_ASSETS_FROM_ROM
#include "sonic1_sound_assets.h"
#include "sonic1_clone_program.h"
#endif
#endif

static int selected, active, verified, bound;
static char settings_file[1200], error[256];

int s1_sound_available(void)
{
#if defined(S1_CLONE_ASSETS_FROM_ROM) && !defined(GENESIS_Z80_RECOMP)
    return 1;
#else
    return 0;
#endif
}
int s1_sound_selected(void) { return selected; }
int s1_sound_uses_rom(void) {
#ifdef S1_CLONE_ASSETS_FROM_ROM
    return 1;
#else
    return 0;
#endif
}
int s1_sound_active(void) { return active; }
int s1_sound_verified(void) { return verified; }
const char *s1_sound_error(void) { return error; }

int s1_sound_select(int enabled)
{
    if(enabled && !s1_sound_available()) {
        snprintf(error,sizeof error,"This build does not include Clone Driver with the standard Z80 backend.");return 0;
    }
    selected=enabled!=0;verified=s1_sound_available();error[0]=0;return 1;
}
void s1_sound_load_settings(const char *settings)
{
    s1_debug_load_settings(settings);
    selected=verified=active=bound=0;error[0]=0;
    const char *slash1=strrchr(settings,'/'), *slash2=strrchr(settings,'\\');
    const char *slash=slash1;
    if (!slash || (slash2 && slash2>slash)) slash=slash2;
    size_t prefix=slash?(size_t)(slash-settings)+1:0;
    if (prefix+25>=sizeof settings_file) { fprintf(stderr,"[sound-driver] Settings path too long\n"); exit(1); }
    memcpy(settings_file,settings,prefix);
    snprintf(settings_file+prefix,sizeof settings_file-prefix,"sonic1-sound.ini");
    SDL_RWops *f=SDL_RWFromFile(settings_file,"rb");
    if (f) {
        Sint64 bytes=SDL_RWsize(f);
        if (bytes>=0 && bytes<4096) {
            char text[4096]; size_t n=SDL_RWread(f,text,1,(size_t)bytes); text[n]=0;
            char *line=text;
            while (line && *line) {
                char *next=strchr(line,'\n'); if(next)*next++=0;
                size_t len=strlen(line);if(len && line[len-1]=='\r')line[len-1]=0;
                if (!strcmp(line,"enabled=1")) selected=1;
                line=next;
            }
        }
        SDL_RWclose(f);
    }
    verified=s1_sound_available();
}
int s1_sound_commit(void)
{
    if (selected && !s1_sound_select(1)) return 0;
    char text[1200];
    int n=snprintf(text,sizeof text,"[sound_driver]\nenabled=%d\n",selected);
    if(n<0 || n>=(int)sizeof text || !genesis_atomic_file_store(settings_file,text,(size_t)n)) {
        snprintf(error,sizeof error,"Unable to save Sonic 1 sound settings.");return 0;
    }
    if(!s1_debug_commit()) {
        snprintf(error,sizeof error,"Unable to save Sonic 1 bug-fix settings.");return 0;
    }
    return 1;
}
void s1_sound_post_reset(void)
{
    s1_debug_post_reset();
    if(bound)return;
    active=selected;bound=1;
    if (!active) return;
    if(!s1_sound_available()){fprintf(stderr,"[sound-driver] Unsupported Z80 backend\n");exit(1);}
#ifdef S1_CLONE_ASSETS_FROM_ROM
    const S1SoundAssetLayout layout={S1_CLONE_MUSICINDEX,S1_CLONE_SOUNDINDEX,S1_CLONE_SPECSOUNDINDEX,S1_CLONE_PSG_INDEX};
    memcpy(g_rom+S1_CLONE_LOAD_ADDRESS,s1_clone_program,sizeof s1_clone_program);
    if(!s1_sound_build_assets(g_rom,0x80000,g_rom,0x400000,&layout,error,sizeof error)) {
        fprintf(stderr,"[sound-driver] %s\n",error);exit(1);
    }
    verified=1;fprintf(stderr,"[sound-driver] Clone Driver + Mega PCM; sound assets prepared locally from ROM\n");
#endif
}

static void move_byte_flags(uint8_t value)
{
    g_cpu.SR=(uint16_t)((g_cpu.SR & ~(SR_N|SR_Z|SR_V|SR_C)) | (value&128?SR_N:0) | (!value?SR_Z:0));
}
int s1_sound_hook(uint32_t pc)
{
#ifdef SONIC1_CLONE_DRIVER
    if (!active) return 0;
    if(pc==0x2EF2u) {
        /* REV00 starts its thirty-frame logo hold here. Mega PCM plays
         * asynchronously; let complete game ticks drain the actual sample
         * before the original hold/Start handling resumes. */
        M68KState saved=g_cpu;
        do {
            m68k_write8(0xFFF62Au,0x14);
            recomp_call_addr(0x29A8u);
            recomp_call_addr(S1_CLONE_PCM_IDLE);
        } while(!(g_cpu.D[0]&1));
        g_cpu=saved;return 0;
    }
    if(pc==0x3562u) {
        /* Display Clone's zero-based IDs; the existing queue bridge keeps
         * translating the unchanged stock selection and playback range. */
        g_cpu.D[0]=(g_cpu.D[0]&0xFFFF0000u)|((g_cpu.D[0]-0x80u)&0xFFFFu);
        return 0;
    }
    if (pc==0x1352u) {
        M68KState saved=g_cpu;
        memset(g_ram+0xF000,0,0x400);
        recomp_call_addr(S1_CLONE_INITIALISE);
        g_cpu=saved;return 1;
    }
    if (pc==0x71B4Cu) { recomp_call_addr(S1_CLONE_GUARDED_UPDATE);return 1; }
    if (pc==0x1396u || pc==0x139Cu || pc==0x13A2u) {
        M68KState saved=g_cpu; uint8_t id=(uint8_t)g_cpu.D[0], mapped=0;
        if ((id>=0x81 && id<=0x93) || (id>=0xA0 && id<=0xD0)) mapped=(uint8_t)(id-0x80);
        else if(id>=0xE0 && id<=0xE4)mapped=(uint8_t)(id+0x1B);
        if(mapped) {
            g_cpu.D[0]=mapped;
            recomp_call_addr(pc==0x1396u?S1_CLONE_QUEUE_MUSIC:pc==0x139Cu?S1_CLONE_QUEUE_SFX:S1_CLONE_QUEUE_EXTRA);
        }
        g_cpu=saved;move_byte_flags(id);return 1;
    }
    if (pc==0x13C4u || pc==0x1404u || pc==0x1418u) {
        uint8_t value=pc==0x13C4u?1:0x80;
        if(pc==0x1404u && g_ram[0xF600]==4) {
            /* Debug pause+A is a scene exit, not an audible resume. Stop
             * while still muted instead of restoring pans for one tick. */
            M68KState saved=g_cpu;g_cpu.A[6]=0xFFF000u;
            recomp_call_addr(S1_CLONE_STOP_ALL);g_cpu=saved;
            m68k_write8(0xFFF000u+S1_CLONE_PAUSE_OFFSET,0);
        } else m68k_write8(0xFFF000u+S1_CLONE_PAUSE_OFFSET,value);
        move_byte_flags(value);
        if(pc!=0x1418u)recomp_tail_call(pc==0x13C4u?0x13CAu:0x140Au);
        return 1;
    }
#else
    (void)pc;
#endif
    return 0;
}
int s1_sound_dispatch_miss(uint32_t pc)
{
#ifdef SONIC1_CLONE_DRIVER
    /* The stock ROM identity remains 512 KiB. Its generic miss recorder
     * filters larger addresses, so enforce coverage for our appended bank. */
    uint32_t end=S1_CLONE_LOAD_ADDRESS+S1_CLONE_PAYLOAD_SIZE;
#ifdef S1_CLONE_ASSETS_FROM_ROM
    end=S1_SOUND_ASSETS_END;
#endif
    if(active && pc>=S1_CLONE_LOAD_ADDRESS && pc<end) {
        fprintf(stderr,"[sound-driver] Missing native sound target $%06X; refusing to skip driver code\n",pc);
        exit(1);
    }
#else
    (void)pc;
#endif
    return 0;
}
void s1_sound_state_save(void *out)
{
    uint8_t bytes[S1_SOUND_STATE_BYTES]={'S','1','S','D',1,0,0,0};
    /* The frame-0 snapshot can precede entry into the game fiber. Its driver
     * identity must already match the first running tick. Selection becomes
     * immutable for this process at entry; later UI edits are for a restart. */
    bytes[5]=(uint8_t)(bound?active:selected);
    bytes[6]=(uint8_t)s1_debug_effective();
#ifdef SONIC1_CLONE_DRIVER
    if(bytes[5])for(unsigned i=0;i<32;i++) {
        unsigned a=S1_CLONE_SHA256[2*i],b=S1_CLONE_SHA256[2*i+1];
        bytes[8+i]=(uint8_t)(((a<='9'?a-'0':a-'a'+10)<<4)|(b<='9'?b-'0':b-'a'+10));
    }
#endif
    memcpy(out,bytes,sizeof bytes);
}
int s1_sound_state_valid(const void *data)
{
    uint8_t expected[S1_SOUND_STATE_BYTES];s1_sound_state_save(expected);
    return !memcmp(data,expected,sizeof expected);
}
int s1_sound_netplay_allowed(void) { return !(selected || active); }
void s1_sound_config_image(char *out,size_t size)
{
    uint8_t state[S1_SOUND_STATE_BYTES];s1_sound_state_save(state);
    char hash[65];for(unsigned i=0;i<32;i++)snprintf(hash+2*i,3,"%02x",state[8+i]);
    snprintf(out,size,"sonic1.sound_driver=%s\nsonic1.sound_pack=%s\nsonic1.debug_transporters=%u\n",state[5]?"clone":"original",hash,state[6]);
}
