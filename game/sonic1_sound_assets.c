#include "sonic1_sound_assets.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ROM_SIZE=0x80000, SOUND_BASE=0x70000, MAX_NODES=8192 };
typedef struct Node {
    uint32_t pc, target;
    uint16_t output;
    uint8_t dac, length, encoded;
} Node;
typedef struct Convert {
    const uint8_t *rom;
    uint8_t *memory;
    int bad, count, voices;
    int pitch[8];
    uint32_t lower_until[8];
    int map[0x80000];
    Node nodes[MAX_NODES];
} Convert;
static unsigned rb(Convert *c,uint32_t p)
{ if(p>=ROM_SIZE){c->bad=1;return 0;}return c->rom[p]; }
static unsigned rw(Convert *c,uint32_t p) { return (rb(c,p)<<8)|rb(c,p+1); }
static uint32_t rl(Convert *c,uint32_t p) { return (rw(c,p)<<16)|rw(c,p+2); }
static void ww(Convert *c,uint32_t p,unsigned v)
{ c->memory[p]=(uint8_t)(v>>8);c->memory[p+1]=(uint8_t)v; }
static void wl(Convert *c,uint32_t p,uint32_t v) { ww(c,p,v>>16);ww(c,p+2,v); }
static unsigned tempo(unsigned n)
{ unsigned t=n?n:256;return (256-((((t-1)<<8)+(t>>1))/t))&255; }
static int node(Convert *c,uint32_t pc,int dac)
{
    if(pc<SOUND_BASE || pc>=ROM_SIZE || c->count>=MAX_NODES){c->bad=1;return 0;}
    unsigned key=((pc-SOUND_BASE)<<3)|(unsigned)dac;
    if(c->map[key]>=0)return c->map[key];
    int n=c->count++;c->map[key]=n;
    c->nodes[n]=(Node){pc,0,0,(uint8_t)dac,0,0};return n;
}
static int order_nodes(const void *a,const void *b)
{
    const Node *x=a,*y=b;
    if(x->dac!=y->dac)return x->dac<y->dac?-1:1;
    return x->pc<y->pc?-1:x->pc>y->pc;
}
static int find_node(Convert *c,uint32_t pc,int dac)
{
    if(pc<SOUND_BASE || pc>=ROM_SIZE){c->bad=1;return 0;}
    int n=c->map[((pc-SOUND_BASE)<<3)|(unsigned)dac];
    if(n<0){c->bad=1;return 0;}return n;
}
static unsigned convert_song(Convert *c,uint32_t source,uint32_t dest,unsigned capacity,int music)
{
    if(source<SOUND_BASE || source>ROM_SIZE-64){c->bad=1;return 0;}
    /* Lengths are original SMPS 68k Type 1b coordination commands E0-F9.
     * Expand to Clone's FF command namespace; FE is no-attack. */
    static const uint8_t lengths[]={2,2,2,1,1,2,2,1,2,2,2,2,2,1,1,2,5,1,1,2,1,2,3,5,3,1};
    static const uint8_t commands[]={0,1,2,3,4,5,6,0,8,9,10,11,12,31,7,13,14,15,16,17,18,19,20,21,22,0xFF};
    uint32_t roots[10];int dacs[10];unsigned headers[10],root_count=0,header;
    unsigned fm=music?rb(c,source+2):0,psg=music?rb(c,source+3):0;
    c->count=0;c->voices=0;memset(c->map,0xFF,sizeof c->map);memset(c->lower_until,0,sizeof c->lower_until);
    if(music) {
        if(!fm || fm>7 || psg>3){c->bad=1;return 0;}
        header=8+fm*4+psg*6;
        memcpy(c->memory+dest,c->rom+source,4);c->memory[dest+4]=c->memory[dest+5]=0;
        c->memory[dest+6]=(uint8_t)rb(c,source+4);c->memory[dest+7]=(uint8_t)tempo(rb(c,source+5));
        for(unsigned i=0;i<fm+psg;i++) {
            unsigned old=source+6+(i<fm?i*4:fm*4+(i-fm)*6),out=dest+old-source+2;
            int kind=i==0?1:i>=fm?(int)(i-fm+2):0;
            if(kind>=2)c->pitch[kind]=(int8_t)rb(c,old+2);
            roots[root_count]=source+rw(c,old);dacs[root_count]=kind;headers[root_count++]=out;
            /* REV00 MZ's opening noise phrase is an octave too high. Lower
             * the complete phrase, including its final in-range note, as
             * the upstream converted arrangement does. No notes are stored
             * here: the phrase still comes entirely from the player's ROM. */
            if(source==0x75194 && i==fm+2) {
                uint32_t p=source+rw(c,old);
                for(unsigned guard=0;guard<32;guard++) {
                    unsigned op=rb(c,p);
                    if(op==0xEC && (int8_t)rb(c,p+1)>0){c->lower_until[kind]=p;break;}
                    if(op>=0xF6)break;
                    p+=op>=0xE0?lengths[op-0xE0]:1;
                }
            }
            unsigned n=i<fm?4:6;memcpy(c->memory+out,c->rom+old,n);
            if(i>=fm){c->memory[out+2]+=12;c->memory[out+3]<<=3;c->memory[out+4]=0;}
        }
        if(fm>1)c->voices=1;
    } else {
        unsigned tracks=rb(c,source+3);
        if(!tracks || tracks>6){c->bad=1;return 0;}
        header=4+tracks*6;memcpy(c->memory+dest,c->rom+source,header);
        for(unsigned i=0;i<tracks;i++) {
            unsigned old=source+4+i*6,out=dest+4+i*6,id=rb(c,old+1);
            int kind=id&128?(int)i+2:0;if(kind>=2)c->pitch[kind]=(int8_t)rb(c,old+4);
            roots[root_count]=source+rw(c,old+2);dacs[root_count]=kind;headers[root_count++]=out+2;
            if(id&128)c->memory[out+4]+=12;else c->voices=1;
            if(id==0x80 || id==0xA0 || id==0xC0)c->memory[out+5]<<=3;
        }
    }
    for(unsigned i=0;i<root_count;i++)node(c,roots[i],dacs[i]);
    for(int i=0;i<c->count && !c->bad;i++) {
        Node *n=&c->nodes[i];unsigned op=rb(c,n->pc),length=1;
        if(op>=0xE0) {
            if(op>0xF9){c->bad=1;break;}
            length=lengths[op-0xE0];n->encoded=(uint8_t)(op==0xE7?1:op==0xF9?8:length+1);
            if(op==0xEF && rb(c,n->pc+1)+1>(unsigned)c->voices)c->voices=(int)rb(c,n->pc+1)+1;
        } else n->encoded=1;
        /* Preserve the converted Credits corrections: remove the erroneous
         * late PSG rests/FM volume command and make three inherited rests
         * explicit for Clone's duration handling. Locations identify format
         * edits in the verified REV00 ROM; their musical values are read. */
        if(n->dac>=2 && n->pc>=0x781BD && n->pc<=0x781C0)n->encoded=0;
        if(n->dac>=2 && (n->pc==0x7825D || n->pc==0x7825E || n->pc==0x78265))n->encoded=2;
        n->length=(uint8_t)length;
        if(op==0xF6 || op==0xF7 || op==0xF8) {
            n->target=n->pc+length-1+(int16_t)rw(c,n->pc+length-2);
            node(c,n->target,n->dac);
        }
        if(op!=0xE3 && op!=0xE4 && op!=0xEE && op!=0xF2 && op!=0xF6)
            node(c,n->pc+length,n->dac);
    }
    if(c->bad)return 0;
    qsort(c->nodes,(size_t)c->count,sizeof c->nodes[0],order_nodes);
    unsigned size=header;
    for(int i=0;i<c->count;i++) {
        Node *n=&c->nodes[i];n->output=(uint16_t)size;size+=n->encoded;
        c->map[((n->pc-SOUND_BASE)<<3)|n->dac]=i;
    }
    if(size+25*c->voices>capacity || c->voices>128){c->bad=1;return 0;}
    for(unsigned i=0;i<root_count;i++)ww(c,headers[i],c->nodes[find_node(c,roots[i],dacs[i])].output);
    for(int i=0;i<c->count;i++) {
        Node *n=&c->nodes[i];uint32_t out=dest+n->output;unsigned op=rb(c,n->pc);
        if(!n->encoded)continue;
        if(op<0xE0) {
            unsigned value=op;
            if(n->dac==1 && op>=0x88 && op<=0x8B)value-=4;
            if(n->dac>=2 && op>=0x81) {
                /* The zero-period PSG maximum occupies one later Clone
                 * table slot. Other notes keep their source pitch. */
                int pitch=c->pitch[n->dac];
                if(n->pc<c->lower_until[n->dac])value-=12;
                if(!pitch && value==0xC6)++value;
            }
            if(n->encoded==2)c->memory[out++]=0x80;
            c->memory[out]=(uint8_t)value;continue;
        }
        if(op==0xE7){c->memory[out]=0xFE;continue;}
        if(op==0xF9){
            static const uint8_t release[]={0xFF,0x20,0x88,0x0F,0xFF,0x20,0x8C,0x0F};
            memcpy(c->memory+out,release,sizeof release);continue;
        }
        c->memory[out]=0xFF;c->memory[out+1]=commands[op-0xE0];
        for(unsigned j=1;j<n->length;j++)c->memory[out+1+j]=(uint8_t)rb(c,n->pc+j);
        if(op==0xEA)c->memory[out+2]=(uint8_t)tempo(rb(c,n->pc+1));
        if(op==0xEC){unsigned v=rb(c,n->pc+1);c->memory[out+2]=(uint8_t)(((v<<3)&0x7F)|(v&0x80));}
        if(n->target) {
            int target=(int)c->nodes[find_node(c,n->target,n->dac)].output;
            int relative=target-(n->output+n->encoded-(op==0xF8?0:1));
            if(relative < -32768 || relative>32767){c->bad=1;return 0;}
            ww(c,out+n->encoded-2,(unsigned)relative);
        }
    }
    /* Sonic 1 stores TL last, with operators 4/3/2/1. Clone uses TL second
     * and operators 4/2/3/1. Carrier flags follow the voice algorithm. */
    uint32_t voices=source+(int16_t)rw(c,source);ww(c,dest,size);
    static const unsigned order[]={0,2,1,3};
    for(int v=0;v<c->voices;v++) {
        unsigned alg=rb(c,voices)&7;c->memory[dest+size]=(uint8_t)rb(c,voices);
        for(unsigned k=0;k<4;k++) {
            unsigned carrier=k==0?alg==7:k==1?alg>=5:k==2?alg>=4:1;
            c->memory[dest+size+1+k]=(uint8_t)rb(c,voices+1+order[k]);
            c->memory[dest+size+5+k]=(uint8_t)((rb(c,voices+21+order[k])&127)|(carrier?128:0));
            for(unsigned group=0;group<4;group++)
                c->memory[dest+size+9+group*4+k]=(uint8_t)rb(c,voices+5+group*4+order[k]);
        }
        size+=25;voices+=25;
    }
    return size;
}
typedef struct Kos {Convert *c;uint32_t pc;unsigned bits,left;} Kos;
static unsigned kb(Kos *k)
{
    unsigned bit=k->bits&1;k->bits>>=1;
    if(!--k->left){k->bits=rb(k->c,k->pc)|(rb(k->c,k->pc+1)<<8);k->pc+=2;k->left=16;}
    return bit;
}
static unsigned unpack_z80(Convert *c,uint8_t *out)
{
    Kos k={c,0x72E7E,rb(c,0x72E7C)|(rb(c,0x72E7D)<<8),16};unsigned size=0;
    for(unsigned guard=0;guard<65536 && !c->bad;guard++) {
        if(kb(&k)){if(size>=8192)break;out[size++]=(uint8_t)rb(c,k.pc++);continue;}
        int offset;unsigned count;
        if(!kb(&k)) {
            unsigned a=kb(&k);count=(a<<1)|kb(&k);count+=2;
            offset=(int)rb(c,k.pc++)-256;
        } else {
            unsigned a=rb(c,k.pc++),b=rb(c,k.pc++);
            offset=(int16_t)(0xE000|((b&0xF8)<<5)|a);count=b&7;
            if(count)count+=2;
            else {count=rb(c,k.pc++);if(!count)return size;if(count==1)continue;++count;}
        }
        if((int)size+offset<0 || size+count>8192)break;
        while(count--){out[size]=out[(int)size+offset];++size;}
    }
    c->bad=1;return 0;
}
int s1_sound_build_assets(const uint8_t *rom,size_t rom_size,uint8_t *memory,size_t memory_size,
                          const S1SoundAssetLayout *layout,char *error,size_t error_size)
{
    const char *failure="Unsupported ROM: Clone Driver requires Sonic 1 JUE REV00.";
    if(error_size)error[0]=0;
    if(!rom || !memory || !layout || rom_size!=ROM_SIZE || memory_size<S1_SOUND_ASSETS_END)goto fail;
    static const uint8_t expected[32]={0x46,0x16,0x0B,0xAA,0x06,0x36,0x2C,0x71,0x1C,0x9F,0x1A,0x50,0x17,0xCB,0x73,0x71,
        0x02,0x64,0x44,0x93,0x6C,0x8A,0xF5,0xE9,0x3A,0x78,0x99,0x6C,0xF3,0x2F,0xF2,0xA6};
    uint8_t digest[32];recompui_sha256_compute(rom,rom_size,digest);
    if(memcmp(digest,expected,sizeof digest))goto fail;
    uint32_t tables[]={layout->music_index,layout->sfx_index,layout->special_index,layout->psg_index};
    for(unsigned i=0;i<4;i++)if(tables[i]<ROM_SIZE || tables[i]>S1_SOUND_MUSIC_BASE-256)goto fail;
    Convert *c=calloc(1,sizeof *c);failure="Unable to allocate sound conversion workspace.";
    if(!c)goto fail;c->rom=rom;c->memory=memory;
    memset(memory+S1_SOUND_MUSIC_BASE,0,S1_SOUND_ASSETS_END-S1_SOUND_MUSIC_BASE);
    uint32_t music=rl(c,0x71998),sfx=rl(c,0x7199C),special=rl(c,0x71994),psg=rl(c,0x719A4),priorities=rl(c,0x71990),speed=rl(c,0x719A0);
    for(unsigned i=0;i<19 && !c->bad;i++) {
        uint32_t dest=S1_SOUND_MUSIC_BASE+i*S1_SOUND_MUSIC_STRIDE;
        uint32_t song=rl(c,music+4*i);
        convert_song(c,song,dest,S1_SOUND_MUSIC_STRIDE,1);
        /* Metadata's low bit selects original PAL tempo for synchronized songs. */
        unsigned pal=i==10 || i==15 || i==16 || i==17;
        unsigned fast=(tempo(rb(c,i<8?speed+i:song+5))-(i==11 || i==12?0x20:0))&255;
        wl(c,layout->music_index+4*i,((uint32_t)fast<<24)|dest|pal);
    }
    for(unsigned i=0;i<49 && !c->bad;i++) {
        uint32_t dest=S1_SOUND_SFX_BASE+i*S1_SOUND_SFX_STRIDE;
        convert_song(c,rl(c,i==48?special:sfx+4*i),dest,S1_SOUND_SFX_STRIDE,0);
        if(i==48)wl(c,layout->special_index,dest);
        else wl(c,layout->sfx_index+4*i,(rb(c,priorities+31+i)<<24)|dest);
    }
    for(unsigned i=0;i<9 && !c->bad;i++) {
        uint32_t p=rl(c,psg+4*i),dest=S1_SOUND_PSG_BASE+256*i;unsigned j;
        wl(c,layout->psg_index+4*i,dest);
        /* The port loops envelope 6; the other S1 envelopes hold their end. */
        for(j=0;j<256;j++){unsigned v=rb(c,p+j);memory[dest+j]=(uint8_t)(v==0x80 && i!=5?0x81:v);if(v==0x80)break;}
        if(j==256)c->bad=1;
    }
    uint8_t z80[8192];unsigned zsize=unpack_z80(c,z80);
    for(unsigned i=0;i<3 && !c->bad;i++) {
        unsigned meta=0xD6+8*i,start=z80[meta]|(z80[meta+1]<<8),size=z80[meta+2]|(z80[meta+3]<<8);
        if(start+size>zsize || size*2>0x4000){c->bad=1;break;}
        uint32_t dest=S1_SOUND_PCM_BASE+0x4000*i;unsigned sample=128;
        for(unsigned j=0;j<size;j++)for(unsigned n=0;n<2;n++) {
            sample=(sample+z80[0x22+((z80[start+j]>>(n?0:4))&15)])&255;
            memory[dest++]=(uint8_t)sample;
        }
    }
    memcpy(memory+S1_SOUND_SEGA_BASE,rom+0x79688,0x6978);
    int ok=!c->bad;free(c);if(ok)return 1;
    failure="The ROM's sound data could not be converted for this build.";
fail:
    if(error_size)snprintf(error,error_size,"%s",failure);return 0;
}
