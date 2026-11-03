/* Experimental Sonic 1 REV00 world renderer.
 *
 * Terrain addressing follows s1disasm GetBlockData ($6BD6):
 * eight interlaced FG/BG layout rows -> 256px chunks -> 16px blocks ->
 * live 8px VRAM patterns. Host scene hooks expand object visibility,
 * ring handling and sprite composition beyond the original hardware limits.
 * Widescreen Off retains the native renderer and original instruction path.
 */
#include "sonic1_video.h"
#include "genesis_runtime.h"
#include "video/genesis_vdp.h"
#include "video/genesis_dac.h"
#include "video/genesis_tile_row.h"
#include "cmd_server.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VIDEO_OFF, VIDEO_FIT, VIDEO_RATIO, VIDEO_STAGE };
static int s_mode;
static double s_ratio = 16.0 / 9.0;
static int s_width, s_left, s_camera, s_stage_width, s_native_x, s_requested_width = 320;
static unsigned s_frames, s_fallbacks, s_terrain_checks, s_terrain_errors;
static unsigned s_bg_checks, s_bg_errors, s_bg_unstreamed;
static int s_title_world;
static int s_interpolation_enabled,s_interpolating;
static double s_phase;
static const uint8_t *s_read_ram;
#define video_ram (s_interpolating?s_read_ram:g_ram)
static void interpolation_reset(void);
static int interpolate_scroll(int line,int which,int current);
static int interpolate_vs(int line,int which,int current);
static int interpolate_background_correction(int line,int margin,int current);
static void scene_mode_changed(int was_enabled);

static uint16_t ram16(unsigned a)
{
    a &= 0xFFFFu;
    return (uint16_t)((video_ram[a] << 8) | video_ram[(a + 1) & 0xFFFFu]);
}
static uint16_t vram16(const GVDP *v, unsigned a)
{
    return (uint16_t)((v->vram[a & 0xFFFFu] << 8) | v->vram[(a + 1) & 0xFFFFu]);
}
static int results_world(void)
{
    int mode=video_ram[0xF600]&0x7F;
    /* RingFlash deletes Sonic before the act-clear card begins. Keep the
     * outgoing level through that gap and the later Special Stage fade;
     * the incoming special-stage player/list takes ownership when installed. */
    return !video_ram[0xD000] &&
        ((mode==12 && video_ram[0xF7CD]) ||
         ((mode==12 || mode==16 || mode==0) && video_ram[0xD5C0]==0x3A));
}
static int gameplay(void)
{
    int mode = video_ram[0xF600] & 0x7F;
    /* Game mode changes BEFORE a fade finishes. Scene identity follows the
     * still-live player/layout until the incoming scene replaces them. */
    int player=video_ram[0xD000];
    return ((mode == 8 || mode == 12 || mode == 16 || mode == 24 || mode == 28 ||
             (mode == 0 && ram16(0xFFF0) == 1)) && player == 1) ||
           ((mode == 24 || mode == 28) && player == 0x87) || results_world();
}
static int recorded_demo(void)
{
    int mode=video_ram[0xF600]&0x7F;
    return ram16(0xFFF0)!=0 || mode==24 || (mode==28 && video_ram[0xD000]==0x87);
}
/* Scripted activation/timing stays native through title-card loading and
 * departure as well as recorded demos. Avoid aspect-dependent guest work
 * shifting ROM animation steps across VBlank. */
static int native_activation(void)
{
    if(recorded_demo())return 1;
    if(!gameplay())return 0;
    if(results_world())return 1; /* no player: presentation only, no host rings/activation */
    if(video_ram[0xF600]&128)return 1;
    for(unsigned o=0xD080;o<0xD200;o+=64)if(video_ram[o]==0x34)return 1;
    return 0;
}
static int stage_width(void)
{
    int last = 1;
    for (int row = 0; row < 8; ++row)
        for (int col = 0; col < 64; ++col)
            if (video_ram[0xA400 + row * 128 + col] && col + 1 > last) last = col + 1;
    return last * 256;
}
static int configure(const char *mode)
{
    if (!mode) return 0;
    int was_enabled = s_mode != VIDEO_OFF;
    if (!strcmp(mode, "off")) s_mode = VIDEO_OFF;
    else if (!strcmp(mode, "fit") || !strcmp(mode, "adaptive")) s_mode = VIDEO_FIT;
    else if (!strcmp(mode, "stage")) s_mode = VIDEO_STAGE;
    else {
        char *end;
        double w = strtod(mode, &end);
        if (end == mode || *end != ':') return 0;
        const char *den = end + 1;
        double h = strtod(den, &end);
        if (end == den || *end || !(w > 0) || !(h > 0) ||
            !(w / h > 0) || w / h > (double)INT_MAX / 480.0) return 0;
        s_ratio = w / h; s_mode = VIDEO_RATIO;
    }
    scene_mode_changed(was_enabled);
    return 1;
}
static int enabled(void) { return s_mode != VIDEO_OFF; }
static int width(int dw, int dh, int nw, int nh)
{
    if (!enabled()) return 0;
    double result = nw;
    if (s_mode == VIDEO_STAGE) {
        if (gameplay()) s_stage_width = stage_width();
        result = s_stage_width > nw ? s_stage_width : nw;
    }
    else if (s_mode == VIDEO_RATIO) result = nh * s_ratio;
    else if (dw > 0 && dh > 0) result = (double)nh * dw / dh;
    /* Bound by representable dimensions and the SDL texture limit (runner),
     * never by an aspect preset. Even a stage-length viewport is supported. */
    if (result > INT_MAX - 1.0) result = INT_MAX - 1.0;
    int pixels = (int)(result + 0.5);
    s_requested_width = pixels < nw ? nw : pixels;
    return s_requested_width;
}

/* Full 16-bit scroll is unwrapped against the live camera's 10-bit VDP
 * position. This avoids the one-frame camera/streaming seam seen in SMB. */
static int unwrap(int value, int reference)
{
    return reference + ((value - reference + 512) & 1023) - 512;
}
static uint16_t world_attr(int wx, int wy, int background)
{
    if (wx < 0 || wx >= 16384) return 0;
    unsigned layout = (background ? 0xA440u : 0xA400u) + ((unsigned)wy / 256u & 7u) * 128u + (unsigned)wx / 256u;
    unsigned chunk = video_ram[layout];
    uint16_t block = 0;
    if (chunk) {
        unsigned address = ((chunk - 1u) & 127u) * 512u +
                           ((unsigned)wy & 240u) * 2u + ((unsigned)wx & 240u) / 8u;
        block = ram16(address);
    }
    int tx = (wx >> 3) & 1, ty = (wy >> 3) & 1;
    if (block & 0x0800) tx ^= 1;
    if (block & 0x1000) ty ^= 1;
    return ram16(0xB000u + (block & 1023u) * 8u + (unsigned)(ty * 2 + tx) * 2u) ^ (block & 0x1800u);
}
static uint8_t pattern_pixel(const GVDP *v, uint16_t attr, int x, int y)
{
    int fx = x & 7, fy = y & 7;
    if (attr & 0x0800) fx ^= 7;
    if (attr & 0x1000) fy ^= 7;
    unsigned a = (attr & 2047u) * 32u + (unsigned)fy * 4u + (unsigned)fx / 2u;
    int byte = v->vram[a & 0xFFFFu];
    int nibble = fx & 1 ? byte & 15 : byte >> 4;
    return nibble ? (uint8_t)(((attr >> 9) & 48) | nibble) : 0;
}
/* Wide sprites can enter view before their native PLC replaces title-card
 * patterns. Decode the small immutable button/signpost assets separately;
 * guest VRAM, PLC order and simulation clocks remain untouched. Format and
 * XOR rows follow REV00 NemDec ($1440), not a second machine execution. */
