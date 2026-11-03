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
    g_ram[0xF600] = 4;
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
    /* Title perspective water wraps the original 512px nametable, not the
     * entire level layout. Negative samples in the left margin must not
     * come from a different chunk at the far end of that level. */
    g_ram[0xA441]=1;g_ram[0xA442]=2;
    for(int n=0;n<256;++n)word(g_ram,512+n*2,1);
    for(int n=0;n<4;++n)word(g_ram,0xB008+n*2,2);
    memset(v.vram+64,0x22,32);v.cram[2]=0xE00;word(v.vram,0xFC02,94);
    sonic1_video.scanline(&v,0,native,320,out,796);
    sonic1_video.scanline(&v,200,native,320,out,796);
    CHECK(out[200]==0xFFFF0000u);
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
    }
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
        unsigned mode=g_ram[0xF600];g_ram[0xF600]=4;
        sonic1_video.scanline(&v,0,native,320,expected,1593);
        sonic1_video.scanline_lazy(&v,0,320,out,1593,fetch_native,native);
        CHECK(native_fetches==2);CHECK(!memcmp(expected,out,sizeof expected));
        g_ram[0xF600]=(uint8_t)mode;
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
    g_ram[0xF600]=0;word(g_ram,0xFFF0,1);
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(!memcmp(level_line,out,sizeof level_line));
    s1_video_command(1,NULL);CHECK(strstr(reply,"\"scene\":1"));
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
    s1_video_hook(0xD65E);s1_video_hook(0xD746);
    upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[279]==0xFF0000FFu); /* 238 + 160 - 48*796/320 */
    word(g_ram,0xD088,288);s1_video_hook(0xD65E);s1_video_hook(0xD746);
    upload_sat(&v);
    sonic1_video.scanline(&v,0,native,320,out,796);
    CHECK(out[398]==0xFF0000FFu); /* centered target, independent of world origin */
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
    /* A scripted FZ boss is created at the original camera threshold, after
     * the wide viewport can see its cylinders. Ready art can be previewed
     * without allocating the boss early or changing any guest state. */
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
    word(g_rom,0x1A462,600);
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
    return 0;
}
