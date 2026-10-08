#define _POSIX_C_SOURCE 200809L
#define QG_SENSOR_MOCK
#include "win32_stubs.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../src/sensor_test_win.c"

/* The production controller AND thread procedures above run unchanged. Only
   Windows APIs and metric provider calls below are substituted. Real pthreads
   preserve event/thread lifetimes; a 100x clock keeps 60s cases under one second. */
struct MockHandle {
 int kind, fd; bool manual;
 _Atomic bool signaled;
 pthread_t thread;
 DWORD (*fn)(LPVOID); LPVOID arg;
};
struct QgMetrics { uint32_t mask; };
static const char *scenario;
static _Thread_local DWORD last_error;
static _Thread_local DWORD (*current_proc)(LPVOID);
static _Atomic unsigned probe_count;
static _Atomic unsigned open_handles, samples, creates, destroys, writes;
static _Atomic bool collecting;
static HANDLE cancel_handle;
static uint64_t clock_start;
static uint64_t cancel_at;
static struct MockHandle process_handle = {.kind = 9};
static struct MockHandle stdout_handle = {.kind = 3, .fd = -1};
static uint64_t ns(void) {
 struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
 return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
static bool is(const char *s) { return strcmp(scenario,s)==0; }
static void pause_real(void) { struct timespec t={0,100000}; nanosleep(&t,NULL); }
static void delay_ms(uint64_t ms) {
 uint64_t stop=GetTickCount64()+ms;
 while(GetTickCount64()<stop) pause_real();
}
static HANDLE make(int kind) {
 HANDLE h=calloc(1,sizeof(*h)); assert(h);h->kind=kind;h->fd=-1;atomic_fetch_add(&open_handles,1u);return h;
}
HANDLE GetCurrentProcess(void) {return &process_handle;}
DWORD GetCurrentProcessId(void) {return 4321u;}
DWORD GetLastError(void) {return last_error;}
uint64_t GetTickCount64(void) {return 100000u+(ns()-clock_start)/(is("maximum_run")?1000u:10000u);}
void Sleep(DWORD ms) {delay_ms(ms);}
LONG InterlockedIncrement(volatile LONG *p) {return __atomic_add_fetch(p,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *p) {return __atomic_sub_fetch(p,1,__ATOMIC_SEQ_CST);}
PVOID InterlockedExchangePointer(PVOID volatile *p,PVOID n) {return __atomic_exchange_n(p,n,__ATOMIC_SEQ_CST);}
PVOID InterlockedCompareExchangePointer(PVOID volatile *p,PVOID n,PVOID cmp) {
 __atomic_compare_exchange_n(p,&cmp,n,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return cmp;
}
LONG InterlockedExchange(volatile LONG *p,LONG n) {return __atomic_exchange_n(p,n,__ATOMIC_SEQ_CST);}
LONG InterlockedCompareExchange(volatile LONG *p,LONG n,LONG cmp) {
 __atomic_compare_exchange_n(p,&cmp,n,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return cmp;
}
BOOL CloseHandle(HANDLE h) {
 assert(h&&h!=INVALID_HANDLE_VALUE&&h!=&process_handle);
 if(h->kind==2) {assert(atomic_load(&h->signaled));assert(pthread_join(h->thread,NULL)==0);}
 if(h->fd>=0) close(h->fd);
 if(h==cancel_handle) cancel_handle=NULL;
 free(h);atomic_fetch_sub(&open_handles,1u);return TRUE;
}
BOOL SetEvent(HANDLE h) {assert(h&&h->kind==1);atomic_store(&h->signaled,true);return TRUE;}
DWORD WaitForMultipleObjects(DWORD n,const HANDLE *hs,BOOL all,DWORD timeout) {
 uint64_t end=GetTickCount64()+timeout;assert(!all);
 for(;;) {
  if(cancel_handle&&cancel_at&&GetTickCount64()>=cancel_at) SetEvent(cancel_handle);
  for(DWORD i=0;i<n;i++) {
   HANDLE h=hs[i]; assert(h&&(h->kind==1||h->kind==2));
   if(atomic_load(&h->signaled)) {
    if(h->kind==1&&!h->manual&&!atomic_exchange(&h->signaled,false)) continue;
    return WAIT_OBJECT_0+i;
   }
  }
  if(timeout!=INFINITE&&GetTickCount64()>=end) return WAIT_TIMEOUT;
  pause_real();
 }
}
DWORD WaitForSingleObject(HANDLE h,DWORD timeout) {return WaitForMultipleObjects(1,&h,FALSE,timeout);}
static void *trampoline(void *p) {
 HANDLE h=p;current_proc=h->fn;h->fn(h->arg);atomic_store(&h->signaled,true);return NULL;
}
HANDLE CreateThread(void *security,size_t stack,DWORD (*fn)(LPVOID),LPVOID arg,DWORD flags,DWORD *id) {
 (void)security;(void)stack;(void)flags;(void)id;
 if(is("thread_failure")&&fn==collect_thread) {last_error=8u;return NULL;}
 HANDLE h=make(2);h->fn=fn;h->arg=arg;assert(pthread_create(&h->thread,NULL,trampoline,h)==0);return h;
}
BOOL CancelSynchronousIo(HANDLE h) {(void)h;return TRUE;}
BOOL TerminateProcess(HANDLE h,UINT code) {
 assert(h==GetCurrentProcess());
 if(atomic_load(&collecting)) assert(atomic_load(&destroys)==0u);
 _Exit((int)code);
}
void ExitProcess(UINT code) {_Exit((int)code);}
HANDLE CreateMutexW(void *s,BOOL owned,const wchar_t *name) {
 (void)s;assert(!owned);assert(wcscmp(name,L"Local\\QuietGuard.SensorTest.v1")==0);
 last_error=is("singleton")?ERROR_ALREADY_EXISTS:0u;return make(4);
}
HANDLE CreateEventW(void *s,BOOL manual,BOOL initial,const wchar_t *name) {
 (void)s;HANDLE h=make(1);h->manual=manual!=0;atomic_store(&h->signaled,initial!=0);last_error=0u;
 if(name) {assert(wcscmp(name,L"Local\\QuietGuard.SensorTest.4321.Cancel")==0);cancel_handle=h;}
 return h;
}
BOOL SetConsoleCtrlHandler(BOOL (*fn)(DWORD),BOOL add) {(void)fn;(void)add;return TRUE;}
HANDLE CreateFileW(const wchar_t *name,DWORD access,DWORD sharing,void *s,DWORD creation,DWORD attrs,HANDLE template) {
 (void)name;(void)s;(void)template;
 if(creation==OPEN_EXISTING) {
  assert(access==FILE_READ_ATTRIBUTES&&sharing==FILE_SHARE_READ);
  assert(attrs==(FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT));return make(6);
 }
 assert(access==GENERIC_WRITE&&sharing==0&&creation==CREATE_NEW);
 if(is("output_open_hang"))for(;;)pause_real();
 if(is("output_open_error")) {last_error=5u;return INVALID_HANDLE_VALUE;}
 HANDLE h=make(3);h->fd=open(getenv("QG_MOCK_REPORT"),O_WRONLY|O_CREAT|O_EXCL,0600);
 if(h->fd<0){CloseHandle(h);last_error=5u;return INVALID_HANDLE_VALUE;}return h;
}
BOOL GetFileInformationByHandle(HANDLE h,BY_HANDLE_FILE_INFORMATION *info) {
 assert(h->kind==6);info->dwFileAttributes=FILE_ATTRIBUTE_DIRECTORY;
 if(is("reparse_output")||is("mixed_reparse_output"))info->dwFileAttributes|=FILE_ATTRIBUTE_REPARSE_POINT;
 return TRUE;
}
DWORD GetFileType(HANDLE h) {return h==&stdout_handle?FILE_TYPE_PIPE:FILE_TYPE_DISK;}
DWORD GetFinalPathNameByHandleW(HANDLE h,wchar_t *out,DWORD cap,DWORD flags) {
 const wchar_t *s=is("remote_output")?L"\\\\?\\UNC\\server\\file.json":L"\\\\?\\C:\\test.json";
 (void)h;(void)flags;assert(wcslen(s)<cap);wcscpy(out,s);return (DWORD)wcslen(s);
}
DWORD GetDriveTypeW(const wchar_t *root) {(void)root;return DRIVE_FIXED;}
HANDLE GetStdHandle(DWORD n) {
 (void)n;if(is("stdout_missing"))return NULL;
 stdout_handle.fd=open(getenv("QG_MOCK_REPORT"),O_WRONLY|O_CREAT|O_EXCL,0600);
 assert(stdout_handle.fd>=0);return &stdout_handle;
}
BOOL DuplicateHandle(HANDLE a,HANDLE b,HANDLE c,HANDLE *d,DWORD e,BOOL f,DWORD g) {
 (void)a;(void)c;(void)e;(void)f;(void)g;assert(b==&stdout_handle);*d=make(3);(*d)->fd=dup(b->fd);assert((*d)->fd>=0);return TRUE;
}
BOOL WriteFile(HANDLE h,const void *p,DWORD n,DWORD *written,void *overlap) {
 (void)overlap;
 if(is("preflight_error")|| (n&&is("output_write_error"))) {last_error=ERROR_WRITE_FAULT;return FALSE;}
 if((n&&(is("output_hang")||is("stdout_hang")))||is("preflight_hang")) for(;;) pause_real();
 if(n) atomic_fetch_add(&writes,1u);
 ssize_t done=write(h->fd,p,n);if(done<0){last_error=ERROR_WRITE_FAULT;return FALSE;}
 *written=(DWORD)done;return TRUE;
}
BOOL GetProcessTimes(HANDLE h,FILETIME *c,FILETIME *e,FILETIME *k,FILETIME *u) {
 if(current_proc==probe_thread) {
  unsigned index=atomic_fetch_add(&probe_count,1u);
  if(is("initial_probe_hang")||(is("final_probe_hang")&&index==1u))for(;;)pause_real();
  if(index==0u&&(is("probe_recover")||is("cancel_probe")))delay_ms(1500u);
  if(index==0u&&is("slow_probe_and_create"))delay_ms(700u);
 }
 (void)h;memset(c,0,sizeof(*c));memset(e,0,sizeof(*e));memset(k,0,sizeof(*k));memset(u,0,sizeof(*u));
 k->dwLowDateTime=10000u;u->dwLowDateTime=20000u;return !is("resources_unknown");
}
BOOL GetProcessMemoryInfo(HANDLE h,PROCESS_MEMORY_COUNTERS *m,DWORD size) {
 (void)h;(void)size;m->PrivateUsage=300u;m->WorkingSetSize=400u;m->PeakPagefileUsage=500u;m->PeakWorkingSetSize=600u;return !is("resources_unknown");
}
BOOL GetProcessHandleCount(HANDLE h,DWORD *n) {(void)h;*n=atomic_load(&open_handles);return !is("resources_unknown");}
BOOL GetProcessIoCounters(HANDLE h,IO_COUNTERS *io) {(void)h;memset(io,0,sizeof(*io));io->ReadTransferCount=42u;return !is("resources_unknown");}
HANDLE CreateToolhelp32Snapshot(DWORD flags,DWORD pid) {
 (void)pid;assert(flags==TH32CS_SNAPTHREAD);return is("resources_unknown")?INVALID_HANDLE_VALUE:make(5);
}
BOOL Thread32First(HANDLE h,THREADENTRY32 *t) {(void)h;t->th32OwnerProcessID=GetCurrentProcessId();return TRUE;}
BOOL Thread32Next(HANDLE h,THREADENTRY32 *t) {(void)h;(void)t;last_error=ERROR_NO_MORE_FILES;return FALSE;}
void GetSystemTimeAsFileTime(FILETIME *t) {uint64_t n=GetTickCount64()*10000u;t->dwLowDateTime=(DWORD)n;t->dwHighDateTime=(DWORD)(n>>32);}
QgMetrics *qg_metrics_create(uint32_t mask) {
 assert(current_proc==collect_thread);
 atomic_fetch_add(&creates,1u);if(is("create_error"))return NULL;
 if(is("slow_probe_and_create"))delay_ms(700u);
 if(is("create_hang"))for(;;)pause_real();
 QgMetrics *m=malloc(sizeof(*m));assert(m);m->mask=mask;return m;
}
void qg_metrics_destroy(QgMetrics *m) {
 assert(current_proc==collect_thread);
 assert(!atomic_load(&collecting));
 if(is("destroy_hang"))for(;;)pause_real();
 atomic_fetch_add(&destroys,1u);free(m);
}
void qg_metrics_sample(QgMetrics *m,bool veto,QgSample *s) {
 assert(current_proc==collect_thread);
 assert(veto);assert(!atomic_exchange(&collecting,true));unsigned index=atomic_fetch_add(&samples,1u);
 memset(s,0,sizeof(*s));s->at_ms=GetTickCount64();s->veto=true;
 if(is("timeout_hang")||is("cancel_hang")||is("duration_hang"))for(;;)pause_real();
 if(is("timeout_recover")||is("cancel_recover"))delay_ms(1500u);
 if(is("duration_deadline")&&index>=11u)delay_ms(10000u);
 s->valid=(is("no_valid")||index==0u)?0u:m->mask;
 if(is("partial"))s->valid=QG_CPU;
 s->discontinuity=index==0u;s->value[0]=1.5;s->value[1]=2.5;s->value[2]=3.5;s->value[3]=4.5;
 atomic_store(&collecting,false);
}
static void maximum_diagnostics(QgDiagnostics *d) {
 memset(d,0,sizeof(*d));
 d->provider_valid_mask=QG_ALL;d->clock_status=UINT32_MAX;d->session_status=UINT32_MAX;
 d->clock_reason=UINT32_MAX;d->session_reason=UINT32_MAX;
 d->elapsed_tick_ms=UINT64_MAX;d->elapsed_awake_ms=UINT64_MAX;
 d->collection_tick_ms=UINT64_MAX;d->collection_awake_ms=UINT64_MAX;
 for(unsigned i=0;i<QG_METRICS;++i) {
  QgMetricDiagnostic *m=&d->metric[i];
  m->stage=QG_D_PDH_EXPAND_SIZE;m->api_status=UINT32_MAX;m->counter_status=UINT32_MAX;
  m->item_count=UINT32_MAX;m->item_index=UINT32_MAX;m->has_formatted=true;m->has_uncapped=true;
  m->formatted_value=-DBL_MAX;m->uncapped_value=-DBL_MAX;
  m->uncapped_api_status=UINT32_MAX;m->uncapped_counter_status=UINT32_MAX;
  if(i==1u||i==2u) {
   for(size_t n=0;n<QG_DIAG_NAME_CAP-1u;++n)m->instance[n]=(wchar_t)0xffffu;
   m->raw_previous=(QgRawDiagnostic){UINT32_MAX,UINT32_MAX,UINT32_MAX,
    INT64_MIN,INT64_MIN,UINT64_MAX,UINT32_MAX,true};
   m->raw_current=m->raw_previous;
  }
 }
}
void qg_metrics_sample_diagnostic(QgMetrics *m,bool veto,QgSample *s,QgDiagnostics *d) {
 assert(current_proc==collect_thread&&d&&atomic_load(&creates)==1u&&atomic_load(&destroys)==0u);
 qg_metrics_sample(m,veto,s);
 memset(d,0,sizeof(*d));d->provider_valid_mask=s->valid;
 d->clock_start_ok=true;d->clock_end_ok=true;
 d->continuous_before=!s->discontinuity;d->continuous_after=true;
 d->sessions_checked=true;d->sessions_ok=true;
 d->elapsed_tick_ms=5000u;d->elapsed_awake_ms=5000u;
 d->collection_tick_ms=7u;d->collection_awake_ms=7u;
 for(unsigned i=0;i<QG_METRICS;++i) {
  QgMetricDiagnostic *metric=&d->metric[i];
  metric->stage=!(m->mask&(1u<<i))?QG_D_DISABLED:
   ((s->valid&(1u<<i))?QG_D_OK:QG_D_BASELINE);
  metric->has_formatted=(s->valid&(1u<<i))!=0u;metric->formatted_value=s->value[i];
 }
 if(is("diagnostic_disk_failure")||is("diagnostic_edges")) {
  QgMetricDiagnostic *disk=&d->metric[2];
  s->valid=0u;d->provider_valid_mask=QG_CPU|QG_GPU|QG_NET;
  disk->stage=QG_D_PDH_RANGE;disk->has_formatted=true;disk->formatted_value=100.25;
  disk->has_uncapped=true;disk->uncapped_value=100.25;disk->item_count=3u;disk->item_index=1u;
  wcscpy(disk->instance,L"0 C:");
  disk->raw_previous=(QgRawDiagnostic){0u,1u,542180608u,INT64_MIN,INT64_MAX,UINT64_MAX,7u,true};
  disk->raw_current=(QgRawDiagnostic){0u,0u,542180608u,100u,200u,300u,2u,true};
  for(unsigned i=0;i<QG_METRICS;++i)if(i!=2u)d->metric[i].stage=QG_D_OK;
  if(is("diagnostic_edges")) {
   static const wchar_t name[]={L'"',L'\\',L'\n',0x4e2d,0xd83d,0xde00,0xd800,L'X',0xdc00,0x1f600,0x110000,0};
   memcpy(disk->instance,name,sizeof(name));disk->formatted_value=NAN;disk->uncapped_value=INFINITY;
   disk->raw_current.present=false;disk->raw_current.api_status=0xc0000bc6u;
   d->metric[1].stage=(QgDiagnosticStage)UINT32_MAX;
   for(size_t i=0;i<QG_DIAG_NAME_CAP;++i)d->metric[1].instance[i]=L'A';
  }
 }
 if(is("diagnostic_clock_failure")) {
  s->valid=0u;s->discontinuity=true;d->provider_valid_mask=QG_ALL;
  d->clock_end_ok=false;d->clock_status=5u;d->clock_reason=6u;d->continuous_after=false;
 }
 if(is("diagnostic_session_failure")) {
  s->valid=0u;d->provider_valid_mask=QG_ALL;d->sessions_ok=false;d->session_status=5u;d->session_reason=3u;
 }
 if(is("maximum_run"))maximum_diagnostics(d);
}

static void serialization_bounds(void) {
 Record r={0};maximum_diagnostics(&r.diagnostics);
 r.elapsed_ms=UINT64_MAX;r.collector_ms=UINT64_MAX;r.sample.at_ms=UINT64_MAX;
 r.sample.valid=UINT32_MAX;r.phase=(QgPhase)UINT32_MAX;
 for(unsigned i=0;i<QG_METRICS;++i)r.sample.value[i]=-DBL_MAX;
 r.process=(Resources){true,true,true,true,true,true,UINT64_MAX,UINT64_MAX,
  UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,
  UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
 Json j={calloc(REPORT_CAP,1u),0,false};assert(j.data);
 write_record(&j,&r,UINT32_MAX);assert(!j.failed);
 /* Use the actual serializer with widest numeric spellings and both bounded
    PDH names fully escaped; reserve 8 KiB for root fields/build identity. */
 if(QG_SAFE_MAX_SAMPLES*j.length+8192u>=REPORT_CAP)
  fprintf(stderr,"Maximum record: %zu; report bound: %zu; cap: %u\n",j.length,
   QG_SAFE_MAX_SAMPLES*j.length+8192u,REPORT_CAP);
 assert(QG_SAFE_MAX_SAMPLES*j.length+8192u<REPORT_CAP);
 memset(j.data,'X',REPORT_CAP);j.data[REPORT_CAP-1u]=0;
 j.length=REPORT_CAP-2u;add(&j,"1234");assert(j.failed&&j.length==REPORT_CAP-2u);
 free(j.data);
}
int main(int argc,char **argv) {
 assert(argc==2);scenario=argv[1];clock_start=ns();
 serialization_bounds();
 QgSafeOptions options={QG_ALL,60u,5u,1u,L"C:\\test.json"};
 if(is("cancel_idle"))cancel_at=107000u;
 if(is("cancel_hang")||is("cancel_recover")||is("cancel_probe"))cancel_at=100500u;
 if(is("maximum_run")){options.duration_seconds=900u;options.timeout_seconds=30u;}
 if(is("diagnostic_cpu_only"))options.mask=QG_CPU;
 if(is("duration_deadline"))options.timeout_seconds=30u;
 if(is("stdout_normal")||is("stdout_missing")||is("stdout_hang"))options.output[0]=0;
 if(is("mixed_reparse_output"))wcscpy(options.output,L"C:/folder\\test.json");
 if(is("invalid"))options.duration_seconds=1u;
 int result=qg_run_sensor_test(&options);
 assert(atomic_load(&open_handles)==0u);
 if(atomic_load(&creates)&&!is("create_error"))assert(atomic_load(&destroys)==1u);
 if(result==0||result==5||result==6||result==7||result==8||result==9||is("thread_failure"))assert(atomic_load(&writes)==1u);
 if(result==2||result==3||result==4)assert(atomic_load(&creates)==0u);
 return result;
}