typedef struct { uint8_t pixels[4096];unsigned ready,size,base;uint32_t hash,display_hash; } SpriteArt;
static SpriteArt s_sprite_art[3];
static unsigned art_bits(unsigned bit,unsigned count)
{
    unsigned value=0;
    for(unsigned i=0;i<count;++i,++bit)value=(value<<1)|((g_rom[bit>>3]>>(7-(bit&7)))&1);
    return value;
}
static int decode_sprite_art(SpriteArt *art,unsigned address)
{
    unsigned header=(g_rom[address]<<8)|g_rom[address+1],size=(header&32767u)*32;
    unsigned p=address+2,palette=0;uint16_t table[256]={0};
    if(!size || size>sizeof art->pixels)return 0;
    while(p<address+4096 && p<0x80000u) {
        unsigned code=g_rom[p++];if(code==255)break;
        if(code&128){palette=code&15;continue;}
        unsigned length=code&15;if(!length || length>8 || p>=0x80000u)return 0;
        unsigned prefix=g_rom[p++],copies=1u<<(8-length);
        if(prefix>=(1u<<length))return 0;
        for(unsigned n=0;n<copies;++n)table[prefix*copies+n]=(uint16_t)((length<<8)|(code&112)|palette);
    }
    if(p>=0x80000u || g_rom[p-1]!=255)return 0;
    memset(art->pixels,0,size);unsigned bit=p*8,made=0;
    while(made<size*2) {
        if(bit+13>0x80000u*8)return 0;
        unsigned look=art_bits(bit,8),value,length;
        if(look>=252){bit+=6;value=art_bits(bit,7);length=7;}
        else {unsigned entry=table[look];length=entry>>8;if(!length)return 0;value=entry&255;}
        bit+=length;
        unsigned repeats=((value>>4)&7)+1,pixel=value&15;
        for(unsigned n=0;n<repeats && made<size*2;++n,++made)
            art->pixels[made>>1]|=(uint8_t)(pixel<<((made&1)?0:4));
    }
    if(header&32768u)for(unsigned n=4;n<size;++n)art->pixels[n]^=art->pixels[n-4];
    art->size=size;art->hash=2166136261u;
    for(unsigned n=0;n<size;++n)art->hash=(art->hash^art->pixels[n])*16777619u;
    art->display_hash=2166136261u;
    for(unsigned n=art->base==0x50F?128:0;n<size;++n)art->display_hash=(art->display_hash^art->pixels[n])*16777619u;
    return 1;
}
static uint8_t sprite_pixel(const GVDP *v,unsigned asset,uint16_t attr,int x,int y)
{
    if(!asset || !enabled())return pattern_pixel(v,attr,x,y);
    static const unsigned addresses[]={0x3A53E,0x3401A,0x32408};
    SpriteArt *art=&s_sprite_art[asset-1];
    if(!art->ready){art->base=asset==1?0x680:asset==3?0x513:0x50F;art->ready=decode_sprite_art(art,addresses[asset-1])?1:2;}
    int tile=(int)(attr&2047u)-(int)art->base;
    if(art->ready!=1 || tile<0 || (unsigned)(tile+1)*32>art->size)return pattern_pixel(v,attr,x,y);
    int fx=x&7,fy=y&7;if(attr&0x800)fx^=7;if(attr&0x1000)fy^=7;
    unsigned byte=art->pixels[tile*32+fy*4+fx/2],pixel=fx&1?byte&15:byte>>4;
    return pixel?(uint8_t)(((attr>>9)&48)|pixel):0;
}
static int plane_tiles(int bits) { return bits == 1 ? 64 : bits == 3 ? 128 : 32; }
static uint16_t plane_attr(const GVDP *v, unsigned base, int x, int y)
{
    int wt = plane_tiles(v->reg[16] & 3), ht = plane_tiles((v->reg[16] >> 4) & 3);
    int col = (x >> 3) & (wt - 1), row = (y >> 3) & (ht - 1);
    return vram16(v, base + (unsigned)(row * wt + col) * 2u);
}

/* Enhanced scene data lives on the host: no SAT coordinate wrap, 80-piece
 * ceiling, or 20-sprites-per-line ceiling. Native BuildSprites still executes
 * for the hardware pass and publishes a signature used to align our list
 * with the exact VBlank upload displayed by the VDP. */
typedef struct { int x, y; uint16_t attr; uint8_t size, hud,slot,space,asset; } SceneSprite;
enum { SCENE_SPRITES = 32768, SCENE_PLACEMENTS = 1024, SCENE_SLOTS=129 };
typedef struct {
    SceneSprite sprites[SCENE_SPRITES];
    unsigned count, serial;
    uint8_t sat[640];
    int scene; /* 1 level, 2 special, 3 title card, 0 menu */
    int16_t ox[SCENE_SLOTS],oy[SCENE_SLOTS];
    uint32_t identity[SCENE_SLOTS];
} SceneFrame;
static SceneFrame s_build, s_history[3], s_display_frame;
static const SceneFrame *s_display;
static SceneFrame s_native_build,s_native_history[3];
static unsigned s_native_serial;
static SceneFrame *s_capture_frame=&s_build;
static unsigned s_capture_slot,s_capture_space;
static int s_capture_readonly;
static void native_publish(void);
static void interpolation_sprite_delta(const SceneSprite *sprite,int line,int *dx,int *dy);
static unsigned s_serial, s_scene_tick, s_spawned, s_pool_pressure, s_ring_collected;
static unsigned s_visible_objects[128], s_visible_count;
typedef struct { unsigned address; uint16_t x, y; uint8_t id, subtype, state, loaded; } Placement;
static Placement s_placements[SCENE_PLACEMENTS];
static unsigned s_placement_count, s_placement_base;
static int s_loader_active, s_rings_adopted;
typedef struct { int x, y; unsigned tick; } RingSpark;
static RingSpark s_sparks[64];
static unsigned s_spark_next;

/* Object-loader decisions and captured mappings belong to the saved timeline.
 * Interpolation images, selected row pointers and immutable ROM art do not. */
typedef struct {
    uint32_t magic, serial, scene_tick, spawned, pool_pressure, ring_collected;
    unsigned placement_count, placement_base, loader_active, rings_adopted;
    unsigned spark_next, visible_count;
    Placement placements[SCENE_PLACEMENTS];
    RingSpark sparks[64];
    unsigned visible_objects[128];
} VideoState;
enum { SCENE_HEADER = sizeof(SceneFrame)-offsetof(SceneFrame,count) };

size_t s1_video_rb_save(void *dst,size_t cap)
{
    const SceneFrame *frames[]={&s_build,&s_history[0],&s_history[1],&s_history[2]};
    size_t need=sizeof(VideoState);
    for(unsigned n=0;n<4;++n)need+=SCENE_HEADER+frames[n]->count*sizeof(SceneSprite);
    if(!dst)return need;
    if(cap<need)return 0;
    VideoState state;memset(&state,0,sizeof state);
    state.magic=0x53315631u;state.serial=s_serial;state.scene_tick=s_scene_tick;
    state.spawned=s_spawned;state.pool_pressure=s_pool_pressure;state.ring_collected=s_ring_collected;
    state.placement_count=s_placement_count;state.placement_base=s_placement_base;
    state.loader_active=s_loader_active;state.rings_adopted=s_rings_adopted;
    state.spark_next=s_spark_next;state.visible_count=s_visible_count;
    memcpy(state.placements,s_placements,sizeof s_placements);
    memcpy(state.sparks,s_sparks,sizeof s_sparks);
    memcpy(state.visible_objects,s_visible_objects,sizeof s_visible_objects);
    uint8_t *out=dst;memcpy(out,&state,sizeof state);out+=sizeof state;
    for(unsigned n=0;n<4;++n) {
        memcpy(out,&frames[n]->count,SCENE_HEADER);out+=SCENE_HEADER;
        size_t bytes=frames[n]->count*sizeof(SceneSprite);
        memcpy(out,frames[n]->sprites,bytes);out+=bytes;
    }
    return need;
}
int s1_video_rb_load(const void *src,size_t len)
{
    VideoState state;
    if(!src || len<sizeof state)return 0;
    memcpy(&state,src,sizeof state);
    if(state.magic!=0x53315631u || state.placement_count>SCENE_PLACEMENTS ||
       state.visible_count>128 || state.loader_active>1 || state.rings_adopted>1)return 0;
    for(unsigned n=0;n<state.visible_count;++n)
        if(state.visible_objects[n]<0xD000 || state.visible_objects[n]>=0xF000 ||
           (state.visible_objects[n]&63))return 0;
    const uint8_t *in=(const uint8_t*)src+sizeof state;
    size_t left=len-sizeof state;
    const uint8_t *headers[4],*sprites[4];unsigned counts[4];
    for(unsigned n=0;n<4;++n) {
        if(left<SCENE_HEADER)return 0;
        headers[n]=in;memcpy(&counts[n],in,sizeof counts[n]);
        if(counts[n]>SCENE_SPRITES)return 0;
        int scene;memcpy(&scene,in+offsetof(SceneFrame,scene)-offsetof(SceneFrame,count),sizeof scene);
        if(scene<0 || scene>3)return 0;
        in+=SCENE_HEADER;left-=SCENE_HEADER;
        size_t bytes=counts[n]*sizeof(SceneSprite);
        if(left<bytes)return 0;
        sprites[n]=in;in+=bytes;left-=bytes;
        for(unsigned k=0;k<counts[n];++k) {
            SceneSprite sprite;memcpy(&sprite,sprites[n]+k*sizeof sprite,sizeof sprite);
            if(sprite.asset>3 || sprite.size>15 || sprite.hud>2 || sprite.slot>=SCENE_SLOTS)return 0;
        }
    }
    if(left)return 0;
    s_serial=state.serial;s_scene_tick=state.scene_tick;s_spawned=state.spawned;
    s_pool_pressure=state.pool_pressure;s_ring_collected=state.ring_collected;
    s_placement_count=state.placement_count;s_placement_base=state.placement_base;
    s_loader_active=state.loader_active;s_rings_adopted=state.rings_adopted;
    s_spark_next=state.spark_next;s_visible_count=state.visible_count;
    memcpy(s_placements,state.placements,sizeof s_placements);
    memcpy(s_sparks,state.sparks,sizeof s_sparks);
    memcpy(s_visible_objects,state.visible_objects,sizeof s_visible_objects);
    SceneFrame *frames[]={&s_build,&s_history[0],&s_history[1],&s_history[2]};
    for(unsigned n=0;n<4;++n) {
        memcpy(&frames[n]->count,headers[n],SCENE_HEADER);
        memcpy(frames[n]->sprites,sprites[n],counts[n]*sizeof(SceneSprite));
    }
    s_display=NULL;s_title_world=0;interpolation_reset();
    return 1;
}

