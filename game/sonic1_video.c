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
             mode == 0 || mode == 4) && player == 1) ||
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
    if((video_ram[0xF600]&127)==0 || (video_ram[0xF600]&127)==4)return 1;
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
 * patterns. Decode complete immutable sprite assets separately;
 * guest VRAM, PLC order and simulation clocks remain untouched. Format and
 * XOR rows follow REV00 NemDec ($1440), not a second machine execution. */
typedef struct { uint8_t pixels[8192];unsigned ready,size,base;uint32_t hash,display_hash; } SpriteArt;
enum { SPRITE_ART_COUNT=21 };
static SpriteArt s_sprite_art[SPRITE_ART_COUNT];
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
    if(!asset || asset>SPRITE_ART_COUNT || !enabled() || s_requested_width<=320)return pattern_pixel(v,attr,x,y);
    static const unsigned addresses[]={0x3A53E,0x3401A,0x32408,0x5E860,0x35102,0x5D7B0,0x6A324,0x5CC62,0x339DA,
        0x3B3DA,0x3B532,0x3B68E,0x3B80A,0x3B926,0x3BA5C,0x3BB96,0x3401A,0x5D4C6,
        0x36DD4,0x3A360,0x3A462};
    static const unsigned bases[]={0x680,0x50F,0x513,0x300,0x518,0x49D,0x400,0x400,0x3CC,
        0x580,0x592,0x580,0x592,0x580,0x592,0x580,0x4A0,0x46C,0x486,0x523,0x533};
    SpriteArt *art=&s_sprite_art[asset-1];
    if(!art->ready) {
        art->base=bases[asset-1];
        if(asset==7) {
            /* The giant ring's source is uncompressed; its incremental
             * VBlank transfer need not finish before the wider view sees it. */
            art->size=0xC40;memcpy(art->pixels,g_rom+addresses[asset-1],art->size);art->ready=1;
        } else art->ready=decode_sprite_art(art,addresses[asset-1])?1:2;
    }
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
enum { SCENE_SPRITES = 32768, SCENE_PLACEMENTS = 1024, SCENE_SLOTS=160 };
typedef struct {
    SceneSprite sprites[SCENE_SPRITES];
    unsigned count, serial;
    uint8_t sat[640];
    int scene; /* 1 level, 2 special, 3 title card, 0 menu */
    int zone,background_y;
    int16_t background_scroll[224];
    int16_t ox[SCENE_SLOTS],oy[SCENE_SLOTS];
    uint32_t identity[SCENE_SLOTS];
} SceneFrame;
enum { SCENE_CAMERA_TAG=0x53314341u };
static int departing_demo(const SceneFrame *scene,const uint8_t *ram)
{
    int mode=ram[0xF600]&127;
    return scene && scene->scene==1 && scene->identity[0]==SCENE_CAMERA_TAG &&
        (mode==0 || mode==4) && ram[0xD000]==1;
}
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
static uint16_t s_overflow_objects[128];
static unsigned s_overflow_count;
typedef struct { unsigned address; uint16_t x, y; uint8_t id, subtype, state, loaded; } Placement;
static Placement s_placements[SCENE_PLACEMENTS];
static unsigned s_placement_count, s_placement_base;
static int s_loader_active, s_rings_adopted;
typedef struct { int x, y; unsigned tick; } RingSpark;
static RingSpark s_sparks[64];
static unsigned s_spark_next;
typedef struct { uint8_t object[64]; uint16_t slot; uint8_t seen, tail; uint32_t generation; } SceneTail;
/* Scene-exclusive continuations share storage and reset at level setup. */
static SceneTail s_actor_tails[128];
static unsigned s_tail_frame;

/* Object-loader decisions and captured mappings belong to the saved timeline.
 * Interpolation images, selected row pointers and immutable ROM art do not. */
typedef struct {
    uint32_t magic, serial, scene_tick, spawned, pool_pressure, ring_collected;
    unsigned placement_count, placement_base, loader_active, rings_adopted;
    unsigned spark_next, visible_count;
    Placement placements[SCENE_PLACEMENTS];
    RingSpark sparks[64];
    unsigned visible_objects[128];
    SceneTail actor_tails[128];
    unsigned tail_frame;
    uint16_t overflow_objects[128];
    unsigned overflow_count;
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
    memcpy(state.actor_tails,s_actor_tails,sizeof s_actor_tails);
    state.tail_frame=s_tail_frame;
    state.overflow_count=s_overflow_count;
    memcpy(state.overflow_objects,s_overflow_objects,sizeof s_overflow_objects);
    uint8_t *out=dst;memcpy(out,&state,sizeof state);out+=sizeof state;
    for(unsigned n=0;n<4;++n) {
        memcpy(out,&frames[n]->count,SCENE_HEADER);out+=SCENE_HEADER;
        size_t bytes=frames[n]->count*sizeof(SceneSprite);
        memcpy(out,frames[n]->sprites,bytes);out+=bytes;
    }
    return need;
}
static int video_state_load(const void *src,size_t len,int apply)
{
    VideoState state;
    if(!src || len<sizeof state)return 0;
    memcpy(&state,src,sizeof state);
    if(state.magic!=0x53315631u || state.placement_count>SCENE_PLACEMENTS ||
       state.visible_count>128 || state.overflow_count>128 ||
       state.loader_active>1 || state.rings_adopted>1)return 0;
    for(unsigned n=0;n<state.overflow_count;++n)
        if(state.overflow_objects[n]<0xD000 || state.overflow_objects[n]>=0xF000 ||
           (state.overflow_objects[n]&63))return 0;
    for(unsigned n=0;n<state.visible_count;++n)
        if(state.visible_objects[n]<0xD000 || state.visible_objects[n]>=0xF000 ||
           (state.visible_objects[n]&63))return 0;
    for(unsigned n=0;n<128;++n) {
        const SceneTail *a=&state.actor_tails[n];
        int player_owner=a->slot==0xD000 && a->object[0]==1;
        if(a->seen>1 || a->tail>1 || (a->slot && !player_owner &&
           (a->slot<0xD800 || a->slot>=0xF000 || (a->slot&63))))return 0;
        if(a->seen && !a->slot && a->object[0]!=0x3F)return 0;
    }
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
            if(sprite.asset>SPRITE_ART_COUNT || sprite.size>15 || sprite.hud>2 || sprite.slot>=SCENE_SLOTS)return 0;
        }
    }
    if(left)return 0;
    if(!apply)return 1;
    s_serial=state.serial;s_scene_tick=state.scene_tick;s_spawned=state.spawned;
    s_pool_pressure=state.pool_pressure;s_ring_collected=state.ring_collected;
    s_placement_count=state.placement_count;s_placement_base=state.placement_base;
    s_loader_active=state.loader_active;s_rings_adopted=state.rings_adopted;
    s_spark_next=state.spark_next;s_visible_count=state.visible_count;
    s_overflow_count=state.overflow_count;
    memcpy(s_overflow_objects,state.overflow_objects,sizeof s_overflow_objects);
    memcpy(s_placements,state.placements,sizeof s_placements);
    memcpy(s_sparks,state.sparks,sizeof s_sparks);
    memcpy(s_visible_objects,state.visible_objects,sizeof s_visible_objects);
    memcpy(s_actor_tails,state.actor_tails,sizeof s_actor_tails);
    s_tail_frame=state.tail_frame;
    SceneFrame *frames[]={&s_build,&s_history[0],&s_history[1],&s_history[2]};
    for(unsigned n=0;n<4;++n) {
        memcpy(&frames[n]->count,headers[n],SCENE_HEADER);
        memcpy(frames[n]->sprites,sprites[n],counts[n]*sizeof(SceneSprite));
    }
    s_display=NULL;s_title_world=0;interpolation_reset();
    return 1;
}
int s1_video_rb_load(const void *src,size_t len)
{
    return video_state_load(src,len,1);
}

/* The shared disk envelope requires a fixed-size, preflightable host section.
 * Keep the compact rollback representation inside a zero-filled reservation;
 * disk loads then recover the same placements and displayed mappings. */
