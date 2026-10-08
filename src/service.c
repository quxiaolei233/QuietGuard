/* QuietGuard service: fixed local protocol; no command/path execution surface. */
#include "qg.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define QG_PIPE L"\\\\.\\pipe\\QuietGuard.v1"
#define QG_GATE L"Global\\QuietGuard.Monitor.v1"
#define QG_IO_MS 2000u
#define QG_ACK 0xa5u
#define QG_INHIBIT_CANCEL 1L
#define QG_INHIBIT_STOP 2L
#define QG_RESET_UNIT 4L
#define QG_COMMITTED 0x40000000L
#define QG_CLIENT_RIGHTS (FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES|SYNCHRONIZE)
/* AU must NOT receive FILE_APPEND_DATA: for a pipe it is CREATE_PIPE_INSTANCE. */
#define QG_PIPE_SD L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;0x00100183;;;AU)"
#define QG_DATA_SD L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;LS)"
#define QG_FILE_SD L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GR;;;LS)"
#define QG_EXE_SD L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;AU)(A;;GRGX;;;LS)"
#define QG_BIN_SD L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;AU)(A;OICI;GRGX;;;LS)"

typedef struct {
    CRITICAL_SECTION lock;
    HANDLE stop, changed, pipe, gate;
    SERVICE_STATUS_HANDLE status_handle;
    SERVICE_STATUS status;
    QgEngine engine;
    QgSample sample;
    volatile LONG generation, revision, commit_gate;
    DWORD error;
    bool dry_run;
    wchar_t config_path[MAX_PATH];
} QgService;
static QgService svc;
_Static_assert(sizeof(QgRequest)<=256,"Bounded request ABI");
_Static_assert(sizeof(QgReply)<=512,"Bounded reply ABI");

static DWORD last_error(void) { DWORD e=GetLastError(); return e?e:ERROR_GEN_FAILURE; }
/* Single linearization word: persistent cancel/stop bits, transient reset count,
   or the irreversible COMMITTED bit. Every invalidation competes with commit's CAS. */
static bool inhibit_commit(LONG reason) {
    LONG old=InterlockedCompareExchange(&svc.commit_gate,0,0);
    for(;;) {
        if(old&QG_COMMITTED)return false;
        LONG next=reason==QG_RESET_UNIT?old+QG_RESET_UNIT:old|reason;
        LONG seen=InterlockedCompareExchange(&svc.commit_gate,next,old);
        if(seen==old)return true;
        old=seen;
    }
}
static void stop_service(void) {
    (void)inhibit_commit(QG_INHIBIT_STOP);
    if(svc.stop)SetEvent(svc.stop);
}

static void copy_config(QgConfig *d,const QgConfig *s) {
    memset(d,0,sizeof(*d)); d->mask=s->mask; d->idle_seconds=s->idle_seconds;
    d->sample_seconds=s->sample_seconds; d->warning_seconds=s->warning_seconds;
    for(unsigned i=0;i<4;i++)d->threshold[i]=s->threshold[i];
    d->enabled=s->enabled; d->veto_active_session=s->veto_active_session;
}
static bool descriptor(const wchar_t *s,PSECURITY_DESCRIPTOR *sd,SECURITY_ATTRIBUTES *sa) {
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(s,SDDL_REVISION_1,sd,NULL))return false;
    sa->nLength=sizeof(*sa); sa->lpSecurityDescriptor=*sd; sa->bInheritHandle=FALSE; return true;
}
static bool has_argument(const wchar_t *arg) {
    int argc=0; wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc); bool found=false;
    if(!argv)return false;
    for(int i=1;i<argc;i++)if(!wcscmp(argv[i],arg))found=true;
    LocalFree(argv); return found;
}
static bool sid_is(PSID sid,WELL_KNOWN_SID_TYPE type) { return sid&&IsWellKnownSid(sid,type)!=FALSE; }

bool qg_is_admin(void) {
    HANDLE token=NULL; BOOL member=FALSE; BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD n=sizeof(sid);
    /* CheckTokenMembership(NULL) correctly uses an impersonation token when present. */
    if(!CreateWellKnownSid(WinBuiltinAdministratorsSid,NULL,sid,&n) ||
       !CheckTokenMembership(NULL,sid,&member)||!member)return false;
    if(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&token)) {
        if(GetLastError()!=ERROR_NO_TOKEN||!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    }
    TOKEN_ELEVATION elevated; DWORD got=0;
    bool ok=GetTokenInformation(token,TokenElevation,&elevated,sizeof(elevated),&got)&&elevated.TokenIsElevated;
    CloseHandle(token); return ok;
}