static uint8_t scene_read8(unsigned a)
{
    a &= 0xFFFFFFu;
    return a < 0x80000u ? g_rom[a] : a >= 0xFF0000u ? video_ram[a & 65535u] : 0;
}
static uint16_t scene_read16(unsigned a) { return (uint16_t)((scene_read8(a)<<8)|scene_read8(a+1)); }
static uint32_t scene_read32(unsigned a) { return ((uint32_t)scene_read16(a)<<16)|scene_read16(a+2); }
static void write8(unsigned a, unsigned v) { glue_poke8(0xFF0000u|(a&65535u),(uint8_t)v); }
static void write16(unsigned a, unsigned v) { glue_poke16(0xFF0000u|(a&65535u),(uint16_t)v); }
static void write32(unsigned a, unsigned v) { glue_poke32(0xFF0000u|(a&65535u),v); }
static int view_left(int camera, int w)
{
    int end = stage_width()-w, left = camera-(w-320)/2;
    if (end<0) return end/2;
    if (left<0) left=0;
    return left>end ? end : left;
}
static void activation_bounds(int camera,int w,int *lo,int *hi)
{
    int left=view_left(camera,w);
    /* out_of_range tests 128px-rounded positions. OPL must only instantiate
     * cells that this same culler retains. Loading at left+w+320 with an
     * unrounded camera let objects spawn one cell early, get deleted, and
     * remain marked loaded until they left the window (missing platforms). */
    *lo=(left-128)&~127;
    *hi=((left+w+192)&~127)+128;
}
static void scene_mode_changed(int was_enabled)
{
    if (was_enabled == enabled()) return;
    s_rings_adopted=0;
    for (unsigned i=0;i<s_placement_count;++i)
        if (s_placements[i].id==0x25) s_placements[i].loaded=0;
}
static void add_mapping(unsigned map, unsigned frame, unsigned gfx, unsigned flags,
                        int x, int y, int hud, int static_mapping)
{
    unsigned p=map;
    int count=1;
    if (!static_mapping) {
        p+=(int16_t)scene_read16(p+(frame&127u)*2u);
        count=scene_read8(p++);
    }
    for (int n=0;n<count && s_capture_frame->count<SCENE_SPRITES;++n,p+=5) {
        int dy=(int8_t)scene_read8(p), size=scene_read8(p+1)&15;
        int dx=(int8_t)scene_read8(p+4);
        uint16_t attr=(uint16_t)(scene_read16(p+2)+gfx);
        if (flags&1) { dx=-dx-(((size>>2)&3)+1)*8; attr^=0x800; }
        if (flags&2) { dy=-dy-((size&3)+1)*8; attr^=0x1000; }
        if (y+dy>=224 || y+dy+((size&3)+1)*8<=0) continue;
        SceneSprite *s=&s_capture_frame->sprites[s_capture_frame->count++];
        s->x=x+dx; s->y=y+dy; s->attr=attr; s->size=(uint8_t)size; s->hud=(uint8_t)hud;s->slot=(uint8_t)s_capture_slot;s->space=(uint8_t)s_capture_space;
        unsigned owner=s_capture_slot?video_ram[0xD000+(s_capture_slot-1)*64]:0;
        s->asset=(uint8_t)(owner==0x0D?1:owner==0x32?(video_ram[0xFE10]==2?3:2):0);
    }
}
static void load_placements(void)
{
    unsigned index=((ram16(0xFE10)&0xFF00u)>>4)|((ram16(0xFE10)&3u)<<2);
    unsigned base=0x6B000u+(int16_t)scene_read16(0x6B000u+index);
    if (base==s_placement_base && s_placement_count) return;
    s_placement_count=0; s_placement_base=base;
    unsigned state=1;
    for (unsigned p=base;p+6<=0x80000u && s_placement_count<SCENE_PLACEMENTS;p+=6) {
        unsigned x=scene_read16(p), id=scene_read8(p+4);
        if (x==65535u) break;
        Placement *q=&s_placements[s_placement_count++];
        q->address=p; q->x=(uint16_t)x; q->y=scene_read16(p+2);
        q->id=(uint8_t)(id&127); q->subtype=scene_read8(p+5);
        q->state=(uint8_t)((id&128) ? state++ : 0); q->loaded=0;
    }
}
static void adopt_rings(void)
{
    if (s_rings_adopted || !enabled()) return;
    /* Existing normal rings move to the host list when enabling mid-level.
     * Collected bits stay in the ROM's respawn table (and in save states).
     * Lost rings (object $37) continue using their original physics. */
    for (unsigned o=0xD800;o<0xF000;o+=64)
        if (video_ram[o]==0x25)
            for (int k=0;k<64;k+=4) write32(o+(unsigned)k,0);
    s_rings_adopted=1;
}
static unsigned placement_object(const Placement *p)
{
    for (unsigned o=0xD800;o<0xF000;o+=64) {
        if (!video_ram[o]) continue;
        if (p->state && video_ram[o+0x23]==p->state) return o;
        if (!p->state && video_ram[o]==p->id && ram16(o+8)==p->x) return o;
    }
    return 0;
}
static int placement_alive(const Placement *p) { return placement_object(p)!=0; }
/* Recorded demos keep native object activation. Reconstruct the harmless
 * MZ platforms/pillars from their placements when the native loader has not
 * instantiated them, or has already deleted them in a wide margin. This is
 * pure presentation: no guest objects, collision state or cached timeline.
 * Their motion follows LGrass_Types/Glass_Types in the original REV00 code. */
static void marble_present(unsigned priority,int camera,int left)
{
    if(!native_activation() || !gameplay() || video_ram[0xFE10]!=2 ||
       (priority!=3 && priority!=4 && priority!=5))return;
    load_placements();s_capture_slot=0;s_capture_space=4;
    for(unsigned i=0;i<s_placement_count;++i) {
        const Placement *p=&s_placements[i];
        if(p->x+64<left || (int)p->x-64>=left+s_requested_width)continue;
        if(p->id!=0x2F && p->id!=0x30)continue;
        if(placement_alive(p))continue;
        unsigned type=p->subtype&7,frame=0,map,gfx;
        int y=p->y&4095,offset=0;
        if(p->id==0x2F) {
            if(priority!=5 || (p->subtype>>4)>2 || type>5)continue;
            frame=p->subtype>>4;map=0xB2D4;gfx=0xC000;
            if(type>=1 && type<=4) {
                static const unsigned oscillators[]={2,6,10,14};
                static const int distances[]={32,48,64,96};
                offset=video_ram[0xFE5E+oscillators[type-1]];
                if(p->subtype&8)offset=distances[type-1]-offset;
            }
            /* Type 5 settles in <=32 ticks after Sonic leaves. By native
             * deletion he is >400px away, so its unoccupied pose is rest. */
        } else {
            if(priority==5 || type>2)continue;
            map=0xB5FA;gfx=0xC38E;frame=priority==3?1:0;
            if(type) {
                offset=video_ram[0xFE70];
                if(type==2)offset=64-offset;
                if(priority==3)offset=((64-offset)&255)/2+32;
            }
        }
        add_mapping(map,frame,gfx,0,
                    (int)p->x-camera,y-offset-ram16(0xF704),0,0);
    }
}
/* SYZ recorded routes expose lamps and Yadrins outside native activation.
 * Yadrin initialization also omits DisplaySprite until the next object tick,
 * after the initial fade. Present their initial/initialized poses without
 * advancing the enemy or changing the recorded route. */
