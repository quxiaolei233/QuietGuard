#include "qg.h"
#include <shlobj.h>
#include <strsafe.h>
#include <stdio.h>

bool qg_user_config_path(wchar_t *out,size_t cap) {
    wchar_t base[MAX_PATH];
    if(FAILED(SHGetFolderPathW(NULL,CSIDL_LOCAL_APPDATA,NULL,SHGFP_TYPE_CURRENT,base))) return false;
    if(FAILED(StringCchPrintfW(out,cap,L"%s\\QuietGuard",base))) return false;
    if(!CreateDirectoryW(out,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS) return false;
    DWORD attr=GetFileAttributesW(out);
    if(attr==INVALID_FILE_ATTRIBUTES||(attr&FILE_ATTRIBUTE_REPARSE_POINT)||!(attr&FILE_ATTRIBUTE_DIRECTORY))return false;
    return SUCCEEDED(StringCchPrintfW(out,cap,L"%s\\QuietGuard\\settings.ini",base));
}
bool qg_read_config_file(const wchar_t *path,QgConfig *c) {
    HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(h==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info; LARGE_INTEGER size;
    bool ok=GetFileInformationByHandle(h,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&GetFileSizeEx(h,&size)&&size.QuadPart>0&&size.QuadPart<QG_CONFIG_MAX;
    char buf[QG_CONFIG_MAX]; DWORD got=0;
    if(ok)ok=ReadFile(h,buf,(DWORD)size.QuadPart,&got,NULL)&&got==(DWORD)size.QuadPart;
    CloseHandle(h);
    return ok&&qg_config_parse(buf,got,c);
}
bool qg_write_config_file(const wchar_t *path,const QgConfig *c) {
    char buf[QG_CONFIG_MAX];size_t n=qg_config_format(c,buf,sizeof(buf));if(!n)return false;
    DWORD a=GetFileAttributesW(path);if(a!=INVALID_FILE_ATTRIBUTES&&(a&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))return false;
    wchar_t temp[MAX_PATH];if(FAILED(StringCchPrintfW(temp,MAX_PATH,L"%s.%lu.%llu.new",path,GetCurrentProcessId(),(unsigned long long)GetTickCount64())))return false;
    HANDLE h=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,NULL);
    if(h==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;bool ok=WriteFile(h,buf,(DWORD)n,&written,NULL)&&written==n&&FlushFileBuffers(h);
    if(!CloseHandle(h))ok=false;
    if(ok)ok=MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok)DeleteFileW(temp);
    return ok;
}
DWORD qg_shutdown_request(bool dry_run) {
    if(dry_run)return ERROR_SUCCESS;
    HANDLE token=NULL;TOKEN_PRIVILEGES next,previous;DWORD previous_size=sizeof(previous),error=ERROR_SUCCESS;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return GetLastError();
    ZeroMemory(&next,sizeof(next));next.PrivilegeCount=1;
    if(!LookupPrivilegeValueW(NULL,SE_SHUTDOWN_NAME,&next.Privileges[0].Luid)){error=GetLastError();CloseHandle(token);return error;}
    next.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;SetLastError(ERROR_SUCCESS);
    if(!AdjustTokenPrivileges(token,FALSE,&next,sizeof(previous),&previous,&previous_size))error=GetLastError();
    else error=GetLastError();
    if(error==ERROR_SUCCESS) {
        if(!InitiateSystemShutdownExW(NULL,L"QuietGuard: all selected resource readings remained below your limits.",0,FALSE,FALSE,SHTDN_REASON_MAJOR_APPLICATION|SHTDN_REASON_MINOR_OTHER|SHTDN_REASON_FLAG_PLANNED))error=GetLastError();
        AdjustTokenPrivileges(token,FALSE,&previous,0,NULL,NULL);
    }
    CloseHandle(token);return error;
}
