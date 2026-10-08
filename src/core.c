#include "qg.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void qg_defaults(QgConfig *c) {
    memset(c, 0, sizeof(*c));
    c->mask=QG_ALL; c->idle_seconds=180*60; c->sample_seconds=60; c->warning_seconds=60;
    c->threshold[0]=5; c->threshold[1]=5; c->threshold[2]=5; c->threshold[3]=32;
    c->veto_active_session=1;
}
bool qg_config_valid(const QgConfig *c) {
    if(!c || !c->mask || (c->mask & ~QG_ALL) || c->enabled>1 || c->veto_active_session>1) return false;
    if(c->idle_seconds<60 || c->idle_seconds>604800 || c->sample_seconds<1 || c->sample_seconds>600 ||
       c->idle_seconds<c->sample_seconds*2 || c->warning_seconds<10 || c->warning_seconds>3600) return false;
    for(unsigned i=0;i<4;i++) if(!isfinite(c->threshold[i]) || c->threshold[i]<0.001 || c->threshold[i]>(i==3?1e9:100.0)) return false;
    return true;
}
static bool decimal(const char *s, double *out, bool integer) {
    if(!s || !*s || strlen(s)>20) return false;
    double v=0, scale=1; bool point=false, digit=false; unsigned decimals=0;
    for(;*s;s++) {
        if(*s=='.' && !point && !integer && digit) {point=true;continue;}
        if(*s<'0'||*s>'9') return false;
        digit=true;
        if(point) { if(++decimals>6) return false; scale*=10; v+=(*s-'0')/scale; }
        else { v=v*10+(*s-'0'); if(v>1e9) return false; }
    }
    if(!digit || (point&&!decimals)) return false;
    *out=v; return true;
}
bool qg_config_parse(const char *text,size_t length,QgConfig *out) {
    static const char *keys[]={"version","mask","idle_seconds","sample_seconds","warning_seconds","cpu","gpu","disk","network","enabled","veto_active_session"};
    if(!text||!out||!length||length>=QG_CONFIG_MAX||memchr(text,0,length)) return false;
    char buf[QG_CONFIG_MAX]; memcpy(buf,text,length); buf[length]=0;
    QgConfig c; qg_defaults(&c); uint32_t seen=0; char *line=buf;
    while(*line) {
        char *end=strchr(line,'\n'); if(end) *end=0;
        size_t n=strlen(line); if(n&&line[n-1]=='\r') line[--n]=0;
        if(!n) return false;
        char *eq=strchr(line,'='); if(!eq||eq==line) return false; *eq++=0;
        unsigned k; for(k=0;k<11;k++) if(!strcmp(line,keys[k])) break;
        if(k==11||(seen&(1u<<k))) return false;
        seen|=1u<<k;
        double v; if(!decimal(eq,&v,k<5||k>=9)) return false;
        switch(k) {
        case 0: if(v!=1) return false; break;
        case 1: c.mask=(uint32_t)v; break;
        case 2: c.idle_seconds=(uint32_t)v; break;
        case 3: c.sample_seconds=(uint32_t)v; break;
        case 4: c.warning_seconds=(uint32_t)v; break;
        case 9: c.enabled=(uint32_t)v; break;
        case 10: c.veto_active_session=(uint32_t)v; break;
        default: c.threshold[k-5]=v; break;
        }
        if(!end) break;
        line=end+1;
    }
    if(seen!=0x7ffu || !qg_config_valid(&c)) return false;
    *out=c; return true;
}
size_t qg_config_format(const QgConfig *c,char *out,size_t cap) {
    if(!qg_config_valid(c)||!out||!cap) return 0;
    int n=snprintf(out,cap,"version=1\nmask=%u\nidle_seconds=%u\nsample_seconds=%u\nwarning_seconds=%u\ncpu=%.6f\ngpu=%.6f\ndisk=%.6f\nnetwork=%.6f\nenabled=%u\nveto_active_session=%u\n",c->mask,c->idle_seconds,c->sample_seconds,c->warning_seconds,c->threshold[0],c->threshold[1],c->threshold[2],c->threshold[3],c->enabled,c->veto_active_session);
    return n>0&&(size_t)n<cap?(size_t)n:0;
}
void qg_engine_init(QgEngine *e,const QgConfig *c) {memset(e,0,sizeof(*e));e->epoch=1;qg_engine_configure(e,c);}
void qg_engine_reset(QgEngine *e,QgPhase why) {
    e->epoch++; e->last_ms=e->idle_since=e->warning_since=0;
    e->have_last=e->have_idle=false; e->phase=why;
}
void qg_engine_configure(QgEngine *e,const QgConfig *c) {
    if(!qg_config_valid(c)) {qg_defaults(&e->cfg);qg_engine_reset(e,QG_FAULT);return;}
    e->cfg=*c; qg_engine_reset(e,c->enabled?QG_UNKNOWN:QG_PAUSED);
}
void qg_engine_cancel(QgEngine *e) {e->cfg.enabled=0;qg_engine_reset(e,QG_PAUSED);}
static void stop_interval(QgEngine *e,QgPhase phase,uint64_t at) {qg_engine_reset(e,phase);e->have_last=true;e->last_ms=at;}
void qg_engine_sample(QgEngine *e,const QgSample *s) {
    if(!e->cfg.enabled || e->phase==QG_REQUESTED || e->phase==QG_FAULT) return;
    uint64_t expected=(uint64_t)e->cfg.sample_seconds*1000;
    if(e->phase==QG_WARNING||e->phase==QG_READY) {if(expected>2000)expected=2000;}
    uint64_t tolerance=expected/10; if(tolerance<2000)tolerance=2000;
    if(s->discontinuity || (e->have_last&&(s->at_ms<=e->last_ms||s->at_ms-e->last_ms>expected+tolerance))) {
        stop_interval(e,QG_UNKNOWN,s->at_ms);return;
    }
    e->last_ms=s->at_ms;e->have_last=true;
    if((s->valid&e->cfg.mask)!=e->cfg.mask) {stop_interval(e,QG_UNKNOWN,s->at_ms);return;}
    for(unsigned i=0;i<4;i++) if(e->cfg.mask&(1u<<i)) {
        if(!isfinite(s->value[i])||s->value[i]<0||(i<3&&s->value[i]>100.0)) {stop_interval(e,QG_UNKNOWN,s->at_ms);return;}
        if(s->value[i]>=e->cfg.threshold[i]) {stop_interval(e,QG_BUSY,s->at_ms);return;}
    }
    if(s->veto) {stop_interval(e,QG_BUSY,s->at_ms);return;}
    if(!e->have_idle) {e->idle_since=s->at_ms;e->have_idle=true;e->phase=QG_IDLE;return;}
    if(e->phase==QG_WARNING||e->phase==QG_READY) {
        if(s->at_ms-e->warning_since>=(uint64_t)e->cfg.warning_seconds*1000) e->phase=QG_READY;
    } else if(s->at_ms-e->idle_since>=(uint64_t)e->cfg.idle_seconds*1000) {
        e->warning_since=s->at_ms;e->phase=QG_WARNING;
    } else e->phase=QG_IDLE;
}
bool qg_engine_can_commit(const QgEngine *e,uint64_t epoch,uint64_t now) {
    return e->cfg.enabled&&e->phase==QG_READY&&e->have_last&&e->have_idle&&e->epoch==epoch&&now>=e->last_ms&&now-e->last_ms<=2000;
}