static void springyard_present(unsigned priority,int camera,int left)
{
    if(!native_activation() || !gameplay() || video_ram[0xFE10]!=4 ||
       (priority!=4 && priority!=6))return;
    load_placements();s_capture_slot=0;s_capture_space=4;
    for(unsigned i=0;i<s_placement_count;++i) {
        const Placement *p=&s_placements[i];
        if((p->id!=0x50 || priority!=4) && (p->id!=0x12 || priority!=6))continue;
        if(p->x+32<left || (int)p->x-32>=left+s_requested_width)continue;
        unsigned o=placement_object(p),map,gfx,frame,flags;
        int x=p->x,y=p->y&4095;
        if(o) {
            if(!(video_ram[0xF600]&128) || s_capture_frame->identity[1+(o-0xD000)/64])continue;
            map=scene_read32(0xFF0000u+o+4);if(!map)continue;
            gfx=ram16(o+2);frame=video_ram[o+0x1A];flags=video_ram[o+1];
            x=ram16(o+8);y=ram16(o+12);
        } else {
            /* A destroyed remembered enemy must not reappear as scenery. */
            if(p->state && (video_ram[0xFC02+p->state]&128))continue;
            unsigned init=p->id==0x50?0xF84C:0xE922;
            map=scene_read32(init+2);gfx=scene_read16(init+10);
            frame=p->id==0x12?1:0;flags=(p->y>>14)&3;
        }
        add_mapping(map,frame,gfx,flags,x-camera,y-ram16(0xF704),0,0);
    }
}
/* DLE_FZboss creates the boss at camera $22B0. At 32:9 its stationary
 * cylinders are visible earlier. Use the ROM's initial pose and already
 * loaded boss art until each native cylinder initializes; no early boss,
 * collisions, random calls or guest object slots are introduced. */
static void final_cylinders_present(unsigned priority,int camera,int left)
{
    int event=video_ram[0xF742];
    if(priority!=3 || !gameplay() || ram16(0xFE10)!=0x502 || event<2 || event>4 ||
       scene_read32(0xFFF680) || ram16(0xF6F8))return;
    for(unsigned o=0xD000;o<0xF000;o+=64)
        if(video_ram[o]==0x85 && video_ram[o+0x24]==2 && video_ram[o+0x34]>=6)return;
    unsigned map=scene_read32(0x1A49A),gfx=scene_read16(0x1A494);
    s_capture_slot=0;s_capture_space=4;
    for(unsigned i=0;i<4;++i) {
        int x=scene_read16(0x1A462+i*4),y=scene_read16(0x1A464+i*4);
        if(x+32<=left || x-32>=left+s_requested_width)continue;
        unsigned o;
        for(o=0xD000;o<0xF000;o+=64)
            if(video_ram[o]==0x84 && video_ram[o+0x28]==i*2 &&
               video_ram[o+0x24]>=2 && scene_read32(0xFF0000u+o+4))break;
        if(o<0xF000)continue;
        add_mapping(map,0,gfx,i>=2?2:0,x-camera,y-ram16(0xF704),0,0);
    }
}
/* Orb_Main initializes the head but only its satellites queue sprites in
 * the first object pass. During the frozen level fade, show that already
 * initialized head too. The next ordinary object pass owns it again. */
static void initial_orbinauts_present(unsigned priority,int camera)
{
    if(!gameplay() || !(video_ram[0xF600]&128))return;
    for(unsigned o=0xD800;o<0xF000;o+=64) {
        if(video_ram[o]!=0x60 || video_ram[o+0x18]!=priority ||
           (video_ram[o+0x24]!=2 && video_ram[o+0x24]!=4) ||
           (video_ram[o+1]&12)!=4)continue;
        unsigned slot=1+(o-0xD000)/64,map=scene_read32(0xFF0000u+o+4);
        if(!map || s_capture_frame->identity[slot])continue;
        s_capture_slot=slot;s_capture_space=4;
        s_capture_frame->ox[slot]=(int16_t)ram16(o+8);
        s_capture_frame->oy[slot]=(int16_t)ram16(o+12);
        s_capture_frame->identity[slot]=0x60000000u|map;
        add_mapping(map,video_ram[o+0x1A],ram16(o+2),video_ram[o+1],
                    (int16_t)(ram16(o+8)-camera),(int16_t)(ram16(o+12)-ram16(0xF704)),0,0);
    }
}
static void spawn_scene(void)
{
    load_placements(); adopt_rings(); s_loader_active=1;
    int camera=ram16(0xF700), w=enabled()?s_requested_width:320;
    int lo,hi;activation_bounds(camera,w,&lo,&hi);
    int player=ram16(0xD008);
    s_pool_pressure=0;
    for (unsigned i=0;i<s_placement_count;++i) {
        Placement *p=&s_placements[i];
        if ((int)p->x<lo || (int)p->x>=hi) p->loaded=0;
        else if (!p->loaded && placement_alive(p)) p->loaded=1;
    }
    /* Fill closest to the player first, reserving space for collision-relevant
     * objects before remote scenery. Host-owned rings consume no object slots. */
    for (unsigned pass=0;pass<s_placement_count;++pass) {
        int best=-1, distance=INT_MAX;
        for (unsigned i=0;i<s_placement_count;++i) {
            Placement *p=&s_placements[i];
            if (p->loaded || (enabled() && p->id==0x25) || (int)p->x<lo || (int)p->x>=hi) continue;
            if (p->state && (video_ram[0xFC02+p->state]&128)) { p->loaded=1; continue; }
            int d=abs((int)p->x-player);
            if (d<distance) { best=(int)i; distance=d; }
        }
        if (best<0) break;
        Placement *p=&s_placements[best];
        unsigned o;
        for (o=0xD800;o<0xF000 && video_ram[o];o+=64) {}
        if (o==0xF000) { ++s_pool_pressure; break; }
        write16(o+8,p->x); write16(o+12,p->y&4095);
        unsigned flip=(p->y>>14)&3;
        write8(o+1,flip); write8(o+0x22,flip);
        write8(o+0x23,p->state); write8(o+0x28,p->subtype);
        if (p->state) write8(0xFC02+p->state,video_ram[0xFC02+p->state]|128);
        write8(o,p->id); p->loaded=1; ++s_spawned;
    }
    /* Keep original loader cursors coherent for tools/save-state resume. */
    unsigned r=0,l=0,rs=1,ls=1;
    while (r<s_placement_count && s_placements[r].x<(unsigned)(camera+640)) { rs+=s_placements[r].state!=0; ++r; }
    while (l<s_placement_count && (int)s_placements[l].x<camera-128) { ls+=s_placements[l].state!=0; ++l; }
    write32(0xF770,s_placement_base+r*6); write32(0xF774,s_placement_base+l*6);
    write8(0xFC00,rs); write8(0xFC01,ls); write16(0xF76E,camera&0xFF80);
}
static void ring_position(const Placement *p,int i,int *x,int *y)
{
    static const int8_t spacing[16][2]={{16,0},{24,0},{32,0},{0,16},{0,24},{0,32},
        {16,16},{24,24},{32,32},{-16,16},{-24,24},{-32,32},{16,8},{24,16},{-16,8},{-24,16}};
    *x=p->x+spacing[p->subtype>>4][0]*i;
    *y=(p->y&4095)+spacing[p->subtype>>4][1]*i;
}
static void collect_host_rings(void)
{
    if (!s_placement_count || ram16(0xD030)>=90) return;
    int px=ram16(0xD008), py=ram16(0xD00C), height=(int)video_ram[0xD016]-3;
    /* ReactToItem's ducking box (fr_Duck=$39 in REV00). */
    if (video_ram[0xD01A]==0x39) { py+=12-height+10; height=10; }
    for (unsigned n=0;n<s_placement_count;++n) {
        Placement *p=&s_placements[n];
        if (p->id!=0x25) continue;
        int count=(p->subtype&7)+1; if(count>7)count=7;
        for(int i=0;i<count;++i) {
            unsigned bit=1u<<i, state=0xFC02+p->state;
            if(video_ram[state]&bit)continue;
            int x,y; ring_position(p,i,&x,&y);
            if(abs(px-x)>14 || abs(py-y)>height+6)continue;
            write8(state,video_ram[state]|bit);
            /* Keep the game's score/life/SFX logic, including 100/200-ring
             * rewards. This call consumes no synthetic guest return slot. */
            M68KState saved=g_cpu;
            recomp_call_addr(0x9C76u);
            g_cpu=saved;
            s_sparks[s_spark_next++%64]=(RingSpark){x,y,s_scene_tick+1};
            ++s_ring_collected;
            return; /* original ReactToItem picks at most one ring per tick */
        }
    }
}
static void capture_objects(void)
{
    s_capture_frame->count=0;s_capture_slot=s_capture_space=0;
    memset(s_capture_frame->identity,0,sizeof s_capture_frame->identity);
    if(!s_capture_readonly){s_visible_count=0;++s_scene_tick;}
    s_capture_frame->scene=gameplay()?1:video_ram[0xD000]==9?2:0;
    int cam=ram16(0xF700), left=gameplay()?view_left(cam,s_requested_width):cam-(s_requested_width-320)/2;
    for(unsigned pri=0;pri<8;++pri) {
        unsigned queue=0xAC00+pri*128, bytes=ram16(queue);
        if(bytes>126)bytes=126;
        for(unsigned k=2;k<=bytes;k+=2) {
            unsigned o=ram16(queue+k);
            if(o<0xD000 || o>=0xF000 || !video_ram[o])continue;
            unsigned flags=video_ram[o+1], space=flags&12;
            s_capture_slot=1+(o-0xD000)/0x40;s_capture_space=space;
            s_capture_frame->ox[s_capture_slot]=(int16_t)ram16(o+8);
            s_capture_frame->oy[s_capture_slot]=(int16_t)ram16(o+12);
            s_capture_frame->identity[s_capture_slot]=((uint32_t)video_ram[o]<<24)|scene_read32(0xFF0000u+o+4);
            int x,y,hud=video_ram[o]==0x21?1:space?0:2;
            if(space) {
                unsigned scroll=space==4?0xF700:space==8?0xF708:0xF718;
                x=(int16_t)(ram16(o+8)-ram16(scroll));
                y=(int16_t)(ram16(o+12)-ram16(scroll+4));
            } else {
                x=(int16_t)ram16(o+8)-128;y=(int16_t)ram16(o+10)-128;
                if(video_ram[o]==0x34) {
                    /* Keep the ROM's arrival/hold/departure times and final
                     * composition, but cover the actual canvas on the way
                     * in/out instead of popping at the native 4:3 boundary. */
                    int target=(int16_t)ram16(o+0x30)-128;
                    x=target+(int)((int64_t)(x-target)*s_requested_width/320);
                    if(!s_capture_frame->scene)s_capture_frame->scene=3;
                }
            }
            unsigned first=s_capture_frame->count;
            add_mapping(scene_read32(0xFF0000u+o+4),video_ram[o+0x1A],ram16(o+2),flags,x,y,hud,(flags&32)!=0);
            if(space) {
                int visible=0,lo=left-cam,hi=lo+s_requested_width;
                /* Composite actors do not consistently populate obActWid
                 * (Final Zone's empty Eggmobile has zero). Cull their actual
                 * mapped pieces, rather than popping the whole actor when
                 * its reference point finally crosses the screen edge. */
                for(unsigned n=first;n<s_capture_frame->count;++n) {
                    const SceneSprite *p=&s_capture_frame->sprites[n];
                    int w=(((p->size>>2)&3)+1)*8;
                    if(p->x+w>lo && p->x<hi) {visible=1;break;}
                }
                if(!visible)s_capture_frame->count=first;
                else if(!s_capture_readonly && s_visible_count<128)s_visible_objects[s_visible_count++]=o;
            }
        }
        if(!s_capture_readonly) {
            /* The result card can select the next act before fading the
             * old layout. Retain its live actors, not next-act placements. */
            if(!results_world()) {
                marble_present(pri,cam,left);
                springyard_present(pri,cam,left);
            }
            final_cylinders_present(pri,cam,left);
            initial_orbinauts_present(pri,cam);
        }
        if(!s_capture_readonly && pri==2 && gameplay() && !native_activation()) {
            s_capture_slot=0;s_capture_space=4;
            for(unsigned n=0;n<s_placement_count;++n) {
                Placement *p=&s_placements[n]; if(p->id!=0x25)continue;
                int count=(p->subtype&7)+1;if(count>7)count=7;
                for(int i=0;i<count;++i) {
                    if(video_ram[0xFC02+p->state]&(1u<<i))continue;
                    int x,y;ring_position(p,i,&x,&y);
                    if(x<left-16 || x>=left+s_requested_width+16)continue;
                    add_mapping(0x9F92,video_ram[0xFEC3]&3,0x27B2,0,x-cam,y-ram16(0xF704),0,0);
                }
            }
            for(unsigned n=0;n<64;++n) {
                RingSpark *p=&s_sparks[n];unsigned age=s_scene_tick-p->tick;
                if(p->tick && age<24)add_mapping(0x9F92,4+age/6,0x27B2,0,p->x-cam,p->y-ram16(0xF704),0,0);
            }
        }
    }
}
static void capture_special(void)
{
    s_build.scene=2;
    unsigned angle=video_ram[0xF780]&252u;
    int sn=(int16_t)scene_read16(0x29F2+angle*2), cs=(int16_t)scene_read16(0x29F2+((angle+64)&255u)*2);
    int cx=ram16(0xF700), cy=ram16(0xF704), margin=(s_requested_width-320)/2;
    for(int row=0;row<128;++row)for(int col=0;col<128;++col) {
        unsigned id=video_ram[row*128+col];if(!id || id>0x4E)continue;
        int dx=col*24-cx-180, dy=row*24-cy-180;
        int x=((cs*dx-sn*dy)>>8)+160, y=((sn*dx+cs*dy)>>8)+112;
        if(x<-margin-32 || x>=320+margin+32 || y<-32 || y>=256)continue;
        unsigned type=0x4000+id*8;
        add_mapping(scene_read32(0xFF0000u+type),ram16(type+4),ram16(type+6),0,x,y,0,0);
    }
}
static void publish_sprites(void)
{
    for(unsigned n=0;!native_activation() && n<s_visible_count;++n) {
        unsigned o=s_visible_objects[n]; if(g_ram[o])write8(o+1,g_ram[o+1]|128);
    }
    s_build.serial=++s_serial;memcpy(s_build.sat,g_ram+0xF800,640);
    SceneFrame *dst=&s_history[s_serial%3];
    dst->count=s_build.count;dst->serial=s_serial;dst->scene=s_build.scene;
    memcpy(dst->ox,s_build.ox,sizeof dst->ox);memcpy(dst->oy,s_build.oy,sizeof dst->oy);memcpy(dst->identity,s_build.identity,sizeof dst->identity);
    memcpy(dst->sat,s_build.sat,640);
    memcpy(dst->sprites,s_build.sprites,s_build.count*sizeof(SceneSprite));
}
int s1_video_hook(uint32_t pc)
{
    if(s_interpolation_enabled && !enabled()) {
        if(pc==0xD65E) {
            s_capture_frame=&s_native_build;s_capture_readonly=1;capture_objects();
            s_capture_frame=&s_build;s_capture_readonly=0;
        } else if(pc==0xD746 || pc==0xD74E)native_publish();
    }
    if(pc==0xD8DA && !video_ram[0xF76C]) {
        s_placement_base=s_placement_count=0;s_loader_active=s_rings_adopted=0;
        memset(s_sparks,0,sizeof s_sparks);
    }
    if(pc==0xD980 && gameplay() && !native_activation() && (enabled() || s_loader_active)) { spawn_scene();return 1; }
    if(!enabled())return 0;
    /* Recorded inputs depend on the original activation, ring and visibility
     * timing. Expanding simulation changes moving-platform/enemy phases and
     * breaks the route. Keep the ROM's simulation intact during demos; the
     * host still composes the wider scenery and captured sprite mappings. */
    if(native_activation()) {
        switch(pc) {
        case 0xD65E:capture_objects();break;
        case 0xD746:case 0xD74E:case 0x1B286:case 0x1B28E:publish_sprites();break;
        case 0x1B156:capture_special();break;
        }
        return 0;
    }
    switch(pc) {
    case 0xD65E:capture_objects();return 0;
    case 0xD746:case 0xD74E:case 0x1B286:case 0x1B28E:publish_sprites();return 0;
    case 0x1B156:capture_special();return 0;
    case 0x1ADE4:if(gameplay())collect_host_rings();return 0;
    case 0xD8DA:return 0;
    case 0xD87E:case 0xD8A6: {
        if(!gameplay())return 0;
        unsigned o=g_cpu.A[0]&65535u;
        int x=ram16(o+8),y=(int16_t)(ram16(o+12)-ram16(0xF704));
        int r=pc==0xD8A6?video_ram[o+0x19]:0,left=view_left(ram16(0xF700),s_requested_width);
        g_cpu.D[0]=(x+r<left || x-r>=left+s_requested_width || y<0 || y>=224)?1:0;
        g_cpu.SR=(uint16_t)((g_cpu.SR&~15u)|(g_cpu.D[0]?0:4));
        return 1;
    }
    default:
        if(gameplay()) {
            /* All remaining configured sites are out_of_range's CMP. D0
             * holds a 128px-rounded offset from the original camera window.
             * Rebase it to our view; the following native CMP/BHI still owns
             * flags and the object's actual deletion/remember-state path. */
            int x=(uint16_t)((uint16_t)g_cpu.D[0]+((ram16(0xF700)-128)&0xFF80));
            int lo,hi;activation_bounds(ram16(0xF700),s_requested_width,&lo,&hi);
            unsigned outside=x<lo || x>=hi;
            g_cpu.D[0]=(g_cpu.D[0]&0xFFFF0000u)|(outside?641u:320u);
        }
        return 0;
    }
}