HANDLE qg_monitor_mutex(void) {
    PSECURITY_DESCRIPTOR sd=NULL; SECURITY_ATTRIBUTES sa;
    if(!descriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;0x00100000;;;AU)",&sd,&sa))return NULL;
    /* Name lifetime is the gate. No mutex ownership must be released on another thread. */
    HANDLE h=CreateMutexExW(&sa,QG_GATE,0,SYNCHRONIZE); DWORD e=GetLastError(); LocalFree(sd);
    if(h&&e==ERROR_ALREADY_EXISTS){CloseHandle(h);h=NULL;SetLastError(ERROR_ALREADY_EXISTS);}
    return h;
}

static bool paths(wchar_t *bin_dir,wchar_t *exe,wchar_t *data_dir,wchar_t *config) {
    wchar_t pf[MAX_PATH],pd[MAX_PATH];
    if(FAILED(SHGetFolderPathW(NULL,CSIDL_PROGRAM_FILES,NULL,SHGFP_TYPE_CURRENT,pf))||
       FAILED(SHGetFolderPathW(NULL,CSIDL_COMMON_APPDATA,NULL,SHGFP_TYPE_CURRENT,pd)))return false;
    return swprintf(bin_dir,MAX_PATH,L"%ls\\QuietGuard",pf)>0 &&
        swprintf(exe,MAX_PATH,L"%ls\\QuietGuard\\QuietGuard.exe",pf)>0 &&
        swprintf(data_dir,MAX_PATH,L"%ls\\QuietGuard",pd)>0 &&
        swprintf(config,MAX_PATH,L"%ls\\QuietGuard\\service.ini",pd)>0;
}
/* Refuse reparse points, hardlinked files, non-admin owners and writable untrusted ACEs.
   Opening with OPEN_REPARSE_POINT validates the named object, not its redirect target. */
static DWORD trusted_path(const wchar_t *path,bool directory,bool missing_ok) {
    HANDLE h=CreateFileW(path,READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),NULL);
    if(h==INVALID_HANDLE_VALUE){DWORD e=last_error();return missing_ok&&e==ERROR_FILE_NOT_FOUND?ERROR_SUCCESS:e;}
    BY_HANDLE_FILE_INFORMATION info; PSECURITY_DESCRIPTOR sd=NULL; PSID owner=NULL; PACL acl=NULL;
    DWORD e=ERROR_ACCESS_DENIED;
    if(!GetFileInformationByHandle(h,&info)) {e=last_error();goto done;}
    if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
       !!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory||(!directory&&info.nNumberOfLinks!=1))goto done;
    e=GetSecurityInfo(h,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,NULL,&acl,NULL,&sd);
    if(e!=ERROR_SUCCESS)goto done;
    e=ERROR_ACCESS_DENIED;
    if((!sid_is(owner,WinBuiltinAdministratorsSid)&&!sid_is(owner,WinLocalSystemSid))||!acl)goto done;
    for(DWORD i=0;i<acl->AceCount;i++) {
        void *raw=NULL; if(!GetAce(acl,i,&raw))goto done;
        ACE_HEADER *header=(ACE_HEADER*)raw;
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE)goto done;
        ACCESS_ALLOWED_ACE *ace=(ACCESS_ALLOWED_ACE*)raw; PSID sid=&ace->SidStart;
        if(sid_is(sid,WinLocalSystemSid)||sid_is(sid,WinBuiltinAdministratorsSid))continue;
        /* Nobody else may write content, attributes, ACL/owner, delete or create children. */
        DWORD writes=GENERIC_ALL|GENERIC_WRITE|DELETE|WRITE_DAC|WRITE_OWNER|FILE_WRITE_DATA|
            FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|FILE_DELETE_CHILD;
        if(ace->Mask&writes)goto done;
    }
    e=ERROR_SUCCESS;
done:
    if(sd)LocalFree(sd);
    CloseHandle(h);return e;
}
static DWORD protected_directory(const wchar_t *path,const wchar_t *sddl) {
    PSECURITY_DESCRIPTOR sd=NULL; SECURITY_ATTRIBUTES sa;
    if(!descriptor(sddl,&sd,&sa))return last_error();
    BOOL made=CreateDirectoryW(path,&sa); DWORD e=made?ERROR_SUCCESS:last_error();LocalFree(sd);
    if(e!=ERROR_SUCCESS&&e!=ERROR_ALREADY_EXISTS)return e;
    return trusted_path(path,true,false);
}
/* Caller must be an elevated admin, including CONFIG's impersonated caller.
   Never overwrite an existing temp path; every temp gets an explicit protected DACL. */
