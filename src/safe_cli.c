#include "safe_cli.h"
#include "qg.h"
#include <stddef.h>
#include <string.h>

static bool equal(const wchar_t *a,const wchar_t *b){return a&&b&&wcscmp(a,b)==0;}
static bool decimal(const wchar_t *s,uint32_t *out){
    uint32_t v=0;size_t n=0;if(!s||!*s)return false;
    for(;*s;s++){
        if(++n>9||*s<L'0'||*s>L'9')return false;
        v=v*10u+(uint32_t)(*s-L'0');
    }
    *out=v;return true;
}
static bool metrics(const wchar_t *s,uint32_t *out){
    if(!s||!*s)return false;
    if(equal(s,L"all")){*out=QG_ALL;return true;}
    uint32_t mask=0;const wchar_t *p=s;
    while(*p){
        const wchar_t *end=p;while(*end&&*end!=L',')end++;
        size_t n=(size_t)(end-p);uint32_t bit=0;
        if(n==3&&wcsncmp(p,L"cpu",3)==0)bit=QG_CPU;
        else if(n==3&&wcsncmp(p,L"gpu",3)==0)bit=QG_GPU;
        else if(n==4&&wcsncmp(p,L"disk",4)==0)bit=QG_DISK;
        else if(n==7&&wcsncmp(p,L"network",7)==0)bit=QG_NET;
        else return false;
        if(mask&bit)return false;
        mask|=bit;
        if(!*end)break;
        p=end+1;if(!*p)return false;
    }
    *out=mask;return mask!=0;
}
static wchar_t upper(wchar_t c){return c>=L'a'&&c<=L'z'?c-(L'a'-L'A'):c;}
static bool device(const wchar_t *s,size_t length){
    wchar_t name[9]={0};size_t n=0;
    while(n<length&&s[n]!=L'.')n++;
    /* Win32 strips trailing basename spaces before resolving DOS devices. */
    while(n&&s[n-1]==L' ')n--;
    if(n>8)return false;
    for(size_t i=0;i<n;i++)name[i]=upper(s[i]);
    if(equal(name,L"CON")||equal(name,L"PRN")||equal(name,L"AUX")||equal(name,L"NUL")||equal(name,L"CONIN$")||equal(name,L"CONOUT$"))return true;
    return n==4&&(wcsncmp(name,L"COM",3)==0||wcsncmp(name,L"LPT",3)==0)&&((name[3]>=L'1'&&name[3]<=L'9')||name[3]==0x00b9||name[3]==0x00b2||name[3]==0x00b3);
}
static bool output_path(const wchar_t *s){
    size_t length=0;if(!s)return false;while(length<QG_SAFE_OUTPUT_CAP&&s[length])length++;
    if(!length)return true; /* Existing stdout only; no implicit file destination. */
    if(length<4||length>=QG_SAFE_OUTPUT_CAP||!((s[0]>=L'A'&&s[0]<=L'Z')||(s[0]>=L'a'&&s[0]<=L'z'))||s[1]!=L':'||(s[2]!=L'\\'&&s[2]!=L'/'))return false;
    size_t begin=3;
    for(size_t i=3;i<=length;i++){
        wchar_t c=s[i];
        if(c==L'\\'||c==L'/'||!c){
            size_t n=i-begin;if(!n||s[i-1]==L'.'||s[i-1]==L' '||device(s+begin,n))return false;
            begin=i+1;
        }else if(c<32||wcschr(L"<>:\"|?*",c))return false;
    }
    return true;
}
bool qg_safe_options_valid(const QgSafeOptions *o){
    return o&&o->mask&&(o->mask&~QG_ALL)==0&&o->duration_seconds>=60&&o->duration_seconds<=900&&
        o->interval_seconds>=5&&o->interval_seconds<=120&&o->duration_seconds>=o->interval_seconds*3&&
        o->timeout_seconds>=1&&o->timeout_seconds<=30&&output_path(o->output);
}
QgSafeParse qg_safe_parse(int argc,const wchar_t *const *argv,QgSafeOptions *out){
    if(argc<1||!argv||!out)return QG_SAFE_INVALID;
    bool present=false;
    for(int i=0;i<argc;i++){
        if(!argv[i])return QG_SAFE_INVALID;
        if(i&&equal(argv[i],L"--sensor-test"))present=true;
    }
    if(!present)return QG_SAFE_ABSENT;
    QgSafeOptions o;memset(&o,0,sizeof(o));o.mask=QG_ALL;o.duration_seconds=QG_SAFE_DEFAULT_SECONDS;
    o.interval_seconds=QG_SAFE_DEFAULT_INTERVAL;o.timeout_seconds=QG_SAFE_DEFAULT_TIMEOUT;uint32_t seen=0;
    for(int i=1;i<argc;i++){
        uint32_t bit;const wchar_t *a=argv[i];
        if(equal(a,L"--sensor-test"))bit=1;
        else if(equal(a,L"--metrics"))bit=2;
        else if(equal(a,L"--duration-seconds"))bit=4;
        else if(equal(a,L"--interval-seconds"))bit=8;
        else if(equal(a,L"--sample-timeout-seconds"))bit=16;
        else if(equal(a,L"--output"))bit=32;
        else return QG_SAFE_INVALID;
        if(seen&bit)return QG_SAFE_INVALID;
        seen|=bit;
        if(bit==1)continue;
        if(++i>=argc||!argv[i]||!*argv[i])return QG_SAFE_INVALID;
        if(bit==2){if(!metrics(argv[i],&o.mask))return QG_SAFE_INVALID;}
        else if(bit==4){if(!decimal(argv[i],&o.duration_seconds))return QG_SAFE_INVALID;}
        else if(bit==8){if(!decimal(argv[i],&o.interval_seconds))return QG_SAFE_INVALID;}
        else if(bit==16){if(!decimal(argv[i],&o.timeout_seconds))return QG_SAFE_INVALID;}
        else {size_t n=0;while(n<QG_SAFE_OUTPUT_CAP&&argv[i][n])n++;if(!n||n>=QG_SAFE_OUTPUT_CAP)return QG_SAFE_INVALID;memcpy(o.output,argv[i],(n+1)*sizeof(wchar_t));}
    }
    if(!(seen&1u)||!qg_safe_options_valid(&o))return QG_SAFE_INVALID;
    *out=o;return QG_SAFE_VALID;
}
