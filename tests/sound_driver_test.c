/* Command, state and launcher contract checks; audio execution is qualified
 * separately in the built game. All file writes use the caller's scratch. */
#include "sonic1_sound_driver.h"
#include "sonic1_clone_metadata.h"
#include "genesis_runtime.h"
#include "recomp_launcher.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1; } } while(0)
M68KState g_cpu;
uint8_t g_ram[65536],g_rom[0x400000];
static uint32_t called,argument,tail;
void recomp_call_addr(uint32_t pc) { called=pc;argument=g_cpu.D[0]; }
void recomp_tail_call(uint32_t pc) { tail=pc; }
void m68k_write8(uint32_t pc,uint8_t value) { g_ram[pc&65535]=value; }
static int common_context;
static int common_count(void *ctx) { return ctx==&common_context?1:0; }
static int common_package(void *ctx,int n,RecompLauncherCModPackage *out)
{ if(ctx!=&common_context||n)return 0;memset(out,0,sizeof *out);strcpy(out->id,"common");return 1; }
int main(int argc,char **argv)
{
    CHECK(argc==3); /* matching pack and <new scratch>/settings.ini */
    s1_sound_load_settings(argv[2]);CHECK(!s1_sound_selected());CHECK(!s1_sound_active());
    g_ram[0xF003]=0x5A;CHECK(!s1_sound_hook(0x1396));CHECK(g_ram[0xF003]==0x5A);
    uint8_t original[S1_SOUND_STATE_BYTES],clone[S1_SOUND_STATE_BYTES];
    s1_sound_state_save(original);CHECK(s1_sound_state_valid(original));
    if(!s1_sound_available()) {
        CHECK(!s1_sound_select(1));CHECK(!s1_sound_set_pack(argv[1]));
        CHECK(!s1_sound_selected() && !s1_sound_active());
        CHECK(s1_sound_state_valid(original));CHECK(!s1_sound_hook(0x71B4C));
        puts("PASS: build without replacement payload stays original and refuses activation");return 0;
    }
    CHECK(!s1_sound_select(1));CHECK(!s1_sound_selected());
    CHECK(s1_sound_set_pack(argv[1]));CHECK(s1_sound_verified());
    RecompLauncherCModProvider base={0};base.ctx=&common_context;
    base.package_count=base.feature_count=common_count;base.package_get=common_package;
    const RecompLauncherCModProvider *mods=s1_sound_mods(&base);
    CHECK(mods->package_count(mods->ctx)==2 && mods->feature_count(mods->ctx)==2);
    RecompLauncherCModPackage package;
    CHECK(mods->package_get(mods->ctx,1,&package)&&!strcmp(package.id,"common"));
    CHECK(mods->package_get(mods->ctx,0,&package)&&!package.enabled);
    CHECK(mods->feature_enable(mods->ctx,"sonic1.audio.clone-driver","sound-driver",1));
    CHECK(mods->commit(mods->ctx,"unused"));
    s1_sound_load_settings(argv[2]);CHECK(s1_sound_selected());CHECK(!strcmp(s1_sound_pack(),argv[1]));
    uint8_t before_boot[S1_SOUND_STATE_BYTES];s1_sound_state_save(before_boot);
    s1_sound_post_reset();CHECK(s1_sound_active());CHECK(!memcmp(g_rom+0x80000,"S1CD",4));
    CHECK(s1_sound_state_valid(before_boot));
    CHECK(!s1_sound_netplay_allowed());s1_sound_state_save(clone);
    CHECK(s1_sound_state_valid(clone)&&!s1_sound_state_valid(original));
    for(unsigned queue=0;queue<3;queue++)for(unsigned id=0;id<256;id++) {
        uint32_t pc=0x1396+6*queue;
        unsigned mapped=((id>=0x81&&id<=0x93)||(id>=0xA0&&id<=0xD0))?id-0x80:
            id>=0xE0&&id<=0xE4?id+0x1B:0;
        g_cpu.D[0]=0x12340000|id;g_cpu.SR=0x271F;called=argument=0;
        CHECK(s1_sound_hook(pc));
        CHECK(g_cpu.D[0]==(0x12340000|id));
        CHECK((g_cpu.SR&31)==(SR_X|(id&128?SR_N:0)|(!id?SR_Z:0)));
        CHECK(!mapped || (argument==mapped && called==(queue==0?S1_CLONE_QUEUE_MUSIC:queue==1?S1_CLONE_QUEUE_SFX:S1_CLONE_QUEUE_EXTRA)));
        CHECK(mapped || !called);CHECK(g_ram[0xF003]==0x5A);
    }
    CHECK(s1_sound_hook(0x13C4));CHECK(tail==0x13CA);
    CHECK(g_ram[0xF000+S1_CLONE_PAUSE_OFFSET]==1&&g_ram[0xF003]==0x5A);
    CHECK(s1_sound_hook(0x1404));CHECK(tail==0x140A);
    CHECK(g_ram[0xF000+S1_CLONE_PAUSE_OFFSET]==128&&g_ram[0xF003]==0x5A);
    CHECK(s1_sound_hook(0x71B4C));CHECK(called==S1_CLONE_GUARDED_UPDATE);
    CHECK(s1_sound_select(0));CHECK(s1_sound_active()&&s1_sound_state_valid(clone));
    s1_sound_post_reset();CHECK(s1_sound_active()); /* rewind/re-entry cannot apply a pending UI edit */
    CHECK(s1_sound_commit());s1_sound_load_settings(argv[2]);s1_sound_post_reset();
    CHECK(!s1_sound_active()&&!s1_sound_state_valid(clone)&&s1_sound_state_valid(original));
    CHECK(s1_sound_netplay_allowed());CHECK(!s1_sound_hook(0x71B4C));
    puts("PASS: all 256 IDs in three queues, pause isolation, boot selection, snapshot identity, mod composition and settings persistence");
    return 0;
}