static DWORD save_config(const wchar_t *path,const QgConfig *config) {
    char text[QG_CONFIG_MAX]; size_t length=qg_config_format(config,text,sizeof(text));
    if(!length)return ERROR_INVALID_DATA;
    DWORD e=trusted_path(path,false,true);if(e)return e;
    PSECURITY_DESCRIPTOR sd=NULL;SECURITY_ATTRIBUTES sa;
    if(!descriptor(QG_FILE_SD,&sd,&sa))return last_error();
    wchar_t tmp[MAX_PATH];HANDLE file=INVALID_HANDLE_VALUE;
    for(unsigned i=0;i<8;i++) {
        if(swprintf(tmp,MAX_PATH,L"%ls.%lu.%llu.%u.tmp",path,(unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64(),i)<=0){e=ERROR_FILENAME_EXCED_RANGE;break;}
        file=CreateFileW(tmp,GENERIC_WRITE,0,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file!=INVALID_HANDLE_VALUE)break;
        e=last_error();if(e!=ERROR_FILE_EXISTS&&e!=ERROR_ALREADY_EXISTS)break;
    }
    LocalFree(sd);if(file==INVALID_HANDLE_VALUE)return e;
    DWORD written=0;
    bool ok=WriteFile(file,text,(DWORD)length,&written,NULL)&&written==length&&FlushFileBuffers(file);
    e=ok?ERROR_SUCCESS:last_error();CloseHandle(file);
    if(ok&&!MoveFileExW(tmp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))e=last_error();
    if(e)DeleteFileW(tmp);
    return e;
}

static bool service_process(DWORD *pid) {
    SC_HANDLE scm=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);if(!scm)return false;
    SC_HANDLE service=OpenServiceW(scm,QG_SERVICE_NAME,SERVICE_QUERY_STATUS);
    SERVICE_STATUS_PROCESS status;DWORD n=0;bool ok=false;
    if(service) {
        ok=QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE*)&status,sizeof(status),&n)&&
            status.dwCurrentState==SERVICE_RUNNING&&status.dwProcessId!=0;
        if(ok&&pid)*pid=status.dwProcessId;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);return ok;
}
bool qg_service_installed(void) {
    SC_HANDLE scm=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);
    /* Access denied is conservatively treated as installed so login mode will not arm. */
    if(!scm)return true;
    SC_HANDLE service=OpenServiceW(scm,QG_SERVICE_NAME,SERVICE_QUERY_STATUS);
    DWORD e=service?ERROR_SUCCESS:last_error();if(service)CloseServiceHandle(service);CloseServiceHandle(scm);
    return e!=ERROR_SERVICE_DOES_NOT_EXIST;
}
static DWORD wait_operation(HANDLE h,OVERLAPPED *op,HANDLE stop,DWORD timeout,DWORD *bytes) {
    HANDLE handles[2]={op->hEvent,stop};DWORD n=stop?2:1;
    DWORD w=WaitForMultipleObjects(n,handles,FALSE,timeout);
    if(w==WAIT_OBJECT_0) return GetOverlappedResult(h,op,bytes,FALSE)?ERROR_SUCCESS:last_error();
    DWORD e=w==WAIT_TIMEOUT?ERROR_TIMEOUT:(w==WAIT_OBJECT_0+1?ERROR_OPERATION_ABORTED:last_error());
    /* Keep OVERLAPPED alive until cancellation completes. Named-pipe operations are cancellable. */
    CancelIoEx(h,op);GetOverlappedResult(h,op,bytes,TRUE);return e;
}
static DWORD pipe_io(HANDLE pipe,bool write,void *buffer,DWORD size,HANDLE stop,DWORD timeout) {
    OVERLAPPED op;memset(&op,0,sizeof(op));op.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!op.hEvent)return last_error();
    DWORD got=0;
    BOOL ok=write?WriteFile(pipe,buffer,size,&got,&op):ReadFile(pipe,buffer,size,&got,&op);
    DWORD e=ok?ERROR_SUCCESS:last_error();
    if(e==ERROR_IO_PENDING)e=wait_operation(pipe,&op,stop,timeout,&got);
    CloseHandle(op.hEvent);
    return e?e:(got==size?ERROR_SUCCESS:ERROR_INVALID_DATA);
}
bool qg_service_exchange(const QgRequest *req,QgReply *reply) {
    if(!req||!reply||req->magic!=QG_WIRE_MAGIC||req->command<QG_CMD_STATUS||req->command>QG_CMD_CONFIG) {
        SetLastError(ERROR_INVALID_PARAMETER);return false;
    }
    DWORD pid=0;if(!service_process(&pid)){SetLastError(ERROR_SERVICE_NOT_ACTIVE);return false;}
    HANDLE pipe=CreateFileW(QG_PIPE,QG_CLIENT_RIGHTS,0,NULL,OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IMPERSONATION,NULL);
    if(pipe==INVALID_HANDLE_VALUE)return false;
    ULONG server=0;DWORD error=ERROR_ACCESS_DENIED,mode=PIPE_READMODE_MESSAGE,current_pid=0;
    if(!GetNamedPipeServerProcessId(pipe,&server)||server!=pid||!service_process(&current_pid)||
       current_pid!=pid||!SetNamedPipeHandleState(pipe,&mode,NULL,NULL))goto done;
    QgRequest clean;memset(&clean,0,sizeof(clean));clean.magic=QG_WIRE_MAGIC;clean.command=req->command;
    if(req->command==QG_CMD_CONFIG) {
        if(!qg_config_valid(&req->config)){error=ERROR_INVALID_DATA;goto done;}
        copy_config(&clean.config,&req->config);
    }
    error=pipe_io(pipe,true,&clean,sizeof(clean),NULL,QG_IO_MS);if(error)goto done;
    memset(reply,0,sizeof(*reply));error=pipe_io(pipe,false,reply,sizeof(*reply),NULL,QG_IO_MS);if(error)goto done;
    if(reply->magic!=QG_WIRE_MAGIC){error=ERROR_INVALID_DATA;goto done;}
    BYTE ack=QG_ACK;error=pipe_io(pipe,true,&ack,sizeof(ack),NULL,QG_IO_MS);
done:
    CloseHandle(pipe);SetLastError(error);return error==ERROR_SUCCESS;
}

