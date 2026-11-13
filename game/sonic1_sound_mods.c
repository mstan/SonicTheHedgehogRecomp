#include "sonic1_sound_driver.h"
#include "sonic1_debug.h"
#if RECOMP_LAUNCHER
#include "recomp_launcher.h"
#include <stdio.h>
#include <string.h>
static const RecompLauncherCModProvider *base;
static const char *package_id="sonic1.audio.clone-driver", *feature_id="sound-driver";
static char mod_error[256];
#define COPY(dst,src) snprintf(dst,sizeof(dst),"%s",src)
static int ours(const char *p) { return p && !strcmp(p,package_id); }
static int bugfix(const char *p) { return p && !strcmp(p,"sonic1.bug-fixes"); }
static int access_package(const char *p) { return p && !strcmp(p,"sonic1.easy-access"); }
static int feature(const char *p,const char *f) { return ours(p) && f && !strcmp(f,feature_id); }
static const char *status(void)
{
    if(!s1_sound_available())return "Requires a build prepared with Clone Driver support";
    if(s1_sound_selected()!=s1_sound_active())return "Restart the game to apply the sound driver selection";
    if(s1_sound_active())return "Clone Driver active; local play";
    return "Ready; sound assets are prepared automatically from your Sonic 1 ROM";
}
static int packages(void *ctx) { (void)ctx;return 3+(base&&base->package_count?base->package_count(base->ctx):0); }
static int features(void *ctx) { (void)ctx;return 4+(base&&base->feature_count?base->feature_count(base->ctx):0); }
static int package_get(void *ctx,int n,RecompLauncherCModPackage *out)
{
    (void)ctx;
    if(n>2)return base&&base->package_get&&base->package_get(base->ctx,n-3,out);
    if(n<0 || !out)return 0;
    if(n==2) {
        memset(out,0,sizeof *out);COPY(out->id,"sonic1.easy-access");COPY(out->name,"Easy level select and debug");
        COPY(out->version,"1.0.0");COPY(out->author,"Sonic 1 recompile contributors");
        COPY(out->description,"Optional shortcuts to the original level select, sound test and debug mode. Choose each feature separately. Disabled by default. Restart after changing.");
        COPY(out->status,s1_access_selected()!=s1_access_active()?"Restart to apply":"Ready");
        out->enabled=s1_access_selected()!=0;return 1;
    }
    if(n==1) {
        memset(out,0,sizeof *out);COPY(out->id,"sonic1.bug-fixes");COPY(out->name,"Base game bug fixes");
        COPY(out->version,"1.0.0");COPY(out->author,"Sonic 1 recompile contributors");
        COPY(out->description,"Optional changes to original game quirks. Debug transporter escape prevents flight-mode capture and a stuck control lock. Disabled by default to preserve original behavior. Restart after changing.");
        COPY(out->status,s1_debug_selected()!=s1_debug_active()?"Restart to apply":"Ready");
        out->enabled=s1_debug_selected();return 1;
    }
    memset(out,0,sizeof *out);COPY(out->id,package_id);COPY(out->name,"Clone Driver + Mega PCM");
    COPY(out->version,"0.2.0");COPY(out->author,"Clownacy; Vladikcomper; TheBlad768; host adapter contributors");
    COPY(out->description,"Alternate Sonic 1 sound driver. Uses music and samples from your Sonic 1 JUE REV00 ROM automatically. Disabled by default. Restart after changing drivers. Local play only.");
    COPY(out->license,"Driver permission notice; Mega PCM MIT; ROM-sourced game assets");
    COPY(out->source_name,"Sonic 1 Clone Driver integration");COPY(out->source_url,"https://github.com/TheBlad768/s1disasm-clone-driver");
    COPY(out->status,status());out->enabled=s1_sound_selected();return 1;
}
static int feature_get(void *ctx,int n,RecompLauncherCModFeature *out)
{
    if(n>3)return base&&base->feature_get&&base->feature_get(base->ctx,n-4,out);
    RecompLauncherCModPackage p;
    if(!out || n<0 || !package_get(ctx,n>=2?2:n,&p))return 0;
    if(n>=2) {
        unsigned bit=n==2?S1_ACCESS_LEVEL_SELECT:S1_ACCESS_DEBUG;
        memset(out,0,sizeof *out);COPY(out->id,n==2?"level-select":"debug-mode");COPY(out->package_id,p.id);
        COPY(out->package_name,p.name);COPY(out->package_version,p.version);COPY(out->author,p.author);
        COPY(out->name,n==2?"Easy level select and sound test":"Easy debug mode");COPY(out->group,"Gameplay");
        COPY(out->description,n==2?"Press Start on the title screen to open the original level select and sound test. No code or held A button required. Restart after changing.":"Press B during normal levels or Special Stages to enter or leave the original debug mode. A changes the item; C places it. No code or held A button required. Restart after changing.");
        COPY(out->status,((s1_access_selected()^s1_access_active())&bit)?"Restart to apply":"Ready");
        out->enabled=(s1_access_selected()&bit)!=0;return 1;
    }
    memset(out,0,sizeof *out);COPY(out->id,n?"debug-transporters":feature_id);COPY(out->package_id,p.id);
    COPY(out->package_name,p.name);COPY(out->package_version,p.version);COPY(out->name,p.name);
    COPY(out->author,p.author);COPY(out->description,p.description);COPY(out->group,n?"Gameplay":"Audio");
    if(n)COPY(out->name,"Debug transporter escape");
    COPY(out->source_name,p.source_name);COPY(out->source_url,p.source_url);COPY(out->status,p.status);
    out->enabled=p.enabled;return 1;
}
static int enable(void *ctx,const char *p,const char *f,int on)
{
    (void)ctx;mod_error[0]=0;
    if(ours(p))return feature(p,f)&&s1_sound_select(on);
    if(bugfix(p))return f && !strcmp(f,"debug-transporters") && s1_debug_select(on);
    if(access_package(p)) {
        if(f && !strcmp(f,"level-select"))return s1_access_select(S1_ACCESS_LEVEL_SELECT,on);
        if(f && !strcmp(f,"debug-mode"))return s1_access_select(S1_ACCESS_DEBUG,on);
        return 0;
    }
    return base&&base->feature_enable&&base->feature_enable(base->ctx,p,f,on);
}
static int package_enable(void *ctx,const char *p,int on)
{
    if(ours(p))return enable(ctx,p,feature_id,on);
    if(bugfix(p))return enable(ctx,p,"debug-transporters",on);
    if(access_package(p))return s1_access_select(S1_ACCESS_LEVEL_SELECT,on) && s1_access_select(S1_ACCESS_DEBUG,on);
    return base&&base->set_enabled&&base->set_enabled(base->ctx,p,on);
}
static int option(void *ctx,const char *p,const char *f,int n,RecompLauncherCModOption *out)
{ (void)ctx;return !ours(p)&&base&&base->feature_option_get&&base->feature_option_get(base->ctx,p,f,n,out); }
static int choice(void *ctx,const char *p,const char *f,const char *o,int n,RecompLauncherCModChoice *out)
{ (void)ctx;return !ours(p)&&base&&base->feature_choice_get&&base->feature_choice_get(base->ctx,p,f,o,n,out); }
static int set_option(void *ctx,const char *p,const char *f,const char *o,const char *v)
{ (void)ctx;return !ours(p)&&base&&base->feature_set_option&&base->feature_set_option(base->ctx,p,f,o,v); }
static int resource_count(void *ctx,const char *p,const char *f)
{
    (void)ctx;if(ours(p))return 0;
    return base&&base->feature_resource_count?base->feature_resource_count(base->ctx,p,f):0;
}
static int resource_get(void *ctx,const char *p,const char *f,int n,RecompLauncherCModResource *out)
{
    (void)ctx;
    if(!ours(p))return base&&base->feature_resource_get&&base->feature_resource_get(base->ctx,p,f,n,out);
    return 0;
}
static int resource_set(void *ctx,const char *p,const char *f,const char *r,const char *path)
{
    (void)ctx;mod_error[0]=0;
    if(ours(p))return 0;
    return base&&base->feature_resource_set_path&&base->feature_resource_set_path(base->ctx,p,f,r,path);
}
static int commit(void *ctx,const char *image)
{
    (void)ctx;mod_error[0]=0;
    if(s1_sound_selected()&&!s1_sound_select(1))return 0;
    if(base&&base->commit&&!base->commit(base->ctx,image))return 0;
    return s1_sound_commit();
}
static int netplay(void *ctx,const char *image)
{
    (void)ctx;
    if(!s1_sound_netplay_allowed()) { COPY(mod_error,"Disable Clone Driver and restart before netplay; this driver is experimental and local-only.");return 0; }
    return !base||!base->commit_netplay||base->commit_netplay(base->ctx,image);
}
static const char *last_error(void *ctx)
{
    (void)ctx;if(*mod_error)return mod_error;if(*s1_sound_error())return s1_sound_error();
    return base&&base->last_error?base->last_error(base->ctx):"";
}
const RecompLauncherCModProvider *s1_sound_mods(const RecompLauncherCModProvider *common)
{
    static RecompLauncherCModProvider p;
    base=common;if(base)p=*base;else memset(&p,0,sizeof p);
    p.package_count=packages;p.package_get=package_get;p.set_enabled=package_enable;
    p.feature_count=features;p.feature_get=feature_get;p.feature_enable=enable;
    p.feature_option_get=option;p.feature_choice_get=choice;p.feature_set_option=set_option;
    p.feature_resource_count=resource_count;p.feature_resource_get=resource_get;p.feature_resource_set_path=resource_set;
    p.commit=commit;p.commit_netplay=netplay;p.last_error=last_error;
    return &p;
}
#else
const struct RecompLauncherCModProvider *s1_sound_mods(const struct RecompLauncherCModProvider *common) { return common; }
#endif