static uint8_t *s_priority;
static int s_priority_capacity;
/* The VDP follows the SAT link chain, not every byte in its 640-byte storage.
 * At level entry, unused tail bytes can differ after the last linked sprite.
 * Reject changes to every active entry (including its link), while ignoring
 * disconnected storage that cannot affect this displayed list. */
static int sat_matches(const SceneFrame *frame,const GVDP *v,unsigned base)
{
    unsigned entry=0;
    for(unsigned count=0;count<80;++count) {
        unsigned offset=entry*8;
        for(unsigned k=0;k<8;++k)
            if(frame->sat[offset+k]!=v->vram[(base+offset+k)&65535u])return 0;
        entry=frame->sat[offset+3]&127u;
        if(!entry)return 1;
        if(entry>=80)return 0;
    }
    return 0; /* malformed/cyclic lists are never associated speculatively */
}
static void select_scene(const GVDP *v)
{
    s_display=NULL;
    unsigned sat=(v->reg[5]&127u)<<9;
    /* Compare the exact linked entries of the installed display list. */
    for(unsigned n=0;n<3;++n) {
        const SceneFrame *p=&s_history[n];
        if(!p->serial || (s_display && p->serial<s_display->serial))continue;
        if(sat_matches(p,v,sat))s_display=p;
    }
    /* The game fiber can publish another list between scanlines. Keep the
     * selected display immutable until the next frame's VBlank selection. */
    if(s_display) {
        s_display_frame.count=s_display->count;s_display_frame.scene=s_display->scene;
        s_display_frame.serial=s_display->serial;
        memcpy(s_display_frame.ox,s_display->ox,sizeof s_display_frame.ox);
        memcpy(s_display_frame.oy,s_display->oy,sizeof s_display_frame.oy);
        memcpy(s_display_frame.identity,s_display->identity,sizeof s_display_frame.identity);
        memcpy(s_display_frame.sprites,s_display->sprites,s_display->count*sizeof(SceneSprite));
        s_display=&s_display_frame;
    }
}
static void draw_scene_sprites(const GVDP *v,int line,uint32_t *out,int width,
                               int origin,const uint32_t *palette)
{
    if(!s_display)return;
    for(unsigned n=0;n<s_display->count;++n) {
        const SceneSprite *s=&s_display->sprites[n];
        int cw=((s->size>>2)&3)+1,ch=(s->size&3)+1;
        int dx=0,dy=0;
        if(s_interpolating && !s->hud)interpolation_sprite_delta(s,line,&dx,&dy);
        int y=s->y+dy;
        if(line<y || line>=y+ch*8)continue;
        int base=s->x+(s->hud==1?0:s->hud==2?(width-320)/2:origin)+dx,iy=line-y;
        if(s->attr&0x1000)iy=ch*8-1-iy;
        for(int i=0;i<cw*8;++i) {
            int x=base+i;
            if(s_display->scene==0 && s->hud==2 &&
               (x<(width-320)/2 || x>=(width-320)/2+320))continue;
            if(x<0 || x>=width || (s_priority[x]&2))continue;
            int ix=(s->attr&0x800)?cw*8-1-i:i;
            uint16_t cell=(uint16_t)((s->attr&0xE000u)|
                (((s->attr&2047u)+(unsigned)(ix/8*ch+iy/8))&2047u));
            uint8_t p=sprite_pixel(v,s->asset,cell,ix,iy);
            if(!p)continue;
            s_priority[x]|=2;
            if(s->hud==1 || (s->attr&0x8000) || !(s_priority[x]&1))out[x]=palette[p];
        }
    }
}
static int attributes_differ(const GVDP *v,uint16_t a,uint16_t b,int x,int y)
{
    if(a==b)return 0;
    /* Empty chunks can inherit stale flip bits from DrawBlock's A0.
     * Compare rendered pixels/priority, not irrelevant empty-tile flags. */
    for(int i=0;i<8;++i) {
        uint8_t ap=pattern_pixel(v,a,x+i,y),bp=pattern_pixel(v,b,x+i,y);
        if(ap!=bp || (ap && ((a^b)&0x8000)))return 1;
    }
    return 0;
}
/* Deform_GHZ's three scroll bands. Reproject the ORIGINAL and WIDE camera
 * through the same integer math, rather than moving a parallax layer by the
 * foreground's full edge-clamp correction. The latter made scenery slide
 * backwards while Sonic moved inside a stationary wide viewport at spawn. */
