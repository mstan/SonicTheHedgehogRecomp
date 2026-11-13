#include "sonic1_debug.h"
#include "genesis_runtime.h"
#include "host_atomic_file.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

static int selected,active,bound;
static char settings_file[1200];
static unsigned access_selected,access_active;
static char access_file[1200];
void s1_debug_load_settings(const char *settings)
{
    selected=active=bound=0;settings_file[0]=access_file[0]=0;
    access_selected=access_active=0;
    const char *slash=strrchr(settings,'/'),*back=strrchr(settings,'\\');
    if(!slash || (back && back>slash))slash=back;
    size_t n=slash?(size_t)(slash-settings)+1:0;
    if(n+32>=sizeof settings_file)return;
    memcpy(settings_file,settings,n);strcpy(settings_file+n,"sonic1-bug-fixes.ini");
    memcpy(access_file,settings,n);strcpy(access_file+n,"sonic1-easy-access.ini");
    SDL_RWops *f=SDL_RWFromFile(settings_file,"rb");
    if(f) {
        char text[512];size_t bytes=SDL_RWread(f,text,1,sizeof text-1);text[bytes]=0;
        SDL_RWclose(f);
        char *line=text;
        while(line && *line) {
            char *next=strchr(line,'\n');if(next)*next++=0;
            size_t length=strlen(line);if(length && line[length-1]=='\r')line[length-1]=0;
            if(!strcmp(line,"debug_transporters=1"))selected=1;
            line=next;
        }
    }
    f=SDL_RWFromFile(access_file,"rb");
    if(f) {
        char text[512];size_t bytes=SDL_RWread(f,text,1,sizeof text-1);text[bytes]=0;
        SDL_RWclose(f);
        char *line=text;
        while(line && *line) {
            char *next=strchr(line,'\n');if(next)*next++=0;
            size_t length=strlen(line);if(length && line[length-1]=='\r')line[length-1]=0;
            if(!strcmp(line,"level_select=1"))access_selected|=S1_ACCESS_LEVEL_SELECT;
            if(!strcmp(line,"debug_mode=1"))access_selected|=S1_ACCESS_DEBUG;
            line=next;
        }
    }
}
void s1_debug_post_reset(void) { if(!bound){active=selected;access_active=access_selected;bound=1;} }
int s1_debug_select(int enabled) { selected=enabled!=0;return 1; }
int s1_debug_selected(void) { return selected; }
int s1_debug_active(void) { return active; }
int s1_debug_effective(void) { return bound?active:selected; }
int s1_access_select(unsigned feature,int enabled)
{
    if(feature!=S1_ACCESS_LEVEL_SELECT && feature!=S1_ACCESS_DEBUG)return 0;
    if(enabled)access_selected|=feature;else access_selected&=~feature;
    return 1;
}
unsigned s1_access_selected(void) { return access_selected; }
unsigned s1_access_active(void) { return access_active; }
unsigned s1_access_effective(void) { return bound?access_active:access_selected; }
int s1_debug_commit(void)
{
    char text[100];int n=snprintf(text,sizeof text,"[bug_fixes]\ndebug_transporters=%d\n",selected);
    if(!*settings_file || !genesis_atomic_file_store(settings_file,text,(size_t)n))return 0;
    n=snprintf(text,sizeof text,"[easy_access]\nlevel_select=%u\ndebug_mode=%u\n",
        !!(access_selected&S1_ACCESS_LEVEL_SELECT),!!(access_selected&S1_ACCESS_DEBUG));
    return *access_file && genesis_atomic_file_store(access_file,text,(size_t)n);
}

int s1_debug_hook(uint32_t pc)
{
    /* REV00 Tit_ChkLevSel is reached after Start is accepted. Reuse the
     * original menu and sound test without changing controller input. */
    if(pc==0x3242u && (access_active&S1_ACCESS_LEVEL_SELECT)) {
        m68k_write8(0xFFFFE0u,1);
        recomp_tail_call(0x3254u);
        return 1;
    }
    /* Original level and special-stage debug gates. Arm the stock B-button
     * toggle without requiring the cheat or holding A at entry. Attract and
     * credits demos keep their original behavior. No registers are changed. */
    if((pc==0x3982u || pc==0x4704u) && (access_active&S1_ACCESS_DEBUG) &&
       !(g_ram[0xFFF0] | g_ram[0xFFF1])) {
        m68k_write8(0xFFFFE2u,1);
        m68k_write8(0xFFFFFAu,1);
    }
    if(!active || !(g_ram[0xFE08] | g_ram[0xFE09]))return 0;
    /* REV00 Tele_Action captures the debug cursor as if it were Sonic.
     * Flying away during its bump can unload the owner before it unlocks
     * the player. Return from this subroutine, retaining Teleport's normal
     * out-of-range handling and all ordinary gameplay instructions. */
    if(pc==0x166C8u)return 1;
    if(pc==0x16668u) {
        unsigned o=g_cpu.A[0]&65535u;
        if(o>=0xD800 && o<0xF000 && !(o&63) && g_ram[o]==0x72 &&
           (g_ram[o+0x24]==4 || g_ram[o+0x24]==6)) {
            /* Also let debug flight leave an already active transporter.
             * Only this active owner may release its own control lock. */
            if(g_ram[0xF7C8]==0x81) {
                m68k_write8(0xFFF7C8,0);
                m68k_write16(0xFFD010,0);
                m68k_write16(0xFFD012,0);
                m68k_write16(0xFFD014,0);
            }
            m68k_write8(0xFF0000u+o+0x24,0);
        }
    }
    return 0;
}
