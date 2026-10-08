#ifndef QG_SENSOR_WIN32_STUBS_H
#define QG_SENSOR_WIN32_STUBS_H
#include "../../src/qg.h"
#include <wchar.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef int32_t LONG;
typedef int BOOL;
typedef void *LPVOID;
typedef void *PVOID;
typedef struct { DWORD dwFileAttributes; } BY_HANDLE_FILE_INFORMATION;
typedef struct MockHandle *HANDLE;
typedef struct QgMetrics QgMetrics;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct {
 DWORD cb; size_t PeakWorkingSetSize, WorkingSetSize, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX;
typedef PROCESS_MEMORY_COUNTERS_EX PROCESS_MEMORY_COUNTERS;
typedef struct {
 uint64_t ReadOperationCount, WriteOperationCount, OtherOperationCount;
 uint64_t ReadTransferCount, WriteTransferCount, OtherTransferCount;
} IO_COUNTERS;
typedef struct { DWORD dwSize, th32OwnerProcessID; } THREADENTRY32;
#define WINAPI
#define FALSE 0
#define TRUE 1
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE UINT32_MAX
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED UINT32_MAX
#define ERROR_SUCCESS 0u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_WRITE_FAULT 29u
#define FILE_TYPE_DISK 1u
#define FILE_TYPE_CHAR 2u
#define FILE_TYPE_PIPE 3u
#define FILE_NAME_NORMALIZED 0u
#define VOLUME_NAME_DOS 0u
#define FILE_READ_ATTRIBUTES 128u
#define FILE_SHARE_READ 1u
#define OPEN_EXISTING 3u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u
#define FILE_ATTRIBUTE_DIRECTORY 16u
#define FILE_ATTRIBUTE_REPARSE_POINT 1024u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1u
#define FILE_ATTRIBUTE_NORMAL 128u
#define DRIVE_FIXED 3u
#define DRIVE_REMOVABLE 2u
#define DRIVE_RAMDISK 6u
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define TH32CS_SNAPTHREAD 4u
#define CTRL_C_EVENT 0u
#define CTRL_BREAK_EVENT 1u
#define CTRL_CLOSE_EVENT 2u
HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
BOOL GetProcessTimes(HANDLE,FILETIME *,FILETIME *,FILETIME *,FILETIME *);
BOOL GetProcessMemoryInfo(HANDLE,PROCESS_MEMORY_COUNTERS *,DWORD);
BOOL GetProcessHandleCount(HANDLE,DWORD *);
BOOL GetProcessIoCounters(HANDLE,IO_COUNTERS *);
HANDLE CreateToolhelp32Snapshot(DWORD,DWORD);
BOOL Thread32First(HANDLE,THREADENTRY32 *);
BOOL Thread32Next(HANDLE,THREADENTRY32 *);
DWORD GetLastError(void);
BOOL CloseHandle(HANDLE);
LONG InterlockedIncrement(volatile LONG *);
LONG InterlockedDecrement(volatile LONG *);
PVOID InterlockedExchangePointer(PVOID volatile *,PVOID);
PVOID InterlockedCompareExchangePointer(PVOID volatile *,PVOID,PVOID);
void Sleep(DWORD);
BOOL GetFileInformationByHandle(HANDLE,BY_HANDLE_FILE_INFORMATION *);
LONG InterlockedExchange(volatile LONG *,LONG);
LONG InterlockedCompareExchange(volatile LONG *,LONG,LONG);
BOOL SetEvent(HANDLE);
DWORD WaitForMultipleObjects(DWORD,const HANDLE *,BOOL,DWORD);
DWORD WaitForSingleObject(HANDLE,DWORD);
uint64_t GetTickCount64(void);
BOOL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
BOOL TerminateProcess(HANDLE,UINT);
void ExitProcess(UINT);
HANDLE CreateThread(void *,size_t,DWORD (*)(LPVOID),LPVOID,DWORD,DWORD *);
BOOL CancelSynchronousIo(HANDLE);
DWORD GetFileType(HANDLE);
DWORD GetFinalPathNameByHandleW(HANDLE,wchar_t *,DWORD,DWORD);
DWORD GetDriveTypeW(const wchar_t *);
HANDLE CreateFileW(const wchar_t *,DWORD,DWORD,void *,DWORD,DWORD,HANDLE);
HANDLE GetStdHandle(DWORD);
BOOL DuplicateHandle(HANDLE,HANDLE,HANDLE,HANDLE *,DWORD,BOOL,DWORD);
HANDLE CreateMutexW(void *,BOOL,const wchar_t *);
HANDLE CreateEventW(void *,BOOL,BOOL,const wchar_t *);
BOOL SetConsoleCtrlHandler(BOOL (*)(DWORD),BOOL);
void GetSystemTimeAsFileTime(FILETIME *);
QgMetrics *qg_metrics_create(uint32_t);
void qg_metrics_destroy(QgMetrics *);
void qg_metrics_sample(QgMetrics *,bool,QgSample *);
#endif