static int ghz_parallax(int camera,int y)
{
    if(y<112)return camera*3/8;
    int distant=camera/2;
    if(y<152)return distant;
    int step=(camera-512-distant)*256/104;
    return distant+((step*(y-152))>>8);
}
static int background_parallax(int camera,int y)
{
    /* Reproject REV00 deformation through the clamped wide camera while
     * retaining each live row's accumulated scroll offset. */
    switch(video_ram[0xFE10]) {
    case 0:case 6:return ghz_parallax(camera,y);
    case 1:return camera/2;
    case 2:return camera*3/4;
    case 4:case 5:return camera/4;
    case 3: {
        int row=((y-192)&1023)/16;
        if(row<28) {
            int neg=-camera,step=((neg>>3)-neg)*16/28;
            return camera-(step*row>>4);
        }
        return row<33?-((-camera)>>3):row<38?-((-camera)>>2):-((-camera)>>1);
    }
    default:return 0;
    }
}
static void render_scanline(const GVDP *v, int line, const uint32_t *native, int nw,
                            uint32_t *out, int width,
                            GameVideoNativeRow native_row, void *native_ctx)
{
    uint32_t palette[64];
    for(int i=0;i<64;++i)palette[i]=genesis_dac_cram_to_argb(v->cram[i],GENESIS_DAC_NORMAL);
    uint32_t backdrop=palette[v->reg[7]&63];
    if(line==0 && !s_interpolating) {
        s_width=width;s_requested_width=width;
        s_terrain_checks=s_terrain_errors=s_bg_checks=s_bg_errors=s_bg_unstreamed=0;
        if(width>s_priority_capacity) {
            uint8_t *p=(uint8_t *)realloc(s_priority,(size_t)width);
            if(p){s_priority=p;s_priority_capacity=width;}
        }
        select_scene(v);
        /* Title objects/layout survive into the outgoing fade. Level select
         * retains those same RAM objects but clears Plane B: do not resurrect
         * the old title scenery behind its black menu. */
        s_title_world=0;
        if(video_ram[0xD040]==0x0E && video_ram[0xA440]) {
            unsigned base=(v->reg[4]&7u)<<13;
            for(unsigned k=0;k<4096;k+=2) {
                unsigned tile=vram16(v,base+k)&2047u;
                /* LevSelTextLoad replaces Plane B with the level-select
                 * font (ArtTile_Level_Select_Font=$680), so nonempty text
                 * cells do not mean the title scenery is still displayed. */
                /* Restored title states can retain old scenery cells next
                 * to the menu font. A live menu takes precedence over those
                 * leftovers; otherwise the host hides its native text. */
                if(tile>=0x680u) {s_title_world=0;break;}
                if(tile)s_title_world=1;
            }
        }
    }
    /* Display-off/fades fill the same full-width canvas, not black sidebars.
     * A rare unsupported mode also retains the selected output dimensions. */
    if(nw!=320 || !(v->reg[1]&64) || (v->reg[12]&14) || s_priority_capacity<width) {
        if (!native) native = native_row(native_ctx);
        uint32_t fill=!(v->reg[1]&64)?native[0]:backdrop;
        for(int x=0;x<width;++x)out[x]=fill;
        int x0=(width-nw)/2;
        if(x0>=0)memcpy(out+x0,native,(size_t)nw*sizeof(uint32_t));
        if(line==0){++s_fallbacks;s_native_x=x0;}
        return;
    }
    unsigned hsbase=(v->reg[13]&63u)<<10;
    int hmode=v->reg[11]&3,hi=hmode==0?0:hmode==2?line&~7:line;
    int hs_a=(int16_t)vram16(v,hsbase+(unsigned)hi*4);
    int hs_b=(int16_t)vram16(v,hsbase+(unsigned)hi*4+2);
    hs_a=interpolate_scroll(line,0,hs_a);hs_b=interpolate_scroll(line,1,hs_b);
    int level=gameplay(),special=s_display && s_display->scene==2;
    /* The next player object can exist while the title-card SAT is still
     * displayed. Do not expose the incoming level before its own display
     * list/planes reach VBlank. */
    if(s_title_world || (s_display && s_display->scene!=1))level=0;
    int camera=level?unwrap(-hs_a,ram16(0xFF10)):ram16(0xF700);
    int camera_y=unwrap(interpolate_vs(line,0,v->vsram[0]),ram16(0xFF14));
    if(line==0) {
        if(level)s_stage_width=stage_width();
        s_camera=camera;if(!s_interpolating)++s_frames;
        s_left=level?view_left(camera,width):camera-(width-nw)/2;
    }
    int origin=level?camera-s_left:(width-nw)/2;
    s_native_x=origin;
    if(!level && !special && !s_title_world &&
       !(s_display && s_display->scene==3)) {
        /* Menu text/logo planes are a centered composition, not repeating
         * scenery. Sampling them beyond 320px repeats stale tiles or text. */
        for(int x=0;x<width;++x)out[x]=backdrop;
        if (!native) native = native_row(native_ctx);
        memcpy(out+origin,native,(size_t)nw*sizeof(uint32_t));
        return;
    }
    unsigned base_a=(v->reg[2]&56u)<<10,base_b=(v->reg[4]&7u)<<13;
    int wy=camera_y+line;
    int bg_world=(level || s_title_world) && !special;
    /* Title VBlank writes its own VSRAM; FF1C still belongs to the last
     * level. After MZ that stale reference unwraps row 14 to 1038, where
     * the title layout is empty. Only level rows use the copied camera. */
    int by=level?unwrap(interpolate_vs(line,1,v->vsram[1]),ram16(0xFF1C))+line:line+(v->vsram[1]&1023);
    int bg_width=0;
    if(bg_world) {
        int row=((by>>8)&7)*128;
        for(int col=0;col<64;++col)if(video_ram[0xA440+row+col])bg_width=(col+1)*256;
    }
    /* Deform_GHZ's title perspective-water band starts at BG row $98.
     * Its scroll crosses X=0 and the hardware repeats a 512px nametable.
     * Wrapping those samples across the full level selects unrelated far-end
     * water chunks, exposing seams in the extended margins. */
    if(s_title_world && by>=0x98)bg_width=512;
    int bg_base=-origin-hs_b;
    if(level) {
        int margin=(width-nw)/2;
        int correction=background_parallax(s_left+margin,by)-background_parallax(camera,by);
        bg_base=-margin-hs_b+interpolate_background_correction(line,margin,correction);
    }
    int background_x=bg_base;
    if(bg_width>0) {
        background_x%=bg_width;
        if(background_x<0)background_x+=bg_width;
    }
    GenesisTileRow fg = {0}, bg = {0};
    for(int x=0;x<width;++x) {
        int nx=x-origin,wx=level?s_left+x:nx-hs_a;
        int reference_bx=nx-hs_b,bx=bg_base+x;
        if (!fg.remaining) {
            uint16_t attr=level?world_attr(wx,wy,0):plane_attr(v,base_a,nx-hs_a,line+(v->vsram[0]&1023));
            gtile_row_begin(&fg,v->vram,attr,level?wx:nx-hs_a,level?wy:line+(v->vsram[0]&1023));
        }
        if (!bg.remaining) {
            uint16_t attr=bg_world?world_attr(background_x,by,1):plane_attr(v,base_b,bx,by);
            gtile_row_begin(&bg,v->vram,attr,bg_world?background_x:bx,by);
        }
        uint16_t a=fg.attr,b=bg.attr;
        uint8_t ap=gtile_row_pixel(&fg),bp=gtile_row_pixel(&bg);
        if(level && (wx<0 || wx>=s_stage_width))ap=0;
        /* Menus have a centered foreground composition, with full-width
         * scenery underneath. Special-stage planes are repeating art. */
        if(!level && !special && (nx<0 || nx>=nw))ap=0;
        s_priority[x]=(uint8_t)(((a&0x8000)&&ap)||((b&0x8000)&&bp));
        uint8_t p=ap&&(a&0x8000)?ap:bp&&(b&0x8000)?bp:ap?ap:bp;
        out[x]=p?palette[p]:backdrop;
        if(!s_interpolating && nx>=0 && nx<nw && !(x&7)) {
            if(level) {
                uint16_t expected=plane_attr(v,base_a,nx-hs_a,line+(v->vsram[0]&1023));
                ++s_terrain_checks;
                s_terrain_errors+=attributes_differ(v,a,expected,wx,wy);
            }
            if(bg_world) {
                /* GHZ's perspective water can address outside the streamed
                 * 320px strip even in native mode, wrapping to stale cells.
                 * The enhanced world deliberately fills that missing data.
                 * Only compare against native cells the game actually keeps
                 * current, and report excluded samples separately. */
                int scroll=ram16(by<ram16(0xF7F0)?0xFF18:0xFF20)&~15;
                int lo=scroll<16?0:scroll-16;
                if((video_ram[0xFE10]==0 || video_ram[0xFE10]==6) &&
                   (reference_bx<lo || reference_bx>=scroll+320))++s_bg_unstreamed;
                else {
                    /* Mapping diagnostic uses the original camera's sample;
                     * the displayed sample intentionally follows the new one. */
                    int rx=reference_bx;
                    if(bg_width>0){rx%=bg_width;if(rx<0)rx+=bg_width;}
                    uint16_t expected=plane_attr(v,base_b,reference_bx,by);
                    ++s_bg_checks;s_bg_errors+=attributes_differ(v,world_attr(rx,by,1),expected,reference_bx,by);
                }
            }
        }
        /* Background coordinates increase by one pixel. Layout widths are
         * whole 256px chunks, so wrap also ends the current pattern row. */
        ++background_x;
        if(bg_width>0 && background_x==bg_width)background_x=0;
    }
    if(!level && !special && !(s_display && s_display->scene==3)) {
        if (!native) native = native_row(native_ctx);
        /* Keep the original title sprite mask as well as its logo/menu
         * composition. Redrawing title sprites bypasses the X=0 SAT mask
         * that hides Sonic's torso behind the emblem. Extend only the
         * surrounding scenery, following the live scene palette. */
        if(origin>=0)memcpy(out+origin,native,(size_t)nw*sizeof(uint32_t));
    } else if(s_display) {
        draw_scene_sprites(v,line,out,width,origin,palette);
    } else {
        /* No matching DMA yet (e.g. load/fade): preserve the live native
         * scene while the host display list catches its next publication. */
        if (!native) native = native_row(native_ctx);
        int first=origin<0?-origin:0,last=origin+nw>width?width-origin:nw;
        if(last>first)memcpy(out+origin+first,native+first,(size_t)(last-first)*sizeof(uint32_t));
    }
}

static void scanline(const GVDP *v, int line, const uint32_t *native, int nw,
                     uint32_t *out, int width)
{
    render_scanline(v, line, native, nw, out, width, NULL, NULL);
}

static void scanline_lazy(const GVDP *v, int line, int nw, uint32_t *out, int width,
                          GameVideoNativeRow native_row, void *native_ctx)
{
    render_scanline(v, line, NULL, nw, out, width, native_row, native_ctx);
}

void s1_video_command(int id, const char *json)
{
    (void)json;
    char reply[768];
    snprintf(reply, sizeof reply,
        "{\"id\":%d,\"enabled\":%d,\"width\":%d,\"view_left\":%d,\"camera\":%d,"
        "\"native_x\":%d,\"stage_width\":%d,\"frames\":%u,\"fallback_frames\":%u,"
        "\"terrain_checks\":%u,\"terrain_errors\":%u,\"background_checks\":%u,\"background_errors\":%u,\"background_unstreamed\":%u,"
        "\"sprites\":%u,\"scene\":%d,\"spawned\":%u,\"pool_pressure\":%u,\"host_rings_collected\":%u,"
        "\"signpost_art_hash\":\"%08x\",\"button_art_hash\":\"%08x\",\"mz_button_art_hash\":\"%08x\",\"button_art_display_hash\":\"%08x\"}",
        id, enabled(), s_width, s_left, s_camera, s_native_x, s_stage_width,
        s_frames, s_fallbacks, s_terrain_checks, s_terrain_errors,s_bg_checks,s_bg_errors,s_bg_unstreamed,
        s_display?s_display->count:0,s_display?s_display->scene:-1,s_spawned,s_pool_pressure,s_ring_collected,
        s_sprite_art[0].hash,s_sprite_art[1].hash,s_sprite_art[2].hash,s_sprite_art[1].display_hash);
    cmd_send_response(reply);
}

/* Native-only capture is read-only and independent of the ordinary host
 * loader/ring/scene history. These disposable snapshots are never serialized. */
