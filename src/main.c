#include "qg.h"
#include "safe_cli.h"
#include "build_identity.h"
#include "ui_layout.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <windowsx.h>
#include <wtsapi32.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#define WM_SAMPLE (WM_APP+1)
#define WM_TRAY (WM_APP+2)
#define TIMER_SAMPLE 1
#define TIMER_COMMIT 2
#define TIMER_UI 3
#define TIMER_INSTALL 4
#define ID_START 100
#define ID_SAVE 101
#define ID_CANCEL 102
#define ID_MODE 103
#define ID_VETO 104
#define ID_HELP 105
#define ID_CHECK 200
#define ID_THRESHOLD 210
#define ID_DURATION 220
#define ID_INTERVAL 221
#define ID_WARNING 222
#define ID_NAV 230
#define ID_TRAY_OPEN 301
#define ID_TRAY_PAUSE 302
#define ID_TRAY_EXIT 303

typedef struct {
    HANDLE event,stop,thread; CRITICAL_SECTION lock; QgConfig config; uint64_t generation;
    QgSample result; QgReply service_reply; uint64_t result_generation; LONG inflight, pending_cancel; uint32_t command, result_command; bool reset, service_ok;
} Worker;
static struct {
    HWND window,content,scrollbar,nav[3],page_title,status_title,status_detail,notice_label,check[4],edit[4],live[4],label[4],field_label[3],duration,interval,warning,mode,veto,start,save,cancel,help;
    HFONT body,small,title,hero,semibold; HBRUSH background,surface; COLORREF bg,fg,muted,accent,card,border,sidebar_color;
    HDC paint_dc; HBITMAP paint_bitmap; HGDIOBJ paint_old; int paint_width,paint_height;
    QgUiLayout geometry; QgUiPage page; QgConfig view_config; bool view_valid,view_service,in_layout,was_minimized;
    int scroll_range,scroll_page,scroll_position; UINT ui_interval; wchar_t tray_tip[128];
    int dpi, scroll; bool dry,background_launch,tray,service,highcontrast,cancel_after_install,cancel_pending_ui,session_events,previous_login_startup,service_config_loaded; QgConfig cfg; QgEngine engine; QgSample last;
    wchar_t config_path[MAX_PATH],staging_path[MAX_PATH],notice[256]; HANDLE monitor,install; int pending_mode;
    uint64_t generation,commit_epoch; Worker worker,ipc;
} g;
static int px(int x){return MulDiv(x,g.dpi,96);}
static void update_view(void);
static void change_timer(void);
static void pause_monitor(bool save);
static void request_sample(void);
static void service_refresh(void);
static void service_request(uint32_t command);
static void scroll_to(int position);
static bool visible_ui(void);
static void refresh_ui_timer(void);
static void select_page(QgUiPage page);
static LRESULT CALLBACK window_proc(HWND,UINT,WPARAM,LPARAM);

