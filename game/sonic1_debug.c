#include "sonic1_debug.h"
#include "genesis_runtime.h"
#include "host_atomic_file.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

static int selected,active,bound;
static char settings_file[1200];
void s1_debug_load_settings(const char *settings)
{
    selected=active=bound=0;settings_file[0]=0;
    const char *slash=strrchr(settings,'/'),*back=strrchr(settings,'\\');
    if(!slash || (back && back>slash))slash=back;
    size_t n=slash?(size_t)(slash-settings)+1:0;
    if(n+32>=sizeof settings_file)return;
    memcpy(settings_file,settings,n);strcpy(settings_file+n,"sonic1-bug-fixes.ini");
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
}
void s1_debug_post_reset(void) { if(!bound){active=selected;bound=1;} }
int s1_debug_select(int enabled) { selected=enabled!=0;return 1; }
int s1_debug_selected(void) { return selected; }
int s1_debug_active(void) { return active; }
int s1_debug_effective(void) { return bound?active:selected; }
int s1_debug_commit(void)
{
    char text[100];int n=snprintf(text,sizeof text,"[bug_fixes]\ndebug_transporters=%d\n",selected);
    return *settings_file && genesis_atomic_file_store(settings_file,text,(size_t)n);
}

int s1_debug_hook(uint32_t pc)
{
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