typedef struct { uint32_t magic,bytes,mode,reserved;double ratio; } VideoDiskHeader;
size_t s1_video_state_size(void)
{
    return sizeof(VideoDiskHeader)+sizeof(VideoState)+
        4*(SCENE_HEADER+SCENE_SPRITES*sizeof(SceneSprite));
}
int s1_video_state_save(void *dst,size_t size)
{
    if(!dst || size!=s1_video_state_size())return 0;
    memset(dst,0,size);
    VideoDiskHeader header={0};
    header.magic=0x53314431u;header.mode=(uint32_t)s_mode;header.ratio=s_ratio;
    header.bytes=(uint32_t)s1_video_rb_save((uint8_t*)dst+sizeof header,size-sizeof header);
    if(!header.bytes)return 0;
    memcpy(dst,&header,sizeof header);return 1;
}
int s1_video_state_load(const void *src,size_t size,int apply)
{
    if(!src || size!=s1_video_state_size())return 0;
    VideoDiskHeader header;memcpy(&header,src,sizeof header);
    if(header.magic!=0x53314431u || header.reserved || header.mode!=(uint32_t)s_mode ||
       (s_mode==VIDEO_RATIO && header.ratio!=s_ratio) ||
       header.bytes>size-sizeof header)return 0;
    return video_state_load((const uint8_t*)src+sizeof header,header.bytes,apply);
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
static int scene_left(const uint8_t *ram,int camera,int w)
{
    /* Eggman's escape extends the original camera beyond the populated
     * foreground's edge. A layout-width clamp pins the wide view behind it.
     * Keep the original camera centered without changing activation bounds. */
    if(ram[0xFE10]==5 && ram[0xFE11]==2 && ram[0xF742]>=8)
        return camera-(w-320)/2;
    return view_left(camera,w);
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
    s_overflow_count=0;
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
        unsigned owner=s_capture_slot && s_capture_slot<=128?video_ram[0xD000+(s_capture_slot-1)*64]:0;
        unsigned animal=0;
        if(owner==0x28) {
            unsigned o=0xD000+(s_capture_slot-1)*64;
            if(!video_ram[o+0x28] && video_ram[o+0x30]<7)animal=10+video_ram[o+0x30];
        }
        /* The shared Eggmobile mappings include the exhaust. Wider object
         * activation can expose them before their PLC completes as well. */
        s->asset=(uint8_t)(map==0xAC70?19:map==0xDD1A?((gfx&2047u)==0x533?21:20):
            owner==0x5C?9:map==0x17D0C?8:map==0x17DD4?18:
            owner==0x82 && map==0xBEA4?17:owner==0x0D?1:owner==0x32?(video_ram[0xFE10]==2?3:2):owner==0x84?4:owner==0x83?5:owner==0x3E?6:owner==0x4B?7:animal);
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
        if (!p->state && video_ram[o]==p->id) {
            /* Pylon owns the level-wide screen-space decoration; its X is
             * derived from the camera rather than its sole placement. */
            if(p->id==0x5C)return o;
            if(ram16(o+8)==p->x)return o;
            /* BasicPlatform moves on its first pass. Its persistent origin,
             * rather than the displaced pose, identifies a native placement
             * when card departure hands loading over to the wide loader. */
            if(p->id==0x18 && ram16(o+0x32)==p->x &&
               ram16(o+0x34)==(p->y&4095))return o;
        }
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
    if(!gameplay() || video_ram[0xFE10]!=2 ||
       (priority!=3 && priority!=4 && priority!=5))return;
    int native=native_activation();
    load_placements();s_capture_slot=0;s_capture_space=4;
    for(unsigned i=0;i<s_placement_count;++i) {
        const Placement *p=&s_placements[i];
        if(p->x+64<left || (int)p->x-64>=left+s_requested_width)continue;
        if(p->id!=0x2F && p->id!=0x30)continue;
        unsigned o=placement_object(p);s_capture_slot=0;
        if(o) {
            unsigned slot=1+(o-0xD000)/64;
            if(p->id!=0x2F || s_capture_frame->identity[slot])continue;
            unsigned map=scene_read32(0xFF0000u+o+4);
            if(video_ram[o+0x24] && map) {
                if(priority!=video_ram[o+0x18])continue;
                s_capture_slot=slot;
                s_capture_frame->ox[slot]=(int16_t)ram16(o+8);
                s_capture_frame->oy[slot]=(int16_t)ram16(o+12);
                s_capture_frame->identity[slot]=0x2F000000u|map;
                add_mapping(map,video_ram[o+0x1A],ram16(o+2),video_ram[o+1],
                            (int)ram16(o+8)-camera,(int)ram16(o+12)-ram16(0xF704),0,0);
                continue;
            }
        } else if(!native && (p->id!=0x2F || p->loaded))continue;
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
 * cylinders are visible earlier. Use the ROM's initial pose and immutable
 * art until each native cylinder initializes; no early boss,
 * collisions, random calls or guest object slots are introduced. */
static void final_cylinders_present(unsigned priority,int camera,int left)
{
    int event=video_ram[0xF742];
    if(priority!=3 || !gameplay() || ram16(0xFE10)!=0x502 || event<2 || event>4)return;
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
        unsigned first=s_capture_frame->count;
        add_mapping(map,0,gfx,i>=2?2:0,x-camera,y-ram16(0xF704),0,0);
        for(unsigned n=first;n<s_capture_frame->count;++n)s_capture_frame->sprites[n].asset=4;
    }
}
static void labyrinth_boss_present(unsigned priority,int camera,int left)
{
    /* DLE_LZ3 waits for the native camera gate. An expanded view can see
     * the stationary ship first; compose its initial ROM pose until the
     * original boss exists, without starting its chase or allocating slots. */
    unsigned event=video_ram[0xF742],pending=0;
    if(priority!=4 || !gameplay() || s_requested_width<=320 ||
       ram16(0xFE10)!=0x102 || event>2 || ram16(0xF704)>=0x600)return;
    for(unsigned o=0xD800;o<0xF000;o+=64)if(video_ram[o]==0x77) {
        if(video_ram[o+0x24]>=2)return;
        pending=1;
    }
    /* DLE can allocate after ExecuteObjects. Cover that uninitialized tick
     * too, but never recreate a boss removed after the event has started. */
    if(event && !pending)return;
    const int x=0x1E10,y=0x5C0;
    if(x+40<=left || x-40>=left+s_requested_width)return;
    s_capture_slot=0;s_capture_space=4;
    static const unsigned frames[]={0,1,10};
    for(unsigned i=0;i<3;++i)
        add_mapping(0x17D0C,frames[i],0x400,4,
                    x-camera,y-ram16(0xF704),0,0);
}
static void stationary_boss_present(unsigned priority,int camera,int left)
{
    /* The stationary ship is visible before the native music/PLC gate.
     * Publish only its initial pose until the real boss initializes. */
    unsigned zone=ram16(0xFE10),event=video_ram[0xF742],pending=0;
    int marble=zone==0x202;
    unsigned id=marble?0x73:0x7A,gate=marble?2:4;
    if((priority!=3 && priority!=4) || !gameplay() || s_requested_width<=320 ||
       (zone!=0x302 && !marble) || event>gate)return;
    for(unsigned o=0xD800;o<0xF000;o+=64)if(video_ram[o]==id) {
        if(video_ram[o+0x24]>=2)return;
        pending=1;
    }
    if(event==gate && !pending)return;
    const int x=marble?0x19F0:0x2188,y=marble?0x22C:0x228;
    if(x+48<=left || x-48>=left+s_requested_width)return;
    s_capture_slot=0;s_capture_space=4;
    if(priority==3)add_mapping(0x17DD4,marble?4:3,0x246C,4,x-camera,y-ram16(0xF704),0,0);
    else {
        static const unsigned frames[]={0,1,8};
        for(unsigned i=0;i<3;++i)
            add_mapping(0x17D0C,frames[i],0x400,4,x-camera,y-ram16(0xF704),0,0);
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
static unsigned actor_word(const uint8_t *p,unsigned a) { return (p[a]<<8)|p[a+1]; }
static void actor_put(uint8_t *p,unsigned a,unsigned v) { p[a]=(uint8_t)(v>>8);p[a+1]=(uint8_t)v; }
static void remember_tail(SceneTail *tail,unsigned object)
{
    memcpy(tail->object,video_ram+object,64);tail->slot=(uint16_t)object;
    tail->seen=tail->tail=1;
}
static int animal_floor(int x,int y);
static int runoff_floor(const uint8_t *p,int x,int y)
{
    int right=animal_floor(x+p[0x17],y+p[0x16]);
    int left=animal_floor(x-p[0x17],y+p[0x16]);
    return right<left?right:left;
}
/* Keep the scripted run-off moving after Sonic reaches the native boundary.
 * Preserve moving entry velocity, or accelerate a stopped pose from rest.
 * SBZ2's pan catches this pose until the real
 * player reaches it, then ordinary presentation resumes without a position
 * jump. Reuse the serialized, level-local continuation storage. */
static int act_runoff_x(int *display_y)
{
    int x=ram16(0xD008),zone=ram16(0xFE10);
    SceneTail *tail=&s_actor_tails[0];
    if(s_capture_readonly || !enabled() || s_requested_width<=320)return x;
    if(zone>0x501 || video_ram[0xD000]!=1 || recorded_demo()) {
        if(tail->seen && tail->object[0]==1)memset(tail,0,sizeof *tail);
        return x;
    }
    if(!tail->seen || tail->object[0]!=1) {
        if((video_ram[0xD5C0]!=0x3A && !video_ram[0xF7CC]) ||
           x<(int)ram16(0xF72A)+352)return x;
        int extra=(s_requested_width-320)/2+32;
        int target=extra>65535-x?65535:x+extra;
        int edge=scene_left(video_ram,ram16(0xF700),s_requested_width)+s_requested_width+32;
        if(edge>target)target=edge>65535?65535:edge;
        /* Stay within SBZ2's approach so the original player catches this
         * pose before the scripted stop and floor collapse. */
        if(zone==0x501 && target>0x20F0)target=0x20F0;
        if(target<=x)return x;
        remember_tail(tail,0xD000);
        int velocity=(int16_t)actor_word(tail->object,0x10);
        actor_put(tail->object,0x10,velocity>0?velocity:0);
        actor_put(tail->object,0x36,velocity<=0);
        actor_put(tail->object,0x30,(unsigned)target);
        actor_put(tail->object,0x32,ram16(0xFE04));
    }
    int target=actor_word(tail->object,0x30);
    if(x>=target) {memset(tail,0,sizeof *tail);return x;}
    unsigned frame=ram16(0xFE04),dt=(uint16_t)(frame-actor_word(tail->object,0x32));
    int shown=actor_word(tail->object,8);
    if(dt) {
        uint8_t *p=tail->object;
        uint64_t position=((uint64_t)shown<<16)|actor_word(p,10);
        uint64_t limit=(uint64_t)target<<16;
        /* Marble's end terrain changes height beyond the native boundary.
         * Follow both original foot sensors, falling when support ends,
         * without moving the guest player or altering the results timing. */
        do {
            unsigned steps=zone==0x202 || actor_word(p,0x36)?1:dt;
            if(actor_word(p,0x36)) {
                int speed=actor_word(p,0x10);
                int maximum=ram16(0xF760),acceleration=ram16(0xF762);
                if(!maximum)maximum=0x600;
                if(!acceleration)acceleration=0xC;
                speed+=acceleration;if(speed>maximum)speed=maximum;
                actor_put(p,0x10,speed);
            }
            position+=(uint64_t)actor_word(p,0x10)*256*steps;
            if(position>limit)position=limit;
            if(position<(uint64_t)x<<16)position=(uint64_t)x<<16;
            shown=(int)(position>>16);
            if(zone==0x202) {
                int y=(int16_t)actor_word(p,12);
                if(p[0x22]&2) {
                    uint32_t yp=((uint32_t)actor_word(p,12)<<16)|actor_word(p,14);
                    yp+=(uint32_t)((int16_t)actor_word(p,0x12)*256);
                    y=(int16_t)(yp>>16);actor_put(p,14,yp);
                    actor_put(p,0x12,actor_word(p,0x12)+56);
                    int d=runoff_floor(p,shown,y);
                    if((int16_t)actor_word(p,0x12)>=0 && d<0) {
                        y+=d;p[0x22]&=(uint8_t)~2u;actor_put(p,0x12,0);
                    }
                } else {
                    int d=runoff_floor(p,shown,y);
                    if(d>=-14 && d<=14)y+=d;
                    else {p[0x22]|=2;actor_put(p,0x12,0);}
                }
                actor_put(p,12,(unsigned)y);
            }
            dt-=steps;
        } while(dt && position<limit);
        actor_put(p,8,(unsigned)shown);actor_put(p,10,(unsigned)position);
        actor_put(tail->object,0x32,frame);
    }
    if(zone==0x202)*display_y=(int16_t)actor_word(tail->object,12);
    return shown;
}
/* DLE_SBZ2 creates the floor at the native camera threshold. Its initial
 * eight unmoving blocks can be seen sooner in a wide view. Present their
 * original mapping/art until each actual block owns its display slot. */
static void scrap_floor_present(unsigned priority,int camera,int left)
{
    if(priority!=3 || !gameplay() || ram16(0xFE10)!=0x501 ||
       video_ram[0xF742]>4)return;
    unsigned map=scene_read32(0x19BBA),gfx=scene_read16(0x19BC2);
    s_capture_slot=0;s_capture_space=4;
    for(int i=0;i<8;++i) {
        int x=0x2010+i*32;
        if(x+16<=left || x-16>=left+s_requested_width)continue;
        unsigned o;
        for(o=0xD800;o<0xF000;o+=64)
            if(video_ram[o]==0x83 && video_ram[o+0x24]>=8 && ram16(o+8)==x)break;
        if(o<0xF000 && (video_ram[o+0x24]!=8 ||
           s_capture_frame->identity[1+(o-0xD000)/64]))continue;
        unsigned first=s_capture_frame->count;
        add_mapping(map,0,gfx,4,x-camera,0x5D0-ram16(0xF704),0,0);
        for(unsigned n=first;n<s_capture_frame->count;++n)s_capture_frame->sprites[n].asset=5;
    }
    /* The switch's PLC is also delayed until the native boss gate. */
    int x=0x2130;
    if(x+16<=left || x-16>=left+s_requested_width)return;
    for(unsigned o=0xD800;o<0xF000;o+=64)
        if(video_ram[o]==0x82 && scene_read32(0xFF0000u+o+4)==0xBEA4 &&
           s_capture_frame->identity[1+(o-0xD000)/64])return;
    unsigned first=s_capture_frame->count;
    add_mapping(0xBEA4,0,0x4A4,4,x-camera,0x5BC-ram16(0xF704),0,0);
    for(unsigned n=first;n<s_capture_frame->count;++n)s_capture_frame->sprites[n].asset=17;
}
static void final_escape_present(unsigned priority,int camera)
{
    if(s_requested_width<=320 || !gameplay() || ram16(0xFE10)!=0x502)return;
    SceneTail *cockpit=&s_actor_tails[0],*body=&s_actor_tails[1],*flame=&s_actor_tails[2];
    SceneTail *art=&s_actor_tails[3];
    unsigned parent=body->slot;
    if(priority==0) {
        unsigned frame=ram16(0xFE04);int advance=frame!=s_tail_frame;s_tail_frame=frame;
        for(unsigned o=0xD800;o<0xF000;o+=64) {
            if(video_ram[o]==0x85 && video_ram[o+0x24]==2 && video_ram[o+0x34]>=12) {
                remember_tail(body,o);parent=o;
            }
            if(video_ram[o]==0x3F && scene_read32(0xFF0000u+o+4))remember_tail(art,o);
        }
        if(!body->seen || parent<0xD800 || parent>=0xF000 || (parent&63) ||
           body->object[0]!=0x85 || body->object[0x24]!=2)return;
        for(unsigned o=0xD800;o<0xF000;o+=64) {
            if(video_ram[o]!=0x85 || (scene_read32(0xFF0000u+o+0x34)&65535u)!=parent)continue;
            if(video_ram[o+0x24]==8)remember_tail(cockpit,o);
            if(video_ram[o+0x24]==12)remember_tail(flame,o);
        }
        int alive=parent>=0xD800 && parent<0xF000 && video_ram[parent]==0x85 && video_ram[parent+0x24]==2;
        if(!alive && (video_ram[0xF600]&127)!=24) {
            for(unsigned n=0;n<12;++n)s_actor_tails[n].seen=s_actor_tails[n].tail=0;
            return;
        }
        int missing=cockpit->seen && (video_ram[cockpit->slot]!=0x85 || video_ram[cockpit->slot+0x24]!=8);
        if(alive && missing && !body->object[0x21] && advance) {
            uint8_t *a=cockpit->object;
            /* Ani_FZEgg: two damaged poses, four ticks each. */
            if((int8_t)--a[0x1E]<0){a[0x1E]=3;a[0x1A]^=1;}
            if(art->seen && !(video_ram[0xFE0F]&7)) {
                SceneTail *fx=&s_actor_tails[4+((frame>>3)&7)];
                memcpy(fx->object,art->object,64);fx->seen=fx->tail=1;fx->slot=0;
                /* Cosmetic continuation only: do not create guest objects,
                 * consume guest RNG or enqueue additional explosion audio. */
                unsigned seed=frame*1103515245u+ram16(0xF638);
                actor_put(fx->object,8,actor_word(body->object,8)+(int)((seed>>2)&63)-32);
                actor_put(fx->object,12,actor_word(body->object,12)+((seed>>11)&31));
                actor_put(fx->object,0x30,frame);
            }
        }
    }
    if(!body->seen || body->slot<0xD800 || body->slot>=0xF000 || (body->slot&63) ||
       body->object[0]!=0x85 || body->object[0x24]!=2)return;
    parent=body->slot;
    int alive=parent>=0xD800 && parent<0xF000 && video_ram[parent]==0x85 && video_ram[parent+0x24]==2;
    if(!alive && (video_ram[0xF600]&127)!=24)return;
    int x=(int16_t)actor_word(body->object,8),y=(int16_t)actor_word(body->object,12);
    for(unsigned n=0;n<3;++n) {
        SceneTail *tail=&s_actor_tails[n];uint8_t *a=tail->object;
        if(!tail->seen || a[0x18]!=priority)continue;
        if(video_ram[tail->slot]==0x85 && video_ram[tail->slot+0x24]==a[0x24])continue;
        /* Preserve the final complete craft through GM_Ending's palette
         * fade. The original parent deletion/Ending trigger stays intact. */
        unsigned slot=1+(parent-0xD000)/64;
        s_capture_slot=slot;s_capture_space=4;
        s_capture_frame->ox[slot]=(int16_t)x;s_capture_frame->oy[slot]=(int16_t)y;
        s_capture_frame->identity[slot]=0x85000000u|
            ((actor_word(body->object,4)<<16)|actor_word(body->object,6));
        add_mapping((actor_word(a,4)<<16)|actor_word(a,6),a[0x1A],actor_word(a,2),a[1],
                    x-camera,y-ram16(0xF704),0,0);
    }
    if(priority!=1)return;
    s_capture_slot=0;s_capture_space=4;
    for(unsigned n=4;n<12;++n) {
        SceneTail *fx=&s_actor_tails[n];if(!fx->seen)continue;
        uint8_t *a=fx->object;unsigned age=(uint16_t)(ram16(0xFE04)-actor_word(a,0x30));
        if(age>=40){fx->seen=fx->tail=0;continue;}
        add_mapping((actor_word(a,4)<<16)|actor_word(a,6),age/8,actor_word(a,2),a[1],
                    (int16_t)actor_word(a,8)-camera,(int16_t)actor_word(a,12)-ram16(0xF704),0,0);
    }
}
/* Read-only GHZ FindFloor/FindFloor2, including neighboring blocks and flips.
 * These continuation poses must not execute guest instructions or charge
 * cycles: the cutscene's native object list and timing remain authoritative. */
static int animal_height(int x,int y)
{
    unsigned chunk=video_ram[0xA400+(((unsigned)y>>8)&7)*128+(((unsigned)x>>8)&127)]&127;
    if(!chunk)return 0;
    unsigned a=(chunk-1)*512+(((unsigned)y&240)*2)+(((unsigned)x&240)/8);
    unsigned block=ram16(a),index=block&2047;
    if(!index || !(block&0x2000))return 0;
    index=scene_read8(scene_read32(0xFFF796)+index);if(!index)return 0;
    int h=(int8_t)scene_read8(0x62A00+index*16+((block&0x800?~x:x)&15));
    return block&0x1000?-h:h;
}
static int animal_floor2(int x,int y)
{
    int h=animal_height(x,y),low=y&15;
    if(!h || (h<0 && h+low>=0))return 15-low;
    return h<0?~low:15-low-h;
}
static int animal_floor(int x,int y)
{
    int h=animal_height(x,y),low=y&15;
    if(!h || (h<0 && h+low>=0))return animal_floor2(x,y+16)+16;
    if(h==16 || h<0)return animal_floor2(x,y-16)-16;
    return 15-low-h;
}
static void animal_move(uint8_t *p)
{
    for(unsigned a=8;a<=12;a+=4) {
        uint32_t pos=((uint32_t)actor_word(p,a)<<16)|actor_word(p,a+2);
        pos+=(uint32_t)((int16_t)actor_word(p,a==8?0x10:0x12)*256);
        actor_put(p,a,pos>>16);actor_put(p,a+2,pos);
    }
}
/* Anml movement branches, applied only after the native object is gone.
 * No collisions, queues, RNG, audio or guest object slots are introduced. */
static void animal_continue(uint8_t *p)
{
    unsigned routine=p[0x24],subtype=p[0x28];
    int delta=(int16_t)(ram16(0xD008)-actor_word(p,8)-184);
    if(routine==20 || routine==22) {
        if((uint16_t)(ram16(0xD008)-actor_word(p,8))>=184)return;
        routine=p[0x24]=14;
        actor_put(p,0x10,actor_word(p,0x32));actor_put(p,0x12,actor_word(p,0x34));
    } else if(routine==26) {
        if(delta>=0)return;
        routine=p[0x24]=4;
        actor_put(p,0x10,actor_word(p,0x32));actor_put(p,0x12,actor_word(p,0x34));
    } else if(routine>=24 && routine<=38 && delta>=0)return;
    int stationary=routine==24 || routine==28 || routine==32 || routine==36;
    if(stationary){actor_put(p,0x10,0);actor_put(p,0x32,0);}
    animal_move(p);
    int flying=routine==6 || routine==14 || routine==24 || routine==38;
    actor_put(p,0x12,actor_word(p,0x12)+(flying?24:56));
    int falling=(int16_t)actor_word(p,0x12)>=0;
    if(!flying)p[0x1A]=(uint8_t)!falling;
    if(falling) {
        int d=animal_floor((int16_t)actor_word(p,8),(int16_t)actor_word(p,12)+12);
        if(d<0) {
            actor_put(p,12,actor_word(p,12)+d);actor_put(p,0x12,actor_word(p,0x34));
            int turn=0;
            if(routine==38 || routine==40){p[0x29]^=255;turn=p[0x29]==0;}
            else if(routine==30 || routine==34 || (routine==14 && subtype && subtype!=10))turn=1;
            if(turn){actor_put(p,0x10,-(int16_t)actor_word(p,0x10));p[1]^=1;}
        }
    }
    if(flying && (int8_t)--p[0x1E]<0){p[0x1E]=1;p[0x1A]=(p[0x1A]+1)&1;}
    if(routine==24 || routine==28 || routine==32 || routine==36)
        p[1]=(uint8_t)((p[1]&~1u)|(actor_word(p,8)>=ram16(0xD008)));
}
static int capsule_animals(void)
{
    return gameplay() && !recorded_demo() && video_ram[0xFE11]==2 &&
        video_ram[0xFE10]<5 && video_ram[0xF7A7]==2;
}
/* Native floor-search initialization stays invisible. Ground only the
 * resting preview; native placements, falling and collisions still run. */
static int resting_preview_y(int x,int y,int height)
{
    int start=y;
    for(unsigned n=0;n<128 && y<2048;++n) {
        int distance=animal_floor(x,y+height);
        if(distance<0)return y+distance;
        y+=distance+1;
    }
    return start;
}
static void retire_capsule_animal(const SceneTail *tail)
{
    for(unsigned n=97;n<128;++n)if(!s_actor_tails[n].seen || !s_actor_tails[n].tail) {
        s_actor_tails[n]=*tail;
        return;
    }
}
/* The capsule waits for guest animals to be deleted. Keep that native
 * visibility/lifetime contract and continue only their margin graphics.
 * Retired poses have independent storage because guest slots are reusable. */
static void capsule_animals_present(unsigned priority,int camera,int left)
{
    if(s_requested_width<=320 || !capsule_animals())return;
    unsigned frame=ram16(0xFE04);int advance=frame!=s_tail_frame;
    if(priority==0)s_tail_frame=frame;
    if(priority==0)for(unsigned n=1;n<=96;++n) {
        unsigned o=0xD800+(n-1)*64;SceneTail *tail=&s_actor_tails[n];
        int live=video_ram[o]==0x28 && !video_ram[o+0x28] && video_ram[o+0x24];
        int replaced=live && tail->seen &&
            (abs((int16_t)(ram16(o+8)-actor_word(tail->object,8)))>32 ||
             abs((int16_t)(ram16(o+12)-actor_word(tail->object,12)))>32);
        if(tail->seen && tail->tail && tail->object[0]==0x28 && (!live || replaced)) {
            retire_capsule_animal(tail);
            memset(tail,0,sizeof *tail);
        }
        if(live) {
            if(!tail->seen)tail->generation=((s_scene_tick<<7)|n)&0xFFFFFFu;
            remember_tail(tail,o);
        }
    }
    for(unsigned n=97;n<128;++n) {
        SceneTail *tail=&s_actor_tails[n];
        if(!tail->seen || !tail->tail || tail->object[0]!=0x28)continue;
        if(priority==0 && advance) {
            uint8_t *a=tail->object;
            if(a[0x24]==18) {
                unsigned wait=actor_word(a,0x36);
                if(wait)actor_put(a,0x36,--wait);
                if(!wait){a[0x24]=2;a[0x18]=3;}
            } else if(a[0x24]==2) {
                animal_move(a);actor_put(a,0x12,actor_word(a,0x12)+56);
                int d=animal_floor((int16_t)actor_word(a,8),(int16_t)actor_word(a,12)+12);
                if((int16_t)actor_word(a,0x12)>=0 && d<0) {
                    actor_put(a,12,actor_word(a,12)+d);
                    actor_put(a,0x10,actor_word(a,0x32));actor_put(a,0x12,actor_word(a,0x34));
                    a[0x1A]=1;a[0x24]=(uint8_t)(4+2*a[0x30]);
                    if(video_ram[0xFE05]&16){actor_put(a,0x10,-(int16_t)actor_word(a,0x10));a[1]^=1;}
                }
            } else animal_continue(a);
        }
        uint8_t *a=tail->object;int x=(int16_t)actor_word(a,8),y=(int16_t)actor_word(a,12)-ram16(0xF704);
        if(x+32<left || x-32>=left+s_requested_width || y+32<0 || y-32>=224) {
            tail->tail=0;continue;
        }
        if(a[0x18]!=priority)continue;
        /* Stable host slots let interpolation follow the continuation even
         * after its native slot is reused by another released animal. */
        s_capture_slot=32+n;s_capture_space=4;
        s_capture_frame->ox[s_capture_slot]=(int16_t)x;
        s_capture_frame->oy[s_capture_slot]=(int16_t)actor_word(a,12);
        s_capture_frame->identity[s_capture_slot]=0x28000000u|(tail->generation&0xFFFFFFu);
        unsigned first=s_capture_frame->count;
        add_mapping(((uint32_t)actor_word(a,4)<<16)|actor_word(a,6),a[0x1A],actor_word(a,2),a[1],x-camera,y,0,0);
        if(a[0x30]<7)for(unsigned k=first;k<s_capture_frame->count;++k)
            s_capture_frame->sprites[k].asset=(uint8_t)(10+a[0x30]);
    }
}
/* Preview resting poses before original activation; continue only the visible
 * tail after native deletion. The native scene itself remains unchanged. */
static void ending_animals_present(unsigned priority,int camera,int left)
{
    if(priority!=6 || s_requested_width<=320 || !gameplay() ||
       (video_ram[0xF600]&127)!=24 || video_ram[0xFE10]!=6)return;
    load_placements();s_capture_slot=0;s_capture_space=4;
    unsigned frame=ram16(0xFE04);int advance=frame!=s_tail_frame;
    s_tail_frame=frame;
    for(unsigned i=0;i<s_placement_count && i<128;++i) {
        Placement *p=&s_placements[i];
        if(p->id!=0x28 || p->subtype<10 || p->subtype>20)continue;
        SceneTail *animal=&s_actor_tails[i];
        unsigned o=animal->slot;
        if(!animal->seen)o=placement_object(p);
        if(o && video_ram[o]==0x28 && video_ram[o+0x28]==p->subtype &&
           (!animal->seen || abs((int16_t)(ram16(o+8)-actor_word(animal->object,8)))<32)) {
            memcpy(animal->object,video_ram+o,64);animal->slot=(uint16_t)o;
            animal->seen=1;animal->tail=1;p->loaded=1;continue;
        }
        if(animal->seen && animal->tail) {
            if(video_ram[0xD000]==0x87 && video_ram[0xD025]>=8){animal->tail=0;continue;}
            if(advance)animal_continue(animal->object);
            uint8_t *a=animal->object;int x=(int16_t)actor_word(a,8),y=(int16_t)actor_word(a,12)-ram16(0xF704);
            if(x+32<left || x-32>=left+s_requested_width || y+32<0 || y-32>=224){animal->tail=0;continue;}
            add_mapping(((uint32_t)actor_word(a,4)<<16)|actor_word(a,6),a[0x1A],actor_word(a,2),a[1],
                        x-camera,y,0,0);continue;
        }
        if(p->loaded || video_ram[0xD000]!=1 || p->x+24<left ||
           (int)p->x-24>=left+s_requested_width)continue;
        unsigned type=p->subtype-10;
        add_mapping(scene_read32(0x8FCA+type*4),0,scene_read16(0x8FF6+type*2),5,
                    (int)p->x-camera,(p->y&4095)-ram16(0xF704),0,0);
    }
}
static void spawn_scene(void)
{
    load_placements(); adopt_rings(); s_loader_active=1;
    int camera=ram16(0xF700), w=enabled()?s_requested_width:320;
    int lo,hi;activation_bounds(camera,w,&lo,&hi);
    int stair_lo,stair_hi;activation_bounds(camera,320,&stair_lo,&stair_hi);
    int player=ram16(0xD008);
    s_pool_pressure=0;
    for (unsigned i=0;i<s_placement_count;++i) {
        Placement *p=&s_placements[i];
        int begin=p->id==0x5B?stair_lo:lo,end=p->id==0x5B?stair_hi:hi;
        if ((int)p->x<begin || (int)p->x>=end) p->loaded=0;
        else if (!p->loaded && placement_alive(p)) p->loaded=1;
    }
    /* Fill closest to the player first, reserving space for collision-relevant
     * objects before remote scenery. Host-owned rings consume no object slots. */
    for (unsigned pass=0;pass<s_placement_count;++pass) {
        int best=-1, distance=INT_MAX;
        for (unsigned i=0;i<s_placement_count;++i) {
            Placement *p=&s_placements[i];
            int begin=p->id==0x5B?stair_lo:lo,end=p->id==0x5B?stair_hi:hi;
            if (p->loaded || (enabled() && p->id==0x25) || (int)p->x<begin || (int)p->x>=end) continue;
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
/* Card departure deliberately retains native activation. Present initial
 * placements visible in the wider view until their original objects queue
 * sprites; no early collision objects or ring collection are introduced. */
static void entry_placements_present(unsigned priority,int camera,int left)
{
    if(!gameplay() || results_world() || video_ram[0xD5C0]==0x3A || s_requested_width<=320 ||
       (priority<2 || priority>5))return;
    int entry=(video_ram[0xF600]&128)!=0;
    for(unsigned o=0xD080;o<0xD200;o+=64)if(video_ram[o]==0x34)entry=1;
    load_placements();s_capture_slot=0;s_capture_space=4;
    for(unsigned i=0;i<s_placement_count;++i) {
        const Placement *p=&s_placements[i];
        if(priority==3 && (p->id==0x1F || p->id==0x22)) {
            if(p->x+32<left || (int)p->x-32>=left+s_requested_width)continue;
            unsigned o=placement_object(p);
            if(!entry && (!o || video_ram[o+0x24]))continue;
            if(p->state && !o && (video_ram[0xFC02+p->state]&128))continue;
            if(o && s_capture_frame->identity[1+(o-0xD000)/64])continue;
            unsigned map=p->id==0x1F?0x9750:0x9A36,gfx=p->id==0x1F?0x400:0x444;
            unsigned frame=p->id==0x22?(video_ram[0xFE0F]>>1)&1:0;
            unsigned flags=(p->y>>14)&3;int x=p->x,y=p->y&4095;
            if(o && video_ram[o+0x24] && scene_read32(0xFF0000u+o+4)) {
                map=scene_read32(0xFF0000u+o+4);gfx=ram16(o+2);
                frame=video_ram[o+0x1A];flags=video_ram[o+1];
                x=ram16(o+8);y=ram16(o+12);
            }
            add_mapping(map,frame,gfx,flags,x-camera,y-ram16(0xF704),0,0);
        } else if(entry && p->id==0x25 && priority==2) {
            int count=(p->subtype&7)+1;if(count>7)count=7;
            for(int n=0;n<count;++n) {
                if(p->state && (video_ram[0xFC02+p->state]&(1u<<n)))continue;
                int x,y;ring_position(p,n,&x,&y);
                if(x+16<left || x-16>=left+s_requested_width)continue;
                unsigned o;
                for(o=0xD800;o<0xF000;o+=64)
                    if(video_ram[o]==0x25 && ram16(o+8)==x && ram16(o+12)==y &&
                       s_capture_frame->identity[1+(o-0xD000)/64])break;
                if(o<0xF000)continue;
                add_mapping(0x9F92,video_ram[0xFEC3]&3,0x27B2,0,x-camera,y-ram16(0xF704),0,0);
            }
        } else if(entry && p->id==0x78 && (priority==4 || priority==5)) {
            if(p->x+48<left || (int)p->x-48>=left+s_requested_width)continue;
            unsigned o=placement_object(p);
            if(p->state && !o && (video_ram[0xFC02+p->state]&128))continue;
            int x=o?ram16(o+8):p->x,y=o?ram16(o+12):(p->y&4095);
            unsigned flags=(p->y>>14)&3;
            if(o && video_ram[o+0x24]) {
                /* Body slots already belong to the initialized native head. */
                if(priority!=4 || s_capture_frame->identity[1+(o-0xD000)/64])continue;
                add_mapping(scene_read32(0xFF0000u+o+4),video_ram[o+0x1A],
                            ram16(o+2),video_ram[o+1],x-camera,y-ram16(0xF704),0,0);
            } else {
                /* Cat_Main waits for floor contact before allocating its
                 * three segments. Keep their resting preview grounded while
                 * that invisible initialization runs. */
                y=resting_preview_y(x,y,7);
                unsigned gfx=video_ram[0xFE10]==5?0x22B0:0x24FF;
                if(priority==4)add_mapping(0x16D6E,0,gfx,flags,x-camera,y-ram16(0xF704),0,0);
                else for(int n=1;n<=3;++n)
                    add_mapping(0x16D6E,8,gfx,flags,x+(flags&1?-12:12)*n-camera,
                                y-ram16(0xF704),0,0);
            }
        } else if((entry || recorded_demo()) && video_ram[0xFE10]==0 &&
                  priority==4 && p->id==0x40) {
            if(p->x+32<left || (int)p->x-32>=left+s_requested_width)continue;
            unsigned o=placement_object(p);
            if(p->state && !o && (video_ram[0xFC02+p->state]&128))continue;
            if(o && s_capture_frame->identity[1+(o-0xD000)/64])continue;
            if(o && video_ram[o+0x24])continue;
            int y=resting_preview_y(p->x,p->y&4095,14);
            add_mapping(scene_read32(0xF632),0,scene_read16(0xF63A),
                        (p->y>>14)&3,(int)p->x-camera,y-ram16(0xF704),0,0);
        } else if(entry && video_ram[0xFE10]==0 && priority==4 &&
                  (p->id==0x3B || (p->id==0x41 && !(p->subtype&0x30)))) {
            if(p->x+32<left || (int)p->x-32>=left+s_requested_width)continue;
            unsigned o=placement_object(p);
            if(o && s_capture_frame->identity[1+(o-0xD000)/64])continue;
            unsigned init=p->id==0x3B?0xD062:0xDB08;
            unsigned map=scene_read32(init+6),gfx=scene_read16(init+14);
            if(p->id==0x41 && (p->subtype&2))gfx|=0x2000;
            add_mapping(map,0,gfx,(p->y>>14)&3,(int)p->x-camera,
                        (p->y&4095)-ram16(0xF704),0,0);
        }
    }
}
/* Keep resettable stairs on native activation/lifetime bounds. Their resting
 * cubes can still be shown in a wide margin before the native group exists. */
static void starlight_stairs_present(unsigned priority,int camera,int left)
{
    if(priority!=3 || !gameplay() || video_ram[0xFE10]!=3 || s_requested_width<=320)return;
    load_placements();s_capture_slot=0;s_capture_space=4;
    for(unsigned i=0;i<s_placement_count;++i) {
        const Placement *p=&s_placements[i];
        if(p->id!=0x5B || p->x+112<left || (int)p->x-16>=left+s_requested_width)continue;
        unsigned o;
        for(o=0xD800;o<0xF000;o+=64)
            if(video_ram[o]==0x5B && video_ram[o+0x24] && ram16(o+0x30)==p->x)break;
        if(o<0xF000)continue;
        for(unsigned n=0;n<4;++n)
            add_mapping(0x1103C,0,0x4000,0,(int)p->x+(int)n*32-camera,
                        (p->y&4095)-ram16(0xF704),0,0);
    }
}
static void capture_objects(void)
{
    /* GotThroughAct selects the next zone before fading this one. Keep the
     * outgoing background's interpretation until its results card is gone. */
    int retain=s_capture_frame->scene==1 && video_ram[0xD5C0]==0x3A;
    if(!retain)s_capture_frame->zone=ram16(0xFE10);
    if(!retain || s_capture_frame->zone==ram16(0xFE10)) {
        s_capture_frame->background_y=(int16_t)ram16(0xFF1C);
        for(unsigned y=0;y<224;++y)
            s_capture_frame->background_scroll[y]=(int16_t)ram16(0xCC02+y*4);
    }
    s_capture_frame->count=0;s_capture_slot=s_capture_space=0;
    memset(s_capture_frame->identity,0,sizeof s_capture_frame->identity);
    if(!s_capture_readonly){s_visible_count=0;++s_scene_tick;}
    if(!s_capture_readonly && ram16(0xFE10)!=0x502 &&
       s_actor_tails[1].seen && s_actor_tails[1].object[0]==0x85) {
        memset(s_actor_tails,0,sizeof s_actor_tails);s_tail_frame=0;
    }
    s_capture_frame->scene=gameplay()?1:video_ram[0xD000]==9?2:0;
    int cam=ram16(0xF700), level=gameplay();
    if(level) {
        s_capture_frame->identity[0]=SCENE_CAMERA_TAG;
        s_capture_frame->ox[0]=(int16_t)cam;
        s_capture_frame->oy[0]=(int16_t)ram16(0xF704);
    }
    int left=level?scene_left(video_ram,cam,s_requested_width):cam-(s_requested_width-320)/2;
    int guest_left=level?view_left(cam,s_requested_width):left;
    int runoff_y=(int16_t)ram16(0xD00C),runoff_x=act_runoff_x(&runoff_y);
    for(unsigned pri=0;pri<8;++pri) {
        unsigned queue=0xAC00+pri*128, bytes=ram16(queue);
        if(bytes>126)bytes=126;
        unsigned objects[63+128],count=0,native_count=bytes/2;
        for(unsigned k=2;k<=bytes;k+=2)objects[count++]=ram16(queue+k);
        if(!s_capture_readonly)for(unsigned n=0;n<s_overflow_count;++n) {
            unsigned o=s_overflow_objects[n];
            if((video_ram[o+0x18]&7)!=pri)continue;
            unsigned k;for(k=0;k<count && objects[k]!=o;++k){}
            if(k==count)objects[count++]=o;
        }
        for(unsigned k=0;k<count;++k) {
            unsigned o=objects[k];
            if(o<0xD000 || o>=0xF000 || !video_ram[o])continue;
            unsigned flags=video_ram[o+1], space=flags&12;
            int pylon=!s_capture_readonly && level &&
                video_ram[o]==0x5C && !space;
            s_capture_slot=1+(o-0xD000)/0x40;s_capture_space=space;
            int object_x=o==0xD000?runoff_x:ram16(o+8);
            int object_y=o==0xD000?runoff_y:ram16(o+12);
            s_capture_frame->ox[s_capture_slot]=(int16_t)object_x;
            s_capture_frame->oy[s_capture_slot]=(int16_t)object_y;
            s_capture_frame->identity[s_capture_slot]=((uint32_t)video_ram[o]<<24)|scene_read32(0xFF0000u+o+4);
            if(!s_capture_readonly && s_requested_width>320 && o>=0xD800 &&
               video_ram[o]==0x28 && !video_ram[o+0x28] && capsule_animals()) {
                const SceneTail *tail=&s_actor_tails[1+(o-0xD800)/64];
                if(tail->seen)s_capture_frame->identity[s_capture_slot]=0x28000000u|tail->generation;
            }
            int x,y,hud=video_ram[o]==0x21?1:space?0:2;
            if(space) {
                unsigned scroll=space==4?0xF700:space==8?0xF708:0xF718;
                x=(int16_t)(object_x-ram16(scroll));
                y=(int16_t)(object_y-ram16(scroll+4));
            } else {
                x=(int16_t)ram16(o+8)-128;y=(int16_t)ram16(o+10)-128;
                if(pylon) {
                    /* Pyl_Display can precede camera deformation. Derive
                     * its 2x phase from this displayed view, rather than
                     * mixing the old object X with the updated camera. */
                    int view_camera=left+(s_requested_width-320)/2;
                    int carry=view_camera==cam && scene_left(video_ram,cam+1,s_requested_width)!=left?
                        ram16(0xF702)>>15:0;
                    x=((-cam-view_camera-carry)&511)-128;hud=0;s_capture_space=4;
                    s_capture_frame->ox[s_capture_slot]=(int16_t)(cam+x);
                    s_capture_frame->oy[s_capture_slot]=(int16_t)(ram16(0xF704)+y);
                }
                if(video_ram[o]==0x34 || video_ram[o]==0x3A || video_ram[o]==0x7E) {
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
            unsigned mapped_end=s_capture_frame->count;
            if(pylon)for(unsigned n=first;n<mapped_end;++n) {
                const SceneSprite source=s_capture_frame->sprites[n];
                int lo=left-cam,hi=lo+s_requested_width;
                int cell_width=(((source.size>>2)&3)+1)*8;
                for(int direction=-1;direction<=1;direction+=2) {
                    SceneSprite fill=source;
                    for(fill.x+=direction*512;fill.x+cell_width>lo && fill.x<hi;
                        fill.x+=direction*512)
                        if(s_capture_frame->count<SCENE_SPRITES)
                            s_capture_frame->sprites[s_capture_frame->count++]=fill;
                }
            }
            if(!s_capture_readonly && s_requested_width>320 && level &&
               video_ram[o]==0x1B && scene_read32(0xFF0000u+o+4)==0x1116C &&
               video_ram[o+0x1A]<6) {
                /* Surf's 192px mapping repeats its live animation into the
                 * margins. Keep all original native-window pieces and use
                 * only their footprints when publishing guest visibility. */
                int lo=left-cam,hi=lo+s_requested_width;
                for(unsigned n=first;n<mapped_end;++n) {
                    const SceneSprite source=s_capture_frame->sprites[n];
                    int cell_width=(((source.size>>2)&3)+1)*8;
                    for(int direction=-1;direction<=1;direction+=2) {
                        SceneSprite fill=source;
                        for(fill.x+=direction*192;fill.x+cell_width>lo && fill.x<hi;
                            fill.x+=direction*192) {
                            if(fill.x<320 && fill.x+cell_width>0)continue;
                            if(s_capture_frame->count<SCENE_SPRITES)
                                s_capture_frame->sprites[s_capture_frame->count++]=fill;
                        }
                    }
                }
            }
            if(!s_capture_readonly && s_requested_width>320 && level &&
               video_ram[0xFE10]==2 && video_ram[o]==0x4E && video_ram[o+0x1A]==4) {
                /* The lava wall's rectangular tail only covers a native
                 * screen. Repeat its leftmost fill column through the margin,
                 * preserving the animated leading edge and guest hit box. */
                unsigned last=s_capture_frame->count;int edge=INT_MAX;
                for(unsigned n=first;n<last;++n)
                    if(s_capture_frame->sprites[n].x<edge)edge=s_capture_frame->sprites[n].x;
                for(unsigned n=first;n<last && edge<left-cam+s_requested_width;++n) {
                    SceneSprite fill=s_capture_frame->sprites[n];if(fill.x!=edge)continue;
                    int step=(((fill.size>>2)&3)+1)*8;
                    for(fill.x-=step;fill.x+step>left-cam && s_capture_frame->count<SCENE_SPRITES;fill.x-=step)
                        s_capture_frame->sprites[s_capture_frame->count++]=fill;
                }
            }
            if(space) {
                int visible=0,guest_visible=0,lo=left-cam,hi=lo+s_requested_width;
                int guest_lo=guest_left-cam,guest_hi=guest_lo+s_requested_width;
                /* Composite actors do not consistently populate obActWid
                 * (Final Zone's empty Eggmobile has zero). Cull their actual
                 * mapped pieces, rather than popping the whole actor when
                 * its reference point finally crosses the screen edge. */
                for(unsigned n=first;n<s_capture_frame->count;++n) {
                    const SceneSprite *p=&s_capture_frame->sprites[n];
                    int w=(((p->size>>2)&3)+1)*8;
                    if(p->x+w>lo && p->x<hi)visible=1;
                    int guest_x=p->x+(int)ram16(o+8)-object_x;
                    if(n<mapped_end && guest_x+w>guest_lo && guest_x<guest_hi)guest_visible=1;
                }
                /* Presentation-only reframing retains the existing guest
                 * visibility flags, lifetimes and event timing. */
                if(guest_visible && k<native_count && !s_capture_readonly && s_visible_count<128 &&
                   !(video_ram[o]==0x28 && capsule_animals()))s_visible_objects[s_visible_count++]=o;
                if(!visible)s_capture_frame->count=first;
            }
        }
        if(!s_capture_readonly) {
            /* The result card can select the next act before fading the
             * old layout. Retain its live actors, not next-act placements. */
            if(!results_world() && video_ram[0xD5C0]!=0x3A) {
                marble_present(pri,cam,left);
                springyard_present(pri,cam,left);
                starlight_stairs_present(pri,cam,left);
            }
            final_cylinders_present(pri,cam,left);
            labyrinth_boss_present(pri,cam,left);
            stationary_boss_present(pri,cam,left);
            scrap_floor_present(pri,cam,left);
            final_escape_present(pri,cam);
            initial_orbinauts_present(pri,cam);
            ending_animals_present(pri,cam,left);
            capsule_animals_present(pri,cam,left);
            entry_placements_present(pri,cam,left);
        }
        /* The outgoing level still owns adopted rings after activation is
         * frozen for a Title/SEGA fade. Draw them without resuming guest work. */
        int outgoing=(video_ram[0xF600]&127)==0 || (video_ram[0xF600]&127)==4;
        if(!s_capture_readonly && pri==2 && gameplay() &&
           (!native_activation() || (s_rings_adopted && outgoing && !recorded_demo()))) {
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
    if(!s_capture_readonly)s_overflow_count=0;
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
    dst->zone=s_build.zone;dst->background_y=s_build.background_y;
    memcpy(dst->background_scroll,s_build.background_scroll,sizeof dst->background_scroll);
    memcpy(dst->ox,s_build.ox,sizeof dst->ox);memcpy(dst->oy,s_build.oy,sizeof dst->oy);memcpy(dst->identity,s_build.identity,sizeof dst->identity);
    memcpy(dst->sat,s_build.sat,640);
    memcpy(dst->sprites,s_build.sprites,s_build.count*sizeof(SceneSprite));
}
int s1_video_hook(uint32_t pc)
{
    if(pc==0xD604 || pc==0xD622) {
        /* DisplaySprite drops submissions after 63 objects per priority.
         * Retain only those real draw attempts for the wider host pass;
         * never promote their guest visibility or synthesize dead actors. */
        if(enabled() && s_requested_width>320 && gameplay() && !native_activation()) {
            unsigned o=g_cpu.A[pc==0xD604?0:1]&65535u;
            if(o>=0xD000 && o<0xF000 && !(o&63) && video_ram[o] &&
               ram16(0xAC00+(video_ram[o+0x18]&7)*128)>=126) {
                unsigned n;for(n=0;n<s_overflow_count && s_overflow_objects[n]!=o;++n){}
                if(n==s_overflow_count && n<128)s_overflow_objects[s_overflow_count++]=(uint16_t)o;
            }
        }
        return 0;
    }
    if(s_interpolation_enabled && !enabled()) {
        if(pc==0xD65E) {
            s_capture_frame=&s_native_build;s_capture_readonly=1;capture_objects();
            s_capture_frame=&s_build;s_capture_readonly=0;
        } else if(pc==0xD746 || pc==0xD74E)native_publish();
    }
    if(pc==0xD8DA && !video_ram[0xF76C]) {
        s_overflow_count=0;
        s_placement_base=s_placement_count=0;s_loader_active=s_rings_adopted=0;
        memset(s_sparks,0,sizeof s_sparks);
        memset(s_actor_tails,0,sizeof s_actor_tails);s_tail_frame=0;
        s_build.scene=s_native_build.scene=0;
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
    case 0xF200:case 0x1165E: {
        /* LavaTag/Seesaw SUB precedes a private BMI as well as the range
         * CMP/BHI. Adjust its input, then let the original SUB set flags.
         * Patching only the later CMP cannot retain left-margin hazards. */
        if(gameplay()) {
            int lo,hi;activation_bounds(ram16(0xF700),s_requested_width,&lo,&hi);
            int x=(int)(g_cpu.D[0]&65535u);
            unsigned outside=x<lo || x>=hi;
            unsigned input=(g_cpu.D[1]+(outside?641u:320u))&65535u;
            g_cpu.D[0]=(g_cpu.D[0]&0xFFFF0000u)|input;
        }
        return 0;
    }
    case 0x10EB4:
        /* Staircase lowering resets on the original unload path. A larger
         * lifetime window can prevent that reset even after it leaves view. */
        return 0;
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
        s_display_frame.zone=s_display->zone;s_display_frame.background_y=s_display->background_y;
        memcpy(s_display_frame.background_scroll,s_display->background_scroll,sizeof s_display_frame.background_scroll);
        s_display_frame.serial=s_display->serial;
        memcpy(s_display_frame.ox,s_display->ox,sizeof s_display_frame.ox);
        memcpy(s_display_frame.oy,s_display->oy,sizeof s_display_frame.oy);
        memcpy(s_display_frame.identity,s_display->identity,sizeof s_display_frame.identity);
        memcpy(s_display_frame.sprites,s_display->sprites,s_display->count*sizeof(SceneSprite));
        s_display=&s_display_frame;
    }
}
/* REV00 FadeIn_AddColour fills blue, green, then red. Infer the displayed
 * step from a white entry and verify the whole live palette, including the
 * separate underwater target. This follows raster timing without a second
 * fade clock or writes to the original palette. */
static uint16_t fade_from_black(uint16_t colour,int steps)
{
    uint16_t result=0;
    for(int shift=9;shift>=1;shift-=4) {
        int channel=(colour>>shift)&7,amount=steps<channel?steps:channel;
        result|=(uint16_t)(amount<<shift);steps-=amount;
    }
    return result;
}
static int world_fade_steps(const GVDP *v,int width)
{
    unsigned range=ram16(0xF626);
    if(width<=320 || !(video_ram[0xF600]&128) ||
       (range!=0x202F && range!=0x003F))return -1;
    unsigned black=16;
    while(black<64 && !(v->cram[black]&0xEEE))++black;
    if(black==64)return 0;
    const unsigned targets[]={0xFB80,0xFA00};
    for(unsigned n=0;n<(video_ram[0xFE10]==1?2u:1u);++n) {
        int steps=-1;
        for(unsigned i=16;i<64;++i)if((ram16(targets[n]+i*2)&0xEEE)==0xEEE) {
            unsigned white=v->cram[i]&0xEEE;
            steps=((white>>9)&7)+((white>>5)&7)+((white>>1)&7);
            if(fade_from_black(0xEEE,steps)!=white)steps=-1;
            break;
        }
        if(steps<0)continue;
        unsigned i;
        for(i=16;i<64;++i)
            if(fade_from_black(ram16(targets[n]+i*2),steps)!=(v->cram[i]&0xEEE))break;
        /* Before PalFadeIn_Alt selects $202F, the stage palettes are black
         * while Sonic/HUD have already loaded palette0. Hide world art in
         * that frozen loading interval, without double-fading a full fade. */
        if(i==64 && (range==0x202F || steps==0))return steps;
    }
    return -1;
}
static void draw_scene_sprites(const GVDP *v,int line,uint32_t *out,int width,
                               int origin,const uint32_t *palette,const uint32_t *world_palette)
{
    if(!s_display)return;
    for(unsigned n=0;n<s_display->count;++n) {
        const SceneSprite *s=&s_display->sprites[n];
        /* A title-card list can contain initialized world sprites before
         * the level planes reach VBlank. Their world origin is not installed
         * yet; wait for that scene instead of centering them with the card. */
        if(width>320 && s_display->scene==3 && s->space)continue;
        int fade_world=world_palette && (s->space || s->hud==1);
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
            if(s->hud==1 || (s->attr&0x8000) || !(s_priority[x]&1)) {
                /* Boss hits flash a shared black ink entry. The capsule is
                 * normally outside that camera; keep its ink black when an
                 * expanded view reveals it during the fight. */
                out[x]=s->asset==6 && p==17 && v->cram[17]==0xEEE?
                    genesis_dac_cram_to_argb(0,GENESIS_DAC_NORMAL):
                    fade_world && p<16?world_palette[p]:palette[p];
            }
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
/* Coverage of the original SAT, including fetch limits and its X=0 mask. Keep native
 * overlay pixels while replacing the title's incomplete cached water layer. */
static void title_sprite_coverage(const GVDP *v,int line,uint8_t *keep)
{
    memset(keep,0,320);
    unsigned base=(v->reg[5]&127u)<<9,link=0,on_line=0,pixels=0;
    for(unsigned n=0;n<80;++n) {
        unsigned e=base+link*8,size=v->vram[(e+2)&65535u];
        int y=(vram16(v,e)&1023)-128,cw=((size>>2)&3)+1,ch=(size&3)+1;
        if(line>=y && line<y+ch*8) {
            unsigned xword=vram16(v,e+6)&1023u;
            if(++on_line>20 || pixels>=320 || (!xword && on_line>1))break;
            /* Offscreen pieces consume the same fetch width as native VDP
             * rendering. They deliberately hide the title's lower torso. */
            pixels+=(unsigned)cw*8;
            unsigned attr=vram16(v,e+4);int iy=line-y;
            if(attr&0x1000)iy=ch*8-1-iy;
            for(int i=0;i<cw*8;++i) {
                int x=(int)xword-128+i,ix=(attr&0x800)?cw*8-1-i:i;
                if(x<0 || x>=320)continue;
                unsigned cell=(attr&0xE000u)|(((attr&2047u)+(unsigned)(ix/8*ch+iy/8))&2047u);
                if(pattern_pixel(v,(uint16_t)cell,ix,iy))keep[x]=1;
            }
        }
        link=v->vram[(e+3)&65535u]&127u;
        if(!link || link>=80)break;
    }
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
static int background_parallax(int camera,int y,int zone)
{
    /* Reproject REV00 deformation through the clamped wide camera while
     * retaining each live row's accumulated scroll offset. */
    switch(zone>>8) {
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
    /* Skipping a demo selects Title before its last deformation call, which
     * zeroes A scroll. Keep the camera that owns the installed sprite list. */
    if(enabled() && departing_demo(s_display,video_ram))hs_a=-s_display->ox[0];
    hs_a=interpolate_scroll(line,0,hs_a);hs_b=interpolate_scroll(line,1,hs_b);
    /* SS_Load installs the repeating background before the first object pass
     * publishes a special-stage sprite list. Its original white-in already
     * exposes those planes; extend them during that interval too. */
    int level=gameplay(),special=(s_display && s_display->scene==2) ||
        ((video_ram[0xF600]&127)==16 && video_ram[0xD000]==9);
    /* The next player object can exist while the title-card SAT is still
     * displayed. Do not expose the incoming level before its own display
     * list/planes reach VBlank. */
    if(s_title_world || (s_display && s_display->scene!=1))level=0;
    int world_steps=level || (s_display && s_display->scene==3)?world_fade_steps(v,width):-1;
    uint32_t world_palette[16];
    if(world_steps>=0)for(unsigned i=0;i<16;++i)
        world_palette[i]=genesis_dac_cram_to_argb(fade_from_black(v->cram[i],world_steps),GENESIS_DAC_NORMAL);
    int camera=level?unwrap(-hs_a,ram16(0xFF10)):ram16(0xF700);
    int camera_y=unwrap(interpolate_vs(line,0,v->vsram[0]),ram16(0xFF14));
    if(line==0) {
        if(level)s_stage_width=stage_width();
        s_camera=camera;if(!s_interpolating)++s_frames;
        s_left=level?scene_left(video_ram,camera,width):camera-(width-nw)/2;
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
    int zone=level && s_display?s_display->zone:ram16(0xFE10);
    if(level && s_display && zone!=ram16(0xFE10)) {
        by=s_display->background_y+line;
        if(line>=0 && line<224)hs_b=s_display->background_scroll[line];
    }
    int bg_width=0;
    if(bg_world) {
        int row=((by>>8)&7)*128;
        for(int col=0;col<64;++col)if(video_ram[0xA440+row+col])bg_width=(col+1)*256;
    }
    /* The title's 512px nametable is a streaming cache. Compose water as one
     * continuous layer across the center and margins; copying cached water
     * back over its middle reintroduces a moving seam at the old boundary. */
    int title_water=s_title_world && width>320 && by>=152;
    uint8_t title_keep[320];
    if(title_water)title_sprite_coverage(v,line,title_keep);
    int bg_base=-origin-hs_b;
    if(level) {
        int margin=(width-nw)/2;
        int correction=background_parallax(s_left+margin,by,zone)-background_parallax(camera,by,zone);
        bg_base=-margin-hs_b+interpolate_background_correction(line,margin,correction);
    }
    int background_x=bg_base;
    if(bg_width>0) {
        background_x%=bg_width;
        if(background_x<0)background_x+=bg_width;
    }
    GenesisTileRow fg = {0}, bg = {0};
    /* REV00's two vertical curtains end at finite layout columns. Extend
     * those curtains while visible, using their complete 32px repeat.
     * Horizontal pools elsewhere must retain their actual empty surroundings. */
    int curtain=level && width>320?(zone==0x200?0x900:zone==0x201?0x100:-1):-1;
    int curtain_width=zone==0x200?0xC0:0x100;
    if(curtain>=0 && (camera<curtain || s_left>=curtain+curtain_width ||
                     s_left+width<=curtain))curtain=-1;
    for(int x=0;x<width;++x) {
        int nx=x-origin,wx=level?s_left+x:nx-hs_a;
        int reference_bx=nx-hs_b,bx=bg_base+x;
        if (!fg.remaining) {
            uint16_t attr=level?world_attr(wx,wy,0):plane_attr(v,base_a,nx-hs_a,line+(v->vsram[0]&1023));
            if(curtain>=0 && wx<curtain && !(attr&2047u)) {
                uint16_t fill=world_attr(curtain+(wx&31),wy,0);
                if((fill&2047u)>=0x2D2 && (fill&2047u)<0x2E2)attr=fill;
            }
            gtile_row_begin(&fg,v->vram,attr,level?wx:nx-hs_a,level?wy:line+(v->vsram[0]&1023));
        }
        if (!bg.remaining) {
            uint16_t attr=bg_world?world_attr(background_x,by,1):plane_attr(v,base_b,bx,by);
            gtile_row_begin(&bg,v->vram,attr,bg_world?background_x:bx,by);
        }
        uint16_t a=fg.attr,b=bg.attr;
        uint8_t ap=gtile_row_pixel(&fg),bp=gtile_row_pixel(&bg);
        if(title_water && nx>=0 && nx<320 && ap)title_keep[nx]=1;
        if(level && (wx<0 || wx>=s_stage_width))ap=0;
        /* Menus have a centered foreground composition, with full-width
         * scenery underneath. Special-stage planes are repeating art. */
        if(!level && !special && (nx<0 || nx>=nw))ap=0;
        s_priority[x]=(uint8_t)(((a&0x8000)&&ap)||((b&0x8000)&&bp));
        uint8_t p=ap&&(a&0x8000)?ap:bp&&(b&0x8000)?bp:ap?ap:bp;
        out[x]=p?(world_steps>=0 && p<16?world_palette[p]:palette[p]):backdrop;
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
                    uint16_t actual=world_attr(rx,by,1);
                    ++s_bg_checks;s_bg_errors+=attributes_differ(v,actual,expected,reference_bx,by);
                }
            }
        }
        /* Background coordinates increase by one pixel. Layout widths are
         * whole 256px chunks, so wrap also ends the current pattern row. */
        ++background_x;
        if(bg_width>0 && background_x==bg_width)background_x=0;
    }
    if((!level && !special && !(s_display && s_display->scene==3)) ||
       (special && !(s_display && s_display->scene==2))) {
        if (!native) native = native_row(native_ctx);
        /* Keep the original title sprite mask as well as its logo/menu
         * composition. Redrawing title sprites bypasses the X=0 SAT mask
         * that hides Sonic's torso behind the emblem. Extend only the
         * surrounding scenery, following the live scene palette. Before
         * the first special-stage sprite publication, the original center
         * likewise owns its early background-only fade composition. */
        if(origin>=0) {
            if(title_water) {
                for(int x=0;x<nw;++x)if(title_keep[x])out[origin+x]=native[x];
            } else memcpy(out+origin,native,(size_t)nw*sizeof(uint32_t));
        }
    } else if(s_display) {
        draw_scene_sprites(v,line,out,width,origin,palette,world_steps>=0?world_palette:NULL);
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
static uint8_t s_interp_previous_slot[SCENE_SLOTS];
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
        int left=scene_left(f->ram,unwrap(-f->scroll[0][0],reference),f->width);
        int y=unwrap(f->vs[line][1],(f->ram[0xFF1C]<<8)|f->ram[0xFF1D])+line;
        int zone=(f->ram[0xFE10]<<8)|f->ram[0xFE11];
        if(f->scene.scene==1 && f->scene.zone!=zone)y=f->scene.background_y+line;
        if(f->scene.scene==1)zone=f->scene.zone;
        correction[n]=background_parallax(left+margin,y,zone)-background_parallax(camera,y,zone);
    }
    /* Interpolate the original integer correction with the same rounding
     * as native B scroll. Recomputing parallax from an already-rounded
     * fractional A camera makes a stationary clamped background jitter. */
    return lerp(correction[0],correction[1],128);
}
static void interpolation_sprite_delta(const SceneSprite *sprite,int line,int *dx,int *dy)
{
    const InterpFrame *a=ix_previous(),*b=ix_current();
    unsigned slot=sprite->slot,previous_slot=slot<SCENE_SLOTS?s_interp_previous_slot[slot]:slot;
    if(slot && slot<SCENE_SLOTS && b->scene.identity[slot] &&
       a->scene.identity[previous_slot]==b->scene.identity[slot]) {
        int ax=a->scene.ox[previous_slot],ay=a->scene.oy[previous_slot];
        if(sprite->space==4 && b->scene.identity[slot]>>24==0x5C) {
            ax=b->scene.ox[slot]+((ax-b->scene.ox[slot]+256)&511)-256;
            ay=b->scene.oy[slot]+((ay-b->scene.oy[slot]+32)&63)-32;
        }
        *dx=lerp(ax,b->scene.ox[slot],48)-b->scene.ox[slot];
        *dy=lerp(ay,b->scene.oy[slot],48)-b->scene.oy[slot];
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
    if(enabled() && departing_demo(&f->scene,f->ram))f->scroll[line][0]=-f->scene.ox[0];
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
    /* Resolve native-to-host animal handoffs once per presentation. Doing
     * the same search for every sprite on every scanline adds needless work
     * precisely on the handoff frames that need smooth motion. */
    for(unsigned slot=0;slot<SCENE_SLOTS;++slot) {
        s_interp_previous_slot[slot]=(uint8_t)slot;
        if(f->scene.identity[slot]>>24!=0x28 ||
           p->scene.identity[slot]==f->scene.identity[slot])continue;
        for(unsigned previous=1;previous<SCENE_SLOTS;++previous)
            if(p->scene.identity[previous]==f->scene.identity[slot]) {
                s_interp_previous_slot[slot]=(uint8_t)previous;break;
            }
    }
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