static void report_status(DWORD state,DWORD error,DWORD hint) {
    svc.status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;svc.status.dwCurrentState=state;
    svc.status.dwControlsAccepted=state==SERVICE_RUNNING?
        SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN|SERVICE_ACCEPT_POWEREVENT|SERVICE_ACCEPT_SESSIONCHANGE:0;
    svc.status.dwWin32ExitCode=error;svc.status.dwWaitHint=hint;
    svc.status.dwCheckPoint=(state==SERVICE_START_PENDING||state==SERVICE_STOP_PENDING)?svc.status.dwCheckPoint+1:0;
    if(svc.status_handle)SetServiceStatus(svc.status_handle,&svc.status);
}
static DWORD WINAPI service_control(DWORD control,DWORD event_type,void *event_data,void *context) {
    (void)event_type;(void)event_data;(void)context;
    switch(control) {
    case SERVICE_CONTROL_STOP:
        /* Stopping this monitor is always allowed, even if Windows later aborted
           an accepted shutdown. It does NOT retract an already committed request. */
        InterlockedIncrement(&svc.generation);stop_service();return NO_ERROR;
    case SERVICE_CONTROL_SHUTDOWN:
        /* An OS shutdown notification must still stop this service after our commit. */
        InterlockedIncrement(&svc.generation);stop_service();return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:case SERVICE_CONTROL_SESSIONCHANGE:
        if(!inhibit_commit(QG_RESET_UNIT))return NO_ERROR;
        EnterCriticalSection(&svc.lock);
        qg_engine_reset(&svc.engine,svc.engine.cfg.enabled?QG_UNKNOWN:QG_PAUSED);
        InterlockedIncrement(&svc.revision);InterlockedIncrement(&svc.generation);
        InterlockedExchangeAdd(&svc.commit_gate,-QG_RESET_UNIT);
        LeaveCriticalSection(&svc.lock);SetEvent(svc.changed);return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:return NO_ERROR;
    default:return ERROR_CALL_NOT_IMPLEMENTED;
    }
}
static void make_reply(QgReply *reply,DWORD error) {
    memset(reply,0,sizeof(*reply));reply->magic=QG_WIRE_MAGIC;reply->error=error;
    EnterCriticalSection(&svc.lock);
    reply->phase=svc.engine.phase;
    if(!error&&svc.engine.phase==QG_FAULT)reply->error=svc.error;
    copy_config(&reply->config,&svc.engine.cfg);
    reply->sample.at_ms=svc.sample.at_ms;reply->sample.valid=svc.sample.valid;
    for(unsigned i=0;i<4;i++)reply->sample.value[i]=svc.sample.value[i];
    reply->sample.veto=svc.sample.veto;reply->sample.discontinuity=svc.sample.discontinuity;
    uint64_t now=GetTickCount64();reply->snapshot_ms=now;
    if(svc.engine.have_idle&&now>=svc.engine.idle_since)reply->idle_ms=now-svc.engine.idle_since;
    if((svc.engine.phase==QG_WARNING||svc.engine.phase==QG_READY)&&now>=svc.engine.warning_since)
        reply->warning_ms=now-svc.engine.warning_since;
    LeaveCriticalSection(&svc.lock);
}
static DWORD process_request(const QgRequest *request) {
    if(request->magic!=QG_WIRE_MAGIC)return ERROR_INVALID_DATA;
    if(request->command==QG_CMD_STATUS)return ERROR_SUCCESS;
    if(request->command==QG_CMD_CANCEL) {
        /* CAS winning this race guarantees commit cannot occur, even before the lock.
           A commit winner instead gets an honest too-late reply; never claim paused. */
        if(!inhibit_commit(QG_INHIBIT_CANCEL))return ERROR_SHUTDOWN_IN_PROGRESS;
        InterlockedIncrement(&svc.generation);
        EnterCriticalSection(&svc.lock);qg_engine_cancel(&svc.engine);InterlockedIncrement(&svc.revision);
        LeaveCriticalSection(&svc.lock);
        SetEvent(svc.changed);return ERROR_SUCCESS;
    }
    if(request->command!=QG_CMD_CONFIG)return ERROR_INVALID_FUNCTION;
    if(!qg_config_valid(&request->config))return ERROR_INVALID_DATA;
    if(!ImpersonateNamedPipeClient(svc.pipe))return last_error();
    DWORD e=ERROR_ACCESS_DENIED;
    if(qg_is_admin()) {
        if(!inhibit_commit(QG_INHIBIT_CANCEL))e=ERROR_SHUTDOWN_IN_PROGRESS;
        else {
            InterlockedIncrement(&svc.generation);
            EnterCriticalSection(&svc.lock);qg_engine_cancel(&svc.engine);InterlockedIncrement(&svc.revision);
            LeaveCriticalSection(&svc.lock);SetEvent(svc.changed);
            e=save_config(svc.config_path,&request->config);
        }
    }
    if(!RevertToSelf()) {
        /* Never continue any thread under an unexpected client's security identity. */
        e=last_error();svc.error=e;stop_service();return e;
    }
    if(e)return e;
    EnterCriticalSection(&svc.lock);qg_engine_configure(&svc.engine,&request->config);
    InterlockedIncrement(&svc.revision);InterlockedIncrement(&svc.generation);
    InterlockedAnd(&svc.commit_gate,~QG_INHIBIT_CANCEL);LeaveCriticalSection(&svc.lock);
    SetEvent(svc.changed);return ERROR_SUCCESS;
}
static DWORD WINAPI pipe_worker(void *unused) {
    (void)unused;
    while(WaitForSingleObject(svc.stop,0)!=WAIT_OBJECT_0) {
        OVERLAPPED op;memset(&op,0,sizeof(op));op.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
        if(!op.hEvent){svc.error=last_error();stop_service();break;}
        BOOL connected=ConnectNamedPipe(svc.pipe,&op);DWORD e=connected?ERROR_SUCCESS:last_error(),bytes=0;
        if(e==ERROR_IO_PENDING)e=wait_operation(svc.pipe,&op,svc.stop,INFINITE,&bytes);
        else if(e==ERROR_PIPE_CONNECTED)e=ERROR_SUCCESS;
        CloseHandle(op.hEvent);
        if(e) {if(e!=ERROR_OPERATION_ABORTED){svc.error=e;stop_service();}break;}
        QgRequest request;memset(&request,0,sizeof(request));
        e=pipe_io(svc.pipe,false,&request,sizeof(request),svc.stop,QG_IO_MS);
        if(!e) {
            DWORD result=process_request(&request);
            if(WaitForSingleObject(svc.stop,0)==WAIT_OBJECT_0)break;
            QgReply reply;make_reply(&reply,result);
            if(!pipe_io(svc.pipe,true,&reply,sizeof(reply),svc.stop,QG_IO_MS)) {
                BYTE ack=0; /* Bounded ACK avoids an unbounded FlushFileBuffers wait. */
                (void)pipe_io(svc.pipe,false,&ack,sizeof(ack),svc.stop,QG_IO_MS);
            }
        }
        DisconnectNamedPipe(svc.pipe);
    }
    return 0;
}
static void monitor(void) {
    QgMetrics *metrics=NULL;LONG current_revision=-1;DWORD wait_ms=0;
    HANDLE events[2]={svc.stop,svc.changed};
    for(;;) {
        DWORD w=WaitForMultipleObjects(2,events,FALSE,wait_ms);
        if(w==WAIT_OBJECT_0)break;
        if(w==WAIT_FAILED){svc.error=last_error();break;}
        EnterCriticalSection(&svc.lock);
        LONG generation=InterlockedCompareExchange(&svc.generation,0,0);
        LONG revision=InterlockedCompareExchange(&svc.revision,0,0);
        if(current_revision!=revision) {
            qg_engine_reset(&svc.engine,svc.engine.cfg.enabled?QG_UNKNOWN:QG_PAUSED);
            memset(&svc.sample,0,sizeof(svc.sample));
        }
        QgConfig config;copy_config(&config,&svc.engine.cfg);
        LeaveCriticalSection(&svc.lock);
        if(current_revision!=revision) {
            qg_metrics_destroy(metrics);metrics=NULL;current_revision=revision;
        }
        if(!config.enabled){wait_ms=INFINITE;continue;}
        if(!metrics)metrics=qg_metrics_create(config.mask);
        QgSample sample;memset(&sample,0,sizeof(sample));sample.at_ms=GetTickCount64();
        if(metrics)qg_metrics_sample(metrics,config.veto_active_session,&sample);
        else sample.veto=true;
        EnterCriticalSection(&svc.lock);
        if(revision!=InterlockedCompareExchange(&svc.revision,0,0)||
           generation!=InterlockedCompareExchange(&svc.generation,0,0)||WaitForSingleObject(svc.stop,0)==WAIT_OBJECT_0) {
            LeaveCriticalSection(&svc.lock);wait_ms=0;continue;
        }
        svc.sample=sample;qg_engine_sample(&svc.engine,&sample);
        uint64_t epoch=svc.engine.epoch;
        /* The warning's final scheduled sample is fresh; no same-tick counter re-read.
           CAS is the irreversible submission boundary, not the preceding checks:
           every cancellation/config/stop/reset inhibits this SAME atomic word. */
        if(qg_engine_can_commit(&svc.engine,epoch,GetTickCount64())&&
           generation==InterlockedCompareExchange(&svc.generation,0,0)&&
           revision==InterlockedCompareExchange(&svc.revision,0,0)&&WaitForSingleObject(svc.stop,0)!=WAIT_OBJECT_0&&
           InterlockedCompareExchange(&svc.commit_gate,QG_COMMITTED,0)==0) {
            DWORD e=qg_shutdown_request(svc.dry_run);
            svc.error=e;svc.engine.cfg.enabled=false;
            svc.engine.phase=e?QG_FAULT:QG_REQUESTED; /* No automatic retry or forced fallback. */
            /* An error did not submit successfully: admin CONFIG may explicitly rearm.
               Success retains COMMITTED for this service lifetime, including dry-run. */
            if(e)InterlockedExchange(&svc.commit_gate,QG_INHIBIT_CANCEL);
        }
        wait_ms=svc.engine.cfg.enabled?svc.engine.cfg.sample_seconds*1000u:INFINITE;
        if(svc.engine.phase==QG_WARNING&&wait_ms>2000u)wait_ms=2000u;
        LeaveCriticalSection(&svc.lock);
    }
    qg_metrics_destroy(metrics);stop_service();
}
static void WINAPI service_entry(DWORD argc,wchar_t **argv) {
    (void)argc;(void)argv;memset(&svc,0,sizeof(svc));svc.pipe=INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&svc.lock);
    svc.stop=CreateEventW(NULL,TRUE,FALSE,NULL);svc.changed=CreateEventW(NULL,FALSE,FALSE,NULL);
    svc.status_handle=RegisterServiceCtrlHandlerExW(QG_SERVICE_NAME,service_control,NULL);
    HANDLE worker=NULL;DWORD e=ERROR_SUCCESS;
    if(!svc.stop||!svc.changed||!svc.status_handle){e=last_error();goto cleanup;}
    report_status(SERVICE_START_PENDING,ERROR_SUCCESS,10000);
    svc.dry_run=has_argument(L"--service-dry-run");
    wchar_t bin_dir[MAX_PATH],exe[MAX_PATH],data_dir[MAX_PATH];
    if(!paths(bin_dir,exe,data_dir,svc.config_path)){e=ERROR_PATH_NOT_FOUND;goto cleanup;}
    e=trusted_path(data_dir,true,false);if(e)goto cleanup;
    e=trusted_path(svc.config_path,false,false);if(e)goto cleanup;
    QgConfig config;memset(&config,0,sizeof(config));
    if(!qg_read_config_file(svc.config_path,&config)){e=ERROR_INVALID_DATA;goto cleanup;}
    qg_engine_init(&svc.engine,&config);
    svc.gate=qg_monitor_mutex();if(!svc.gate){e=last_error();goto cleanup;}
    PSECURITY_DESCRIPTOR sd=NULL;SECURITY_ATTRIBUTES sa;
    if(!descriptor(QG_PIPE_SD,&sd,&sa)){e=last_error();goto cleanup;}
    svc.pipe=CreateNamedPipeW(QG_PIPE,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
        1,sizeof(QgReply),sizeof(QgRequest),QG_IO_MS,&sa);
    e=svc.pipe==INVALID_HANDLE_VALUE?last_error():ERROR_SUCCESS;LocalFree(sd);if(e)goto cleanup;
    worker=CreateThread(NULL,0,pipe_worker,NULL,0,NULL);if(!worker){e=last_error();goto cleanup;}
    report_status(SERVICE_RUNNING,ERROR_SUCCESS,0);monitor();e=svc.error;
