#include "sonic1_debug.h"
#include "genesis_runtime.h"

int s1_debug_hook(uint32_t pc)
{
    if(!(g_ram[0xFE08] | g_ram[0xFE09]))return 0;
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