static const wchar_t *phase_text(QgPhase p) {
    switch(p){case QG_PAUSED:return L"已暂停";case QG_UNKNOWN:return L"等待可靠读数";case QG_BUSY:return g.last.veto?L"连接中的会话正在保护此电脑":L"电脑仍在忙碌";case QG_IDLE:return L"静候电脑闲下来";case QG_WARNING:return L"即将关机 · 现在仍可取消";case QG_READY:return L"正在最后确认";case QG_REQUESTED:return g.dry?L"演练完成 · 未执行关机":L"Windows 已接受关机请求";default:return L"已安全暂停 · 请检查设置";}
}
static void set_notice(const wchar_t *s){StringCchCopyW(g.notice,256,s);update_view();}
static void error_box(const wchar_t *what,DWORD error){wchar_t b[512];StringCchPrintfW(b,512,L"%s\n\nWindows 错误代码：%lu\n已保持安全状态，不会强制关机。",what,error);MessageBoxW(g.window,b,L"QuietGuard",MB_OK|MB_ICONWARNING);}
static void worker_reset(void){if(!g.worker.thread){g.generation++;return;}EnterCriticalSection(&g.worker.lock);g.worker.reset=true;LeaveCriticalSection(&g.worker.lock);g.generation++;}
static DWORD WINAPI worker_main(void *unused){
    Worker *worker=(Worker*)unused;QgMetrics *m=NULL;uint32_t mask=0;HANDLE handles[2]={worker->stop,worker->event};
    while(WaitForMultipleObjects(2,handles,FALSE,INFINITE)==WAIT_OBJECT_0+1){
        EnterCriticalSection(&worker->lock);QgConfig c=worker->config;uint64_t gen=worker->generation;bool reset=worker->reset;uint32_t cmd=worker->command;worker->reset=false;LeaveCriticalSection(&worker->lock);
        QgSample s;QgReply reply;bool service_ok=false;ZeroMemory(&s,sizeof(s));ZeroMemory(&reply,sizeof(reply));s.at_ms=GetTickCount64();
        if(cmd){QgRequest req={0};req.magic=QG_WIRE_MAGIC;req.command=cmd;service_ok=qg_service_exchange(&req,&reply);
            if(InterlockedExchange(&worker->pending_cancel,0)){req.command=QG_CMD_CANCEL;cmd=QG_CMD_CANCEL;service_ok=qg_service_exchange(&req,&reply);}
        }else{if(reset||!m||mask!=c.mask){qg_metrics_destroy(m);m=qg_metrics_create(c.mask);mask=c.mask;}
            if(m)qg_metrics_sample(m,c.veto_active_session!=0,&s);else s.discontinuity=true;}
        EnterCriticalSection(&worker->lock);worker->result=s;worker->result_generation=gen;worker->result_command=cmd;worker->service_reply=reply;worker->service_ok=service_ok;LeaveCriticalSection(&worker->lock);
        if(!PostMessageW(g.window,WM_SAMPLE,worker==&g.ipc?1:0,0))InterlockedExchange(&worker->inflight,0);
    }
    qg_metrics_destroy(m);return 0;
}
static bool worker_init(Worker *worker){
    InitializeCriticalSection(&worker->lock);worker->event=CreateEventW(NULL,FALSE,FALSE,NULL);worker->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!worker->event||!worker->stop)return false;
    worker->thread=CreateThread(NULL,128*1024,worker_main,worker,0,NULL);return worker->thread!=NULL;
}
static void request_sample(void){
    if(g.service||!g.engine.cfg.enabled)return;
    if(InterlockedCompareExchange(&g.worker.inflight,1,0)!=0)return;
    EnterCriticalSection(&g.worker.lock);g.worker.config=g.engine.cfg;g.worker.generation=g.generation;g.worker.command=0;LeaveCriticalSection(&g.worker.lock);SetEvent(g.worker.event);
}
static void worker_close(Worker *worker){
    if(worker->stop)SetEvent(worker->stop);
    if(worker->event)SetEvent(worker->event);
    if(worker->thread&&WaitForSingleObject(worker->thread,3000)==WAIT_OBJECT_0){CloseHandle(worker->thread);CloseHandle(worker->event);CloseHandle(worker->stop);DeleteCriticalSection(&worker->lock);}
    /* A hung provider is killed only by process exit; never free state under it. */
}
static void tray_update(void){
    NOTIFYICONDATAW n;ZeroMemory(&n,sizeof(n));n.cbSize=sizeof(n);n.hWnd=g.window;n.uID=1;n.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;n.uCallbackMessage=WM_TRAY;n.hIcon=(HICON)LoadImageW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(101),IMAGE_ICON,px(16),px(16),LR_SHARED);
    StringCchPrintfW(n.szTip,128,L"QuietGuard · %s",phase_text(g.engine.phase));
    if(!g.tray){if(Shell_NotifyIconW(NIM_ADD,&n)){g.tray=true;n.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&n);}}
    else if(wcscmp(g.tray_tip,n.szTip))Shell_NotifyIconW(NIM_MODIFY,&n);
    StringCchCopyW(g.tray_tip,128,n.szTip);
}
static void tray_remove(void){if(g.tray){NOTIFYICONDATAW n={0};n.cbSize=sizeof(n);n.hWnd=g.window;n.uID=1;Shell_NotifyIconW(NIM_DELETE,&n);g.tray=false;}}
static bool login_startup(bool enabled){
    HKEY key;LONG r=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,NULL,0,KEY_SET_VALUE,NULL,&key,NULL);if(r!=ERROR_SUCCESS)return false;
    if(enabled){wchar_t exe[MAX_PATH],value[MAX_PATH+40];DWORD n=GetModuleFileNameW(NULL,exe,MAX_PATH);if(!n||n>=MAX_PATH){RegCloseKey(key);return false;}StringCchPrintfW(value,MAX_PATH+40,L"\"%s\" --background",exe);r=RegSetValueExW(key,L"QuietGuard",0,REG_SZ,(BYTE*)value,(DWORD)((wcslen(value)+1)*sizeof(wchar_t)));}
    else {r=RegDeleteValueW(key,L"QuietGuard");if(r==ERROR_FILE_NOT_FOUND)r=ERROR_SUCCESS;}
    RegCloseKey(key);return r==ERROR_SUCCESS;
}
static bool has_login_startup(void){HKEY k;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS)return false;DWORD bytes=0;LONG r=RegQueryValueExW(k,L"QuietGuard",NULL,NULL,NULL,&bytes);RegCloseKey(k);return r==ERROR_SUCCESS&&bytes>0;}
static bool parse_edit(HWND h,double *out){wchar_t b[48],*end;GetWindowTextW(h,b,48);if(!*b)return false;for(wchar_t *p=b;*p;p++)if((*p<L'0'||*p>L'9')&&*p!=L'.')return false;double v=wcstod(b,&end);if(*end||!isfinite(v))return false;*out=v;return true;}
static bool read_fields(QgConfig *c){
    *c=g.cfg;c->enabled=g.engine.cfg.enabled;c->mask=0;
    for(unsigned i=0;i<4;i++){if(Button_GetCheck(g.check[i])==BST_CHECKED)c->mask|=1u<<i;if(!parse_edit(g.edit[i],&c->threshold[i]))goto bad;}
    double duration,interval,warning;if(!parse_edit(g.duration,&duration)||!parse_edit(g.interval,&interval)||!parse_edit(g.warning,&warning)||duration!=floor(duration)||interval!=floor(interval)||warning!=floor(warning)||duration<1||duration>10080||interval<1||interval>600||warning<10||warning>3600)goto bad;
    c->idle_seconds=(uint32_t)duration*60;c->sample_seconds=(uint32_t)interval;c->warning_seconds=(uint32_t)warning;c->veto_active_session=Button_GetCheck(g.veto)==BST_CHECKED?1u:0u;
    if(qg_config_valid(c)){if(!g.service&&c->veto_active_session&&!g.session_events){error_box(L"无法注册会话通知；请重试或明确关闭会话保护",ERROR_NOT_READY);return false;}return true;}
bad:MessageBoxW(g.window,L"请选择至少一项指标。百分比阈值：0.001–100；网络：0.001–1000000000 KiB/s。\n\n持续时间：1–10080 分钟；采样：1–600 秒；预告：10–3600 秒。持续时间至少为采样间隔的两倍。",L"检查设置",MB_OK|MB_ICONINFORMATION);return false;
}
static void fill_fields(void){
    wchar_t b[48];for(unsigned i=0;i<4;i++){Button_SetCheck(g.check[i],(g.cfg.mask&(1u<<i))?BST_CHECKED:BST_UNCHECKED);StringCchPrintfW(b,48,L"%.3f",g.cfg.threshold[i]);SetWindowTextW(g.edit[i],b);}
    StringCchPrintfW(b,48,L"%u",g.cfg.idle_seconds/60);SetWindowTextW(g.duration,b);StringCchPrintfW(b,48,L"%u",g.cfg.sample_seconds);SetWindowTextW(g.interval,b);StringCchPrintfW(b,48,L"%u",g.cfg.warning_seconds);SetWindowTextW(g.warning,b);Button_SetCheck(g.veto,g.cfg.veto_active_session?BST_CHECKED:BST_UNCHECKED);
}
static bool elevate(const wchar_t *verb,int pending){
    if(g.dry){set_notice(L"安全演练不修改启动项或安装服务");return false;}
    wchar_t exe[MAX_PATH],args[MAX_PATH+80];DWORD n=GetModuleFileNameW(NULL,exe,MAX_PATH);if(!n||n>=MAX_PATH){error_box(L"无法确定程序路径，启动设置未改变",ERROR_BAD_PATHNAME);return false;}
    if(FAILED(StringCchPrintfW(g.staging_path,MAX_PATH,L"%s.service-request",g.config_path))||!qg_write_config_file(g.staging_path,&g.cfg)){error_box(L"无法暂存服务配置，启动设置未改变",GetLastError());return false;}
    if(!wcscmp(verb,L"--install-service")&&!login_startup(false)){DeleteFileW(g.staging_path);error_box(L"无法移除登录启动项，未安装服务",GetLastError());return false;}
    StringCchPrintfW(args,MAX_PATH+80,L"%s --config \"%s\"",verb,g.staging_path);
    SHELLEXECUTEINFOW x={0};x.cbSize=sizeof(x);x.fMask=SEE_MASK_NOCLOSEPROCESS;x.hwnd=g.window;x.lpVerb=L"runas";x.lpFile=exe;x.lpParameters=args;x.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&x)){DWORD e=GetLastError();DeleteFileW(g.staging_path);g.cfg.enabled=0;bool saved=qg_write_config_file(g.config_path,&g.cfg);if(!wcscmp(verb,L"--install-service")&&!qg_service_installed()){if(!login_startup(g.previous_login_startup))set_notice(L"管理员操作已取消；旧登录启动项未能恢复，请检查启动设置");else set_notice(saved?L"管理员操作已取消；登录设置已恢复，监测保持暂停":L"管理员操作已取消，但暂停配置保存失败；请检查下次启动设置");}else set_notice(L"管理员操作未应用；服务保持原状态，请检查当前状态");error_box(L"管理员操作未完成",e);return false;}
    g.install=x.hProcess;g.pending_mode=pending;g.cancel_after_install=false;SetTimer(g.window,TIMER_INSTALL,400,NULL);EnableWindow(g.save,FALSE);EnableWindow(g.start,FALSE);set_notice(L"等待管理员确认并应用设置…");return true;
}
static void pause_monitor(bool save){
    if(!g.service&&g.engine.phase==QG_REQUESTED&&!g.dry){set_notice(L"关机请求已交给 Windows，无法确认撤回；停止监测不等于取消关机");return;}
    if(g.install)g.cancel_after_install=true;
    KillTimer(g.window,TIMER_COMMIT);KillTimer(g.window,TIMER_SAMPLE);g.generation++;
    if(g.service){service_request(QG_CMD_CANCEL);set_notice(L"正在请求服务取消；收到确认前请保持窗口打开");return;}
    else {qg_engine_cancel(&g.engine);g.cfg.enabled=0;if(g.monitor){CloseHandle(g.monitor);g.monitor=NULL;}if(save&&!g.dry&&!qg_write_config_file(g.config_path,&g.cfg))error_box(L"当前已暂停，但暂停状态无法保存；下次启动可能按旧配置恢复",GetLastError());}
    update_view();
}
static bool start_local(void){
    if(g.cfg.veto_active_session&&!g.session_events){error_box(L"会话通知注册失败，已阻止启动；请重试或明确关闭会话保护",GetLastError());return false;}
    if(!g.monitor)g.monitor=qg_monitor_mutex();
    if(!g.monitor){error_box(L"另一个登录会话或服务正在监测，请先暂停它",ERROR_ALREADY_EXISTS);return false;}
    g.cfg.enabled=1;qg_engine_configure(&g.engine,&g.cfg);worker_reset();if(!g.dry&&!qg_write_config_file(g.config_path,&g.cfg)){qg_engine_cancel(&g.engine);CloseHandle(g.monitor);g.monitor=NULL;error_box(L"设置未保存，监测没有启动",GetLastError());return false;}
    g.notice[0]=0;change_timer();request_sample();update_view();return true;
}
static void save_settings(bool start){
    QgConfig c;if(!read_fields(&c))return;int mode=(int)SendMessageW(g.mode,CB_GETCURSEL,0,0);if(mode<0)mode=0;
    if(g.install)return;
    if(g.dry){g.cfg=c;if(start){if(g.engine.cfg.enabled)pause_monitor(false);else start_local();}else{bool enabled=g.engine.cfg.enabled;g.cfg.enabled=enabled;qg_engine_configure(&g.engine,&g.cfg);worker_reset();change_timer();request_sample();set_notice(L"已应用演练设置，不会写入启动项或执行关机");}return;}
    if(mode==2){
        if(!qg_service_installed()){
            if(MessageBoxW(g.window,L"开机服务会在登录前监测整台电脑，以 LocalService 身份运行。\n\n将安装到 Program Files，并移除本账户的登录启动项。服务没有桌面预告窗口；默认连接会话保护开启，重连即可中断倒计时。安装需要管理员确认。\n\n要继续吗？",L"启用开机服务",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
            g.previous_login_startup=has_login_startup();pause_monitor(false);if(!qg_write_config_file(g.config_path,&g.cfg)){error_box(L"无法保存暂停状态，启动设置未改变",GetLastError());return;}g.cfg=c;g.cfg.enabled=start?1:c.enabled;elevate(L"--install-service",2);return;
        }
        g.cfg=c;if(start)g.cfg.enabled=g.engine.cfg.enabled?0:1;
        if(!g.cfg.enabled&&start){pause_monitor(false);return;}
        elevate(L"--configure-service",2);return;
    }
    if(qg_service_installed()){
        if(MessageBoxW(g.window,L"切换到登录/手动模式前，需要停止并移除 QuietGuard 服务。\n\n服务配置会保留，卸载不会关闭电脑。继续？",L"切换运行方式",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
        g.cfg=c;g.cfg.enabled=0;elevate(L"--remove-service",mode);return;
    }
    if(mode==1&&!has_login_startup()&&MessageBoxW(g.window,L"登录 Windows 后将从当前程序位置自动启动监测。\n\n断开远程桌面后仍会运行；重启后首次登录前不会监测。请保留程序在当前位置。允许添加登录启动项吗？",L"登录后自动启动",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
    if(!login_startup(mode==1)){error_box(L"启动设置未能应用",GetLastError());return;}
    bool was=g.engine.cfg.enabled;g.cfg=c;g.cfg.enabled=was?1u:0u;
    if(start){if(was)pause_monitor(true);else start_local();}
    else {qg_engine_configure(&g.engine,&g.cfg);worker_reset();if(!qg_write_config_file(g.config_path,&g.cfg)){pause_monitor(false);error_box(L"设置没有保存",GetLastError());return;}change_timer();request_sample();set_notice(L"设置已保存；重新开始累计空闲时间");}
}
static void change_timer(void){
    KillTimer(g.window,TIMER_SAMPLE);if(!g.service&&g.engine.cfg.enabled){UINT ms=g.engine.cfg.sample_seconds*1000;if((g.engine.phase==QG_WARNING||g.engine.phase==QG_READY)&&ms>2000)ms=2000;SetTimer(g.window,TIMER_SAMPLE,ms,NULL);}
    refresh_ui_timer();
}
static void service_request(uint32_t command){
    if(command==QG_CMD_CANCEL)g.cancel_pending_ui=true;
    if(InterlockedCompareExchange(&g.ipc.inflight,1,0)!=0){if(command==QG_CMD_CANCEL)InterlockedExchange(&g.ipc.pending_cancel,1);return;}
    EnterCriticalSection(&g.ipc.lock);g.ipc.command=command;g.ipc.generation=g.generation;LeaveCriticalSection(&g.ipc.lock);SetEvent(g.ipc.event);
}
static void service_refresh(void){service_request(QG_CMD_STATUS);}
static void service_result(const QgReply *r,bool ok,uint32_t cmd){
    if(cmd==QG_CMD_CANCEL)g.cancel_pending_ui=false;
    if(ok&&r->error==ERROR_SUCCESS){if(!g.service_config_loaded){g.cfg=r->config;g.service_config_loaded=true;fill_fields();if(!g.install){EnableWindow(g.save,TRUE);EnableWindow(g.start,TRUE);}}g.engine.cfg=r->config;g.engine.phase=r->phase;g.last=r->sample;g.engine.idle_since=r->snapshot_ms>=r->idle_ms?r->snapshot_ms-r->idle_ms:0;g.engine.have_idle=r->idle_ms>0;g.engine.warning_since=r->snapshot_ms>=r->warning_ms?r->snapshot_ms-r->warning_ms:0;if(cmd==QG_CMD_CANCEL)StringCchCopyW(g.notice,256,L"服务已确认暂停；服务重启或电脑重启后会按保存的设置恢复");}
    else {g.engine.phase=ok?r->phase:QG_UNKNOWN;if(ok){StringCchPrintfW(g.notice,256,r->error==ERROR_SHUTDOWN_IN_PROGRESS?L"已越过提交点，不能确认取消；Windows 错误 %lu":L"服务返回错误 %lu；未确认操作完成",r->error);update_view();return;}StringCchCopyW(g.notice,256,cmd==QG_CMD_CANCEL?L"取消尚未确认！请立即重连远程会话或重试":L"服务无响应：读数与取消状态尚未确认");}
    update_view();
}
static bool visible_ui(void){return g.window&&IsWindowVisible(g.window)&&!IsIconic(g.window);}
static void refresh_ui_timer(void){
    UINT interval=visible_ui()?(g.service?2000u:((g.engine.phase==QG_WARNING||g.engine.phase==QG_READY)?1000u:0u)):0u;
    if(interval==g.ui_interval)return;
    KillTimer(g.window,TIMER_UI);g.ui_interval=0;
    if(interval&&SetTimer(g.window,TIMER_UI,interval,NULL))g.ui_interval=interval;
}
static void set_text_if_changed(HWND h,const wchar_t *text){
    wchar_t previous[512];if(!h)return;GetWindowTextW(h,previous,512);if(wcscmp(previous,text))SetWindowTextW(h,text);
}
static void status_detail(wchar_t *out,size_t count){
    if(g.engine.phase==QG_WARNING||g.engine.phase==QG_READY){uint64_t now=GetTickCount64();uint64_t elapsed=now>=g.engine.warning_since?now-g.engine.warning_since:0;uint64_t total=(uint64_t)g.engine.cfg.warning_seconds*1000;StringCchPrintfW(out,count,L"约 %llu 秒后请求关机。点击“取消本次”可暂停。",(unsigned long long)(elapsed>=total?0:(total-elapsed+999)/1000));}
    else if(g.engine.have_idle){uint64_t elapsed=g.last.at_ms>=g.engine.idle_since?g.last.at_ms-g.engine.idle_since:0;StringCchPrintfW(out,count,L"已连续观察低使用率 %llu / %u 分钟。任一指标繁忙即重新计时。",(unsigned long long)(elapsed/60000),g.cfg.idle_seconds/60);}
    else StringCchCopyW(out,count,g.service?L"开机服务独立运行；关闭此窗口不会停止服务。":L"所有选中指标都须低于阈值；读数缺失时不会关机。");
}
static void update_view(void){
    if(!g.window)return;
    refresh_ui_timer();
    if(!g.service)tray_update();
    if(!visible_ui())return;
    static const wchar_t *names[]={L"CPU",L"GPU",L"磁盘",L"网络"};
    wchar_t b[512];
    for(unsigned i=0;i<4;i++){
        if(!(g.cfg.mask&(1u<<i)))StringCchPrintfW(b,512,L"%s · 未选用",names[i]);
        else if(!(g.last.valid&(1u<<i)))StringCchPrintfW(b,512,L"%s · 等待有效读数",names[i]);
        else StringCchPrintfW(b,512,i==3?L"%s  %.2f KiB/s":L"%s  %.1f %%",names[i],g.last.value[i]);
        set_text_if_changed(g.live[i],b);
    }
    set_text_if_changed(g.status_title,phase_text(g.engine.phase));status_detail(b,512);set_text_if_changed(g.status_detail,b);
    set_text_if_changed(g.start,g.engine.cfg.enabled?L"暂停监测 (&P)":L"开始监测 (&S)");
    bool can_cancel=g.engine.cfg.enabled||g.service||g.install||g.cancel_pending_ui;
    if((IsWindowEnabled(g.cancel)!=FALSE)!=can_cancel)EnableWindow(g.cancel,can_cancel);
    if(g.engine.phase==QG_WARNING||g.engine.phase==QG_READY){status_detail(b,512);if(g.cancel_pending_ui)StringCchCatW(b,512,L" 取消请求仍待服务确认。");set_text_if_changed(g.notice_label,b);}
    else set_text_if_changed(g.notice_label,g.notice[0]?g.notice:(g.dry?L"安全演练：不会执行关机，也不会修改启动项。":L"不强制关闭应用 · 不上传数据 · Esc 隐藏到托盘"));
    if(!g.view_valid||memcmp(&g.view_config,&g.cfg,sizeof(g.cfg))||g.view_service!=g.service){
        g.view_config=g.cfg;g.view_service=g.service;g.view_valid=true;
        InvalidateRect(g.content,NULL,FALSE);RECT header={px(g.geometry.header.x),0,px(g.geometry.header.x+g.geometry.header.w),px(g.geometry.header.h)};InvalidateRect(g.window,&header,FALSE);
    }
}
static BOOL CALLBACK apply_font(HWND h,LPARAM unused){(void)unused;SendMessageW(h,WM_SETFONT,(WPARAM)g.body,FALSE);return TRUE;}
static int CALLBACK font_available(const LOGFONTW *font,const TEXTMETRICW *metrics,DWORD kind,LPARAM result){
    (void)font;(void)metrics;(void)kind;*(bool*)result=true;return 0;
}
static void fonts(void){
    HFONT old[]={g.body,g.small,g.title,g.hero,g.semibold};
    const wchar_t *face=L"Microsoft YaHei UI";
    NONCLIENTMETRICSW ncm={0};ncm.cbSize=sizeof(ncm);
    LOGFONTW request={0};request.lfCharSet=DEFAULT_CHARSET;StringCchCopyW(request.lfFaceName,LF_FACESIZE,face);
    bool available=false;HDC screen=GetDC(NULL);
    if(screen){EnumFontFamiliesExW(screen,&request,font_available,(LPARAM)&available,0);ReleaseDC(NULL,screen);}
    if(!available)face=(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(ncm),&ncm,0)&&ncm.lfMessageFont.lfFaceName[0])?ncm.lfMessageFont.lfFaceName:L"Segoe UI";
    g.body=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,face);
    g.small=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,face);
    g.title=CreateFontW(-px(22),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,face);
    g.hero=CreateFontW(-px(22),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,face);
    g.semibold=CreateFontW(-px(15),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,face);
    if(g.window){EnumChildWindows(g.window,apply_font,0);SendMessageW(g.status_title,WM_SETFONT,(WPARAM)g.hero,FALSE);SendMessageW(g.page_title,WM_SETFONT,(WPARAM)g.title,FALSE);}
    for(unsigned i=0;i<5;i++)if(old[i])DeleteObject(old[i]);
}
static void theme(void){
    HIGHCONTRASTW h={sizeof(h),0,NULL};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(h),&h,0);g.highcontrast=(h.dwFlags&HCF_HIGHCONTRASTON)!=0;
    g.bg=g.highcontrast?GetSysColor(COLOR_WINDOW):RGB(236,236,236);g.sidebar_color=g.highcontrast?GetSysColor(COLOR_WINDOW):RGB(245,245,245);
    g.card=GetSysColor(COLOR_WINDOW);g.fg=g.highcontrast?GetSysColor(COLOR_WINDOWTEXT):RGB(29,29,31);g.muted=g.highcontrast?g.fg:RGB(100,100,108);
    g.accent=g.highcontrast?GetSysColor(COLOR_HIGHLIGHT):RGB(0,122,255);g.border=g.highcontrast?g.fg:RGB(225,225,228);
    if(g.background)DeleteObject(g.background);
    if(g.surface)DeleteObject(g.surface);
    g.background=CreateSolidBrush(g.bg);g.surface=CreateSolidBrush(g.card);
}
static HWND control(HWND parent,const wchar_t *klass,const wchar_t *text,DWORD style,int id){
    HWND h=CreateWindowExW(0,klass,text,WS_CHILD|style,0,0,10,10,parent,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(h,WM_SETFONT,(WPARAM)g.body,FALSE);return h;
}
static void buffer_release(void){
    if(g.paint_dc){if(g.paint_old)SelectObject(g.paint_dc,g.paint_old);if(g.paint_bitmap)DeleteObject(g.paint_bitmap);DeleteDC(g.paint_dc);}
    g.paint_dc=NULL;g.paint_bitmap=NULL;g.paint_old=NULL;g.paint_width=g.paint_height=0;
}
static bool buffer_prepare(HDC target){
    RECT r;GetClientRect(g.window,&r);int width=r.right,height=r.bottom;
    /* Exactly one window-sized bitmap, capped at 16 MiB at 32 bits/pixel. */
    if(!visible_ui()||width<=0||height<=0||(uint64_t)width*(uint64_t)height>4194304u){buffer_release();return false;}
    if(g.paint_dc&&g.paint_width==width&&g.paint_height==height)return true;
    buffer_release();g.paint_dc=CreateCompatibleDC(target);if(!g.paint_dc)return false;
    g.paint_bitmap=CreateCompatibleBitmap(target,width,height);if(!g.paint_bitmap){buffer_release();return false;}
    g.paint_old=SelectObject(g.paint_dc,g.paint_bitmap);if(!g.paint_old||g.paint_old==(HGDIOBJ)(INT_PTR)-1){buffer_release();return false;}
    g.paint_width=width;g.paint_height=height;return true;
}
typedef struct { HWND window; int x,y,width,height; UINT flags; } Placement;
typedef struct { Placement items[32]; unsigned count; } PlacementBatch;
static void place(PlacementBatch *batch,HWND h,QgUiRect r,bool visible,bool scrolling){
    if(!h)return;
    int x=px(r.x),y=px(r.y)-(scrolling?px(g.scroll):0),width=px(r.w),height=px(r.h);
    RECT old;GetWindowRect(h,&old);MapWindowPoints(NULL,GetParent(h),(POINT*)&old,2);
    bool shown=(GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)!=0;
    UINT flags=SWP_NOZORDER|SWP_NOACTIVATE;
    if(old.left==x&&old.top==y)flags|=SWP_NOMOVE;
    if(old.right-old.left==width&&old.bottom-old.top==height)flags|=SWP_NOSIZE;
    if(shown!=visible)flags|=visible?SWP_SHOWWINDOW:SWP_HIDEWINDOW;
    if((flags&(SWP_NOMOVE|SWP_NOSIZE))==(SWP_NOMOVE|SWP_NOSIZE)&&shown==visible)return;
    Placement p={h,x,y,width,height,flags};
    if(batch->count<32)batch->items[batch->count++]=p;
}
static void apply_placements(const PlacementBatch *batch){
    if(!batch->count)return;
    HDWP deferred=BeginDeferWindowPos((int)batch->count);
    for(unsigned i=0;deferred&&i<batch->count;i++){
        const Placement *p=&batch->items[i];deferred=DeferWindowPos(deferred,p->window,NULL,p->x,p->y,p->width,p->height,p->flags);
    }
    if(deferred&&EndDeferWindowPos(deferred))return;
    /* Any batching failure replays the entire same-parent batch. */
    for(unsigned i=0;i<batch->count;i++){
        const Placement *p=&batch->items[i];SetWindowPos(p->window,NULL,p->x,p->y,p->width,p->height,p->flags);
    }
}
static void layout(void){
    if(g.in_layout||!g.content)return;
    g.in_layout=true;RECT client;GetClientRect(g.window,&client);
    QgUiLayout next;qg_ui_layout(MulDiv(client.right,96,g.dpi),MulDiv(client.bottom,96,g.dpi),g.page,g.scroll,&next);
    bool changed=memcmp(&next,&g.geometry,sizeof(next))!=0;
    bool shell_changed=memcmp(&next.sidebar,&g.geometry.sidebar,sizeof(next.sidebar))||memcmp(&next.viewport,&g.geometry.viewport,sizeof(next.viewport));
    g.geometry=next;g.scroll=next.scroll;
    PlacementBatch shell={0},body={0};
    place(&shell,g.content,next.viewport,true,false);place(&shell,g.scrollbar,next.scrollbar,next.scroll_max>0,false);
    for(int i=0;i<3;i++)place(&shell,g.nav[i],next.nav[i],true,false);
    HWND actions[]={g.start,g.save,g.cancel,g.help};for(int i=0;i<4;i++)place(&shell,actions[i],next.actions[i],true,false);
    place(&shell,g.notice_label,next.notice,true,false);
    bool rules=g.page==QG_PAGE_RULES,overview=g.page==QG_PAGE_OVERVIEW;
    for(int i=0;i<4;i++){
        place(&body,g.check[i],next.check[i],rules,true);place(&body,g.label[i],next.label[i],rules,true);place(&body,g.edit[i],next.edit[i],rules,true);
        place(&body,g.live[i],next.live[i],rules||overview,true);
    }
    for(int i=0;i<3;i++)place(&body,g.field_label[i],next.field_label[i],rules,true);
    place(&body,g.duration,next.field[0],rules,true);place(&body,g.interval,next.field[1],rules,true);place(&body,g.warning,next.field[2],rules,true);
    place(&body,g.mode,next.mode,g.page==QG_PAGE_STARTUP,true);place(&body,g.veto,next.veto,g.page==QG_PAGE_STARTUP,true);
    place(&body,g.status_title,next.status_title,overview,true);place(&body,g.status_detail,next.status_detail,overview,true);
    place(&shell,g.page_title,(QgUiRect){next.header.x+22,14,next.header.w-180,36},true,false);
    apply_placements(&shell);apply_placements(&body);
    /* A child scrollbar cannot change the parent client size or trigger a WM_SIZE loop. */
    if(g.scroll_range!=next.content_height||g.scroll_page!=next.viewport.h||g.scroll_position!=next.scroll){
        SCROLLINFO si={sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS,0,next.content_height-1,(UINT)next.viewport.h,next.scroll,0};
        SetScrollInfo(g.scrollbar,SB_CTL,&si,TRUE);g.scroll_range=next.content_height;g.scroll_page=next.viewport.h;g.scroll_position=next.scroll;
    }
    if(shell_changed)InvalidateRect(g.window,NULL,FALSE);
    if(changed)InvalidateRect(g.content,NULL,FALSE);
    g.in_layout=false;
}
static void scroll_to(int position){
    if(position<0)position=0;
    if(position>g.geometry.scroll_max)position=g.geometry.scroll_max;
    if(position==g.scroll)return;
    g.scroll=position;layout();
}
static void select_page(QgUiPage page){
    if(g.page==page)return;
    HWND focused=GetFocus();if(focused&&GetParent(focused)==g.content)SetFocus(g.nav[(int)page]);
    int previous=(int)g.page;g.page=page;g.scroll=0;
    static const wchar_t *titles[]={L"总览",L"监测规则",L"启动与保护"};
    static const wchar_t *labels[]={L"总览 (&O)",L"监测规则 (&R)",L"启动与保护 (&B)"};
    set_text_if_changed(g.page_title,titles[(int)page]);
    for(int i=0;i<3;i++){wchar_t name[64];StringCchPrintfW(name,64,i==(int)page?L"%s · 当前":L"%s",labels[i]);set_text_if_changed(g.nav[i],name);}
    layout();
    InvalidateRect(g.nav[previous],NULL,FALSE);InvalidateRect(g.nav[(int)page],NULL,FALSE);InvalidateRect(g.window,NULL,FALSE);InvalidateRect(g.content,NULL,FALSE);
}
static void text_at(HDC dc,HFONT font,COLORREF color,QgUiRect r,const wchar_t *s,UINT flags){
    RECT bounds={px(r.x),px(r.y),px(r.x+r.w),px(r.y+r.h)};SelectObject(dc,font);SetTextColor(dc,color);DrawTextW(dc,s,-1,&bounds,DT_NOPREFIX|flags);
}
static void box(HDC dc,QgUiRect r,COLORREF fill,COLORREF edge,int radius){
    HGDIOBJ old_brush=SelectObject(dc,GetStockObject(DC_BRUSH)),old_pen=SelectObject(dc,GetStockObject(DC_PEN));
    SetDCBrushColor(dc,fill);SetDCPenColor(dc,edge);RoundRect(dc,px(r.x),px(r.y),px(r.x+r.w),px(r.y+r.h),px(radius*2),px(radius*2));SelectObject(dc,old_brush);SelectObject(dc,old_pen);
}
static void line_at(HDC dc,int x,int y,int width,COLORREF color){
    HGDIOBJ old=SelectObject(dc,GetStockObject(DC_PEN));SetDCPenColor(dc,color);MoveToEx(dc,px(x),px(y),NULL);LineTo(dc,px(x+width),px(y));SelectObject(dc,old);
}
static QgUiRect ui_rect(int x,int y,int width,int height){QgUiRect r={x,y,width,height};return r;}
static void card(HDC dc,QgUiRect r,const wchar_t *title,int symbol){
    box(dc,r,g.card,g.highcontrast?g.border:g.card,10);
    box(dc,ui_rect(r.x+16,r.y+11,30,30),g.highcontrast?g.card:RGB(227,239,255),g.highcontrast?g.border:RGB(227,239,255),6);
    text_at(dc,g.semibold,g.accent,ui_rect(r.x+16,r.y+12,30,28),symbol==0?L"◷":symbol==1?L"≡":L"✓",DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    text_at(dc,g.semibold,g.fg,ui_rect(r.x+58,r.y+11,r.w-76,30),title,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    line_at(dc,r.x,r.y+52,r.w,g.border);
}
static void paint_content(HDC dc){
    QgUiLayout *u=&g.geometry;
    if(g.page==QG_PAGE_OVERVIEW){
        card(dc,u->cards[0],g.dry?L"监测状态 · 安全演练":L"监测状态",0);
        static const wchar_t *names[]={L"CPU 使用率",L"GPU 最忙引擎",L"磁盘活动时间",L"网络收发合计"};
        for(int i=0;i<4;i++)card(dc,u->cards[i+1],names[i],1);
        text_at(dc,g.small,g.muted,ui_rect(30,u->content_height-54,u->viewport.w-60,48),L"低使用率不代表没有重要任务。采样可能漏过短暂活动。\n阈值与持续时间可在“监测规则”中调整。",DT_WORDBREAK);
    }else if(g.page==QG_PAGE_RULES){
        card(dc,u->cards[0],L"全部选中指标低于阈值",1);card(dc,u->cards[1],L"持续时间与预告",0);
        QgUiRect c=u->cards[1];text_at(dc,g.small,g.muted,ui_rect(c.x+18,c.y+c.h-62,c.w-36,50),L"任何繁忙、未知读数或睡眠都会重新计时。",DT_WORDBREAK);
        text_at(dc,g.small,g.muted,ui_rect(30,u->content_height-62,u->viewport.w-60,58),L"CPU 为全机平均；GPU / 磁盘取最忙项。\n网络包含虚拟接口，VPN 可能重复计入。",DT_WORDBREAK);
    }else{
        card(dc,u->cards[0],L"启动方式",0);card(dc,u->cards[1],L"连接会话保护",2);
        QgUiRect c=u->cards[0];text_at(dc,g.small,g.muted,ui_rect(c.x+18,c.y+112,c.w-36,84),L"登录启动：首次登录后运行，断开远程桌面后继续。\n开机服务：管理员安装后，登录前也可监测。服务没有桌面预告窗口；关闭此窗口不会停止服务。",DT_WORDBREAK);
        c=u->cards[1];text_at(dc,g.small,g.muted,ui_rect(c.x+18,c.y+116,c.w-36,86),L"已连接的控制台 / RDP 会话会阻止计时，包含仍连接的锁屏。断开后可计时，重新连接会取消。\n关闭保护前，请确认无人值守任务适合自动关机。",DT_WORDBREAK);
    }
}
static void paint_shell(HDC dc){
    QgUiLayout *u=&g.geometry;RECT side={0,0,px(u->sidebar.w),px(u->sidebar.h)};
    SetDCBrushColor(dc,g.sidebar_color);FillRect(dc,&side,(HBRUSH)GetStockObject(DC_BRUSH));
    RECT header={px(u->header.x),0,px(u->header.x+u->header.w),px(u->header.h)};FillRect(dc,&header,g.surface);
    RECT footer={px(u->footer.x),px(u->footer.y),px(u->footer.x+u->footer.w),px(u->footer.y+u->footer.h)};FillRect(dc,&footer,g.surface);
    HICON icon=(HICON)LoadImageW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(101),IMAGE_ICON,px(28),px(28),LR_SHARED);DrawIconEx(dc,px(16),px(26),icon,px(28),px(28),0,NULL,DI_NORMAL);
    text_at(dc,g.semibold,g.fg,ui_rect(52,25,u->sidebar.w-60,32),L"QuietGuard",DT_SINGLELINE|DT_VCENTER);

    text_at(dc,g.small,g.dry?g.accent:g.muted,ui_rect(u->header.x+u->header.w-154,22,136,24),g.dry?L"安全演练":(g.service?L"开机服务":L"登录会话"),DT_SINGLELINE|DT_RIGHT);
    line_at(dc,u->header.x,u->header.h-1,u->header.w,g.border);line_at(dc,u->footer.x,u->footer.y,u->footer.w,g.border);
    if(u->sidebar.h>360)text_at(dc,g.small,g.muted,ui_rect(18,u->sidebar.h-66,u->sidebar.w-32,44),L"原生轻量监测\n无后台动画",DT_WORDBREAK);
}
static void paint(HWND h,bool content){
    PAINTSTRUCT ps;HDC target=BeginPaint(h,&ps);if(!visible_ui()){EndPaint(h,&ps);return;}
    bool buffered=buffer_prepare(target);HDC dc=buffered?g.paint_dc:target;int saved=SaveDC(dc);
    IntersectClipRect(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right,ps.rcPaint.bottom);SetBkMode(dc,TRANSPARENT);
    FillRect(dc,&ps.rcPaint,g.background);
    if(content){SetViewportOrgEx(dc,0,-px(g.scroll),NULL);paint_content(dc);}else paint_shell(dc);
    RestoreDC(dc,saved);
    if(buffered)BitBlt(target,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,dc,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
    EndPaint(h,&ps);
}
static void draw_button(const DRAWITEMSTRUCT *d){
    if(d->CtlType!=ODT_BUTTON)return;
    int saved=SaveDC(d->hDC);SetBkMode(d->hDC,TRANSPARENT);
    bool nav=d->CtlID>=ID_NAV&&d->CtlID<ID_NAV+3,selected=nav&&(int)(d->CtlID-ID_NAV)==(int)g.page;
    bool primary=d->CtlID==ID_START,disabled=(d->itemState&ODS_DISABLED)!=0,pressed=(d->itemState&ODS_SELECTED)!=0;
    COLORREF fill=nav?g.sidebar_color:g.card,fg=g.fg,edge=g.border;
    if(selected){fill=g.highcontrast?GetSysColor(COLOR_HIGHLIGHT):RGB(210,230,252);fg=g.highcontrast?GetSysColor(COLOR_HIGHLIGHTTEXT):g.fg;}
    else if(primary){fill=g.accent;fg=GetSysColor(COLOR_HIGHLIGHTTEXT);}
    if(pressed&&!g.highcontrast)fill=primary?RGB(0,98,214):RGB(225,230,237);
    if(disabled){fill=g.highcontrast?GetSysColor(COLOR_BTNFACE):RGB(238,238,240);fg=GetSysColor(COLOR_GRAYTEXT);}
    SetDCBrushColor(d->hDC,nav?g.sidebar_color:g.card);FillRect(d->hDC,&d->rcItem,(HBRUSH)GetStockObject(DC_BRUSH));
    HGDIOBJ oldb=SelectObject(d->hDC,GetStockObject(DC_BRUSH)),oldp=SelectObject(d->hDC,GetStockObject(DC_PEN));SetDCBrushColor(d->hDC,fill);SetDCPenColor(d->hDC,nav?fill:((primary&&!disabled)?fill:edge));
    RoundRect(d->hDC,0,0,d->rcItem.right,d->rcItem.bottom,px(16),px(16));SelectObject(d->hDC,oldb);SelectObject(d->hDC,oldp);
    wchar_t label[80];GetWindowTextW(d->hwndItem,label,80);if(nav){wchar_t *suffix=wcsstr(label,L" · 当前");if(suffix)*suffix=0;}if(d->itemState&ODS_NOACCEL){wchar_t *accelerator=wcsstr(label,L" (&");if(accelerator)*accelerator=0;}RECT text=d->rcItem;text.left+=px(nav?14:4);text.right-=px(4);SetTextColor(d->hDC,fg);SelectObject(d->hDC,g.body);
    UINT flags=DT_SINGLELINE|DT_VCENTER|(nav?DT_LEFT:DT_CENTER);if(d->itemState&ODS_NOACCEL)flags|=DT_HIDEPREFIX;DrawTextW(d->hDC,label,-1,&text,flags);
    if((d->itemState&ODS_FOCUS)&&!(d->itemState&ODS_NOFOCUSRECT)){RECT focus=d->rcItem;InflateRect(&focus,-px(4),-px(4));DrawFocusRect(d->hDC,&focus);}
    RestoreDC(d->hDC,saved);
}
static LRESULT CALLBACK content_proc(HWND h,UINT msg,WPARAM w,LPARAM l){
    switch(msg){
        case WM_PAINT:paint(h,true);return 0;
        case WM_ERASEBKGND:return 1;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLOREDIT:case WM_DRAWITEM:case WM_COMMAND:case WM_MOUSEWHEEL:return SendMessageW(g.window,msg,w,l);
    }
    return DefWindowProcW(h,msg,w,l);
}
static void create_controls(void){
    g.content=CreateWindowExW(WS_EX_CONTROLPARENT,L"QuietGuardContent",NULL,WS_CHILD|WS_CLIPCHILDREN,0,0,10,10,g.window,NULL,GetModuleHandleW(NULL),NULL);
    g.scrollbar=control(g.window,L"SCROLLBAR",L"",SBS_VERT,0);
    const wchar_t *nav[]={L"总览 (&O)",L"监测规则 (&R)",L"启动与保护 (&B)"};
    for(int i=0;i<3;i++)g.nav[i]=control(g.window,L"BUTTON",nav[i],BS_OWNERDRAW|WS_TABSTOP,ID_NAV+i);
    set_text_if_changed(g.nav[0],L"总览 (&O) · 当前");
    g.page_title=control(g.window,L"STATIC",L"总览",SS_LEFT,0);SendMessageW(g.page_title,WM_SETFONT,(WPARAM)g.title,FALSE);
    g.status_title=control(g.content,L"STATIC",L"已暂停",SS_LEFT,0);SendMessageW(g.status_title,WM_SETFONT,(WPARAM)g.hero,FALSE);
    g.status_detail=control(g.content,L"STATIC",L"",SS_LEFT,0);
    static const wchar_t *names[]={L"CPU (&C)",L"GPU (&G)",L"磁盘 (&K)",L"网络 (&N)"};
    static const wchar_t *labels[]={L"CPU 阈值 % (&1)",L"GPU 阈值 % (&2)",L"磁盘阈值 % (&3)",L"网络 KiB/s (&4)"};
    for(int i=0;i<4;i++){
        g.check[i]=control(g.content,L"BUTTON",names[i],BS_AUTOCHECKBOX|WS_TABSTOP,ID_CHECK+i);
        g.label[i]=control(g.content,L"STATIC",labels[i],SS_LEFT,0);g.edit[i]=control(g.content,L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,ID_THRESHOLD+i);SendMessageW(g.edit[i],EM_SETLIMITTEXT,20,0);
        g.live[i]=control(g.content,L"STATIC",L"等待有效读数",SS_LEFT,0);
    }
    g.field_label[0]=control(g.content,L"STATIC",L"持续分钟 (&D)",SS_LEFT,0);g.duration=control(g.content,L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,ID_DURATION);
    g.field_label[1]=control(g.content,L"STATIC",L"采样秒数 (&I)",SS_LEFT,0);g.interval=control(g.content,L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,ID_INTERVAL);
    g.field_label[2]=control(g.content,L"STATIC",L"预告秒数 (&W)",SS_LEFT,0);g.warning=control(g.content,L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,ID_WARNING);
    g.mode=control(g.content,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,ID_MODE);SendMessageW(g.mode,CB_ADDSTRING,0,(LPARAM)L"仅手动启动");SendMessageW(g.mode,CB_ADDSTRING,0,(LPARAM)L"登录后自动启动（推荐）");SendMessageW(g.mode,CB_ADDSTRING,0,(LPARAM)L"开机服务（需要管理员）");SendMessageW(g.mode,CB_SETCURSEL,g.service?2:(has_login_startup()||GetFileAttributesW(g.config_path)==INVALID_FILE_ATTRIBUTES?1:0),0);
    g.veto=control(g.content,L"BUTTON",L"有人连接时，不进行关机计时 (&V)",BS_AUTOCHECKBOX|BS_MULTILINE|WS_TABSTOP,ID_VETO);
    g.notice_label=control(g.window,L"STATIC",L"",SS_LEFT,0);
    g.start=control(g.window,L"BUTTON",L"开始监测 (&S)",BS_OWNERDRAW|WS_TABSTOP,ID_START);g.save=control(g.window,L"BUTTON",L"保存设置 (&A)",BS_OWNERDRAW|WS_TABSTOP,ID_SAVE);g.cancel=control(g.window,L"BUTTON",L"取消本次 (&X)",BS_OWNERDRAW|WS_TABSTOP,ID_CANCEL);g.help=control(g.window,L"BUTTON",L"说明 (&H)",BS_OWNERDRAW|WS_TABSTOP,ID_HELP);fill_fields();layout();
}
static void show_main(void){ShowWindow(g.window,SW_RESTORE);SetForegroundWindow(g.window);refresh_ui_timer();update_view();if(g.service)service_refresh();}
static void tray_menu(void){HMENU m=CreatePopupMenu();AppendMenuW(m,MF_STRING,ID_TRAY_OPEN,L"打开 QuietGuard");AppendMenuW(m,MF_STRING,ID_TRAY_PAUSE,L"取消并暂停监测");AppendMenuW(m,MF_SEPARATOR,0,NULL);AppendMenuW(m,MF_STRING,ID_TRAY_EXIT,L"退出登录监测");POINT p;GetCursorPos(&p);SetForegroundWindow(g.window);TrackPopupMenu(m,TPM_RIGHTBUTTON,p.x,p.y,0,g.window,NULL);DestroyMenu(m);PostMessageW(g.window,WM_NULL,0,0);}
static void install_finished(void){DWORD exit_code;if(!GetExitCodeProcess(g.install,&exit_code)||exit_code==STILL_ACTIVE)return;CloseHandle(g.install);g.install=NULL;DeleteFileW(g.staging_path);KillTimer(g.window,TIMER_INSTALL);EnableWindow(g.save,TRUE);EnableWindow(g.start,TRUE);
    if(exit_code!=0){g.service=qg_service_installed();SendMessageW(g.mode,CB_SETCURSEL,g.service?2:0,0);if(g.cancel_after_install){g.cancel_after_install=false;if(g.service){service_request(QG_CMD_CANCEL);set_notice(L"管理员操作报错；服务可能已经启动，正在确认取消");}else pause_monitor(true);}else set_notice(L"启动方式尚未完整切换，请检查后重试");change_timer();error_box(L"管理员操作未完成；请以服务状态为准",exit_code);return;}
    g.service=qg_service_installed();if(g.service){g.service_config_loaded=false;EnableWindow(g.save,FALSE);EnableWindow(g.start,FALSE);tray_remove();service_refresh();}else {g.cfg.enabled=0;qg_engine_configure(&g.engine,&g.cfg);if(!login_startup(g.pending_mode==1))error_box(L"服务已移除，但登录启动未能应用",GetLastError());qg_write_config_file(g.config_path,&g.cfg);tray_update();}
    if(g.cancel_after_install){g.cancel_after_install=false;if(g.service){service_request(QG_CMD_CANCEL);set_notice(L"设置已应用，正在再次确认取消；请保持窗口打开");}else pause_monitor(true);}
    else set_notice(L"启动方式已应用；首次部署请先使用安全演练验证");
    change_timer();
}
static LRESULT CALLBACK window_proc(HWND h,UINT msg,WPARAM w,LPARAM l){
    static UINT taskbar_created=0;
    if(taskbar_created&&msg==taskbar_created){g.tray=false;if(!g.service)tray_update();return 0;}
    switch(msg){
    case WM_CREATE:g.window=h;g.session_events=WTSRegisterSessionNotification(h,NOTIFY_FOR_ALL_SESSIONS)!=FALSE;taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");create_controls();return 0;
    case WM_PAINT:paint(h,false);return 0;
    case WM_DRAWITEM:draw_button((const DRAWITEMSTRUCT*)l);return TRUE;
    case DM_GETDEFID:case DM_SETDEFID:return 0;
    case WM_ERASEBKGND:return 1;
    case WM_CTLCOLORSTATIC:SetTextColor((HDC)w,(HWND)l==g.status_title?g.accent:((HWND)l==g.page_title?g.fg:g.muted));SetBkColor((HDC)w,g.card);return (LRESULT)g.surface;
    case WM_CTLCOLOREDIT:SetTextColor((HDC)w,g.fg);SetBkColor((HDC)w,g.card);return (LRESULT)g.surface;
    case WM_CTLCOLORBTN:SetTextColor((HDC)w,g.fg);SetBkColor((HDC)w,g.card);return (LRESULT)g.surface;
    case WM_DPICHANGED:{g.dpi=HIWORD(w);fonts();buffer_release();RECT *r=(RECT*)l;SetWindowPos(h,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();RedrawWindow(h,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN);return 0;}
    case WM_SETTINGCHANGE:case WM_THEMECHANGED:theme();RedrawWindow(h,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN);return 0;
    case WM_GETMINMAXINFO:{MINMAXINFO *limits=(MINMAXINFO*)l;RECT work;SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);limits->ptMinTrackSize.x=px(640);if(limits->ptMinTrackSize.x>work.right-work.left)limits->ptMinTrackSize.x=work.right-work.left;limits->ptMinTrackSize.y=px(300);if(limits->ptMinTrackSize.y>work.bottom-work.top)limits->ptMinTrackSize.y=work.bottom-work.top;return 0;}
    case WM_SHOWWINDOW:if(!w){KillTimer(h,TIMER_UI);g.ui_interval=0;buffer_release();}break;
    case WM_WINDOWPOSCHANGED:{
        LRESULT result=DefWindowProcW(h,msg,w,l);const WINDOWPOS *position=(const WINDOWPOS*)l;
        /* WM_SHOWWINDOW is pre-change; refresh after visibility is actually applied. */
        if(position->flags&SWP_SHOWWINDOW){refresh_ui_timer();update_view();if(g.service&&g.ipc.thread)service_refresh();}
        return result;
    }
    case WM_POWERBROADCAST:if(w==PBT_APMSUSPEND||w==PBT_APMRESUMEAUTOMATIC||w==PBT_APMRESUMESUSPEND){if(!g.service&&g.engine.phase!=QG_REQUESTED){qg_engine_reset(&g.engine,g.engine.cfg.enabled?QG_UNKNOWN:QG_PAUSED);worker_reset();KillTimer(h,TIMER_COMMIT);change_timer();update_view();}}return TRUE;
    case WM_WTSSESSION_CHANGE:if(!g.service&&g.engine.phase!=QG_REQUESTED){qg_engine_reset(&g.engine,g.engine.cfg.enabled?QG_UNKNOWN:QG_PAUSED);worker_reset();KillTimer(h,TIMER_COMMIT);change_timer();update_view();}return 0;
    case WM_SAMPLE:{Worker *worker=w?&g.ipc:&g.worker;EnterCriticalSection(&worker->lock);QgSample s=worker->result;uint64_t gen=worker->result_generation;uint32_t cmd=worker->result_command;QgReply reply=worker->service_reply;bool ok=worker->service_ok;LeaveCriticalSection(&worker->lock);InterlockedExchange(&worker->inflight,0);
        if(cmd){if(g.service)service_result(&reply,ok,cmd);if(InterlockedExchange(&g.ipc.pending_cancel,0))service_request(QG_CMD_CANCEL);return 0;}
        if(gen!=g.generation||g.service||!g.engine.cfg.enabled)return 0;
        g.last=s;QgPhase prior=g.engine.phase;qg_engine_sample(&g.engine,&s);if(g.engine.phase==QG_WARNING&&prior!=QG_WARNING){show_main();MessageBeep(MB_ICONWARNING);}if(g.engine.phase==QG_READY){g.commit_epoch=g.engine.epoch;SetTimer(h,TIMER_COMMIT,200,NULL);}else KillTimer(h,TIMER_COMMIT);change_timer();update_view();return 0;}
    case WM_SIZE:if(w==SIZE_MINIMIZED){g.was_minimized=true;KillTimer(h,TIMER_UI);g.ui_interval=0;buffer_release();}else{bool restored=g.was_minimized;g.was_minimized=false;layout();refresh_ui_timer();update_view();if(restored&&g.service&&g.ipc.thread)service_refresh();}return 0;
    case WM_VSCROLL:{if((HWND)l!=g.scrollbar)return 0;SCROLLINFO si={sizeof(si),SIF_TRACKPOS,0,0,0,0,0};GetScrollInfo(g.scrollbar,SB_CTL,&si);int pos=g.scroll;switch(LOWORD(w)){case SB_LINEUP:pos-=32;break;case SB_LINEDOWN:pos+=32;break;case SB_PAGEUP:pos-=g.geometry.viewport.h;break;case SB_PAGEDOWN:pos+=g.geometry.viewport.h;break;case SB_THUMBTRACK:case SB_THUMBPOSITION:pos=si.nTrackPos;break;case SB_TOP:pos=0;break;case SB_BOTTOM:pos=g.geometry.scroll_max;break;default:break;}scroll_to(pos);return 0;}
    case WM_MOUSEWHEEL:scroll_to(g.scroll-GET_WHEEL_DELTA_WPARAM(w)*72/WHEEL_DELTA);return 0;
    case WM_TIMER:
        if(w==TIMER_SAMPLE)request_sample();
        else if(w==TIMER_UI){if(!visible_ui()){KillTimer(h,TIMER_UI);g.ui_interval=0;buffer_release();}else if(g.service)service_refresh();else update_view();}
        else if(w==TIMER_INSTALL)install_finished();
        else if(w==TIMER_COMMIT){KillTimer(h,TIMER_COMMIT);if(qg_engine_can_commit(&g.engine,g.commit_epoch,GetTickCount64())){qg_engine_cancel(&g.engine);KillTimer(h,TIMER_SAMPLE);DWORD e=qg_shutdown_request(g.dry);g.engine.phase=e==ERROR_SUCCESS?QG_REQUESTED:QG_FAULT;if(e!=ERROR_SUCCESS)error_box(L"Windows 未接受关机请求",e);if(g.monitor){CloseHandle(g.monitor);g.monitor=NULL;}change_timer();update_view();}}
        return 0;
    case WM_COMMAND:switch(LOWORD(w)){
        case ID_NAV:case ID_NAV+1:case ID_NAV+2:select_page((QgUiPage)(LOWORD(w)-ID_NAV));break;
        case ID_START:if(g.engine.cfg.enabled)pause_monitor(true);else save_settings(true);break;case ID_SAVE:save_settings(false);break;case ID_CANCEL:case ID_TRAY_PAUSE:pause_monitor(true);break;
        case ID_TRAY_OPEN:show_main();break;case ID_TRAY_EXIT:if(g.install||g.cancel_pending_ui){show_main();break;}if(g.engine.cfg.enabled&&MessageBoxW(h,L"退出后将停止此登录会话的监测。继续？",L"退出 QuietGuard",MB_YESNO|MB_ICONQUESTION)!=IDYES)break;pause_monitor(false);DestroyWindow(h);break;
        case ID_HELP:MessageBoxW(h,L"运行逻辑\n每次采样中，所有选中指标都须严格低于阈值。繁忙、缺失、睡眠、配置变更或采样过期会重新计时；不保留上次开机的进度。\n\n安全边界\n低资源使用率不能证明没有重要任务；等待网络、长时间小负载任务和短暂高峰都可能被遗漏。GPU/磁盘计数器不可用时，保持等待。\n\n登录启动\n首次登录后运行，断开 RDP 后继续；注销会停止，重启后登录前不运行。\n\n开机服务\n管理员安装后，登录前也运行。服务没有桌面预告窗口；重连会触发会话保护，或打开本窗口取消。\n\n关机\n预告结束后重新验证读数，向 Windows 发送不强制关闭应用的关机请求。应用可阻止关机。请求交给 Windows 之后无法保证取消。\n\n本版本须先完成目标 Windows 电脑上的安全演练与性能验证。",L"QuietGuard · 使用说明",MB_OK|MB_ICONINFORMATION);break;
        }return 0;
    case WM_TRAY:if(LOWORD(l)==NIN_SELECT||LOWORD(l)==NIN_KEYSELECT||LOWORD(l)==WM_LBUTTONDBLCLK)show_main();else if(LOWORD(l)==WM_CONTEXTMENU||LOWORD(l)==WM_RBUTTONUP)tray_menu();return 0;
    case WM_CLOSE:if(g.cancel_pending_ui){MessageBoxW(h,L"取消尚未得到服务确认，请保持窗口打开。",L"QuietGuard",MB_OK|MB_ICONINFORMATION);return 0;}if(g.install){MessageBoxW(h,L"正在应用启动方式，请等待管理员操作完成。可点击“取消本次”要求完成后暂停。",L"QuietGuard",MB_OK|MB_ICONINFORMATION);return 0;}if(g.service){DestroyWindow(h);}else if(g.tray){ShowWindow(h,SW_HIDE);change_timer();}else DestroyWindow(h);return 0;
    case WM_QUERYENDSESSION:return TRUE;
    case WM_ENDSESSION:if(w){qg_engine_cancel(&g.engine);DestroyWindow(h);}return 0;
    case WM_DESTROY:buffer_release();if(g.session_events)WTSUnRegisterSessionNotification(h);KillTimer(h,TIMER_SAMPLE);KillTimer(h,TIMER_COMMIT);KillTimer(h,TIMER_UI);KillTimer(h,TIMER_INSTALL);tray_remove();PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,msg,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show){
    (void)previous;(void)command;setlocale(LC_NUMERIC,"C");int argc=0;wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);const wchar_t *input=NULL;int action=0;bool self_test=false,service_mode=false,build_info=false,invalid=false;
    if(!argv)return ERROR_INVALID_PARAMETER;
    QgSafeOptions safe_options;QgSafeParse safe=qg_safe_parse(argc,(const wchar_t *const *)argv,&safe_options);
    if(safe!=QG_SAFE_ABSENT){LocalFree(argv);return safe==QG_SAFE_VALID?qg_run_sensor_test(&safe_options):ERROR_INVALID_PARAMETER;}
    for(int i=1;i<argc;i++){
        int found=0;
        if(!wcscmp(argv[i],L"--build-info")){if(build_info)invalid=true;build_info=true;}
        else if(!wcscmp(argv[i],L"--self-test")){if(self_test)invalid=true;self_test=true;}
        else if(!wcscmp(argv[i],L"--dry-run")){if(g.dry)invalid=true;g.dry=true;}
        else if(!wcscmp(argv[i],L"--background")){if(g.background_launch)invalid=true;g.background_launch=true;}
        else if(!wcscmp(argv[i],L"--service")||!wcscmp(argv[i],L"--service-dry-run")){if(service_mode)invalid=true;service_mode=true;}
        else if(!wcscmp(argv[i],L"--install-service"))found=1;
        else if(!wcscmp(argv[i],L"--remove-service"))found=2;
        else if(!wcscmp(argv[i],L"--configure-service"))found=3;
        else if(!wcscmp(argv[i],L"--install-service-dry-run"))found=4;
        else if(!wcscmp(argv[i],L"--config")&&i+1<argc){if(input)invalid=true;input=argv[++i];}
        else invalid=true;
        if(found){if(action)invalid=true;action=found;}
    }
    if(invalid||(build_info&&argc!=2)||(self_test&&argc!=2)||(service_mode&&argc!=2)||(g.dry&&(action||input))||(input&&!action)||(action&&g.background_launch)||(action!=2&&action&&!input)){LocalFree(argv);return ERROR_INVALID_PARAMETER;}
    if(build_info){LocalFree(argv);MessageBoxW(NULL,L"QuietGuard " QG_VERSION_W L" preview\n\nSource commit:\n" QG_BUILD_COMMIT_W L"\nSource bytes SHA256:\n" QG_BUILD_SOURCE_W L"\nUncommitted changes at build: " QG_BUILD_DIRTY_W L"\n\nNative Windows runtime validation: pending",L"Build identity",MB_OK);return 0;}
    if(self_test){LocalFree(argv);int core=qg_self_test();int arguments=qg_safe_cli_self_test();return core?core:arguments;}
    if(service_mode){LocalFree(argv);return qg_service_main();}
    if(action){DWORD error=ERROR_ACCESS_DENIED;if(qg_is_admin()){if(action==2)error=qg_service_remove();else{QgConfig c;if(input&&qg_read_config_file(input,&c)){if(action==1||action==4)error=qg_service_install(&c);else{QgRequest q={0};QgReply r;q.magic=QG_WIRE_MAGIC;q.command=QG_CMD_CONFIG;q.config=c;error=qg_service_exchange(&q,&r)?r.error:GetLastError();}}else error=ERROR_INVALID_DATA;}}LocalFree(argv);return (int)error;}
    LocalFree(argv);HANDLE ui_instance=CreateMutexW(NULL,FALSE,g.dry?L"Local\\QuietGuard.UI.DryRun.v1":L"Local\\QuietGuard.UI.v1");if(!ui_instance)return 1;if(GetLastError()==ERROR_ALREADY_EXISTS){HWND existing=FindWindowW(L"QuietGuardWindow",NULL);if(existing){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}CloseHandle(ui_instance);return 0;}g.dpi=96;g.generation=1;qg_defaults(&g.cfg);
    if(!qg_user_config_path(g.config_path,MAX_PATH)){MessageBoxW(NULL,L"无法创建安全的配置目录。应用未启动。",L"QuietGuard",MB_OK|MB_ICONWARNING);return 1;}
    if(GetFileAttributesW(g.config_path)!=INVALID_FILE_ATTRIBUTES&&!qg_read_config_file(g.config_path,&g.cfg)){qg_defaults(&g.cfg);StringCchCopyW(g.notice,256,L"配置文件无效，已安全暂停；请重新保存设置");}
    if(g.dry)g.cfg.enabled=0;
    g.service=!g.dry&&qg_service_installed();
    if(g.service){QgRequest q={0};QgReply r;q.magic=QG_WIRE_MAGIC;q.command=QG_CMD_STATUS;if(qg_service_exchange(&q,&r))g.cfg=r.config;else g.cfg.enabled=0;}
    qg_engine_init(&g.engine,&g.cfg);INITCOMMONCONTROLSEX cc={sizeof(cc),ICC_STANDARD_CLASSES};InitCommonControlsEx(&cc);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);g.dpi=(int)GetDpiForSystem();theme();fonts();
    WNDCLASSEXW wc={0};wc.cbSize=sizeof(wc);wc.lpfnWndProc=window_proc;wc.hInstance=instance;wc.lpszClassName=L"QuietGuardWindow";wc.hCursor=LoadCursorW(NULL,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));wc.hIconSm=wc.hIcon;RegisterClassExW(&wc);
    WNDCLASSEXW content_class=wc;content_class.lpfnWndProc=content_proc;content_class.lpszClassName=L"QuietGuardContent";RegisterClassExW(&content_class);
    RECT work;SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    DWORD style=WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN;
    RECT wr={0,0,px(1060),px(688)};AdjustWindowRectExForDpi(&wr,style,FALSE,0,(UINT)g.dpi);
    int width=wr.right-wr.left,height=wr.bottom-wr.top,available_w=work.right-work.left,available_h=work.bottom-work.top;
    if(width>available_w-px(16))width=available_w-px(16);
    if(height>available_h-px(16))height=available_h-px(16);
    HWND h=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"QuietGuard · 轻量闲时关机",style,work.left+(available_w-width)/2,work.top+(available_h-height)/2,width,height,NULL,NULL,instance,NULL);if(!h)return 1;
    if(!worker_init(&g.worker)||!worker_init(&g.ipc)){error_box(L"无法启动监测线程",GetLastError());DestroyWindow(h);worker_close(&g.worker);worker_close(&g.ipc);return 1;}
    bool enabled=g.cfg.enabled;g.engine.cfg.enabled=0;g.engine.phase=QG_PAUSED;
    if(!g.background_launch||!g.cfg.enabled||g.service||g.dry)ShowWindow(h,show);
    if(g.service){EnableWindow(g.save,FALSE);EnableWindow(g.start,FALSE);service_refresh();}else {tray_update();if(enabled)start_local();}
    change_timer();update_view();MSG msg;
    while(GetMessageW(&msg,NULL,0,0)>0){
        if(msg.message==WM_KEYDOWN&&msg.wParam==VK_RETURN){
            HWND focus=GetFocus();bool action=focus==g.start||focus==g.save||focus==g.cancel||focus==g.help;
            for(int i=0;i<3;i++)if(focus==g.nav[i])action=true;
            if(action){if(!(msg.lParam&((LPARAM)1<<30))&&IsWindowEnabled(focus)&&IsWindowVisible(focus))SendMessageW(focus,BM_CLICK,0,0);}
            else if(focus==g.mode){TranslateMessage(&msg);DispatchMessageW(&msg);}
            /* Editing a number never implicitly starts, pauses, or applies settings. */
            continue;
        }
        if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE){PostMessageW(h,WM_CLOSE,0,0);continue;}
        if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        else if((msg.message==WM_KEYDOWN&&msg.wParam==VK_TAB)||msg.message==WM_SYSCHAR){
            HWND focus=GetFocus();RECT fr,cr;
            if(focus&&GetParent(focus)==g.content&&GetWindowRect(focus,&fr)){
                MapWindowPoints(NULL,g.content,(POINT*)&fr,2);GetClientRect(g.content,&cr);
                if(fr.top<px(8))scroll_to(g.scroll+MulDiv(fr.top,96,g.dpi)-8);
                else if(fr.bottom>cr.bottom-px(8))scroll_to(g.scroll+MulDiv(fr.bottom-cr.bottom,96,g.dpi)+8);
            }
        }
    }
    worker_close(&g.worker);worker_close(&g.ipc);if(g.monitor)CloseHandle(g.monitor);if(g.install)CloseHandle(g.install);HFONT fs[]={g.body,g.small,g.title,g.hero,g.semibold};for(unsigned i=0;i<5;i++)DeleteObject(fs[i]);DeleteObject(g.background);DeleteObject(g.surface);CloseHandle(ui_instance);return 0;
}
