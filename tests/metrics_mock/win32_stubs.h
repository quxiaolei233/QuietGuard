/* Deterministic API-semantic stubs for the host-only provider tests.
   These are intentionally NOT Windows ABI/driver/localization emulation.
   The application never includes this file in a Windows build. */
#ifndef QG_WIN32_STUBS_H
#define QG_WIN32_STUBS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <float.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include "../../src/qg.h"
#define ERROR_SUCCESS 0
#define NO_ERROR 0
#define FALSE 0
#define TRUE 1
#define ALL_PROCESSOR_GROUPS 65535u
#define PDH_MORE_DATA 10
#define PDH_MAX_COUNTER_PATH 2048u
#define PDH_REFRESHCOUNTERS 4
#define PDH_FMT_DOUBLE 1
#define PDH_FMT_NOCAP100 2
#define PDH_CSTATUS_VALID_DATA 0u
#define PDH_CSTATUS_NEW_DATA 1u
#define PDH_CSTATUS_NO_INSTANCE 0x800007D1u
#define PDH_CSTATUS_INVALID_DATA 0xC0000BBAu
#define PDH_INVALID_DATA ((int)0xC0000BC6u)
#define PERF_PRECISION_100NS_TIMER 542573824u
#define PERF_100NSEC_TIMER 542180608u
#define IF_TYPE_SOFTWARE_LOOPBACK 24u
#define IfOperStatusUp 1
#define WTS_CURRENT_SERVER_HANDLE NULL
#define WTSUserName 1
#define _wcsicmp wcscmp
#define WTSDisconnected 4
#define WTSListen 6
#define WTSIdle 5
#define WTSActive 0
#define WTSConnected 1
#define WTSConnectQuery 2
#define WTSShadow 3
#define WTSDown 8

