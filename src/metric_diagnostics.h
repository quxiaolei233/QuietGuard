#ifndef QG_METRIC_DIAGNOSTICS_H
#define QG_METRIC_DIAGNOSTICS_H
#include "qg.h"
#include <wchar.h>
/* Optional, bounded diagnostic output. Not part of QgSample or the service ABI.
   No diagnostic values authorize idle: QgSample remains the only engine input. */
#define QG_DIAG_NAME_CAP 96u
typedef enum {
    QG_D_DISABLED, QG_D_OK, QG_D_BASELINE, QG_D_CPU_GROUPS,
    QG_D_CPU_QUERY, QG_D_CPU_COUNTERS, QG_D_PDH_OPEN,
    QG_D_PDH_LOCALIZE, QG_D_PDH_INFO_SIZE, QG_D_PDH_INFO,
    QG_D_PDH_PATH, QG_D_PDH_EXPAND_SIZE, QG_D_PDH_EXPAND,
    QG_D_PDH_PARSE, QG_D_PDH_EMPTY, QG_D_LIMIT,
    QG_D_PDH_DUPLICATE, QG_D_PDH_ADD, QG_D_PDH_COLLECT,
    QG_D_PDH_FORMAT, QG_D_PDH_CSTATUS, QG_D_PDH_RANGE,
    QG_D_PDH_AGGREGATE, QG_D_NET_QUERY, QG_D_NET_TOPOLOGY,
    QG_D_NET_COUNTERS, QG_D_NET_EMPTY, QG_D_NET_RANGE,
    QG_D_ALLOCATION
} QgDiagnosticStage;
typedef struct {
    uint32_t api_status, counter_status, counter_type;
    int64_t first, second;
    uint64_t timestamp;
    uint32_t multi_count;
    bool present;
} QgRawDiagnostic;
typedef struct {
    QgDiagnosticStage stage;
    uint32_t api_status, counter_status, item_count, item_index;
    double formatted_value, uncapped_value;
    uint32_t uncapped_api_status, uncapped_counter_status;
    bool has_formatted, has_uncapped, topology_changed, name_truncated;
    QgRawDiagnostic raw_previous, raw_current;
    wchar_t instance[QG_DIAG_NAME_CAP];
} QgMetricDiagnostic;
typedef struct {
    QgMetricDiagnostic metric[QG_METRICS];
    uint32_t provider_valid_mask, clock_status, session_status;
    /* Clock: 0 ok,1 baseline,2 start API,3 start regression,4 paired delay,
       5 elapsed sleep gap,6 end API,7 end regression,8 collection sleep gap.
       Session: 0 ok,1 enumeration API,2 enumeration shape,3 username API,
       4 username shape,5 unknown state. Codes are evidence, not idle policy. */
    uint32_t clock_reason, session_reason;
    bool clock_start_ok, clock_end_ok, continuous_before, continuous_after;
    bool sessions_checked, sessions_ok;
    uint64_t elapsed_tick_ms, elapsed_awake_ms, collection_tick_ms, collection_awake_ms;
} QgDiagnostics;
typedef struct QgMetrics QgMetrics;
void qg_metrics_sample_diagnostic(QgMetrics *m, bool veto_active,
                                  QgSample *out, QgDiagnostics *diagnostics);
#endif
