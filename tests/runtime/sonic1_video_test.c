#include "sonic1_video.h"
#include "sonic1_audio.h"
#include "genesis_runtime.h"
#include "video/genesis_vdp.h"
#include "video/genesis_dac.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t g_ram[65536];
uint8_t g_rom[0x400000];
M68KState g_cpu;
void m68k_write8(uint32_t a,uint8_t v) { g_ram[a&65535]=v; }
void m68k_write16(uint32_t a,uint16_t v) { m68k_write8(a,v>>8);m68k_write8(a+1,v); }
void m68k_write32(uint32_t a,uint32_t v) { m68k_write16(a,v>>16);m68k_write16(a+2,v); }
/* Host-side writes from the video adapter (glue_poke*, genesis_host_mem.h). */
void glue_poke8(uint32_t a,uint8_t v){m68k_write8(a,v);}
void glue_poke16(uint32_t a,uint16_t v){m68k_write16(a,v);}
void glue_poke32(uint32_t a,uint32_t v){m68k_write32(a,v);}
static unsigned ring_calls;
static unsigned fm_writes;
void recomp_call_addr(uint32_t a) {
    if(a==0x9C76)++ring_calls;
    if(a==0x7272E && g_cpu.D[0]==0x2B && g_cpu.D[1]==0)++fm_writes;
}
static char reply[1024];
void cmd_send_response(const char *json) { snprintf(reply,sizeof reply,"%s",json); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void word(uint8_t *p, int a, int v) { p[a] = (uint8_t)(v >> 8); p[a+1] = (uint8_t)v; }
static void upload_sat(GVDP *v)
{
    memcpy(v->vram+((v->reg[5]&127u)<<9),g_ram+0xF800,640);
}
static unsigned native_fetches;
static const uint32_t *fetch_native(void *ctx)
{
    ++native_fetches;
    return (const uint32_t *)ctx;
}

static void read_capture(const char *path,uint8_t *out,size_t size)
{
    FILE *f=fopen(path,"rb");CHECK(f);
    CHECK(fread(out,1,size,f)==size);CHECK(fgetc(f)==EOF);fclose(f);
}

int main(int argc,char **argv)
{
    static GVDP v, before;
    static uint8_t ram_before[65536];
    uint32_t native[320], out[8192];
    for (int i = 0; i < 320; ++i) native[i] = 0xFF123400u + i;
    g_ram[0xF600] = 12; g_ram[0xD000] = 1;
    for (int i = 0; i < 5; ++i) g_ram[0xA400+i] = 1;
    for (int i = 0; i < 4; ++i) word(g_ram, 0xB000+i*2, 1);
    v.reg[1] = 64; v.reg[2] = 0x30; v.reg[4] = 7;
    v.reg[5] = 0x50; v.reg[12] = 1; v.reg[13] = 0x3F; v.reg[16] = 1;
    v.cram[1] = 0xE; memset(v.vram+32, 0x11, 32);
    /* Optional offline replay of GHZ RAM/VRAM captures. No game is launched:
     * use the captured VBlank scroll buffers and standard Sonic plane setup.
     * This tests layout reconstruction, not sprite timing or live raster IRQs. */
    if(argc==4 && !strcmp(argv[1],"--replay")) {
        read_capture(argv[2],g_ram,sizeof g_ram);
        read_capture(argv[3],v.vram,sizeof v.vram);
        v.reg[11]=3;
        v.vsram[0]=(uint16_t)((g_ram[0xF616]<<8)|g_ram[0xF617]);
        v.vsram[1]=(uint16_t)((g_ram[0xF618]<<8)|g_ram[0xF619]);
        CHECK(sonic1_video.configure("32:9"));
        for(int y=0;y<224;++y)sonic1_video.scanline(&v,y,native,320,out,796);
        s1_video_command(1,NULL);puts(reply);
        return strstr(reply,"\"terrain_errors\":0,") &&
               strstr(reply,"\"background_errors\":0,") ? 0:1;
    }
    /* Preserve S1's qualified native policy while using the expanded engine
     * callback table: FPS snapshots never take ownership of ordinary rows. */
    CHECK(!sonic1_video.renderer_enabled && !sonic1_video.raster_enabled && !sonic1_video.scanline_presented);
    CHECK(sonic1_video.presentation_configure && sonic1_video.presentation_capture &&
          sonic1_video.interpolate && sonic1_video.presentation_reset);
    CHECK(sonic1_video.configure("off"));
    GameVideoFrame plan=game_video_prepare_frame(&sonic1_video,2560,720,320,224);
    CHECK(plan.adapter==&sonic1_video && !plan.owner && !plan.viewport &&
          game_video_output_width(&plan,320)==320);
    sonic1_video.presentation_configure(1);
    plan=game_video_prepare_frame(&sonic1_video,2560,720,320,224);
    CHECK(!plan.owner && !plan.viewport && game_video_output_width(&plan,320)==320);
    sonic1_video.presentation_configure(0);
    CHECK(sonic1_video.configure("32:9"));
    plan=game_video_prepare_frame(&sonic1_video,2560,720,320,224);
    CHECK(plan.owner==&sonic1_video && plan.viewport && plan.width==796);
    CHECK(sonic1_video.configure("fit"));
    CHECK(sonic1_video.width(320,224,320,224) == 320);
    CHECK(sonic1_video.width(4000,500,320,224) == 1792);
    CHECK(sonic1_video.width(100,1000,320,224) == 320);
    CHECK(sonic1_video.configure("32:9"));
    CHECK(sonic1_video.width(640,448,320,224) == 796);
    CHECK(sonic1_video.configure("64:9"));
    CHECK(sonic1_video.width(640,448,320,224) == 1593);
    CHECK(!sonic1_video.configure("32:0")); CHECK(!sonic1_video.configure("nan:9"));
    CHECK(!sonic1_video.configure("32:9junk")); CHECK(!sonic1_video.configure("inf:1"));
    CHECK(sonic1_video.configure("stage"));
    CHECK(sonic1_video.width(640,448,320,224) == 1280);
    before = v; memcpy(ram_before,g_ram,sizeof g_ram);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(!memcmp(out,native,sizeof native));
    CHECK(out[400] == 0xFFFF0000u); /* terrain beyond the native viewport */
    CHECK(!memcmp(&before,&v,sizeof v)); CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    word(g_ram,0xFF10,500); word(v.vram,0xFC00,(uint16_t)-500);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(!memcmp(out+238,native,sizeof native)); /* centered during scrolling */
    CHECK(out[10] == 0xFFFF0000u); CHECK(out[795] == 0xFFFF0000u);
    g_ram[0xF600] = 4;g_ram[0xD000]=0;
    sonic1_video.scanline(&v,0,native,320,out,8192);
    CHECK(!memcmp(out+(8192-320)/2,native,sizeof native));
    CHECK(out[0] == 0xFF000000u); CHECK(out[8191] == 0xFF000000u);
    /* A scene change does not collapse stage mode back to the window aspect. */
    CHECK(sonic1_video.width(640,448,320,224)==1280);

    /* Title scenery spans the window, stays through an outgoing fade, and
     * does not leak into level select after its background plane is cleared. */
    g_ram[0xD040]=0x0E;g_ram[0xD000]=0;g_ram[0xA440]=1;
    word(v.vram,0xE000,1);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[0]==0xFFFF0000u);CHECK(out[795]==0xFFFF0000u);
    CHECK(!memcmp(out+238,native,sizeof native));
    /* The title's X=0 sprite mask hides Sonic's torso. Publishing a host
     * list must not replace that hardware-composed center with unmasked
     * title sprites; only the surrounding background needs extension. */
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(!memcmp(out+238,native,sizeof native));
    /* Title VBlank installs its own scroll directly, leaving the level's
     * copied FF1C reference stale after MZ. It must not unwrap title Y=0
     * into layout row 4 and turn the extended scene entirely blue. */
    word(g_ram,0xFF1C,0x31A);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[0]==0xFFFF0000u && out[795]==0xFFFF0000u);
    /* A rewritten column in the circular nametable must not sweep through
     * the extended water as a diagonal boundary. The layout and live tile
     * art own the margins; native composition remains authoritative inside. */
    g_ram[0xA441]=1;g_ram[0xA442]=2;
    for(int n=0;n<256;++n)word(g_ram,512+n*2,1);
    for(int n=0;n<4;++n)word(g_ram,0xB008+n*2,2);
    memset(v.vram+64,0x22,32);v.cram[2]=0xE00;
    word(v.vram,0xFC00,0);word(v.vram,0xFC02,94);
    for(int n=0;n<64;++n)word(v.vram,0xE000+25*128+n*2,2);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,200,native,320,out,796);
    CHECK(out[200]==0xFF0000FFu);
    CHECK(out[700]==0xFFFF0000u);
    CHECK(out[238]==0xFF0000FFu); /* no cached-water seam inside the canvas */
    word(v.vram,0xC000+25*128+2,1); /* original foreground overlay */
    word(v.vram,0xA000,328);v.vram[0xA003]=1;
    word(v.vram,0xA004,1);word(v.vram,0xA006,144);
    word(v.vram,0xA008,328);v.vram[0xA00B]=2;word(v.vram,0xA00E,0);
    word(v.vram,0xA010,328);word(v.vram,0xA014,1);word(v.vram,0xA016,160);
    sonic1_video.scanline(&v,200,native,320,out,796);
    CHECK(out[246]==native[8] && out[254]==native[16]);
    CHECK(out[270]==0xFF0000FFu); /* masked sprite must not preserve stale BG */
    word(v.vram,0xC000+25*128+2,0);memset(v.vram+0xA000,0,24);
    memset(v.vram+0xE000+25*128,0,128);
    g_ram[0xA441]=g_ram[0xA442]=0;memset(g_ram+512,0,512);
    memset(g_ram+0xB008,0,8);word(v.vram,0xFC02,0);
    word(g_ram,0xFF1C,0);
    g_ram[0xF600]=12;
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[0]==0xFFFF0000u);
    g_ram[0xF600]=4;word(v.vram,0xE000,0);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[0]==0xFF000000u);
    /* The real level-select Plane B is not empty: it contains font cells,
     * which must not resurrect the title's world layout in either margin. */
    word(v.vram,0xE000,0xE681);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[0]==0xFF000000u && out[795]==0xFF000000u);
    CHECK(!memcmp(out+238,native,sizeof native));
    /* Real restored-title entry leaves 59 scenery cells alongside the font.
     * Those remnants must not override an active level-select composition. */
    word(v.vram,0xE000,0xA001);word(v.vram,0xE004,0xE681);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[0]==0xFF000000u && out[795]==0xFF000000u);
    CHECK(!memcmp(out+238,native,sizeof native));
    g_ram[0xD000]=1;g_ram[0xD040]=0;g_ram[0xA440]=0;

    /* Build a frame-aligned host list: one screen HUD and one world sprite
     * beyond the native screen and SAT's representable coordinate range. */
    g_ram[0xF600]=12;
    word(g_ram,0xF700,500);word(g_ram,0xFF10,500);
    word(v.vram,0xFC00,(uint16_t)-500);
    CHECK(sonic1_video.configure("64:9"));
    CHECK(sonic1_video.width(640,448,320,224)==1593);
    word(g_rom,0x1000,2);g_rom[0x1002]=1;
    g_rom[0x1003]=0;g_rom[0x1004]=0;word(g_rom,0x1005,2);g_rom[0x1007]=0;
    g_ram[0xD040]=0x21;word(g_ram,0xD048,144);word(g_ram,0xD04A,128);
    m68k_write32(0xD044,0x1000);
    g_ram[0xD800]=0x26;g_ram[0xD801]=4;g_ram[0xD819]=8;
    word(g_ram,0xD808,1200);word(g_ram,0xD80C,0);m68k_write32(0xD804,0x1000);
    word(g_ram,0xAC00,4);word(g_ram,0xAC02,0xD040);word(g_ram,0xAC04,0xD800);
    v.cram[2]=0xE00;memset(v.vram+64,0x22,32);
    CHECK(!s1_video_hook(0xD65E));CHECK(!s1_video_hook(0xD746));
    CHECK(g_ram[0xD801]&128); /* extended visibility reaches object behavior */
    upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,1593);
    CHECK(out[16]==0xFF0000FFu); /* left anchored HUD, not native_x+16 */
    CHECK(out[1356]==0xFF0000FFu); /* world sprite at 1200 - (-156) */
    /* Palette0 world objects follow the displayed level fade, while the HUD
     * and Sonic retain the original exception. Dry/water targets can differ. */
    uint16_t saved_cram[64];memcpy(saved_cram,v.cram,sizeof saved_cram);
    memset(g_ram+0xFB80,0,128);memset(g_ram+0xFA00,0,128);
    memset(v.cram+16,0,48*sizeof v.cram[0]);v.cram[2]=0xEEE;
    word(g_ram,0xFBAC,0xEEE);word(g_ram,0xF626,0x202F);g_ram[0xF600]=0x8C;
    const uint16_t white_steps[]={0,0xE00,0xEE0,0xEEE};
    for(unsigned n=0;n<4;++n) {
        v.cram[22]=white_steps[n];
        sonic1_video.scanline(&v,0,native,320,out,1593);
        CHECK(out[1356]==genesis_dac_cram_to_argb(white_steps[n],GENESIS_DAC_NORMAL));
        CHECK(out[16]==genesis_dac_cram_to_argb(0xEEE,GENESIS_DAC_NORMAL));
        /* Terrain can share palette0 too (GHZ's metallic foreground pieces).
         * It belongs to the stage fade, independently of HUD/Sonic sprites. */
        uint32_t terrain=out[800];
        if(n==0)CHECK(terrain==0xFF000000u);
    }
    word(g_ram,0xF626,0x003F);memset(v.cram+16,0,48*sizeof v.cram[0]);
    sonic1_video.scanline(&v,0,native,320,out,1593);
    CHECK(out[1356]==0xFF000000u && out[16]==genesis_dac_cram_to_argb(0xEEE,GENESIS_DAC_NORMAL));
    word(g_ram,0xF626,0x202F);
    g_ram[0xFE10]=1;word(g_ram,0xFBA0,0xE);word(g_ram,0xFA2C,0xEEE);
    v.cram[22]=0xE00;sonic1_video.scanline(&v,0,native,320,out,1593);
    CHECK(out[1356]==genesis_dac_cram_to_argb(0xE00,GENESIS_DAC_NORMAL));
    g_ram[0xD800]=1;s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,1593);
    CHECK(out[1356]==genesis_dac_cram_to_argb(0xEEE,GENESIS_DAC_NORMAL));
    g_ram[0xD800]=0x26;word(g_ram,0xD808,550);
    CHECK(sonic1_video.configure("4:3"));sonic1_video.width(640,448,320,224);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,320);
    CHECK(out[50]==genesis_dac_cram_to_argb(0xEEE,GENESIS_DAC_NORMAL));
    g_ram[0xFE10]=0;word(g_ram,0xF626,0);g_ram[0xF600]=12;word(g_ram,0xD808,1200);
    memset(g_ram+0xFB80,0,128);memset(g_ram+0xFA00,0,128);memcpy(v.cram,saved_cram,sizeof saved_cram);
    CHECK(sonic1_video.configure("64:9"));sonic1_video.width(640,448,320,224);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,1593);
    /* A complete published gameplay row does not depend on the hardware
     * image. Unsupported modes and unpublished/menu scenes still fetch it. */
    {
        uint32_t expected[1593];
        memcpy(expected,out,sizeof expected);
        native_fetches=0;
        sonic1_video.scanline_lazy(&v,0,320,out,1593,fetch_native,native);
        CHECK(native_fetches==0);CHECK(!memcmp(expected,out,sizeof expected));
        sonic1_video.scanline(&v,1,native,256,expected,1593);
        sonic1_video.scanline_lazy(&v,1,256,out,1593,fetch_native,native);
        CHECK(native_fetches==1);CHECK(!memcmp(expected,out,sizeof expected));
        unsigned mode=g_ram[0xF600],player=g_ram[0xD000];g_ram[0xF600]=4;g_ram[0xD000]=0;
        sonic1_video.scanline(&v,0,native,320,expected,1593);
        sonic1_video.scanline_lazy(&v,0,320,out,1593,fetch_native,native);
        CHECK(native_fetches==2);CHECK(!memcmp(expected,out,sizeof expected));
        g_ram[0xF600]=(uint8_t)mode;g_ram[0xD000]=(uint8_t)player;
        sonic1_video.scanline(&v,0,native,320,out,1593);
    }
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":2"));
    /* Level-entry DMA can change unused SAT storage after the captured list.
     * Only linked entries describe the displayed sprites. Tail differences
     * must retain the wide composition; an active-entry change must not. */
    unsigned sat_base=(v.reg[5]&127u)<<9;
    v.vram[(sat_base+516)&65535u]^=1;
    sonic1_video.scanline(&v,0,native,320,out,1593);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":2"));
    v.vram[sat_base]^=1;
    sonic1_video.scanline(&v,0,native,320,out,1593);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"scene\":-1"));
    v.vram[sat_base]^=1;
    sonic1_video.scanline(&v,0,native,320,out,1593);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":2"));
    v.vram[(sat_base+516)&65535u]^=1;
    /* Mid-frame publications cannot overwrite the selected display list. */
    word(g_ram,0xD048,200);
    for(int n=0;n<3;++n) {s1_video_hook(0xD65E);s1_video_hook(0xD746);}
    sonic1_video.scanline(&v,1,native,320,out,1593);
    CHECK(out[16]==0xFF0000FFu);
    /* Widened culling allows both margins, rejects beyond the actual view. */
    g_cpu.A[0]=0xFFD800;
    CHECK(s1_video_hook(0xD87E));CHECK(g_cpu.D[0]==0);
    word(g_ram,0xD808,1600);
    CHECK(s1_video_hook(0xD87E));CHECK(g_cpu.D[0]==1);
    g_cpu.D[0]=(uint16_t)(1200-((500-128)&0xFF80));
    CHECK(!s1_video_hook(0x9C36));CHECK((g_cpu.D[0]&65535)==320);

    /* Full-layout loader: rings live on the host, distant real objects still
     * instantiate. Ring state uses the original group's collected-bit byte. */
    memset(g_ram+0xD800,0,0x1800);memset(g_ram+0xFC00,0,0xC0);
    word(g_rom,0x6B000,0x100);
    word(g_rom,0x6B100,100);word(g_rom,0x6B102,80);g_rom[0x6B104]=0xA5;g_rom[0x6B105]=2;
    word(g_rom,0x6B106,1100);word(g_rom,0x6B108,80);g_rom[0x6B10A]=0xA6;
    word(g_rom,0x6B10C,65535);
    word(g_ram,0xD008,100);word(g_ram,0xD00C,80);g_ram[0xD016]=19;
    g_ram[0xF76C]=0;CHECK(!s1_video_hook(0xD8DA));g_ram[0xF76C]=2;
    CHECK(s1_video_hook(0xD980));
    CHECK(g_ram[0xD800]==0x26);CHECK(g_ram[0xD840]==0);
    CHECK(g_ram[0xD823]==2);CHECK(g_ram[0xFC04]&128);
    CHECK(!s1_video_hook(0x1ADE4));CHECK(ring_calls==1);CHECK(g_ram[0xFC03]&1);
    CHECK(!s1_video_hook(0x1ADE4));CHECK(ring_calls==1); /* no duplicate collection */
    CHECK(s1_video_hook(0xD980));CHECK(g_ram[0xD840]==0); /* no duplicate spawn */

    /* Adopted rings remain visible while the live level fades to Title or
     * SEGA. Freezing activation must not remove their presentation owner. */
    for(unsigned frame=0;frame<4;++frame)word(g_rom,0x9F92+frame*2,8);
    g_rom[0x9F9A]=1;
    for(unsigned n=1;n<=5;++n)g_rom[0x9F9A+n]=0;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    upload_sat(&v);sonic1_video.scanline(&v,0,native,320,out,1593);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":3"));
    for(unsigned mode=0;mode<=4;mode+=4) {
        g_ram[0xF600]=(uint8_t)mode;
        memcpy(ram_before,g_ram,sizeof g_ram);M68KState cpu_before=g_cpu;
        CHECK(!s1_video_hook(0xD980));
        s1_video_hook(0xD65E);s1_video_hook(0xD746);
        upload_sat(&v);sonic1_video.scanline(&v,0,native,320,out,1593);
        s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":3"));
        CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
        CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    }
    g_ram[0xF600]=12;memset(g_rom+0x9F92,0,14);

    /* MZ's first grassy platform is at X=640. A smooth camera at X=1
     * previously spawned it, then the 128px-rounded culler deleted it and
     * the host loader remembered it forever. Load and retain the SAME cells. */
    CHECK(sonic1_video.configure("10:7"));sonic1_video.width(640,448,320,224);
    memset(g_ram+0xD800,0,0x1800);memset(g_ram+0xFC00,0,0xC0);
    word(g_rom,0x6B100,640);word(g_rom,0x6B102,680);g_rom[0x6B104]=0x2F;g_rom[0x6B105]=1;
    word(g_rom,0x6B106,65535);
    word(g_ram,0xF700,1);g_ram[0xF76C]=0;s1_video_hook(0xD8DA);g_ram[0xF76C]=2;
    CHECK(s1_video_hook(0xD980));CHECK(g_ram[0xD800]==0);
    word(g_ram,0xF700,128);CHECK(s1_video_hook(0xD980));CHECK(g_ram[0xD800]==0x2F);
    g_cpu.D[0]=640;CHECK(!s1_video_hook(0xB0DC));CHECK((g_cpu.D[0]&65535)==320);
    /* The same invariant holds with a non-128-aligned wide canvas. */
    CHECK(sonic1_video.configure("32:9"));sonic1_video.width(796,224,320,224);
    memset(g_ram+0xD800,0,0x1800);word(g_rom,0x6B100,1024);word(g_ram,0xF700,238);
    g_ram[0xF76C]=0;s1_video_hook(0xD8DA);g_ram[0xF76C]=2;
    CHECK(s1_video_hook(0xD980));CHECK(g_ram[0xD800]==0);
    word(g_ram,0xF700,274);CHECK(s1_video_hook(0xD980));CHECK(g_ram[0xD800]==0x2F);
    g_cpu.D[0]=1024-128;CHECK(!s1_video_hook(0xB0DC));CHECK((g_cpu.D[0]&65535)==320);
    CHECK(sonic1_video.configure("64:9"));sonic1_video.width(1593,224,320,224);

    /* Special-stage layout: draw a block far beyond the old 320px window,
     * using the game's sine table and live block frame/palette descriptors. */
    memset(g_ram,0,0x4000);memset(g_ram+0xAC00,0,0x400);
    g_ram[0xF600]=16;g_ram[0xD000]=9;
    /* The white-in displays its repeating background before any special
     * sprite list exists. Extend both edges, preserving the native center. */
    before=v;g_ram[0xD040]=0;
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=0x11;
    v.vsram[0]=v.vsram[1]=0;word(v.vram,0xFC00,0);word(v.vram,0xFC02,0);
    memset(v.vram+0xC000,0,128);
    for(unsigned a=0xE000;a<0xE080;a+=2)word(v.vram,a,1);
    v.cram[1]=0xE00;memset(v.vram+32,0x11,32);
    sonic1_video.scanline(&v,0,native,320,out,1593);
    CHECK(out[0]==0xFF0000FFu && out[1592]==0xFF0000FFu);
    CHECK(!memcmp(out+(1593-320)/2,native,sizeof native));
    v=before;
    word(g_ram,0xF700,588);word(g_ram,0xF704,588);
    word(g_rom,0x29F2,0);word(g_rom,0x29F2+128,256);
    m68k_write32(0x4008,0x1000);word(g_ram,0x400C,0);word(g_ram,0x400E,0);
    g_ram[32*128+50]=1;
    CHECK(!s1_video_hook(0xD65E));CHECK(!s1_video_hook(0xD746));
    CHECK(!s1_video_hook(0x1B156));CHECK(!s1_video_hook(0x1B286));
    upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,1593);
    sonic1_video.scanline(&v,112,native,320,out,1593);
    CHECK(out[1228]==0xFF0000FFu);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"scene\":2"));

    CHECK(sonic1_video.configure("off")); CHECK(!sonic1_video.enabled());
    g_cpu.D[0]=123;CHECK(!s1_video_hook(0x9C36));CHECK(g_cpu.D[0]==123);
    /* A clamped wide camera must not make GHZ's sky slide when the native
     * camera starts following Sonic. A striped BG makes sub-tile drift visible. */
    memset(g_ram,0,sizeof g_ram);
    g_ram[0xF600]=12;g_ram[0xD000]=1;
    for(int i=0;i<5;++i)g_ram[0xA400+i]=1;
    g_ram[0xA440]=2;
    for(int i=0;i<256;++i)word(g_ram,512+i*2,1);
    for(int i=0;i<4;++i)word(g_ram,0xB008+i*2,0x8001);
    for(int i=0;i<32;++i)v.vram[32+i]=(uint8_t)(i&1?0x22:0x11);
    v.vsram[0]=v.vsram[1]=0;
    CHECK(sonic1_video.configure("32:9"));sonic1_video.width(796,224,320,224);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    upload_sat(&v);
    word(v.vram,0xFC00,0);word(v.vram,0xFC02,0);
    sonic1_video.scanline(&v,0,native,320,out,796);
    uint32_t sky[32];memcpy(sky,out,sizeof sky);
    word(g_ram,0xF700,32);word(g_ram,0xFF10,32);
    word(v.vram,0xFC00,(uint16_t)-32);word(v.vram,0xFC02,(uint16_t)-12);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(!memcmp(sky,out,sizeof sky));
    /* Every zone's original parallax must follow the stationary wide camera
     * at a stage boundary, including Scrap Brain / Final Zone and ending. */
    const int bg_scrolls[]={12,16,24,32,8,8,12};
    for(int zone=0;zone<7;++zone) {
        g_ram[0xFE10]=(uint8_t)zone;
        v.vsram[1]=zone==3?192:0;
        word(g_ram,0xFF1C,v.vsram[1]);
        word(g_ram,0xF700,0);word(g_ram,0xFF10,0);
        word(v.vram,0xFC00,0);word(v.vram,0xFC02,0);
        sonic1_video.scanline(&v,0,native,320,out,796);memcpy(sky,out,sizeof sky);
        word(g_ram,0xF700,32);word(g_ram,0xFF10,32);
        word(v.vram,0xFC00,(uint16_t)-32);word(v.vram,0xFC02,(uint16_t)-bg_scrolls[zone]);
        sonic1_video.scanline(&v,0,native,320,out,796);
        CHECK(!memcmp(sky,out,sizeof sky));
    }
    /* SLZ's fixed bands use ASR on a negative camera, so even its first
     * pixel of movement advances the native background. Preserve that
     * rounding or the clamped wide background jitters every 2/4/8 pixels. */
    g_ram[0xFE10]=3;
    const int slz_rows[]={28,33,38};
    for(int band=0;band<3;++band) {
        v.vsram[1]=192+slz_rows[band]*16;word(g_ram,0xFF1C,v.vsram[1]);
        word(g_ram,0xF700,0);word(g_ram,0xFF10,0);
        word(v.vram,0xFC00,0);word(v.vram,0xFC02,0);
        sonic1_video.scanline(&v,0,native,320,out,796);memcpy(sky,out,sizeof sky);
        word(g_ram,0xF700,1);word(g_ram,0xFF10,1);
        word(v.vram,0xFC00,(uint16_t)-1);word(v.vram,0xFC02,(uint16_t)-1);
        sonic1_video.scanline(&v,0,native,320,out,796);
        CHECK(!memcmp(sky,out,sizeof sky));
    }
    g_ram[0xFE10]=0;v.vsram[1]=0;word(g_ram,0xFF1C,0);
    word(g_ram,0xF700,32);word(g_ram,0xFF10,32);word(v.vram,0xFC00,(uint16_t)-32);
    word(v.vram,0xFC02,(uint16_t)-12);
    sonic1_video.scanline(&v,0,native,320,out,796);

    /* SEGA is selected before the demo's sixty-frame fade finishes. Keep
     * rendering the outgoing level until its player/planes are replaced. */
    uint32_t level_line[796];memcpy(level_line,out,sizeof level_line);
    for(int demo=0;demo<=1;++demo)for(int mode=0;mode<=4;mode+=4) {
        word(g_ram,0xFFF0,demo);
        /* Start skips select Title; timeout selects SEGA. Both still own
         * the outgoing level until its objects/planes are replaced. */
        g_ram[0xF600]=(uint8_t)mode;
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);
        CHECK(!memcmp(level_line,out,sizeof level_line));
        s1_video_command(1,NULL);CHECK(strstr(reply,"\"scene\":1"));
    }
    /* The giant ring removes Sonic before the signpost/results finish.
     * The outgoing level still owns the canvas, including after the game
     * selects Special Stage but before it installs that scene. */
    word(g_ram,0xFFF0,0);g_ram[0xD000]=0;g_ram[0xF7CD]=1;
    for(int phase=0;phase<3;++phase) {
        g_ram[0xF600]=phase==2?16:12;g_ram[0xD5C0]=phase?0x3A:0;
        memcpy(ram_before,g_ram,sizeof g_ram);M68KState cpu_before=g_cpu;
        CHECK(!s1_video_hook(0xD980));
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);
        CHECK(!memcmp(level_line,out,sizeof level_line));
        CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
        CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
        s1_video_command(1,NULL);CHECK(strstr(reply,"\"scene\":1"));
    }
    g_ram[0xD000]=1;g_ram[0xD5C0]=g_ram[0xF7CD]=0;
    /* The ending uses both normal Sonic and object $87, and retains its
     * world through the outgoing credits fade. Scripted simulation stays
     * original while the same full-width scene renderer remains active. */
    g_ram[0xF600]=24;word(g_ram,0xFFF0,0);g_ram[0xFE10]=6;
    for(int i=0;i<4;++i) {
        g_ram[0xD000]=i==0 || i==3?1:0x87;g_ram[0xF600]=i>=2?28:24;
        word(g_ram,0xFFF0,i==3?0xFFFF:0); /* credits demo -> credits fade */
        s1_video_hook(0xD65E);s1_video_hook(0xD746);
        upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);
        CHECK(!memcmp(level_line,out,sizeof level_line));
        s1_video_command(1,NULL);CHECK(strstr(reply,"\"scene\":1"));
    }
    g_ram[0xD000]=1;g_ram[0xF600]=0;word(g_ram,0xFFF0,1);

    /* Presentation must never alter the simulation of recorded demos:
     * spawning/culling, native rings, CPU flags and on-screen bits stay ROM
     * owned, including during the outgoing fade. */
    memcpy(ram_before,g_ram,sizeof g_ram);M68KState cpu_before=g_cpu;
    const unsigned demo_hooks[]={0xD8DA,0xD980,0xD87E,0xD8A6,0x9C36,0x1ADE4,0xD65E,0xD746};
    for(unsigned i=0;i<sizeof demo_hooks/sizeof demo_hooks[0];++i)
        CHECK(!s1_video_hook(demo_hooks[i]));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));

    /* A normal level also keeps original activation while its card holds
     * or leaves, so aspect width cannot change the ROM animation timeline. */
    word(g_ram,0xFFF0,0);g_ram[0xF600]=12;g_ram[0xD080]=0x34;
    memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
    for(unsigned i=0;i<sizeof demo_hooks/sizeof demo_hooks[0];++i)
        CHECK(!s1_video_hook(demo_hooks[i]));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    g_ram[0xD080]=0;g_ram[0xF600]=0x8C;
    CHECK(!s1_video_hook(0xD980));CHECK(!s1_video_hook(0xD87E));
    g_ram[0xF600]=12;
    /* A title card keeps its centered target when the world camera is
     * clamped at spawn, and its moving position spans the widened canvas. */
    word(g_ram,0xFFF0,0);g_ram[0xF600]=12;
    memset(g_ram+0xD040,0,0x1FC0);memset(g_ram+0xAC00,0,1024);
    word(g_ram,0xF700,0);word(g_ram,0xFF10,0);word(v.vram,0xFC00,0);
    g_ram[0xD080]=0x34;word(g_ram,0xD082,0x8000);word(g_ram,0xD088,240);word(g_ram,0xD08A,128);
    word(g_ram,0xD0B0,288);m68k_write32(0xD084,0x1000);
    word(g_ram,0xAC00,2);word(g_ram,0xAC02,0xD080);
    const unsigned cards[]={0x34,0x3A,0x7E};
    for(unsigned card=0;card<3;++card) {
    g_ram[0xD080]=(uint8_t)cards[card];word(g_ram,0xD088,240);
    g_ram[0xF600]=card==2?16:12;g_ram[0xD000]=card==2?0:1;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[279]==0xFF0000FFu); /* 238 + 160 - 48*796/320 */
    word(g_ram,0xD088,288);s1_video_hook(0xD65E);s1_video_hook(0xD746);
    upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[398]==0xFF0000FFu); /* centered target, independent of world origin */
    if(card==2){s1_video_command(1,NULL);CHECK(strstr(reply,"\"scene\":3"));}
    }
    g_ram[0xF600]=12;g_ram[0xD000]=1;
    /* A composite piece can already be visible while its zero-radius actor
     * reference is outside the right edge (the FZ Eggmobile's pop-in). */
    memset(g_ram+0xD040,0,0x1FC0);memset(g_ram+0xAC00,0,1024);
    g_ram[0xD800]=0x85;g_ram[0xD801]=4;
    word(g_ram,0xD802,0x8000);word(g_ram,0xD808,830);word(g_ram,0xD80C,0);
    m68k_write32(0xD804,0x1000);g_rom[0x1007]=(uint8_t)-64;
    word(g_ram,0xAC00,2);word(g_ram,0xAC02,0xD800);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[766]==0xFF0000FFu);CHECK(g_ram[0xD801]&128);
    /* MZ demos delete objects using native activation bounds. Their final
     * wide-margin pieces remain visual only, with the original oscillator.
     * Removing the guest slot/queue must not remove a still-visible piece. */
    memset(g_ram+0xD040,0,0x1FC0);memset(g_ram+0xAC00,0,1024);
    g_ram[0xF600]=8;word(g_ram,0xFFF0,1);g_ram[0xFE10]=2;
    g_ram[0xA405]=1; /* keep this fixture away from the right stage clamp */
    word(g_ram,0xFE04,100);word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    word(v.vram,0xFC00,(uint16_t)-800);
    word(g_rom,0x6B020,0x200);word(g_rom,0x6B200,600);word(g_rom,0x6B202,8);
    g_rom[0x6B204]=0x2F;g_rom[0x6B205]=0;word(g_rom,0x6B206,0xFFFF);
    word(g_rom,0xB2D4,2);g_rom[0xB2D6]=1;g_rom[0xB2D7]=0;g_rom[0xB2D8]=0;
    word(g_rom,0xB2D9,0);g_rom[0xB2DB]=0;memset(v.vram,0x11,32);v.cram[33]=0xE00;
    g_ram[0xD800]=0x2F;g_ram[0xD801]=4;word(g_ram,0xD802,0xC000);
    word(g_ram,0xD808,600);word(g_ram,0xD80C,8);word(g_ram,0xD82C,8);
    m68k_write32(0xD804,0xB2D4);word(g_ram,0xAC00+5*128,2);word(g_ram,0xAC02+5*128,0xD800);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,8,native,320,out,796);CHECK(out[38]==0xFF0000FFu);
    memset(g_ram+0xD800,0,64);memset(g_ram+0xAC00,0,1024);word(g_ram,0xFE04,101);
    memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,8,native,320,out,796);CHECK(out[38]==0xFF0000FFu);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    /* Stateless previews retain the same valid geometry after load/rewind. */
    word(g_ram,0xFE04,50);s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,8,native,320,out,796);CHECK(out[38]==0xFF0000FFu);
    /* Card departure can allocate the platform before its first display
     * submission. The visual preview must survive that loader handoff. */
    g_ram[0xF600]=12;word(g_ram,0xFFF0,0);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,8,native,320,out,796);CHECK(out[38]==0xFF0000FFu);
    g_ram[0xD800]=0x2F;word(g_ram,0xD808,600);word(g_ram,0xD80C,8);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,8,native,320,out,796);CHECK(out[38]==0xFF0000FFu);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    /* Recorded SYZ fades show both unloaded margin Yadrins and initialized
     * enemies that have not yet queued a sprite. They remain visual-only,
     * hand over once queued, and never resurrect a destroyed enemy. */
    memset(g_ram+0xD800,0,0x1800);memset(g_ram+0xAC00,0,1024);
    g_ram[0xF600]=0x88;word(g_ram,0xFFF0,0xFFFF);word(g_ram,0xFE10,0x402);
    word(g_rom,0x6B048,0x300);word(g_rom,0x6B300,600);word(g_rom,0x6B302,8);
    g_rom[0x6B304]=0xD0;g_rom[0x6B305]=0;
    word(g_rom,0x6B306,780);word(g_rom,0x6B308,8);g_rom[0x6B30A]=0x50;
    word(g_rom,0x6B30C,65535);
    g_rom[0xF84E]=0;g_rom[0xF84F]=0;word(g_rom,0xF850,0x1000);word(g_rom,0xF856,0x8001);
    g_ram[0xD800]=0x50;g_ram[0xD801]=4;word(g_ram,0xD802,0x8001);
    m68k_write32(0xD804,0x1000);word(g_ram,0xD808,780);word(g_ram,0xD80C,8);
    g_ram[0xF76C]=0;s1_video_hook(0xD8DA);g_ram[0xF76C]=2;
    memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":2"));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    word(g_ram,0xAC00+4*128,2);word(g_ram,0xAC02+4*128,0xD800);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":2"));
    g_ram[0xFC03]=128;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":1"));
    /* A head initialized without a queue entry stays visible through the
     * fade; queuing it later must neither duplicate it nor mutate the guest. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    g_ram[0xF600]=0x88;g_ram[0xD000]=1;word(g_ram,0xFFF0,0xFFFF);word(g_ram,0xFE10,0x302);
    for(int n=0;n<6;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    word(v.vram,0xFC00,-800);v.cram[1]=0xE00;memset(v.vram+32,0x11,32);
    word(g_rom,0x1300,2);g_rom[0x1302]=1;g_rom[0x1303]=0;g_rom[0x1304]=0;word(g_rom,0x1305,0);g_rom[0x1307]=0;
    g_ram[0xD800]=0x60;g_ram[0xD801]=4;g_ram[0xD818]=4;g_ram[0xD824]=4;
    word(g_ram,0xD802,0x8001);m68k_write32(0xD804,0x1300);word(g_ram,0xD808,600);word(g_ram,0xD80C,8);
    memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF0000FFu);s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":1"));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    word(g_ram,0xAC00+4*128,2);word(g_ram,0xAC02+4*128,0xD800);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":1"));
    memset(g_ram+0xAC00,0,1024);g_ram[0xF600]=8;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":0"));
    /* A saturated native priority queue must not hide a real wider-view
     * draw attempt. Preserve its pending submission across rollback, leave
     * guest visibility alone, and never keep a deleted or unsubmitted orb. */
    word(g_ram,0xFFF0,0);word(g_ram,0xFE10,0x100);
    memset(g_ram+0xD800,0,0x1800);
    for(unsigned n=0;n<63;++n) {
        unsigned o=0xD800+n*64;
        g_ram[o]=0x60;g_ram[o+1]=4;g_ram[o+0x18]=4;g_ram[o+0x24]=6;
        word(g_ram,o+2,0x8001);m68k_write32(o+4,0x1300);
        word(g_ram,o+8,4000);word(g_ram,o+12,8);
        word(g_ram,0xAC02+4*128+n*2,o);
    }
    word(g_ram,0xAC00+4*128,126);
    memcpy(g_ram+0xE800,g_ram+0xD800,64);word(g_ram,0xE808,600);
    g_cpu.A[0]=g_cpu.A[1]=0xFFE800;
    memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
    CHECK(!s1_video_hook(0xD604));CHECK(!s1_video_hook(0xD622));
    size_t pending_size=s1_video_rb_save(NULL,0);void *pending=malloc(pending_size);CHECK(pending);
    CHECK(s1_video_rb_save(pending,pending_size)==pending_size);
    for(unsigned n=0;n<2;++n) {
        if(n)CHECK(s1_video_rb_load(pending,pending_size));
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
        CHECK(out[38]==0xFF0000FFu);s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":1"));
        CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    }
    free(pending);
    s1_video_hook(0xD604);memset(g_ram+0xE800,0,64);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":0"));
    memcpy(g_ram+0xE800,g_ram+0xD800,64);word(g_ram,0xE808,600);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":0"));
    /* A scripted FZ boss is created at the original camera threshold, after
     * the wide viewport can see its cylinders. An unrelated pending PLC
     * must not suppress their draw-only preview or allocate the boss early. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x502);g_ram[0xF742]=2;
    for(int n=0;n<6;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-800);
    v.cram[1]=0xE00;memset(v.vram+32,0x11,32);
    word(g_rom,0x1A494,0x8001);g_rom[0x1A49A]=0;g_rom[0x1A49B]=0;
    g_rom[0x1A49C]=0x10;g_rom[0x1A49D]=0;
    word(g_rom,0x1000,2);g_rom[0x1002]=1;memset(g_rom+0x1003,0,5);
    for(int n=0;n<4;++n){word(g_rom,0x1A462+n*4,5000);word(g_rom,0x1A464+n*4,8);}
    word(g_rom,0x1A462,600);g_ram[0xF683]=1;word(g_ram,0xF6F8,1);
    memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF0000FFu);CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    /* The original initialized object replaces its preview exactly once. */
    g_ram[0xD800]=0x84;g_ram[0xD801]=4;g_ram[0xD824]=2;
    word(g_ram,0xD802,0x8001);m68k_write32(0xFFD804,0x1000);
    word(g_ram,0xD808,600);word(g_ram,0xD80C,8);
    word(g_ram,0xAC00+3*128,2);word(g_ram,0xAC02+3*128,0xD800);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF0000FFu);s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":1"));
    /* Once escape begins, removed cylinders must not be resurrected. */
    memset(g_ram+0xD800,0,64);memset(g_ram+0xAC00,0,1024);
    g_ram[0xD840]=0x85;g_ram[0xD864]=2;g_ram[0xD874]=6;g_ram[0xF742]=4;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF000000u);
    /* Wide GHZ entry placements exist through card departure without early
     * guest activation; queued native ring/spring/rock sprites replace them
     * exactly once. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    g_ram[0xF600]=0x8C;g_ram[0xD000]=1;g_ram[0xD080]=0x34;
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    word(g_rom,0x6B000,0x500);
    word(g_rom,0x6B500,600);word(g_rom,0x6B502,8);g_rom[0x6B504]=0xA5;g_rom[0x6B505]=0x31;
    word(g_rom,0x6B506,620);word(g_rom,0x6B508,8);g_rom[0x6B50A]=0x41;g_rom[0x6B50B]=2;
    word(g_rom,0x6B50C,640);word(g_rom,0x6B50E,8);g_rom[0x6B510]=0x3B;g_rom[0x6B511]=0;
    word(g_rom,0x6B512,65535);
    g_rom[0xDB0E]=g_rom[0xDB0F]=0;word(g_rom,0xDB10,0x1300);word(g_rom,0xDB16,1);
    g_rom[0xD068]=g_rom[0xD069]=0;word(g_rom,0xD06A,0x1300);word(g_rom,0xD070,1);
    word(g_rom,0x9F92,2);g_rom[0x9F94]=1;memset(g_rom+0x9F95,0,5);
    word(g_rom,0x1300,2);g_rom[0x1302]=1;memset(g_rom+0x1303,0,5);
    g_ram[0xF76C]=0;s1_video_hook(0xD8DA);g_ram[0xF76C]=2;
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":4"));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    const unsigned entry_ids[]={0x25,0x25,0x41,0x3B};
    for(unsigned n=0;n<4;++n) {
        unsigned o=0xD800+n*64,pri=n<2?2:4;
        g_ram[o]=(uint8_t)entry_ids[n];g_ram[o+1]=4;m68k_write32(o+4,0x1300);
        word(g_ram,o+8,n<2?600:n==2?620:640);word(g_ram,o+12,n==1?24:8);
        word(g_ram,0xAC02+pri*128+(n&1)*2,o);word(g_ram,0xAC00+pri*128,4);
    }
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":4"));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    /* Caterkiller's floor-search initialization leaves its head and body
     * unqueued during the frozen entry. Preview the complete initial pose,
     * follow its falling head, then hand off without duplicate segments. */
    memset(g_ram,0,sizeof g_ram);g_ram[0xF600]=0x8C;
    g_ram[0xD000]=1;g_ram[0xD080]=0x34;word(g_ram,0xFE10,0x201);
    for(unsigned n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF704,512);word(g_rom,0x6B024,0x600);
    word(g_rom,0x6B600,512);word(g_rom,0x6B602,680);
    g_rom[0x6B604]=0xF8;g_rom[0x6B605]=0;word(g_rom,0x6B606,65535);
    for(unsigned frame=0;frame<32;++frame)word(g_rom,0x16D6E+frame*2,0x82);
    g_rom[0x16DF0]=1;memset(g_rom+0x16DF1,0,5);
    g_ram[0xD800]=0x78;g_ram[0xD823]=1;
    word(g_ram,0xD808,512);word(g_ram,0xD80C,680);
    s1_video_hook(0xD8DA);
    for(unsigned pass=0;pass<3;++pass) {
        if(pass==1)word(g_ram,0xD80C,686);
        if(pass==2)for(unsigned n=0;n<4;++n) {
            unsigned o=0xD800+n*64,pri=n?5:4;
            g_ram[o]=0x78;g_ram[o+1]=4;g_ram[o+0x24]=(uint8_t)(2+n*2);
            m68k_write32(o+4,0x16D6E);word(g_ram,o+8,512+n*12);
            word(g_ram,o+12,696);g_ram[o+0x1A]=n?8:0;
            word(g_ram,0xAC00+pri*128,n?6:2);
            word(g_ram,0xAC02+pri*128+(n?n-1:0)*2,o);
        }
        memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
        CHECK(strstr(reply,"\"sprites\":4"));
        CHECK(!memcmp(ram_before,g_ram,sizeof g_ram) && !memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    }
    memset(g_ram+0xD800,0,256);memset(g_ram+0xAC00,0,1024);g_ram[0xFC03]=128;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"sprites\":0"));

    /* Ending animals hand off from a resting preview to their live sprite,
     * then retain a draw-only tail after native deletion. No guest lifetime
     * or visibility changes, and a consumed tail must not return. */
    memset(g_ram,0,sizeof g_ram);memset(g_ram+0xAC00,0,1024);
    g_ram[0xF600]=24;g_ram[0xD000]=1;word(g_ram,0xFE10,0x600);
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    word(g_rom,0x6B060,0x400);word(g_rom,0x6B400,600);word(g_rom,0x6B402,8);
    g_rom[0x6B404]=0x28;g_rom[0x6B405]=13;word(g_rom,0x6B406,65535);
    g_rom[0x8FD6]=0;g_rom[0x8FD7]=0;word(g_rom,0x8FD8,0x1000);word(g_rom,0x8FFC,0x8001);
    g_ram[0xF76C]=0;s1_video_hook(0xD8DA);g_ram[0xF76C]=2;
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":1"));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    g_ram[0xD800]=0x28;g_ram[0xD801]=4;g_ram[0xD828]=13;g_ram[0xD824]=4;
    word(g_ram,0xD802,0x8001);m68k_write32(0xD804,0x1000);
    word(g_ram,0xD808,600);word(g_ram,0xD80C,8);
    word(g_ram,0xAC00+6*128,2);word(g_ram,0xAC02+6*128,0xD800);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":1"));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    memset(g_ram+0xD800,0,64);memset(g_ram+0xAC00,0,1024);word(g_ram,0xFE04,1);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":1"));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    word(g_ram,0xF700,1600);word(g_ram,0xFE04,2);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    word(g_ram,0xF700,800);word(g_ram,0xFE04,3);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":0"));
    /* FM6 music resumes after the 1-UP without changing guest registers or
     * touching a song that restores DAC percussion instead. */
    g_ram[0xF160]=128;g_ram[0xF040]=0;cpu_before=g_cpu;
    CHECK(!s1_audio_hook(0x72B24));CHECK(fm_writes==1);CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    g_ram[0xF040]=128;CHECK(!s1_audio_hook(0x72B24));CHECK(fm_writes==1);
    g_ram[0xF040]=0;g_ram[0xF160]=0;CHECK(!s1_audio_hook(0x72B24));CHECK(fm_writes==1);
    /* Signpost art is available to wide presentation before its PLC, with
     * the original XOR-row format and without changing guest state. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    g_ram[0xF600]=8;g_ram[0xD000]=1;word(g_ram,0xFFF0,1);
    for(int n=0;n<6;++n)g_ram[0xA400+n]=1;
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    v.cram[1]=0xE00;v.cram[2]=14;memset(v.vram+0x680*32,0x22,32);
    word(g_rom,0x3A53E,0x8001);g_rom[0x3A540]=255;memset(g_rom+0x3A541,0,16);
    unsigned bit=0x3A541*8;
    for(int row=0;row<8;++row)for(int n=12;n>=0;--n,++bit)
        g_rom[bit>>3]|=(uint8_t)(((0x1FF1u>>n)&1)<<(7-(bit&7)));
    word(g_rom,0x1200,16);g_rom[0x1210]=1;g_rom[0x1211]=0;g_rom[0x1212]=0;
    word(g_rom,0x1213,0);g_rom[0x1215]=0;
    g_ram[0xD800]=0x0D;g_ram[0xD801]=4;word(g_ram,0xD802,0x680);
    m68k_write32(0xD804,0x1200);word(g_ram,0xD808,100);word(g_ram,0xD80C,10);
    word(g_ram,0xAC00,2);word(g_ram,0xAC02,0xD800);
    memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,10,native,320,out,796);CHECK(out[100]==0xFF0000FFu);
    sonic1_video.scanline(&v,11,native,320,out,796);CHECK(out[100]==0xFF000000u);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    /* Rewind must recover the displayed mappings as well as guest RAM: the
     * later frame's sprite history may otherwise survive a frozen fade. */
    size_t state_size=s1_video_rb_save(NULL,0);
    uint8_t *video_state=malloc(state_size),*roundtrip=malloc(state_size);
    CHECK(video_state && roundtrip);
    CHECK(!s1_video_rb_save(video_state,state_size-1));
    CHECK(s1_video_rb_save(video_state,state_size)==state_size);
    uint32_t restored_row[796];memcpy(restored_row,out,sizeof restored_row);
    memcpy(ram_before,g_ram,sizeof g_ram);before=v;cpu_before=g_cpu;
    for(unsigned n=0;n<4;++n) {
        word(g_ram,0xD808,900+n*8);
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);
    }
    memcpy(g_ram,ram_before,sizeof g_ram);v=before;
    CHECK(!s1_video_rb_load(video_state,state_size-1));
    CHECK(s1_video_rb_load(video_state,state_size));
    CHECK(s1_video_rb_save(roundtrip,state_size)==state_size);
    CHECK(!memcmp(video_state,roundtrip,state_size));
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(!memcmp(restored_row,out,sizeof restored_row));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    video_state[0]^=1;CHECK(!s1_video_rb_load(video_state,state_size));
    CHECK(s1_video_rb_save(roundtrip,state_size)==state_size);
    video_state[0]^=1;CHECK(!memcmp(video_state,roundtrip,state_size));
    free(roundtrip);free(video_state);
    puts("Sonic 1 video/audio: geometry, terrain, sprites, demo previews, mode continuity and FM6 restore passed");

    {

    /* FPS-only capture must not adopt rings/objects or change the normal
     * renderer's publication/counters. Fractional draws own their RAM/art. */
    CHECK(sonic1_video.configure("off"));memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    g_ram[0xF600]=12;g_ram[0xD000]=1;g_ram[0xD001]=4;
    for(int n=0;n<5;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xAC00,4);word(g_ram,0xAC02,0xD000);word(g_ram,0xAC04,0xD040);
    g_ram[0xD040]=2;g_ram[0xD041]=8;word(g_ram,0xD042,1);m68k_write32(0xD044,0x1000);
    m68k_write32(0xD004,0x1000);word(g_ram,0xD002,1);
    word(g_rom,0x1000,2);g_rom[0x1002]=1;memset(g_rom+0x1003,0,5);
    v.reg[1]=64;v.reg[5]=0x50;v.reg[12]=1;v.reg[13]=0x3F;v.cram[1]=14;
    memset(v.vram+32,0x11,32);
    static uint32_t fractional[320*224];
    char canonical_before[1024];s1_video_command(1,NULL);snprintf(canonical_before,sizeof canonical_before,"%s",reply);
    sonic1_video.presentation_configure(1);
    CHECK(!sonic1_video.enabled() && !sonic1_video.width(1920,1080,320,224));
    for(unsigned tick=1;tick<=2;++tick) {
        word(g_ram,0xD008,100+(int)(tick-1)*8);word(g_ram,0xD00C,10);word(g_ram,0xFE04,(int)tick);
        word(g_ram,0xD048,200+(int)(tick-1)*8);word(g_ram,0xD04C,10);word(g_ram,0xF708,(int)(tick-1)*8);
        memcpy(ram_before,g_ram,sizeof ram_before);M68KState cpu_before=g_cpu;
        s1_video_hook(0xD65E);s1_video_hook(0xD746);
        CHECK(!memcmp(ram_before,g_ram,sizeof ram_before) && !memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
        for(unsigned x=0;x<320;++x)native[x]=0xFF123400u+tick;
        for(int y=0;y<224;++y)sonic1_video.presentation_capture(&v,y,native,320,320,NULL);
    }
    s1_video_command(1,NULL);CHECK(!strcmp(reply,canonical_before));
    memset(g_ram,0xAA,sizeof g_ram);memset(v.vram+32,0x22,32);v.cram[1]=0xE00;
    M68KState cpu_before=g_cpu;
    CHECK(sonic1_video.interpolate(0,fractional,320,224) && fractional[0]==0xFF123401);
    CHECK(sonic1_video.interpolate(1,fractional,320,224) && fractional[0]==0xFF123402);
    CHECK(sonic1_video.interpolate(.5,fractional,320,224));
    for(int x=100;x<116;++x)CHECK(fractional[10*320+x]==(x>=104 && x<112?0xFFFF0000u:0xFF000000u));
    for(int x=196;x<212;++x)CHECK(fractional[10*320+x]==(x>=200 && x<208?0xFFFF0000u:0xFF000000u));
    CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    for(unsigned n=0;n<sizeof g_ram;++n)CHECK(g_ram[n]==0xAA);
    s1_video_command(1,NULL);CHECK(!strcmp(reply,canonical_before));
    /* Ordinary vertical scrolling must also use the fractional VSRAM. */
    memcpy(g_ram,ram_before,sizeof g_ram);word(g_ram,0xAC00,0);word(g_ram,0xFE04,3);
    word(g_ram,0xFF14,8);v.vsram[0]=8;
    for(int n=1;n<=8;++n)v.cram[n]=(uint16_t)((n&7)*2);
    for(int y=0;y<8;++y)memset(v.vram+y*4,(y+1)*17,4);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    for(int y=0;y<224;++y)sonic1_video.presentation_capture(&v,y,native,320,320,NULL);
    CHECK(sonic1_video.interpolate(.5,fractional,320,224));
    CHECK(fractional[0]==genesis_dac_cram_to_argb(10,GENESIS_DAC_NORMAL));
    sonic1_video.presentation_reset();CHECK(!sonic1_video.interpolate(.5,fractional,320,224));
    sonic1_video.presentation_configure(0);
    /* At the GHZ left clamp, both completed frames have the same stationary
     * world/background. A fractional frame must not introduce a one-pixel
     * parallax excursion between those identical endpoints. */
    CHECK(sonic1_video.configure("32:9"));sonic1_video.width(640,448,320,224);
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    g_ram[0xF600]=12;g_ram[0xD000]=1;
    for(int n=0;n<5;++n){g_ram[0xA400+n]=1;g_ram[0xA440+n]=2;}
    for(int n=0;n<256;++n)word(g_ram,0x200+n*2,1);
    for(int n=0;n<4;++n)word(g_ram,0xB008+n*2,1+(n&1));
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    memset(v.vram+32,0x11,32);memset(v.vram+64,0x22,32);
    v.cram[1]=14;v.cram[2]=0xE00;
    sonic1_video.presentation_configure(1);
    uint32_t row[796],stationary[796];static uint32_t clamped[796*224];
    for(int tick=0;tick<2;++tick) {
        word(g_ram,0xFE04,4+tick);word(g_ram,0xF700,32+tick*8);word(g_ram,0xFF10,32+tick*8);
        word(v.vram,0xFC00,-32-tick*8);word(v.vram,0xFC02,-12-tick*3);
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        for(int y=0;y<224;++y) {
            sonic1_video.scanline(&v,y,native,320,row,796);
            sonic1_video.presentation_capture(&v,y,row,796,320,NULL);
        }
    }
    CHECK(sonic1_video.interpolate(0,clamped,796,224));memcpy(stationary,clamped,sizeof stationary);
    CHECK(sonic1_video.interpolate(1,clamped,796,224));CHECK(!memcmp(stationary,clamped,sizeof stationary));
    CHECK(sonic1_video.interpolate(.5,clamped,796,224));CHECK(!memcmp(stationary,clamped,sizeof stationary));
    sonic1_video.presentation_reset();sonic1_video.presentation_configure(0);
    puts("Sonic 1 custom video: geometry, terrain, anchored HUD, wide sprites, culling, mode continuity and native opt-out and owned interpolation passed");
    }
    /* Final Zone's escape camera legitimately passes the populated layout
     * edge. Center its original framing through the outgoing level fade;
     * ordinary stage clamping and guest activation remain unchanged. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    CHECK(sonic1_video.configure("32:9"));sonic1_video.width(796,224,320,224);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x502);
    for(int n=0;n<40;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,0x2700);word(g_ram,0xFF10,0x2700);g_ram[0xF742]=6;
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-0x2700);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"view_left\":9444,") && strstr(reply,"\"native_x\":540,"));
    g_ram[0xF742]=8;memcpy(ram_before,g_ram,sizeof g_ram);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"view_left\":9746,") && strstr(reply,"\"native_x\":238,"));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    word(g_ram,0xD808,10400);word(g_ram,0xD80C,10);g_cpu.A[0]=0xFFD800;
    CHECK(s1_video_hook(0xD87E) && g_cpu.D[0]==1);
    g_ram[0xD800]=0x26;g_ram[0xD801]=4;m68k_write32(0xFFD804,0x1000);
    word(g_rom,0x1000,2);g_rom[0x1002]=1;memset(g_rom+0x1003,0,5);
    word(g_ram,0xAC00+2*128,2);word(g_ram,0xAC02+2*128,0xD800);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    CHECK(!(g_ram[0xD801]&128)); /* newly visible only to presentation */
    word(g_ram,0xD808,9500);g_ram[0xD801]=4;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    CHECK(g_ram[0xD801]&128); /* prior guest visibility survives reframing */
    /* A damaged cockpit gets a render-only tail after native deletion.
     * Guest flags/timing stay exact, and the tail belongs to rewind state. */
    word(g_ram,0xD808,10400);g_ram[0xD801]=4;g_ram[0xD800]=0x85;
    g_ram[0xD824]=8;m68k_write32(0xFFD834,0xFFFFD840);
    g_ram[0xD818]=2;g_ram[0xD858]=3;
    g_ram[0xD840]=0x85;g_ram[0xD864]=2;g_ram[0xD874]=14;
    word(g_ram,0xD848,10408);word(g_ram,0xD84C,10);
    m68k_write32(0xFFD844,0x1100);word(g_ram,0xD842,3);
    word(g_rom,0x1100,2);g_rom[0x1102]=1;memset(g_rom+0x1103,0,5);g_rom[0x1107]=8;
    word(g_rom,0x1000,4);word(g_rom,0x1002,4);
    g_rom[0x1004]=1;memset(g_rom+0x1005,0,5); /* both damaged poses */
    word(g_ram,0xD802,0x8001);memset(v.vram+32,0x22,32);v.cram[2]=0xE00;
    memset(v.vram+96,0x33,32);v.cram[3]=0xE0;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    CHECK(!(g_ram[0xD801]&128));
    g_ram[0xD800]=0;word(g_ram,0xAC00+2*128,0);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    upload_sat(&v);sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,10,native,320,out,796);
    CHECK(out[662]==0xFF0000FFu);
    /* Extend visible damage effects without executing guest objects/RNG or
     * adding sound. The native initialized explosion supplies its art. */
    g_ram[0xD880]=0x3F;g_ram[0xD898]=1;g_ram[0xD881]=4;
    m68k_write32(0xFFD884,0x1000);word(g_ram,0xD882,0x8001);
    word(g_ram,0xFE04,8);g_ram[0xFE0F]=0;
    memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram) && !memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"sprites\":2"));
    size_t cockpit_size=s1_video_rb_save(NULL,0);void *cockpit_state=malloc(cockpit_size);
    CHECK(cockpit_state && s1_video_rb_save(cockpit_state,cockpit_size)==cockpit_size);
    /* A continuation owner is mandatory; anonymous cosmetic particles are
     * the only records that may have no guest slot. Reject a corrupt owner. */
    unsigned owner_records=0;
    for(size_t i=0;i+68<=cockpit_size;++i) {
        uint8_t *bytes=(uint8_t *)cockpit_state;uint16_t owner;
        memcpy(&owner,bytes+i+64,sizeof owner);
        if(owner!=0xD840 || memcmp(bytes+i,g_ram+0xD840,64))continue;
        uint16_t invalid=0;memcpy(bytes+i+64,&invalid,sizeof invalid);
        CHECK(!s1_video_rb_load(cockpit_state,cockpit_size));
        memcpy(bytes+i+64,&owner,sizeof owner);++owner_records;
    }
    CHECK(owner_records==1);
    g_ram[0xD840]=0;s1_video_hook(0xD65E);s1_video_hook(0xD746);
    g_ram[0xD840]=0x85;CHECK(s1_video_rb_load(cockpit_state,cockpit_size));free(cockpit_state);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,10,native,320,out,796);CHECK(out[662]==0xFF0000FFu);
    g_ram[0xF600]=24;g_ram[0xD840]=0;memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,10,native,320,out,796);CHECK(out[662]==0xFF0000FFu);
    CHECK(out[670]==0xFF00FF00u); /* the deleted parent remains through fade */
    g_ram[0xF600]=24;sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"view_left\":9746,"));
    word(g_ram,0xFE10,0x600);g_ram[0xF742]=0;
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"view_left\":9444,"));
    /* The last Deform_GHZ call after Start selects Title's zero A scroll.
     * That must not move the still-displayed demo to a 1024px page edge. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    g_ram[0xF600]=8;g_ram[0xD000]=1;word(g_ram,0xFFF0,1);
    for(int n=0;n<20;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,3154);word(g_ram,0xFF10,3154);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    g_ram[0xF600]=4;word(g_ram,0xF700,3160);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"camera\":3160,"));
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    /* SBZ's scripted exit moves only the displayed player. Its continuation
     * survives rollback, joins the real player during the pan, and hands the
     * initial floor to native pieces without duplicating or resurrecting it. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    s1_video_hook(0xD8DA);
    CHECK(sonic1_video.configure("32:9"));
    CHECK(sonic1_video.width(3200,900,320,224)==796);
    g_ram[0xF600]=12;word(g_ram,0xFE10,0x501);g_ram[0xF742]=0;
    g_ram[0xD000]=1;g_ram[0xD001]=0x84;word(g_ram,0xD002,0x8001);
    m68k_write32(0xFFD004,0x1000);word(g_ram,0xD008,0x1FA8);
    word(g_ram,0xD00C,0x574);g_ram[0xD5C0]=0x3A;
    word(g_ram,0xF700,0x1E40);word(g_ram,0xFF10,0x1E40);
    word(g_ram,0xF704,0x510);word(g_ram,0xF72A,0x1E40);
    for(int n=0;n<40;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xAD00,2);word(g_ram,0xAD02,0xD000);
    word(g_rom,0x1000,2);g_rom[0x1002]=1;g_rom[0x1003]=0;
    g_rom[0x1004]=0;word(g_rom,0x1005,0);g_rom[0x1007]=0;
    word(g_rom,0x1100,2);g_rom[0x1102]=1;g_rom[0x1103]=0xF0;
    g_rom[0x1104]=15;word(g_rom,0x1105,0);g_rom[0x1107]=0xF0;
    word(g_rom,0x19BBA,0);word(g_rom,0x19BBC,0x1100);
    word(g_rom,0x19BC2,0x4518);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    v.cram[1]=0xE;memset(v.vram+32,0x11,32);
    word(v.vram,0xFC00,(uint16_t)-0x1E40);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    word(g_ram,0xFE04,1);memcpy(ram_before,g_ram,sizeof g_ram);
    before=v;cpu_before=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    CHECK(!memcmp(&before,&v,sizeof v) && !memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,100,native,320,out,796);
    CHECK(out[604]==0xFFFF0000u && out[598]==0xFF000000u);
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":5"));
    g_ram[0xF742]=2;
    size_t runoff_size=s1_video_rb_save(NULL,0);void *runoff_state=malloc(runoff_size);
    CHECK(runoff_state && s1_video_rb_save(runoff_state,runoff_size)==runoff_size);
    word(g_ram,0xFE04,2);s1_video_hook(0xD65E);s1_video_hook(0xD746);
    word(g_ram,0xFE04,1);CHECK(s1_video_rb_load(runoff_state,runoff_size));free(runoff_state);
    word(g_ram,0xFE04,2);s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,100,native,320,out,796);CHECK(out[610]==0xFFFF0000u);
    g_ram[0xD5C0]=0;word(g_ram,0xFE04,100);word(g_ram,0xD008,0x20B0);
    word(g_ram,0xF700,0x2016);word(g_ram,0xFF10,0x2016);
    word(v.vram,0xFC00,(uint16_t)-0x2016);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,100,native,320,out,796);CHECK(out[398]==0xFFFF0000u);
    word(g_ram,0xD008,0x20B6);word(g_ram,0xFE04,101);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,100,native,320,out,796);CHECK(out[398]==0xFFFF0000u);
    word(g_ram,0xAD80,2);word(g_ram,0xAD82,0xD800);
    g_ram[0xD800]=0x83;g_ram[0xD801]=0x84;g_ram[0xD824]=8;
    m68k_write32(0xFFD804,0x1100);word(g_ram,0xD802,0x4518);
    word(g_ram,0xD808,0x2010);word(g_ram,0xD80C,0x5D0);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"sprites\":9"));
    g_ram[0xD800]=0;word(g_ram,0xAD80,0);g_ram[0xF742]=6;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"sprites\":1"));
    /* A runoff continuation keeps a fractional entry speed and follows a
     * lower floor instead of sliding horizontally above it. The original
     * player, collision layout and result timing remain untouched. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;g_ram[0xD001]=0x84;
    word(g_ram,0xFE10,0x202);g_ram[0xF742]=4;g_ram[0xF7CC]=1;
    for(unsigned n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,1024);word(g_ram,0xFF10,1024);word(g_ram,0xF704,96);
    word(g_ram,0xF72A,1024);word(g_ram,0xD008,1384);word(g_ram,0xD00C,108);
    word(g_ram,0xD010,0x480);g_ram[0xD016]=19;g_ram[0xD017]=9;
    word(g_ram,0xD002,0x8001);m68k_write32(0xD004,0x1000);
    word(g_rom,0x1000,2);g_rom[0x1002]=1;memset(g_rom+0x1003,0,5);
    word(g_ram,0xAD00,2);word(g_ram,0xAD02,0xD000);
    m68k_write32(0xF796,0x2000);g_rom[0x2001]=1;memset(g_rom+0x62A10,16,16);
    for(unsigned col=0;col<16;++col)word(g_ram,(col<8?128:144)*2+col*2,0x2001);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    v.cram[1]=0xE;memset(v.vram+32,0x11,32);word(v.vram,0xFC00,-1024);
    for(unsigned frame=0;frame<=30;++frame) {
        word(g_ram,0xFE04,frame);memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        CHECK(!memcmp(ram_before,g_ram,sizeof g_ram) && !memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
        sonic1_video.scanline(&v,0,native,320,out,796);
        if(frame==1) {
            sonic1_video.scanline(&v,12,native,320,out,796);
            CHECK(out[602]==0xFFFF0000u && out[610]==0xFF000000u);
        }
    }
    sonic1_video.scanline(&v,28,native,320,out,796);CHECK(out[733]==0xFFFF0000u);
    sonic1_video.scanline(&v,12,native,320,out,796);CHECK(out[733]==0xFF000000u);
    /* Early wide-view capsule/ring pixels come from complete ROM art even
     * while those VRAM tiles contain another asset. Capsule ink also stays
     * black during the shared boss hit flash, without changing CRAM/RAM. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=0x8C;g_ram[0xD000]=1;
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    word(g_rom,0x1000,2);g_rom[0x1002]=1;memset(g_rom+0x1003,0,5);
    word(g_rom,0x5D7B0,1);g_rom[0x5D7B2]=0x81;g_rom[0x5D7B3]=0x71;
    g_rom[0x5D7B4]=0;g_rom[0x5D7B5]=255;memset(g_rom+0x5D7B6,0,8);
    memset(g_rom+0x6A324,0x33,0xC40);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-800);
    v.cram[17]=0xE0;v.cram[19]=0xE00;
    const unsigned early_ids[]={0x3E,0x4B},early_gfx[]={0x249D,0x2400};
    for(unsigned n=0;n<2;++n) {
        unsigned o=0xD800+n*64;g_ram[o]=(uint8_t)early_ids[n];g_ram[o+1]=4;
        m68k_write32(o+4,0x1000);word(g_ram,o+2,early_gfx[n]);
        word(g_ram,o+8,600+n*20);word(g_ram,o+12,8);
        word(g_ram,0xAC02+n*2,o);
    }
    word(g_ram,0xAC00,4);memcpy(ram_before,g_ram,sizeof g_ram);before=v;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF00FF00u && out[58]==0xFF0000FFu);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    CHECK(!memcmp(before.cram,v.cram,sizeof v.cram));
    v.cram[17]=0xEEE;
    sonic1_video.scanline(&v,8,native,320,out,796);CHECK(out[38]==0xFF000000u);
    CHECK(v.cram[17]==0xEEE);
    /* Shared Eggmobile art, including exhaust, is complete before the PLC
     * reaches these patterns. This must not rewrite guest VRAM. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=0x8C;g_ram[0xD000]=1;word(g_ram,0xFE10,0x202);
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    word(g_rom,0x5CC62,1);g_rom[0x5CC64]=0x81;g_rom[0x5CC65]=0x71;
    g_rom[0x5CC66]=0;g_rom[0x5CC67]=255;memset(g_rom+0x5CC68,0,8);
    word(g_rom,0x17D0C,2);g_rom[0x17D0E]=1;memset(g_rom+0x17D0F,0,5);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-800);
    v.cram[1]=0xE00;v.cram[2]=0xE0;memset(v.vram+0x400*32,0x22,32);
    g_ram[0xD800]=0x73;g_ram[0xD801]=4;word(g_ram,0xD802,0x400);
    m68k_write32(0xD804,0x17D0C);word(g_ram,0xD808,600);word(g_ram,0xD80C,8);
    word(g_ram,0xAC00,2);word(g_ram,0xAC02,0xD800);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);before=v;
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF0000FFu && !memcmp(before.vram,v.vram,sizeof v.vram));
    /* Disk preflight is read-only. A cold apply recovers the displayed host
     * list and rejects incompatible aspect settings or malformed framing. */
    size_t disk_bytes=s1_video_state_size();uint8_t *disk=malloc(disk_bytes);
    CHECK(disk && s1_video_state_save(disk,disk_bytes));
    memcpy(ram_before,g_ram,sizeof g_ram);before=v;
    word(g_ram,0xD808,1200);s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    size_t preflight_bytes=s1_video_rb_save(NULL,0);
    uint8_t *preflight=malloc(preflight_bytes),*postflight=malloc(preflight_bytes);
    CHECK(preflight && postflight && s1_video_rb_save(preflight,preflight_bytes)==preflight_bytes);
    CHECK(s1_video_state_load(disk,disk_bytes,0));
    CHECK(s1_video_rb_save(postflight,preflight_bytes)==preflight_bytes);
    CHECK(!memcmp(preflight,postflight,preflight_bytes));
    CHECK(!s1_video_state_load(disk,disk_bytes-1,1));
    disk[0]^=1;CHECK(!s1_video_state_load(disk,disk_bytes,1));disk[0]^=1;
    CHECK(sonic1_video.configure("off"));CHECK(!s1_video_state_load(disk,disk_bytes,1));
    CHECK(sonic1_video.configure("32:9"));
    memcpy(g_ram,ram_before,sizeof g_ram);v=before;
    CHECK(s1_video_state_load(disk,disk_bytes,1));
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF0000FFu);
    free(preflight);free(postflight);free(disk);
    /* LZ3's native camera gate must not make its stationary ship pop into
     * an already visible margin. The preview shares complete immutable art
     * and never starts a guest boss, changes its gate, or survives takeover. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x102);
    for(unsigned i=0;i<40;++i)g_ram[0xA400+i]=1;
    word(g_ram,0xF700,0x1C40);word(g_ram,0xFF10,0x1C40);
    word(g_ram,0xF704,0x5B8);word(g_ram,0xFF14,0x5B8);
    const unsigned ship_frames[]={0,1,10};
    for(unsigned i=0;i<3;++i) {
        unsigned p=0x17D22+i*6;word(g_rom,0x17D0C+ship_frames[i]*2,p-0x17D0C);
        g_rom[p]=1;memset(g_rom+p+1,0,5);g_rom[p+5]=(uint8_t)(i*8);
    }
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-0x1C40);
    v.cram[1]=0xE00;memset(v.vram+0x400*32,0x22,32);
    memcpy(ram_before,g_ram,sizeof g_ram);before=v;M68KState ship_cpu=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[702]==0xFF0000FFu && out[710]==0xFF0000FFu && out[718]==0xFF0000FFu);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram) && !memcmp(&ship_cpu,&g_cpu,sizeof g_cpu));
    CHECK(!memcmp(before.vram,v.vram,sizeof v.vram));
    g_ram[0xD800]=0x77;g_ram[0xF742]=2;word(g_ram,0xF700,0x1CA0);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
    CHECK(strstr(reply,"\"sprites\":3")); /* allocated, awaiting its first tick */
    for(unsigned stop=0;stop<4;++stop) {
        g_ram[0xD800]=stop==0?0x77:0;g_ram[0xD824]=2;g_ram[0xF742]=stop==1?2:0;
        word(g_ram,0xF704,stop==2?0x600:0x5B8);
        word(g_ram,0xFE10,stop==3?0x101:0x102);
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
        CHECK(strstr(reply,"\"sprites\":0"));
    }
    /* SLZ3's ship and weapon are visible before the native music/PLC gate.
     * Keep their initial pose complete, then hand it to native objects. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x302);g_ram[0xF742]=2;
    for(unsigned i=0;i<40;++i)g_ram[0xA400+i]=1;
    word(g_ram,0xF700,0x1F80);word(g_ram,0xFF10,0x1F80);word(g_ram,0xF704,0x220);
    const unsigned slz_frames[]={0,1,8};
    for(unsigned i=0;i<3;++i) {
        unsigned p=0x17D40+i*6;word(g_rom,0x17D0C+slz_frames[i]*2,p-0x17D0C);
        g_rom[p]=1;memset(g_rom+p+1,0,5);g_rom[p+5]=(uint8_t)(i*8);
    }
    word(g_rom,0x17DD4+6,12);g_rom[0x17DE0]=1;memset(g_rom+0x17DE1,0,5);g_rom[0x17DE5]=24;
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-0x1F80);
    v.cram[1]=v.cram[17]=0xE00;memset(v.vram+0x400*32,0x11,32);memset(v.vram+0x46C*32,0x11,32);
    memcpy(ram_before,g_ram,sizeof g_ram);ship_cpu=g_cpu;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[758]==0xFF0000FFu && out[766]==0xFF0000FFu && out[774]==0xFF0000FFu && out[782]==0xFF0000FFu);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram) && !memcmp(&ship_cpu,&g_cpu,sizeof g_cpu));
    g_ram[0xD800]=0x7A;g_ram[0xF742]=4;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":4"));
    for(unsigned stop=0;stop<3;++stop) {
        g_ram[0xD800]=stop==0?0x7A:0;g_ram[0xD824]=2;g_ram[0xF742]=stop==2?6:4;
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);CHECK(strstr(reply,"\"sprites\":0"));
    }
    /* Marble's initial ship and tube obey the same visual-only handoff,
     * with their own authored pose and earlier native event gate. */
    memset(g_ram+0xD800,0,64);word(g_ram,0xFE10,0x202);g_ram[0xF742]=0;
    word(g_ram,0xF700,0x17D0);word(g_ram,0xFF10,0x17D0);word(g_ram,0xF704,0x224);
    word(v.vram,0xFC00,-0x17D0);word(g_rom,0x17DD4+8,12);
    for(unsigned phase=0;phase<4;++phase) {
        g_ram[0xD800]=phase==1 || phase==2?0x73:0;
        g_ram[0xD824]=phase==2?2:0;g_ram[0xF742]=phase?2:0;
        memcpy(ram_before,g_ram,sizeof g_ram);ship_cpu=g_cpu;
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);s1_video_command(1,NULL);
        CHECK(strstr(reply,phase<2?"\"sprites\":4":"\"sprites\":0"));
        CHECK(!memcmp(ram_before,g_ram,sizeof g_ram) && !memcmp(&ship_cpu,&g_cpu,sizeof g_cpu));
    }
    /* An early world sprite must not borrow a title card's centered origin
     * before its level scene is installed. The screen-space card stays. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=0x8C;word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    word(g_rom,0x1000,2);g_rom[0x1002]=1;memset(g_rom+0x1003,0,5);
    g_ram[0xD800]=0x12;g_ram[0xD801]=4;word(g_ram,0xD802,1);
    m68k_write32(0xD804,0x1000);word(g_ram,0xD808,600);word(g_ram,0xD80C,8);
    g_ram[0xD840]=0x34;word(g_ram,0xD842,1);m68k_write32(0xD844,0x1000);
    word(g_ram,0xD848,228);word(g_ram,0xD84A,136);word(g_ram,0xD870,228);
    word(g_ram,0xAC00,4);word(g_ram,0xAC02,0xD800);word(g_ram,0xAC04,0xD840);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    v.cram[1]=0xE00;memset(v.vram+32,0x11,32);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF000000u && out[338]==0xFF0000FFu);
    /* Surface pieces fill an actual 32:9 canvas while retaining their live
     * tile phase and without extending the actors' visibility footprints. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x100);
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    for(int n=0;n<6;++n)word(g_rom,0x1116C+n*2,12);
    g_rom[0x11178]=6;
    for(int n=0;n<6;++n) {
        unsigned p=0x11179+n*5;g_rom[p]=0;g_rom[p+1]=13;
        word(g_rom,p+2,0);g_rom[p+4]=(uint8_t)(-96+n*32);
    }
    for(int n=0;n<2;++n) {
        unsigned o=0xD800+n*64;g_ram[o]=0x1B;g_ram[o+1]=132;g_ram[o+0x1A]=3;
        m68k_write32(o+4,0x1116C);word(g_ram,o+2,0xC300);
        word(g_ram,o+8,896+n*192);word(g_ram,o+12,8);word(g_ram,0xAC02+n*2,o);
    }
    word(g_ram,0xAC00,4);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-800);
    v.cram[33]=0xE00;memset(v.vram+0x300*32,0x11,8*32);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    for(int n=0;n<796;++n)CHECK(out[n]==0xFF0000FFu);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    /* Incoming placements cannot appear over an outgoing results scene,
     * even after the ROM has selected its next zone/loading mode. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=0x8C;g_ram[0xD000]=1;word(g_ram,0xFE10,0x400);
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    word(g_rom,0x6B040,0x600);word(g_rom,0x6B600,600);word(g_rom,0x6B602,8);
    g_rom[0x6B604]=0x25;g_rom[0x6B605]=0;word(g_rom,0x6B606,65535);
    word(g_rom,0x9F92,2);g_rom[0x9F94]=1;memset(g_rom+0x9F95,0,5);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-800);
    v.cram[17]=0xE00;memset(v.vram+0x7B2*32,0x11,32);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF0000FFu);
    g_ram[0xD5C0]=0x3A;
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF000000u);
    /* A moved native platform is adopted by its saved placement origin.
     * Switching to wide loading must not allocate a second platform. */
    memset(g_ram,0,sizeof g_ram);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_rom,0x6B000,0x600);
    word(g_rom,0x6B600,600);word(g_rom,0x6B602,80);g_rom[0x6B604]=0x18;
    word(g_rom,0x6B606,65535);
    g_ram[0xD800]=0x18;word(g_ram,0xD808,664);word(g_ram,0xD80C,80);
    word(g_ram,0xD832,600);word(g_ram,0xD834,80);
    CHECK(s1_video_hook(0xD980));CHECK(g_ram[0xD840]==0);
    /* Each ordinary act exit uses the same presentation-only continuation.
     * Cover all seventeen results routes; SBZ3/FZ are separate sequences. */
    for(unsigned zone=0;zone<6;++zone)for(unsigned act=0;act<(zone==5?2u:3u);++act) {
        memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
        g_ram[0xF600]=12;g_ram[0xD000]=1;g_ram[0xD001]=132;
        word(g_ram,0xFE10,(zone<<8)|act);g_ram[0xD5C0]=0x3A;g_ram[0xF7CC]=1;
        for(int n=0;n<5;++n)g_ram[0xA400+n]=1;
        word(g_ram,0xF700,800);word(g_ram,0xFF10,800);word(g_ram,0xF72A,800);
        word(g_ram,0xD008,1160);word(g_ram,0xD00C,100);
        m68k_write32(0xD004,0x1000);word(g_ram,0xD002,1);
        word(g_ram,0xAC00,2);word(g_ram,0xAC02,0xD000);
        v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
        v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-800);
        v.cram[1]=0xE00;memset(v.vram+32,0x11,32);
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,100,native,320,out,796);
        CHECK(out[676]==0xFF0000FFu);
        word(g_ram,0xFE04,90);memcpy(ram_before,g_ram,sizeof g_ram);
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
        sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,100,native,320,out,796);
        CHECK(out[676]==0xFF000000u);
    }
    /* Retired capsule animals keep their ink when results reuse VRAM, and
     * a new native animal in the same slot must not erase a visible tail. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x302);g_ram[0xF7A7]=2;
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);word(g_ram,0xFE04,1);
    word(g_rom,0x1000,4);word(g_rom,0x1002,4);g_rom[0x1004]=1;
    memset(g_rom+0x1005,0,5);
    word(g_rom,0x3B926,1);g_rom[0x3B928]=0x82;g_rom[0x3B929]=0x71;
    g_rom[0x3B92A]=0;g_rom[0x3B92B]=255;memset(g_rom+0x3B92C,0,8);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-800);
    v.cram[2]=0xE0;v.cram[3]=0xE;memset(v.vram+0x580*32,0x22,32);
    g_ram[0xD800]=0x28;g_ram[0xD801]=4;g_ram[0xD818]=6;
    g_ram[0xD824]=4;g_ram[0xD830]=4;
    m68k_write32(0xD804,0x1000);word(g_ram,0xD802,0x580);
    word(g_ram,0xD808,1260);word(g_ram,0xD80C,8);
    word(g_ram,0xAC00+6*128,2);word(g_ram,0xAC02+6*128,0xD800);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[698]==0xFF00FF00u);
    uint8_t replacement_animal[64];memcpy(replacement_animal,g_ram+0xD800,64);
    memset(g_ram+0xD800,0,64);word(g_ram,0xAC00+6*128,0);word(g_ram,0xFE04,2);
    memset(v.vram+0x580*32,0x33,32);memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[698]==0xFF00FF00u);
    memcpy(g_ram+0xD800,replacement_animal,64);word(g_ram,0xD808,1200);
    word(g_ram,0xAC00+6*128,2);word(g_ram,0xFE04,3);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[638]==0xFF00FF00u && out[698]==0xFF00FF00u);
    size_t animals_state_size=s1_video_rb_save(NULL,0);
    void *animals_state=malloc(animals_state_size);CHECK(animals_state);
    CHECK(s1_video_rb_save(animals_state,animals_state_size)==animals_state_size);
    CHECK(s1_video_rb_load(animals_state,animals_state_size));free(animals_state);
    /* Capsule animal lifetime stays native. Its last margin pose continues
     * without promoting the guest visibility bit or changing guest RAM. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,2);g_ram[0xF7A7]=2;
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);word(g_ram,0xFE04,1);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-800);
    v.cram[1]=0xE00;memset(v.vram+32,0x11,32);
    g_ram[0xD800]=0x28;g_ram[0xD801]=4;g_ram[0xD818]=6;g_ram[0xD824]=4;
    m68k_write32(0xD804,0x1000);word(g_ram,0xD802,1);
    word(g_ram,0xD808,600);word(g_ram,0xD80C,8);word(g_ram,0xD810,-512);
    word(g_ram,0xAC00+6*128,2);word(g_ram,0xAC02+6*128,0xD800);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    memset(g_ram+0xD800,0,64);word(g_ram,0xAC00+6*128,0);word(g_ram,0xFE04,2);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[36]==0xFF0000FFu);
    /* A retired margin animal still moves at fractional presentation
     * phases, after its reusable native object slot has been deleted. */
    static uint32_t animal_fractional[796*224];
    sonic1_video.presentation_configure(1);
    for(unsigned tick=3;tick<=4;++tick) {
        word(g_ram,0xFE04,tick);s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        for(int y=0;y<224;++y) {
            sonic1_video.scanline(&v,y,native,320,out,796);
            sonic1_video.presentation_capture(&v,y,out,796,320,NULL);
        }
    }
    CHECK(sonic1_video.interpolate(.5,animal_fractional,796,224));
    CHECK(animal_fractional[8*796+32]==0xFF000000u);
    CHECK(animal_fractional[8*796+33]==0xFF0000FFu);
    sonic1_video.presentation_configure(0);
    /* Fixed magma fills only blank margin cells. Foreground stonework,
     * native coordinates and the guest layout remain untouched. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x200);
    g_ram[0xA408]=2;g_ram[0xA409]=1;g_ram[0xA40B]=2;
    word(g_ram,0xF700,0x970);word(g_ram,0xFF10,0x970);
    word(g_ram,0,1);word(g_ram,2,2);word(g_ram,528,3);
    for(int n=0;n<4;++n){word(g_ram,0xB008+n*2,0xE2D2);word(g_ram,0xB010+n*2,0xE2D3);word(g_ram,0xB018+n*2,0x8002);}
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-0x970);
    v.cram[49]=0xE00;v.cram[50]=0xE;v.cram[2]=0xE0;
    memset(v.vram+0x2D2*32,0x11,32);memset(v.vram+0x2D3*32,0x22,32);memset(v.vram+64,0x22,32);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[0]==0xFF00FF00u && out[30]==0xFF0000FFu && out[46]==0xFFFF0000u);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    /* Leaving the native curtain column must not truncate its visible tail. */
    word(g_ram,0xF700,0x9D0);word(g_ram,0xFF10,0x9D0);word(v.vram,0xFC00,-0x9D0);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[14]==0xFFFF0000u);
    /* Identical pool tiles in MZ3 do not create a new curtain in empty space. */
    word(g_ram,0xFE10,0x202);s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[14]==0xFF000000u);
    /* The moving wall extends only its flat rear fill. Its forward boundary
     * remains in place, and capture/publish do not change guest data. */
    memset(g_ram,0,sizeof g_ram);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x201);
    for(int n=0;n<8;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    for(int n=0;n<5;++n)word(g_rom,0x1000+n*2,10);
    g_rom[0x100A]=1;memset(g_rom+0x100B,0,5);
    g_ram[0xD800]=0x4E;g_ram[0xD801]=132;g_ram[0xD81A]=4;
    m68k_write32(0xD804,0x1000);word(g_ram,0xD802,0xE2D2);
    word(g_ram,0xD808,600);word(g_ram,0xD80C,8);
    word(g_ram,0xAC00+128,2);word(g_ram,0xAC02+128,0xD800);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[0]==0xFF0000FFu && out[38]==0xFF0000FFu && out[47]==0xFF000000u);
    word(g_ram,0xD808,1400);g_ram[0xD801]=4;memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[0]==0xFF000000u && out[38]==0xFF000000u);
    /* Selecting MZ for the next act must not reinterpret the outgoing GHZ
     * background as MZ rows while the old results card is still fading. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,2);g_ram[0xD5C0]=0x3A;
    for(int n=0;n<8;++n){g_ram[0xA400+7*128+n]=1;g_ram[0xA440+n]=2;}
    for(int n=0;n<256;++n)word(g_ram,0x200+n*2,1);
    for(int n=0;n<4;++n)word(g_ram,0xB008+n*2,1);
    word(g_ram,0xF700,800);word(g_ram,0xFF10,800);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;
    v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;word(v.vram,0xFC00,-800);
    v.cram[1]=0xE00;memset(v.vram+32,0x11,32);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF0000FFu);
    word(g_ram,0xFE10,0x200);word(g_ram,0xFF1C,512);v.vsram[1]=512;
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF0000FFu);
    size_t outgoing_size=s1_video_rb_save(NULL,0);void *outgoing=malloc(outgoing_size);
    CHECK(outgoing && s1_video_rb_save(outgoing,outgoing_size)==outgoing_size);
    g_ram[0xF76C]=0;s1_video_hook(0xD8DA);s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    CHECK(s1_video_rb_load(outgoing,outgoing_size));free(outgoing);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[38]==0xFF0000FFu);
    /* The lava tag and seesaw have a negative-distance branch before its range CMP.
     * Its original SUB must see a positive in/out result for either edge;
     * native output and recorded-demo execution keep the original operand. */
    const unsigned signed_cull_sites[]={0xF200,0x1165E};
    for(unsigned site=0;site<2;++site) {
        unsigned pc=signed_cull_sites[site];
        memset(g_ram,0,sizeof g_ram);s1_video_hook(0xD8DA);
        g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x200);
        for(int n=0;n<20;++n)g_ram[0xA400+n]=1;
        word(g_ram,0xF700,0x160);
        CHECK(sonic1_video.configure("16:9"));
        CHECK(sonic1_video.width(1600,900,320,224)==398);
        const unsigned lava_positions[]={0,128,896,1024};
        const unsigned lava_results[]={641,320,320,641};
        for(unsigned n=0;n<4;++n) {
            g_cpu.D[0]=0xC0DE0000u|lava_positions[n];g_cpu.D[1]=0x12340080u;
            g_cpu.SR=0x230F;cpu_before=g_cpu;memcpy(ram_before,g_ram,sizeof g_ram);
            CHECK(!s1_video_hook(pc));
            CHECK(((g_cpu.D[0]-g_cpu.D[1])&65535u)==lava_results[n]);
            CHECK((g_cpu.D[0]>>16)==0xC0DE && g_cpu.D[1]==cpu_before.D[1]);
            CHECK(g_cpu.SR==cpu_before.SR && !memcmp(ram_before,g_ram,sizeof g_ram));
        }
        /* A stage-start window extends left of zero. The native subtraction
         * wraps $FF80, but the host bounds comparison must remain signed. */
        word(g_ram,0xF700,0);CHECK(sonic1_video.configure("32:9"));
        CHECK(sonic1_video.width(3200,900,320,224)==796);
        g_cpu.D[0]=0x380;g_cpu.D[1]=0xFF80;
        CHECK(!s1_video_hook(pc));
        CHECK(((g_cpu.D[0]-g_cpu.D[1])&65535u)==320);
        g_cpu=cpu_before;word(g_ram,0xFFF0,1);cpu_before=g_cpu;
        CHECK(!s1_video_hook(pc) && !memcmp(&g_cpu,&cpu_before,sizeof g_cpu));
        word(g_ram,0xFFF0,0);CHECK(sonic1_video.configure("off"));
        CHECK(!s1_video_hook(pc) && !memcmp(&g_cpu,&cpu_before,sizeof g_cpu));
    }
    /* A wrapped screen-space pylon repeats from the native camera origin,
     * rather than the centered UI origin. Keep the original draw-only actor
     * and do not create another singleton when returning toward its layout. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x300);
    for(int n=0;n<6;++n)g_ram[0xA400+n]=1;
    g_ram[0xF76C]=0;s1_video_hook(0xD8DA);
    CHECK(sonic1_video.configure("32:9"));CHECK(sonic1_video.width(3200,900,320,224)==796);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    v.cram[1]=0xE00;memset(v.vram+32,0x11,32);
    word(g_rom,0x11096,2);g_rom[0x11098]=1;memset(g_rom+0x11099,0,5);
    word(g_rom,0x6B030,0x200);memset(g_rom+0x6B200,0,6);g_rom[0x6B204]=0x5C;word(g_rom,0x6B206,0xFFFF);
    CHECK(s1_video_hook(0xD980));CHECK(g_ram[0xD800]==0x5C);
    g_ram[0xD801]=0;word(g_ram,0xD802,0x8001);m68k_write32(0xFFD804,0x11096);
    word(g_ram,0xD80A,136);g_ram[0xD818]=1;
    word(g_ram,0xAC00+128,2);word(g_ram,0xAC02+128,0xD800);
    const int pylon_cameras[]={0,768,800},pylon_x[][2]={{146,658},{110,622},{46,558}};
    for(unsigned n=0;n<3;++n) {
        word(g_ram,0xF700,pylon_cameras[n]);word(g_ram,0xFF10,pylon_cameras[n]);
        word(g_ram,0xD808,-2*pylon_cameras[n]);word(v.vram,0xFC00,-pylon_cameras[n]);
        memcpy(ram_before,g_ram,sizeof g_ram);cpu_before=g_cpu;
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
        CHECK(out[pylon_x[n][0]]==0xFF0000FFu);
        if(pylon_x[n][1]>=0)CHECK(out[pylon_x[n][1]]==0xFF0000FFu);
        CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    }
    /* A clamped expanded view keeps its foreground fixed while the native
     * camera catches up. Cover both ends and fractional presentation. */
    const int clamp_cameras[2][5]={{0,64,128,176,176},{978,978,1000,1100,1216}};
    uint32_t pylon_locked[796];
    for(unsigned edge=0;edge<2;++edge) {
        sonic1_video.presentation_configure(1);
        for(unsigned tick=0;tick<5;++tick) {
            int camera=clamp_cameras[edge][tick];
            word(g_ram,0xF700,camera);word(g_ram,0xFF10,camera);word(g_ram,0xFE04,tick+10);
            /* Pyl_Display doubles the full 16.16 camera before taking its
             * integer part. A half-pixel carry must not shake a fixed view. */
            word(g_ram,0xF702,tick&1?0x9000:0x7000);
            word(g_ram,0xD808,-2*camera-(tick&1));word(v.vram,0xFC00,-camera);
            memcpy(ram_before,g_ram,sizeof g_ram);
            s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
            CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
            for(int y=0;y<224;++y) {
                sonic1_video.scanline(&v,y,native,320,out,796);
                if(y==8) {
                    if(!tick)memcpy(pylon_locked,out,sizeof pylon_locked);
                    else CHECK(!memcmp(pylon_locked,out,sizeof pylon_locked));
                }
                sonic1_video.presentation_capture(&v,y,out,796,320,NULL);
            }
            if(tick) {
                static uint32_t locked_frame[796*224];
                CHECK(sonic1_video.interpolate(.5,locked_frame,796,224));
                CHECK(!memcmp(pylon_locked,locked_frame+8*796,sizeof pylon_locked));
            }
        }
        sonic1_video.presentation_configure(0);
    }
    word(g_ram,0xF702,0);
    /* Interpolate through the hardware's X wrap without taking a 512px
     * excursion. Sixteen pixels of camera movement moves each column 32px. */
    sonic1_video.presentation_configure(1);
    for(unsigned tick=0;tick<2;++tick) {
        int camera=tick?264:248;
        word(g_ram,0xF700,camera);word(g_ram,0xFF10,camera);word(g_ram,0xFE04,tick+1);
        word(g_ram,0xD808,-2*camera);word(v.vram,0xFC00,-camera);
        s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
        for(int y=0;y<224;++y) {
            sonic1_video.scanline(&v,y,native,320,out,796);
            sonic1_video.presentation_capture(&v,y,out,796,320,NULL);
        }
    }
    static uint32_t pylon_fractional[796*224];
    CHECK(sonic1_video.interpolate(.5,pylon_fractional,796,224));
    CHECK(pylon_fractional[8*796+110]==0xFF0000FFu);
    CHECK(pylon_fractional[8*796+622]==0xFF0000FFu);
    sonic1_video.presentation_configure(0);
    /* Adaptive Fit can also select native width while keeping this adapter. */
    CHECK(sonic1_video.configure("fit"));CHECK(sonic1_video.width(640,448,320,224)==320);
    word(g_ram,0xF700,816);word(g_ram,0xFF10,816);word(g_ram,0xD808,-2*816);word(v.vram,0xFC00,-816);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,320);sonic1_video.scanline(&v,8,native,320,out,320);
    CHECK(out[288]==0xFF0000FFu);
    word(g_ram,0xF700,1024);CHECK(s1_video_hook(0xD980));
    word(g_ram,0xF700,128);word(g_ram,0xD808,500);CHECK(s1_video_hook(0xD980));
    unsigned pylons=0;for(unsigned o=0xD800;o<0xF000;o+=64)pylons+=g_ram[o]==0x5C;
    CHECK(pylons==1);
    /* A lowered staircase must use native unload/reset bounds, even when
     * the wider activation buffer still contains its placement. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    CHECK(sonic1_video.configure("32:9"));CHECK(sonic1_video.width(3200,900,320,224)==796);
    g_ram[0xF600]=12;g_ram[0xD000]=1;word(g_ram,0xFE10,0x300);
    for(int n=0;n<20;++n)g_ram[0xA400+n]=1;
    word(g_ram,0xF700,0xE00);word(g_rom,0x6B030,0x600);
    word(g_rom,0x6B600,0xC90);word(g_rom,0x6B602,8);g_rom[0x6B604]=0x5B;
    g_rom[0x6B605]=0;word(g_rom,0x6B606,65535);
    g_cpu.D[0]=0xC0DEFF00;g_cpu.SR=0x230F;cpu_before=g_cpu;
    CHECK(!s1_video_hook(0x10EB4));CHECK(!memcmp(&cpu_before,&g_cpu,sizeof g_cpu));
    CHECK(s1_video_hook(0xD980));CHECK(g_ram[0xD800]==0);
    /* Its last visible margin cube is presentation only until reactivation. */
    word(g_ram,0xF700,0xDD0);word(g_ram,0xFF10,0xDD0);
    word(g_rom,0x1103C,2);g_rom[0x1103E]=1;memset(g_rom+0x1103F,0,5);word(g_rom,0x11041,1);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    word(v.vram,0xFC00,-0xDD0);v.cram[33]=0xE00;memset(v.vram+32,0x11,32);
    memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,8,native,320,out,796);
    CHECK(out[14]==0xFF0000FFu);CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    word(g_ram,0xF700,0xBF0);CHECK(s1_video_hook(0xD980));CHECK(g_ram[0xD800]==0x5B);
    /* Stage planes can arrive while the card SAT still owns the display.
     * Black stage palettes must suppress palette0 scenery too, even before
     * the fade target is loaded; the card and HUD keep their own palette. */
    memset(g_ram,0,sizeof g_ram);memset(&v,0,sizeof v);s1_video_hook(0xD8DA);
    g_ram[0xF600]=0x8C;word(g_ram,0xFE10,0x400);word(g_ram,0xF626,0x003F);
    g_ram[0xD080]=0x34;word(g_ram,0xD082,2);word(g_ram,0xD088,288);word(g_ram,0xD08A,144);word(g_ram,0xD0B0,288);
    m68k_write32(0xD084,0x1200);word(g_rom,0x1200,2);g_rom[0x1202]=1;memset(g_rom+0x1203,0,5);
    g_ram[0xD040]=0x21;word(g_ram,0xD042,1);word(g_ram,0xD048,144);word(g_ram,0xD04A,144);m68k_write32(0xD044,0x1200);
    word(g_ram,0xAC00,4);word(g_ram,0xAC02,0xD080);word(g_ram,0xAC04,0xD040);
    v.reg[1]=64;v.reg[2]=0x30;v.reg[4]=7;v.reg[5]=0x50;v.reg[12]=1;v.reg[13]=0x3F;v.reg[16]=1;
    v.cram[1]=0xE00;v.cram[2]=0xE;memset(v.vram+32,0x11,32);memset(v.vram+64,0x22,32);
    word(v.vram,0xC000+24*128+24*2,1);memcpy(ram_before,g_ram,sizeof g_ram);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);sonic1_video.scanline(&v,192,native,320,out,796);
    CHECK(out[430]==0xFF000000u);
    sonic1_video.scanline(&v,16,native,320,out,796);
    CHECK(out[398]==0xFFFF0000u && out[16]==0xFF0000FFu);
    CHECK(!memcmp(ram_before,g_ram,sizeof g_ram));
    return 0;
}
