; Development probe only. Assemble with the pinned s1disasm-clone-driver source.
; Keep gameplay, vectors, art and level data out of this image.
MSUMode = 0
ZeroOffsetOptimization = 1
Snd_driver_RAM = $FFFFF000
Snd_driver_RAM_end = $FFFFF400
v_megadrive = $FFFFFFF8

    cpu 68000
    include "MacroSetup.asm"

; The only shared game macros required by the sound code.
disableIntsSave macro
    move.w sr,-(sp)
    ori.w #$700,sr
    endm
enableIntsSave macro
    move.w (sp)+,sr
    endm

    include "sound/Definitions.asm"

; The stock REV00 ROM ends at $80000. This is a proposed extension bank,
; not a patch to the user's ROM. No runtime code loads this probe yet.
    org $80000
ProbeStart:
    dc.b "S1CD"
    dc.l ProbeEnd-ProbeStart
    dc.l SMPS_LoadDACDriver,SMPS_UpdateDriver
    dc.l SMPS_QueueSound1,SMPS_QueueSound2,SMPS_QueueSound3
    dc.l Clone_Driver_RAM,SMPS_RAM.len
    dc.l SMPS_RAM.variables.queue,SMPS_RAM.f_pause
    dc.l MegaPCM_LoadDriver,MegaPCM_LoadSampleTable
    dc.l MegaPCM_DAC_Table
    include "sound/engine/Functions.asm"
    include "sound/engine/MegaPCM.asm"
    include "sound/MegaPCM - DAC Table.asm"
    include "sound/DAC Samples.asm"
    include "sound/engine/Sonic 2 Clone Driver v2.asm"
ProbeEnd:
