#include "sonic1_audio.h"
#include "genesis_runtime.h"

int s1_audio_hook(uint32_t pc)
{
    /* REV00 cfFadeInToPrevious, immediately after the music RAM restore.
     * The 1-UP uses DAC percussion; SS/SLZ use FM6 instead. Restoring their
     * track data without clearing YM $2B leaves that instrument inaudible.
     * Leave songs that actually resume a DAC track alone. */
    if(pc==0x72B24u && (g_ram[0xF160]&128) && !(g_ram[0xF040]&128)) {
        M68KState saved=g_cpu;
        g_cpu.D[0]=0x2B;g_cpu.D[1]=0;
        recomp_call_addr(0x7272Eu); /* original WriteFMI, including busy waits */
        g_cpu=saved;
    }
    return 0;
}
