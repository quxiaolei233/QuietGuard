#ifndef QG_SAFE_CLI_H
#define QG_SAFE_CLI_H
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>
#define QG_SAFE_OUTPUT_CAP 260
#define QG_SAFE_MAX_SAMPLES 181u
#define QG_SAFE_DEFAULT_SECONDS 720u
#define QG_SAFE_DEFAULT_INTERVAL 60u
#define QG_SAFE_DEFAULT_TIMEOUT 15u
/* Test-only options. They are never copied to persistent production configuration. */
typedef struct {
    uint32_t mask, duration_seconds, interval_seconds, timeout_seconds;
    wchar_t output[QG_SAFE_OUTPUT_CAP];
} QgSafeOptions;
typedef enum { QG_SAFE_ABSENT=0, QG_SAFE_VALID=1, QG_SAFE_INVALID=2 } QgSafeParse;
QgSafeParse qg_safe_parse(int argc,const wchar_t *const *argv,QgSafeOptions *out);
bool qg_safe_options_valid(const QgSafeOptions *options);
int qg_safe_cli_self_test(void);
#ifdef _WIN32
/* Headless diagnostics only: implementation may not reference power/startup/config/GUI APIs. */
int qg_run_sensor_test(const QgSafeOptions *options);
#endif
#endif