enum { IX_LINES=224 };
typedef struct {
    GVDP video;
    uint8_t ram[65536];
    SceneFrame scene;
    uint32_t *image;
    size_t capacity;
    int width,lines,valid;
    unsigned tick,zone;
    int16_t scroll[IX_LINES][2];
    uint16_t vs[IX_LINES][2],cram[IX_LINES][64];
} InterpFrame;
static InterpFrame s_ix[2];
static unsigned s_ix_front;
static InterpFrame *ix_current(void){return &s_ix[s_ix_front];}
static InterpFrame *ix_previous(void){return &s_ix[s_ix_front^1u];}
static void interpolation_reset(void)
{
    s_ix[0].valid=s_ix[1].valid=0;s_ix[0].lines=s_ix[1].lines=0;
    for(unsigned n=0;n<3;++n)s_native_history[n].serial=0;
    s_native_build.serial=s_native_serial=0;
}
static void interpolation_configure(int on)
{
    on=on!=0;
    if(on!=s_interpolation_enabled){interpolation_reset();s_interpolation_enabled=on;}
}
static int lerp(int a,int b,int limit)
{
    int d=(int16_t)(uint16_t)(b-a);
    return d>limit || d<-limit?b:b-d+(int)(d*s_phase+(d<0?-.5:.5));
}
static int interpolate_scroll(int line,int which,int current)
{
    return s_interpolating && line>=0 && line<IX_LINES?
        lerp(ix_previous()->scroll[line][which],ix_current()->scroll[line][which],128):current;
}
static int interpolate_vs(int line,int which,int current)
{
    return s_interpolating && line>=0 && line<IX_LINES?
        lerp(ix_previous()->vs[line][which],ix_current()->vs[line][which],128):current;
}
static int interpolate_background_correction(int line,int margin,int current)
{
    if(!s_interpolating || line<0 || line>=IX_LINES)return current;
    const InterpFrame *frames[]={ix_previous(),ix_current()};
    int correction[2];
    for(int n=0;n<2;++n) {
        const InterpFrame *f=frames[n];
        int reference=(f->ram[0xFF10]<<8)|f->ram[0xFF11];
        int camera=unwrap(-f->scroll[line][0],reference);
        int left=view_left(unwrap(-f->scroll[0][0],reference),f->width);
        int y=unwrap(f->vs[line][1],(f->ram[0xFF1C]<<8)|f->ram[0xFF1D])+line;
        correction[n]=background_parallax(left+margin,y)-background_parallax(camera,y);
    }
    /* Interpolate the original integer correction with the same rounding
     * as native B scroll. Recomputing parallax from an already-rounded
     * fractional A camera makes a stationary clamped background jitter. */
    return lerp(correction[0],correction[1],128);
}
static void interpolation_sprite_delta(const SceneSprite *sprite,int line,int *dx,int *dy)
{
    const InterpFrame *a=ix_previous(),*b=ix_current();
    unsigned slot=sprite->slot;
    if(slot && slot<SCENE_SLOTS && b->scene.identity[slot] &&
       a->scene.identity[slot]==b->scene.identity[slot]) {
        *dx=lerp(a->scene.ox[slot],b->scene.ox[slot],48)-b->scene.ox[slot];
        *dy=lerp(a->scene.oy[slot],b->scene.oy[slot],48)-b->scene.oy[slot];
    }
    if(sprite->space==4) {
        /* Current mappings already align with the current installed scroll.
         * Reproject by only the fractional difference, preserving that pose. */
        *dx+=interpolate_scroll(line,0,b->scroll[line][0])-b->scroll[line][0];
        *dy+=b->vs[line][0]-interpolate_vs(line,0,b->vs[line][0]);
    } else if(sprite->space==8 || sprite->space==12) {
        unsigned scroll=sprite->space==8?0xF708u:0xF718u;
        int ax=(a->ram[scroll]<<8)|a->ram[scroll+1],bx=(b->ram[scroll]<<8)|b->ram[scroll+1];
        int ay=(a->ram[scroll+4]<<8)|a->ram[scroll+5],by=(b->ram[scroll+4]<<8)|b->ram[scroll+5];
        *dx+=bx-lerp(ax,bx,128);*dy+=by-lerp(ay,by,128);
    }
}
static void native_publish(void)
{
    s_native_build.serial=++s_native_serial;
    memcpy(s_native_build.sat,g_ram+0xF800,sizeof s_native_build.sat);
    s_native_history[s_native_serial%3]=s_native_build;
}
static const SceneFrame *native_scene(const GVDP *v)
{
    const SceneFrame *best=NULL;unsigned sat=(v->reg[5]&127u)<<9;
    for(unsigned n=0;n<3;++n) {
        const SceneFrame *f=&s_native_history[n];
        if(!f->serial || (best && f->serial<=best->serial))continue;
        if(sat_matches(f,v,sat))best=f;
    }
    return best;
}
static void presentation_capture(const GVDP *v,int line,const uint32_t *pixels,
                                  int width,int native_width,const GVDPRasterRow *raster)
{
    if(!s_interpolation_enabled || line<0 || line>=IX_LINES || width<320)return;
    if(line==0) {
        s_ix_front^=1u;InterpFrame *f=ix_current();f->valid=0;f->lines=0;f->width=width;
        size_t size=(size_t)width*IX_LINES;
        if(size>f->capacity) {
            uint32_t *p=(uint32_t*)realloc(f->image,size*sizeof *p);
            if(!p)return;f->image=p;f->capacity=size;
        }
        f->video=*v;memcpy(f->ram,g_ram,sizeof f->ram);
        f->tick=ram16(0xFE04);f->zone=ram16(0xFE10);
        const SceneFrame *scene=enabled()?s_display:native_scene(v);
        if(scene)f->scene=*scene;else memset(&f->scene,0,sizeof f->scene);
        f->valid=native_width==320 && gameplay() && scene && scene->scene==1 &&
            (v->reg[1]&64) && !(v->reg[12]&14) && stage_width()>=320;
    }
    InterpFrame *f=ix_current();
    if(f->width!=width || !f->image || f->capacity<(size_t)width*IX_LINES)return;
    memcpy(f->image+(size_t)line*width,pixels,(size_t)width*sizeof *pixels);
    memcpy(f->cram[line],v->cram,sizeof f->cram[line]);
    f->vs[line][0]=v->vsram[0];f->vs[line][1]=v->vsram[1];
    unsigned base=(v->reg[13]&63u)<<10,mode=v->reg[11]&3;
    unsigned row=mode==0?0:mode==2?(unsigned)line&~7u:(unsigned)line;
    f->scroll[line][0]=(int16_t)vram16(v,base+row*4);
    f->scroll[line][1]=(int16_t)vram16(v,base+row*4+2);
    /* This stock S1 adapter does not own optional private-provider art. */
    if(raster && raster->private_planes)f->valid=0;
    f->lines=line+1;
}
static int interpolate(double phase,uint32_t *image,int stride,int height)
{
    InterpFrame *f=ix_current(),*p=ix_previous();
    if(!s_interpolation_enabled || !f->valid || !p->valid || f->lines!=IX_LINES ||
       p->lines!=IX_LINES || f->width!=p->width || height!=IX_LINES || stride<f->width ||
       f->zone!=p->zone || (unsigned)(uint16_t)(f->tick-p->tick)>2)return 0;
    if(phase<=0 || phase>=1) {
        const InterpFrame *from=phase<=0?p:f;
        for(int y=0;y<IX_LINES;++y)memcpy(image+(size_t)y*stride,from->image+(size_t)y*f->width,(size_t)f->width*sizeof *image);
        return 1;
    }
    if(f->width>s_priority_capacity) {
        uint8_t *q=(uint8_t*)realloc(s_priority,(size_t)f->width);
        if(!q)return 0;s_priority=q;s_priority_capacity=f->width;
    }
    int saved[]={s_width,s_left,s_camera,s_stage_width,s_native_x,s_requested_width,s_title_world};
    const SceneFrame *scene=s_display;
    s_display=&f->scene;s_interpolating=1;s_phase=phase;s_read_ram=f->ram;s_title_world=0;
    for(int y=0;y<IX_LINES;++y) {
        memcpy(f->video.cram,f->cram[y],sizeof f->cram[y]);
        render_scanline(&f->video,y,f->image+(size_t)y*f->width,320,image+(size_t)y*stride,f->width,NULL,NULL);
    }
    memcpy(f->video.cram,f->cram[0],sizeof f->cram[0]);
    s_interpolating=0;s_read_ram=NULL;s_display=scene;
    s_width=saved[0];s_left=saved[1];s_camera=saved[2];s_stage_width=saved[3];
    s_native_x=saved[4];s_requested_width=saved[5];s_title_world=saved[6];
    return 1;
}

const GameVideo sonic1_video = {
    .configure=configure, .enabled=enabled, .width=width,
    .scanline=scanline, .scanline_lazy=scanline_lazy,
    /* Native Off keeps the shared VDP row owner. FPS capture is independent. */
    .presentation_configure=interpolation_configure,
    .presentation_capture=presentation_capture,
    .interpolate=interpolate, .presentation_reset=interpolation_reset
};
