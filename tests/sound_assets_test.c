#include "sonic1_sound_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"sound assets line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(int argc,char **argv)
{
    CHECK(argc==2);
    uint8_t *rom=malloc(0x80000),*a=malloc(0x400000),*b=malloc(0x400000);
    CHECK(rom && a && b);
    FILE *f=fopen(argv[1],"rb");CHECK(f);CHECK(fread(rom,1,0x80000,f)==0x80000);fclose(f);
    S1SoundAssetLayout layout={0x81000,0x81100,0x81200,0x81300};char error[256];
    memset(a,0xA5,0x400000);memcpy(b,a,0x400000);
    CHECK(!s1_sound_build_assets(rom,0x7FFFF,a,0x400000,&layout,error,sizeof error));
    CHECK(!memcmp(a,b,0x400000));CHECK(*error);
    rom[0]^=1;
    CHECK(!s1_sound_build_assets(rom,0x80000,a,0x400000,&layout,error,sizeof error));
    CHECK(!memcmp(a,b,0x400000));rom[0]^=1;
    CHECK(!s1_sound_build_assets(rom,0x80000,a,S1_SOUND_ASSETS_END-1,&layout,error,sizeof error));
    CHECK(!memcmp(a,b,0x400000));
    S1SoundAssetLayout bad=layout;bad.music_index=0x7FFFF;
    CHECK(!s1_sound_build_assets(rom,0x80000,a,0x400000,&bad,error,sizeof error));
    CHECK(!memcmp(a,b,0x400000));
    memcpy(a,rom,0x80000);memcpy(b,rom,0x80000);
    CHECK(s1_sound_build_assets(rom,0x80000,a,0x400000,&layout,error,sizeof error));CHECK(!*error);
    CHECK(s1_sound_build_assets(b,0x80000,b,0x400000,&layout,error,sizeof error));
    CHECK(!memcmp(a,b,0x400000));CHECK(!memcmp(a,rom,0x80000));
    CHECK(s1_sound_build_assets(a,0x80000,a,0x400000,&layout,error,sizeof error));
    CHECK(!memcmp(a,b,0x400000));
    /* Reserved gaps and the end of the output remain caller-owned. */
    for(size_t i=0x85000;i<S1_SOUND_MUSIC_BASE;i++)CHECK(a[i]==0xA5);
    for(size_t i=S1_SOUND_ASSETS_END;i<0x400000;i++)CHECK(a[i]==0xA5);
    free(rom);free(a);free(b);
    puts("ROM validation, bounds, immutable source, aliased output and repeat conversion passed");return 0;
}
