; Driver software only. Music, effects, instruments and PCM are produced from
; the player's verified ROM by sonic1_sound_assets.c; no game assets go here.
MSUMode = 0
ZeroOffsetOptimization = 1
Snd_driver_RAM = $FFFFF000
Snd_driver_RAM_end = $FFFFF400
v_megadrive = $FFFFFFF8
    cpu 68000
    include "MacroSetup.asm"
disableIntsSave macro
    move.w sr,-(sp)
    ori.w #$700,sr
    endm
enableIntsSave macro
    move.w (sp)+,sr
    endm
    include "sound/Definitions.asm"
    org $80000
ProbeStart:
    dc.b "S1CR"
    dc.l ProbeEnd-ProbeStart
    dc.l SMPS_LoadDACDriver,SMPS_UpdateDriver
    dc.l SMPS_QueueSound1,SMPS_QueueSound2,SMPS_QueueSound3
    dc.l Clone_Driver_RAM,SMPS_RAM.len
    dc.l SMPS_RAM.variables.queue,SMPS_RAM.f_pause
    dc.l MegaPCM_LoadDriver,MegaPCM_LoadSampleTable
    dc.l MegaPCM_DAC_Table
    dc.l Clone_GuardedUpdate
Clone_GuardedUpdate:
    SMPS_UpdateSoundDriver
    rts
    include "sound/engine/Functions.asm"
    include "sound/engine/MegaPCM.asm"

; Sample descriptors contain format/rate/address metadata, never sample bytes.
Kick = $160000
Kick_End = Kick+1700
Snare = $164000
Snare_End = Snare+3808
Timpani = $168000
Timpani_End = Timpani+8236
SegaPCM = $170000
SegaPCM_End = SegaPCM+27000
MegaPCM_DAC_Table:
dac81: dcSample TYPE_PCM,Kick,8201
dac82: dcSample TYPE_PCM,Snare,23784
dac85: dcSample TYPE_PCM,Timpani,7328
dac88: dcSample TYPE_PCM,Timpani,9635
dac89: dcSample TYPE_PCM,Timpani,8720
dac8A: dcSample TYPE_PCM,Timpani,7138
dac8B: dcSample TYPE_PCM,Timpani,6957
dacE0: dcSample TYPE_PCM,SegaPCM,16000,FLAGS_SFX
    dc.w -1
    include "sound/engine/Sonic 2 Clone Driver v2.asm"
ProbeEnd:
