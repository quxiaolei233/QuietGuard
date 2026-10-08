#ifndef QG_H
#define QG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define QG_VERSION "0.3.1"
#define QG_VERSION_W L"0.3.1"
#define QG_METRICS 4
#define QG_CPU 1u
#define QG_GPU 2u
#define QG_DISK 4u
#define QG_NET 8u
#define QG_ALL 15u
#define QG_CONFIG_MAX 2048
/* CPU/GPU/disk percent, network KiB/s, strictly BELOW each selected threshold. */
typedef struct { uint32_t mask, idle_seconds, sample_seconds, warning_seconds; double threshold[4]; uint32_t enabled, veto_active_session; } QgConfig;
typedef struct { uint64_t at_ms; uint32_t valid; double value[4]; bool veto, discontinuity; } QgSample;
typedef enum { QG_PAUSED, QG_UNKNOWN, QG_BUSY, QG_IDLE, QG_WARNING, QG_READY, QG_REQUESTED, QG_FAULT } QgPhase;
typedef struct { QgConfig cfg; QgPhase phase; uint64_t epoch, last_ms, idle_since, warning_since; bool have_last, have_idle; } QgEngine;
void qg_defaults(QgConfig *c);
bool qg_config_valid(const QgConfig *c);
bool qg_config_parse(const char *text, size_t length, QgConfig *out);
size_t qg_config_format(const QgConfig *c, char *out, size_t cap);
void qg_engine_init(QgEngine *e, const QgConfig *c);
void qg_engine_reset(QgEngine *e, QgPhase why);
void qg_engine_configure(QgEngine *e, const QgConfig *c);
void qg_engine_sample(QgEngine *e, const QgSample *s);
bool qg_engine_can_commit(const QgEngine *e, uint64_t epoch, uint64_t now);
void qg_engine_cancel(QgEngine *e);
int qg_self_test(void);
#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
typedef struct QgMetrics QgMetrics;
QgMetrics *qg_metrics_create(uint32_t mask);
void qg_metrics_destroy(QgMetrics *m);
void qg_metrics_sample(QgMetrics *m, bool veto_active, QgSample *out);
/* Local only; nonforcing request. dry_run never invokes a power API. */
DWORD qg_shutdown_request(bool dry_run);
bool qg_user_config_path(wchar_t *out, size_t cap);
bool qg_read_config_file(const wchar_t *path, QgConfig *c);
bool qg_write_config_file(const wchar_t *path, const QgConfig *c);
#define QG_SERVICE_NAME L"QuietGuard"
#define QG_WIRE_MAGIC 0x51473131u
#define QG_CMD_STATUS 1u
#define QG_CMD_CANCEL 2u
#define QG_CMD_CONFIG 3u
/* Fixed protocol: configuration requires an elevated admin token. */
typedef struct { uint32_t magic, command; QgConfig config; } QgRequest;
typedef struct { uint32_t magic, error; QgPhase phase; QgConfig config; QgSample sample; uint64_t idle_ms, warning_ms, snapshot_ms; } QgReply;
int qg_service_main(void);
DWORD qg_service_install(const QgConfig *config);
DWORD qg_service_remove(void);
bool qg_service_installed(void);
bool qg_service_exchange(const QgRequest *req, QgReply *reply);
HANDLE qg_monitor_mutex(void); /* NULL if a different monitor already exists; CloseHandle releases. */
bool qg_is_admin(void);
#endif
#endif