cleanup:
    stop_service();
    if(worker) {report_status(SERVICE_STOP_PENDING,ERROR_SUCCESS,10000);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);}
    if(svc.pipe!=INVALID_HANDLE_VALUE){DisconnectNamedPipe(svc.pipe);CloseHandle(svc.pipe);}
    if(svc.gate)CloseHandle(svc.gate);
    if(svc.changed)CloseHandle(svc.changed);
    if(svc.stop)CloseHandle(svc.stop);
    report_status(SERVICE_STOPPED,e,0);DeleteCriticalSection(&svc.lock);
}
int qg_service_main(void) {
    SERVICE_TABLE_ENTRYW table[]={{QG_SERVICE_NAME,service_entry},{NULL,NULL}};
    return StartServiceCtrlDispatcherW(table)?0:(int)last_error();
}

/* Stream into an explicitly protected CREATE_NEW temp: CopyFile would briefly copy
   the portable source's possibly user-writable DACL into the service directory. */
static DWORD copy_binary(const wchar_t *source,const wchar_t *destination) {
    HANDLE input=CreateFileW(source,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if(input==INVALID_HANDLE_VALUE)return last_error();
    LARGE_INTEGER size;DWORD e=ERROR_FILE_TOO_LARGE;
    if(!GetFileSizeEx(input,&size)){e=last_error();CloseHandle(input);return e;}
    if(size.QuadPart<=0||size.QuadPart>64*1024*1024){CloseHandle(input);return e;}
    PSECURITY_DESCRIPTOR sd=NULL;SECURITY_ATTRIBUTES sa;
    if(!descriptor(QG_EXE_SD,&sd,&sa)){e=last_error();CloseHandle(input);return e;}
    wchar_t tmp[MAX_PATH];HANDLE output=INVALID_HANDLE_VALUE;
    for(unsigned i=0;i<8;i++) {
        if(swprintf(tmp,MAX_PATH,L"%ls.%lu.%llu.%u.tmp",destination,(unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64(),i)<=0){e=ERROR_FILENAME_EXCED_RANGE;break;}
        output=CreateFileW(tmp,GENERIC_WRITE,0,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(output!=INVALID_HANDLE_VALUE)break;
        e=last_error();if(e!=ERROR_FILE_EXISTS&&e!=ERROR_ALREADY_EXISTS)break;
    }
    LocalFree(sd);
    if(output==INVALID_HANDLE_VALUE){CloseHandle(input);return e;}
    BYTE buffer[8192];LONGLONG total=0;e=ERROR_SUCCESS;
    while(total<size.QuadPart) {
        DWORD got=0,written=0;
        if(!ReadFile(input,buffer,sizeof(buffer),&got,NULL)){e=last_error();break;}
        if(!got){e=ERROR_HANDLE_EOF;break;}
        if(total+(LONGLONG)got>size.QuadPart){e=ERROR_INVALID_DATA;break;}
        if(!WriteFile(output,buffer,got,&written,NULL)||written!=got){e=last_error();break;}
        total+=got;
    }
    if(!e&&!FlushFileBuffers(output))e=last_error();
    CloseHandle(input);CloseHandle(output);
    if(!e&&!MoveFileExW(tmp,destination,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))e=last_error();
    if(e)DeleteFileW(tmp);
    return e;
}

DWORD qg_service_install(const QgConfig *config) {
    if(!qg_is_admin())return ERROR_ELEVATION_REQUIRED;
    if(!qg_config_valid(config))return ERROR_INVALID_DATA;
    SC_HANDLE scm=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT|SC_MANAGER_CREATE_SERVICE);
    if(!scm)return last_error();
    SC_HANDLE existing=OpenServiceW(scm,QG_SERVICE_NAME,SERVICE_QUERY_CONFIG);
    if(existing){CloseServiceHandle(existing);CloseServiceHandle(scm);return ERROR_SERVICE_EXISTS;}
    DWORD e=last_error();if(e!=ERROR_SERVICE_DOES_NOT_EXIST){CloseServiceHandle(scm);return e;}
    wchar_t bin_dir[MAX_PATH],exe[MAX_PATH],data_dir[MAX_PATH],config_path[MAX_PATH],source[MAX_PATH];
    if(!paths(bin_dir,exe,data_dir,config_path)){CloseServiceHandle(scm);return ERROR_PATH_NOT_FOUND;}
    e=protected_directory(bin_dir,QG_BIN_SD);if(e)goto done;
    e=protected_directory(data_dir,QG_DATA_SD);if(e)goto done;
    e=trusted_path(exe,false,true);if(e)goto done;
    DWORD n=GetModuleFileNameW(NULL,source,MAX_PATH);
    if(!n||n>=MAX_PATH){e=ERROR_FILENAME_EXCED_RANGE;goto done;}
    if(_wcsicmp(source,exe)) {e=copy_binary(source,exe);if(e)goto done;}
    e=trusted_path(exe,false,false);if(e)goto done;
    e=save_config(config_path,config);if(e)goto done;
    wchar_t command[MAX_PATH+64];
    if(swprintf(command,sizeof(command)/sizeof(command[0]),L"\"%ls\" %ls",exe,
        (has_argument(L"--install-service-dry-run")||has_argument(L"--service-dry-run"))?L"--service-dry-run":L"--service")<=0){e=ERROR_FILENAME_EXCED_RANGE;goto done;}
    SC_HANDLE service=CreateServiceW(scm,QG_SERVICE_NAME,L"QuietGuard idle shutdown",SERVICE_START|SERVICE_QUERY_STATUS|DELETE,
        SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,SERVICE_ERROR_NORMAL,command,NULL,NULL,NULL,L"NT AUTHORITY\\LocalService",NULL);
    if(!service){e=last_error();goto done;}
    /* Installation is opt-in. Start is explicit inside this user-approved install operation. */
    if(!StartServiceW(service,0,NULL)) {
        e=last_error();
        /* Leave the fixed registered service for a diagnosable retry/remove, rather than silent rollback. */
    } else {
        SERVICE_STATUS_PROCESS status;DWORD count=0;uint64_t end=GetTickCount64()+10000;
        do {
            if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE*)&status,sizeof(status),&count)){e=last_error();break;}
            if(status.dwCurrentState==SERVICE_RUNNING){e=ERROR_SUCCESS;break;}
            if(status.dwCurrentState==SERVICE_STOPPED){e=status.dwWin32ExitCode?status.dwWin32ExitCode:ERROR_SERVICE_NOT_ACTIVE;break;}
            e=ERROR_TIMEOUT;Sleep(100);
        }while(GetTickCount64()<end);
    }
    CloseServiceHandle(service);