typedef uint32_t DWORD, ULONG;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef int PDH_STATUS, IF_OPER_STATUS, NET_IF_ADMIN_STATUS, NET_IF_MEDIA_CONNECT_STATE;
typedef struct { uint32_t dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct QgMetrics QgMetrics;
typedef struct Counter { wchar_t path[256]; } Counter;
typedef struct { Counter *c[16]; size_t count; } Query;
typedef Query *PDH_HQUERY;
typedef Counter *PDH_HCOUNTER;
typedef struct { wchar_t *szFullPath; } PDH_COUNTER_INFO_W;
typedef struct { wchar_t *szInstanceName, *szParentInstance; } PDH_COUNTER_PATH_ELEMENTS_W;
typedef struct { DWORD CStatus; double doubleValue; } PDH_FMT_COUNTERVALUE;
typedef struct {
    DWORD CStatus;
    FILETIME TimeStamp;
    LONGLONG FirstValue, SecondValue;
    DWORD MultiCount;
} PDH_RAW_COUNTER;
typedef struct { struct { uint64_t Value; } InterfaceLuid; uint64_t InOctets, OutOctets; ULONG Type; int OperStatus, AdminStatus, MediaConnectState; struct { bool FilterInterface; } InterfaceAndOperStatusFlags; } MIB_IF_ROW2;
typedef struct { DWORD NumEntries; MIB_IF_ROW2 Table[8]; } MIB_IF_TABLE2;
typedef struct { DWORD SessionId; int State; } WTS_SESSION_INFOW;
static uint64_t tick = 1000, awake = 10000000, cpu_idle = 100, cpu_kernel = 200, cpu_user = 100;
static unsigned groups = 1, processors = 4;
static bool clock_error, cpu_error, net_error, pdh_error, wts_error, username_error;
static MIB_IF_TABLE2 net_table;
static size_t net_allocs, net_frees, pdh_opens, pdh_closes, session_calls;
static const wchar_t *gpu_names[8], *disk_names[8];
static double gpu_values[8], disk_values[8];
static DWORD gpu_status[8], disk_status[8];
static PDH_STATUS gpu_format_error[8], disk_format_error[8];
static PDH_STATUS gpu_raw_error[8], disk_raw_error[8];
static DWORD gpu_types[8], disk_types[8];
static PDH_RAW_COUNTER gpu_raw[8], disk_raw[8];
/* Indices are GPU=0 and disk=1. API errors are independent of CStatus. */
static PDH_STATUS pdh_open_error, pdh_localize_error, pdh_info_size_error, pdh_info_error;
static PDH_STATUS pdh_expand_size_error[2], pdh_expand_error[2], pdh_collect_error[2];
static PDH_STATUS pdh_parse_error, pdh_add_error;
static size_t pdh_raw_calls, pdh_capped_calls[2], pdh_uncapped_calls[2];
static size_t gpu_count, disk_count, session_count;
static WTS_SESSION_INFOW session_rows[4];
static const wchar_t *session_users[4];
static bool suspend_in_collect;
static size_t clock_query_calls, clock_fail_on_call;
static DWORD mock_last_error = 5u;
static inline DWORD GetLastError(void) { return mock_last_error; }
static uint64_t GetTickCount64(void) { return tick; }
static int QueryUnbiasedInterruptTime(ULONGLONG *u) { ++clock_query_calls; if(clock_error || clock_query_calls==clock_fail_on_call)return FALSE; *u=awake; return TRUE; }
static unsigned GetActiveProcessorGroupCount(void) { return groups; }
static DWORD GetActiveProcessorCount(unsigned g) { (void)g; return processors; }
static FILETIME ft(uint64_t t) { FILETIME f = {(uint32_t)t,(uint32_t)(t>>32)}; return f; }
static int GetSystemTimes(FILETIME *i,FILETIME *k,FILETIME *u) { *i=ft(cpu_idle); *k=ft(cpu_kernel); *u=ft(cpu_user); return !cpu_error; }
static int GetIfTable2(MIB_IF_TABLE2 **t) { if(net_error)return 1; *t=malloc(sizeof(**t)); **t=net_table; ++net_allocs; return 0; }
static void FreeMibTable(void *t) { free(t); ++net_frees; }
static int PdhOpenQueryW(const wchar_t *s, int n, PDH_HQUERY *q) { (void)s;(void)n; if(pdh_open_error)return pdh_open_error; *q=calloc(1,sizeof(**q)); ++pdh_opens; return 0; }
static int PdhCloseQuery(PDH_HQUERY q) { for(size_t i=0;i<q->count;++i) free(q->c[i]); free(q); ++pdh_closes; return 0; }
static int PdhAddCounterW(PDH_HQUERY q,const wchar_t *p,int n,PDH_HCOUNTER *c) { (void)n; if(pdh_add_error && !wcschr(p,L'*'))return pdh_add_error; assert(q->count<16); *c=calloc(1,sizeof(**c)); wcscpy((*c)->path,p); q->c[q->count++]=*c; return 0; }
static int PdhAddEnglishCounterW(PDH_HQUERY q,const wchar_t *p,int n,PDH_HCOUNTER *c) { if(pdh_localize_error)return pdh_localize_error; return PdhAddCounterW(q,p,n,c); }
static int PdhGetCounterInfoW(PDH_HCOUNTER c,int e,DWORD *sz,PDH_COUNTER_INFO_W *i) { (void)e; size_t bytes=sizeof(*i)+(wcslen(c->path)+1)*sizeof(wchar_t); if(!i){if(pdh_info_size_error)return pdh_info_size_error; *sz=(DWORD)bytes;return PDH_MORE_DATA;} if(pdh_info_error)return pdh_info_error; assert(*sz>=bytes); i->szFullPath=(wchar_t*)(i+1);wcscpy(i->szFullPath,c->path);return 0; }
static bool is_gpu(const wchar_t *p) { return wcsstr(p,L"GPU Engine")!=NULL; }
static int PdhExpandWildCardPathW(const wchar_t *ds,const wchar_t *p,wchar_t *out,DWORD *sz,int f) { (void)ds;(void)f; bool g=is_gpu(p);size_t kind=g?0u:1u;const wchar_t **names=g?gpu_names:disk_names;size_t count=g?gpu_count:disk_count;DWORD needed=1;for(size_t i=0;i<count;++i)needed+=(DWORD)wcslen(names[i])+1;if(needed==1)needed=2;if(!out){if(pdh_expand_size_error[kind])return pdh_expand_size_error[kind];*sz=needed;return PDH_MORE_DATA;} if(pdh_expand_error[kind])return pdh_expand_error[kind];if(*sz<needed)return PDH_MORE_DATA; for(size_t i=0;i<count;++i){wcscpy(out,names[i]);out+=wcslen(out)+1;}*out=0;if(!count)out[1]=0;*sz=needed;return 0; }
static int PdhParseCounterPathW(const wchar_t *p,PDH_COUNTER_PATH_ELEMENTS_W *e,DWORD *sz,int f) { (void)f;if(pdh_parse_error)return pdh_parse_error;const wchar_t *a=wcschr(p,L'('),*b=wcschr(p,L')');if(!a||!b||b<=a)return 1; size_t n=(size_t)(b-a-1);assert(*sz>=sizeof(*e)+(n+1)*sizeof(wchar_t));e->szInstanceName=(wchar_t*)(e+1);e->szParentInstance=NULL;wmemcpy(e->szInstanceName,a+1,n);e->szInstanceName[n]=0;return 0; }
static int PdhCollectQueryData(PDH_HQUERY q) { if(suspend_in_collect){tick+=10000;suspend_in_collect=false;}if(pdh_error)return pdh_error;return pdh_collect_error[q->count && is_gpu(q->c[0]->path)?0u:1u]; }
/* Model the documented PDH_FMT_NOCAP100 choice so a flag-only regression
   cannot silently pass. https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcountervalue */
static int PdhGetFormattedCounterValue(PDH_HCOUNTER c,int f,DWORD *t,PDH_FMT_COUNTERVALUE *v)
{
    bool g=is_gpu(c->path);
    size_t kind=g?0u:1u, count=g?gpu_count:disk_count;
    const wchar_t **names=g?gpu_names:disk_names;
    assert(f & PDH_FMT_DOUBLE);
    if(f & PDH_FMT_NOCAP100)++pdh_uncapped_calls[kind];else ++pdh_capped_calls[kind];
    for(size_t i=0;i<count;++i)if(!wcscmp(c->path,names[i])){
        v->doubleValue=g?gpu_values[i]:disk_values[i];
        v->CStatus=g?gpu_status[i]:disk_status[i];
        if(t)*t=g?gpu_types[i]:disk_types[i];
        if(!(f & PDH_FMT_NOCAP100) && isfinite(v->doubleValue) && v->doubleValue>100.0)
            v->doubleValue=100.0;
        /* Populate even on API failure: callers must check the return code. */
        return g?gpu_format_error[i]:disk_format_error[i];
    }
    v->CStatus=PDH_CSTATUS_NO_INSTANCE;
    return PDH_INVALID_DATA;
}
/* A missing instance may still return API success with an invalid CStatus.
   https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetrawcountervalue */
static inline int PdhGetRawCounterValue(PDH_HCOUNTER c,DWORD *t,PDH_RAW_COUNTER *v)
{
    bool g=is_gpu(c->path);
    const wchar_t **names=g?gpu_names:disk_names;
    size_t count=g?gpu_count:disk_count;
    ++pdh_raw_calls;
    for(size_t i=0;i<count;++i)if(!wcscmp(c->path,names[i])){
        *v=g?gpu_raw[i]:disk_raw[i];
        if(t)*t=g?gpu_types[i]:disk_types[i];
        return g?gpu_raw_error[i]:disk_raw_error[i];
    }
    memset(v,0,sizeof(*v));v->CStatus=PDH_CSTATUS_NO_INSTANCE;
    return ERROR_SUCCESS;
}
static int WTSEnumerateSessionsW(void *h,int a,int b,WTS_SESSION_INFOW **out,DWORD *n) { (void)h;(void)a;(void)b;++session_calls;if(wts_error)return 0;*n=(DWORD)session_count;*out=malloc(sizeof(session_rows));memcpy(*out,session_rows,sizeof(session_rows));return 1; }
static int WTSQuerySessionInformationW(void *h,DWORD id,int x,wchar_t **out,DWORD *n) { (void)h;(void)x;if(username_error)return 0;const wchar_t *s=session_users[id];*n=(DWORD)((wcslen(s)+1)*sizeof(wchar_t));*out=malloc(*n);memcpy(*out,s,*n);return 1; }
static void WTSFreeMemory(void *p) { free(p); }

#endif