done:
    CloseServiceHandle(scm);return e;
}
DWORD qg_service_remove(void) {
    if(!qg_is_admin())return ERROR_ELEVATION_REQUIRED;
    SC_HANDLE scm=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);if(!scm)return last_error();
    SC_HANDLE service=OpenServiceW(scm,QG_SERVICE_NAME,SERVICE_STOP|SERVICE_QUERY_STATUS|DELETE);
    DWORD e=service?ERROR_SUCCESS:last_error();CloseServiceHandle(scm);if(!service)return e;
    SERVICE_STATUS_PROCESS status;DWORD n=0;
    if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE*)&status,sizeof(status),&n)){e=last_error();goto done;}
    if(status.dwCurrentState!=SERVICE_STOPPED) {
        SERVICE_STATUS ignored;
        if(!ControlService(service,SERVICE_CONTROL_STOP,&ignored)&&GetLastError()!=ERROR_SERVICE_NOT_ACTIVE){e=last_error();goto done;}
        uint64_t end=GetTickCount64()+10000;
        do {
            if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE*)&status,sizeof(status),&n)){e=last_error();goto done;}
            if(status.dwCurrentState==SERVICE_STOPPED)break;
            Sleep(100);
        }while(GetTickCount64()<end);
        if(status.dwCurrentState!=SERVICE_STOPPED){e=ERROR_TIMEOUT;goto done;}
    }
    if(!DeleteService(service))e=last_error();
    /* No recursive deletion: retain binary/config for safe manual removal and audit. */
done:
    CloseServiceHandle(service);return e;
}
