#ifdef QG_METRICS_MOCK
#ifdef _WIN32
#error QG_METRICS_MOCK is for host-only tests, never a Windows build
#endif
#include "../tests/metrics_mock/win32_stubs.h"
#else
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "qg.h"
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wtsapi32.h>
#endif
#include "metric_diagnostics.h"
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Application-owned caches have hard limits; no background threads or timers.
   The caller serializes calls and chooses its own sampling interval. */
#define QG_MAX_ITEMS 4096u
#define QG_MAX_BYTES (1024u * 1024u)
#define QG_CLOCK_TOLERANCE_MS 100u

typedef struct {
    wchar_t *path;
    PDH_HCOUNTER counter;
    uint32_t gpu[4];             /* adapter LUID high/low, physical index, engine */
    size_t group;
} QgCounter;

typedef struct {
    PDH_HQUERY query;
    wchar_t localized[PDH_MAX_COUNTER_PATH];
    wchar_t *paths, *next_paths;
    size_t paths_cap, next_paths_cap;
    QgCounter *items, *next_items;
    size_t items_cap, next_items_cap, count;
    double *groups;
    size_t groups_cap, group_count;
    bool baseline, raw_ready;
    QgRawDiagnostic *raw; /* allocated only for explicitly requested diagnostics */
    size_t raw_cap;
} QgPdh;

typedef struct {
    uint64_t luid, rx, tx;
    ULONG type;
    IF_OPER_STATUS oper;
    NET_IF_ADMIN_STATUS admin;
    NET_IF_MEDIA_CONNECT_STATE media;
    bool active;
} QgNet;

struct QgMetrics {
    uint32_t mask;
    bool clock_ready, cpu_ready, net_ready;
    uint64_t tick, unbiased, idle, kernel, user, net_tick;
    DWORD processors;
    QgPdh gpu, disk;
    QgNet *net, *next_net;
    size_t net_cap, next_net_cap, net_count;
};

static bool reserve(void **buffer, size_t *capacity, size_t count, size_t width)
{
    size_t n;
    void *p;
    if (!width || count > QG_MAX_BYTES / width) return false;
    if (*capacity >= count) return true;
    n = *capacity ? *capacity : 16u;
    while (n < count) {
        if (n > (QG_MAX_BYTES / width) / 2u) { n = count; break; }
        n *= 2u;
    }
    p = realloc(*buffer, n * width);
    if (!p) return false;
    *buffer = p;
    *capacity = n;
    return true;
}

static uint64_t file_ticks(FILETIME f)
{
    return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static bool finite_number(double n)
{
    /* Avoid CRT-specific isfinite macro narrowing under strict MinGW warnings. */
    return n == n && n >= -DBL_MAX && n <= DBL_MAX;
}

static bool diagnostic_fail(QgMetricDiagnostic *d, QgDiagnosticStage stage, uint32_t status)
{
    if (d) { d->stage = stage; d->api_status = status; }
    return false;
}

static bool cpu_sample(QgMetrics *m, double *value, QgMetricDiagnostic *d)
{
    FILETIME fi, fk, fu;
    uint64_t idle, kernel, user, di, dk, du, total;
    DWORD processors;
    bool ready = m->cpu_ready;
    m->cpu_ready = false;
    /* GetSystemTimes covers only the calling group on multi-group machines. */
    if (GetActiveProcessorGroupCount() != 1u) return diagnostic_fail(d, QG_D_CPU_GROUPS, 0);
    processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!processors) return diagnostic_fail(d, QG_D_CPU_GROUPS, 0);
    if (!GetSystemTimes(&fi, &fk, &fu)) return diagnostic_fail(d, QG_D_CPU_QUERY, GetLastError());
    idle = file_ticks(fi); kernel = file_ticks(fk); user = file_ticks(fu);
    if (ready && m->processors == processors && idle >= m->idle &&
        kernel >= m->kernel && user >= m->user) {
        di = idle - m->idle; dk = kernel - m->kernel; du = user - m->user;
        if (dk <= UINT64_MAX - du && di <= dk) {
            total = dk + du; /* Kernel already includes idle. */
            if (total) {
                *value = 100.0 * (double)(total - di) / (double)total;
                m->idle = idle; m->kernel = kernel; m->user = user;
                m->processors = processors; m->cpu_ready = true;
                if (d) { d->has_formatted = true; d->formatted_value = *value; }
                return finite_number(*value) && *value >= 0.0 && *value <= 100.0;
            }
        }
    }
    m->idle = idle; m->kernel = kernel; m->user = user;
    m->processors = processors; m->cpu_ready = true;
    return diagnostic_fail(d, ready ? QG_D_CPU_COUNTERS : QG_D_BASELINE, 0);
}

static void pdh_close(QgPdh *p)
{
    if (p->query) PdhCloseQuery(p->query);
    p->query = NULL;
    p->baseline = false; p->raw_ready = false;
    p->count = 0;
}

static bool pdh_localize(QgPdh *p, bool gpu, QgMetricDiagnostic *d)
{
    PDH_HQUERY query = NULL;
    PDH_HCOUNTER counter = NULL;
    PDH_COUNTER_INFO_W *info = NULL;
    DWORD bytes = 0;
    size_t length;
    bool ok = false;
    PDH_STATUS status;
    const wchar_t *english = gpu ? L"\\GPU Engine(*)\\Utilization Percentage"
                                 : L"\\PhysicalDisk(*)\\% Idle Time";
    if (p->localized[0]) return true;
    status = PdhOpenQueryW(NULL, 0, &query);
    if (status != ERROR_SUCCESS) return diagnostic_fail(d, QG_D_PDH_OPEN, (uint32_t)status);
    status = PdhAddEnglishCounterW(query, english, 0, &counter);
    if (status != ERROR_SUCCESS) { diagnostic_fail(d, QG_D_PDH_LOCALIZE, (uint32_t)status); goto done; }
    status = PdhGetCounterInfoW(counter, FALSE, &bytes, NULL);
    if (status != (PDH_STATUS)PDH_MORE_DATA || bytes < sizeof(*info) || bytes > QG_MAX_BYTES) {
        diagnostic_fail(d, QG_D_PDH_INFO_SIZE, (uint32_t)status); goto done;
    }
    info = (PDH_COUNTER_INFO_W *)malloc(bytes);
    if (!info) { diagnostic_fail(d, QG_D_ALLOCATION, 0); goto done; }
    status = PdhGetCounterInfoW(counter, FALSE, &bytes, info);
    if (status != ERROR_SUCCESS || !info->szFullPath) {
        diagnostic_fail(d, QG_D_PDH_INFO, (uint32_t)status); goto done;
    }
    length = wcslen(info->szFullPath);
    if (!length || length >= PDH_MAX_COUNTER_PATH) {
        diagnostic_fail(d, QG_D_PDH_PATH, 0); goto done;
    }
    memcpy(p->localized, info->szFullPath, (length + 1u) * sizeof(wchar_t));
    ok = true;
done:
    free(info);
    PdhCloseQuery(query);
    return ok;
}

static bool take_literal(const wchar_t **at, const wchar_t *literal)
{
    size_t n = wcslen(literal);
    if (wcsncmp(*at, literal, n) != 0) return false;
    *at += n;
    return true;
}

static bool take_number(const wchar_t **at, unsigned base, uint32_t *value)
{
    const wchar_t *start = *at;
    uint32_t n = 0;
    for (;;) {
        unsigned digit;
        wchar_t c = **at;
        if (c >= L'0' && c <= L'9') digit = (unsigned)(c - L'0');
        else if (base == 16u && c >= L'a' && c <= L'f') digit = (unsigned)(c - L'a') + 10u;
        else if (base == 16u && c >= L'A' && c <= L'F') digit = (unsigned)(c - L'A') + 10u;
        else break;
        if (digit >= base || n > (UINT32_MAX - digit) / base) return false;
        n = n * base + digit;
        ++*at;
    }
    if (*at == start) return false;
    *value = n;
    return true;
}

static bool gpu_key(const wchar_t *name, uint32_t key[4])
{
    uint32_t pid;
    return take_literal(&name, L"pid_") && take_number(&name, 10u, &pid) &&
        take_literal(&name, L"_luid_0x") && take_number(&name, 16u, &key[0]) &&
        take_literal(&name, L"_0x") && take_number(&name, 16u, &key[1]) &&
        take_literal(&name, L"_phys_") && take_number(&name, 10u, &key[2]) &&
        take_literal(&name, L"_eng_") && take_number(&name, 10u, &key[3]) &&
        take_literal(&name, L"_engtype_") && *name != L'\0';
}

static int counter_compare(const void *a, const void *b)
{
    return wcscmp(((const QgCounter *)a)->path, ((const QgCounter *)b)->path);
}

static void diagnostic_instance(QgMetricDiagnostic *d, const wchar_t *path, size_t index);

/* Use the documented English -> localized wildcard -> expanded concrete
   counters sequence, not an English wildcard passed to an array formatter. */
static bool pdh_refresh(QgPdh *p, bool gpu, bool *changed, QgMetricDiagnostic *d)
{
    DWORD chars = 0, supplied;
    PDH_STATUS status;
    size_t used = 0, count = 0, i, j;
    wchar_t *path;
    union { PDH_COUNTER_PATH_ELEMENTS_W align; unsigned char bytes[8192]; } parsed;
    if (!pdh_localize(p, gpu, d)) return false;
    status = PdhExpandWildCardPathW(NULL, p->localized, NULL, &chars, PDH_REFRESHCOUNTERS);
    if (status != (PDH_STATUS)PDH_MORE_DATA || chars < 2u || chars > QG_MAX_BYTES / sizeof(wchar_t))
        return diagnostic_fail(d, QG_D_PDH_EXPAND_SIZE, (uint32_t)status);
    if (!reserve((void **)&p->next_paths, &p->next_paths_cap, chars, sizeof(wchar_t)))
        return diagnostic_fail(d, QG_D_ALLOCATION, 0);
    supplied = chars;
    status = PdhExpandWildCardPathW(NULL, p->localized, p->next_paths, &supplied, PDH_REFRESHCOUNTERS);
    if (status != ERROR_SUCCESS || supplied > chars || supplied < 2u)
        return diagnostic_fail(d, QG_D_PDH_EXPAND, (uint32_t)status);
    /* A race between the size and data calls is deliberately an unknown sample. */
    path = p->next_paths;
    while (used < supplied && *path) {
        size_t n = 0;
        DWORD bytes = (DWORD)sizeof(parsed.bytes);
        PDH_COUNTER_PATH_ELEMENTS_W *elements = &parsed.align;
        while (used + n < supplied && path[n]) ++n;
        if (used + n >= supplied || !n || n >= PDH_MAX_COUNTER_PATH)
            return diagnostic_fail(d, QG_D_PDH_PATH, 0);
        status = PdhParseCounterPathW(path, elements, &bytes, 0);
        if (status != ERROR_SUCCESS || !elements->szInstanceName || elements->szParentInstance)
            return diagnostic_fail(d, QG_D_PDH_PARSE, (uint32_t)status);
        if (gpu || _wcsicmp(elements->szInstanceName, L"_Total") != 0) {
            if (count == QG_MAX_ITEMS ||
                !reserve((void **)&p->next_items, &p->next_items_cap,
                         count + 1u, sizeof(QgCounter))) return diagnostic_fail(d, QG_D_LIMIT, 0);
            memset(&p->next_items[count], 0, sizeof(QgCounter));
            p->next_items[count].path = path;
            if (gpu && !gpu_key(elements->szInstanceName, p->next_items[count].gpu))
                return diagnostic_fail(d, QG_D_PDH_PARSE, 0);
            ++count;
        }
        used += n + 1u;
        path += n + 1u;
    }
    if (d) d->item_count = (uint32_t)count;
    if (used >= supplied || !count) return diagnostic_fail(d, QG_D_PDH_EMPTY, 0);
    qsort(p->next_items, count, sizeof(QgCounter), counter_compare);
    for (i = 1; i < count; ++i)
        if (wcscmp(p->next_items[i - 1u].path, p->next_items[i].path) == 0)
            return diagnostic_fail(d, QG_D_PDH_DUPLICATE, 0);
    *changed = !p->query || count != p->count;
    if (!*changed) {
        for (i = 0; i < count; ++i)
            if (wcscmp(p->items[i].path, p->next_items[i].path) != 0) {
                *changed = true;
                break;
            }
    }
    if (d) d->topology_changed = *changed;
    if (!*changed) return true;
    pdh_close(p);
    {
        wchar_t *swap_paths = p->paths;
        QgCounter *swap_items = p->items;
        size_t swap_paths_cap = p->paths_cap, swap_items_cap = p->items_cap;
        p->paths = p->next_paths; p->paths_cap = p->next_paths_cap;
        p->items = p->next_items; p->items_cap = p->next_items_cap;
        p->next_paths = swap_paths; p->next_paths_cap = swap_paths_cap;
        p->next_items = swap_items; p->next_items_cap = swap_items_cap;
    }
    status = PdhOpenQueryW(NULL, 0, &p->query);
    if (status != ERROR_SUCCESS) return diagnostic_fail(d, QG_D_PDH_OPEN, (uint32_t)status);
    p->count = count;
    p->group_count = 0;
    for (i = 0; i < count; ++i) {
        status = PdhAddCounterW(p->query, p->items[i].path, 0, &p->items[i].counter);
        if (status != ERROR_SUCCESS) {
            diagnostic_instance(d, p->items[i].path, i);
            diagnostic_fail(d, QG_D_PDH_ADD, (uint32_t)status); goto failed;
        }
        if (!gpu) continue;
        /* Built only on topology change; subsequent samples aggregate in O(n). */
        for (j = 0; j < i; ++j)
            if (memcmp(p->items[i].gpu, p->items[j].gpu, sizeof(p->items[i].gpu)) == 0)
                break;
        p->items[i].group = j < i ? p->items[j].group : p->group_count++;
    }
    if (gpu && !reserve((void **)&p->groups, &p->groups_cap, p->group_count, sizeof(double))) {
        diagnostic_fail(d, QG_D_ALLOCATION, 0); goto failed;
    }
    return true;
failed:
    pdh_close(p);
    return false;
}

static void diagnostic_instance(QgMetricDiagnostic *d, const wchar_t *path, size_t index)
{
    const wchar_t *a, *b;
    size_t n;
    if (!d) return;
    d->item_index = (uint32_t)index;
    a = wcschr(path, L'('); b = wcsrchr(path, L')');
    if (!a || !b || b <= a) return;
    ++a; n = (size_t)(b - a);
    d->name_truncated = n >= QG_DIAG_NAME_CAP;
    if (n >= QG_DIAG_NAME_CAP) n = QG_DIAG_NAME_CAP - 1u;
    wmemcpy(d->instance, a, n); d->instance[n] = L'\0';
}

static QgRawDiagnostic diagnostic_raw(PDH_HCOUNTER counter)
{
    PDH_RAW_COUNTER raw;
    DWORD type = 0;
    PDH_STATUS status;
    QgRawDiagnostic r;
    memset(&raw, 0, sizeof(raw)); memset(&r, 0, sizeof(r));
    raw.CStatus = UINT32_MAX;
    status = PdhGetRawCounterValue(counter, &type, &raw);
    r.api_status = (uint32_t)status; r.counter_status = raw.CStatus;
    if (status == ERROR_SUCCESS) {
        r.counter_status = raw.CStatus; r.counter_type = type;
        r.first = (int64_t)raw.FirstValue; r.second = (int64_t)raw.SecondValue;
        r.timestamp = file_ticks(raw.TimeStamp); r.multi_count = raw.MultiCount;
        r.present = raw.CStatus == PDH_CSTATUS_VALID_DATA || raw.CStatus == PDH_CSTATUS_NEW_DATA;
    }
    return r;
}

static bool pdh_sample(QgPdh *p, bool gpu, double *value, QgMetricDiagnostic *d)
{
    bool changed = false, ready;
    size_t i;
    double maximum = 0.0;
    PDH_STATUS status;
    if (!pdh_refresh(p, gpu, &changed, d)) { pdh_close(p); return false; }
    ready = p->baseline && !changed;
    p->baseline = false;
    status = PdhCollectQueryData(p->query);
    if (status != ERROR_SUCCESS) {
        pdh_close(p); return diagnostic_fail(d, QG_D_PDH_COLLECT, (uint32_t)status);
    }
    p->baseline = true;
    /* Raw disk evidence is auxiliary and only requested by the safe diagnostic
       entry. No allocation or extra raw API calls occur in ordinary sampling. */
    if (d && !gpu) {
        if (!reserve((void **)&p->raw, &p->raw_cap, p->count, sizeof(*p->raw))) {
            p->baseline = false;
            return diagnostic_fail(d, QG_D_ALLOCATION, 0);
        }
        if (!p->raw_ready) memset(p->raw, 0, p->count * sizeof(*p->raw));
        p->raw_ready = true;
    } else p->raw_ready = false;
    if (!ready && (!d || gpu)) return diagnostic_fail(d, QG_D_BASELINE, 0);
    if (gpu) memset(p->groups, 0, p->group_count * sizeof(double));
    for (i = 0; i < p->count; ++i) {
        PDH_FMT_COUNTERVALUE v;
        QgMetricDiagnostic item;
        double n;
        if (d) {
            memset(&item, 0, sizeof(item));
            item.item_count = d->item_count; item.topology_changed = d->topology_changed;
            diagnostic_instance(&item, p->items[i].path, i);
            if (!gpu) {
                item.raw_previous = p->raw[i];
                item.raw_current = diagnostic_raw(p->items[i].counter);
                p->raw[i] = item.raw_current;
            }
            if (!i) *d = item;
        }
        if (!ready) continue;
        memset(&v, 0, sizeof(v)); v.CStatus = UINT32_MAX;
        status = PdhGetFormattedCounterValue(p->items[i].counter,
                                            PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, NULL, &v);
        if (d) {
            item.api_status = (uint32_t)status;
            item.uncapped_api_status = (uint32_t)status;
            item.counter_status = v.CStatus; item.uncapped_counter_status = v.CStatus;
            if (status == ERROR_SUCCESS) {
                item.counter_status = v.CStatus; item.uncapped_counter_status = v.CStatus;
                item.formatted_value = v.doubleValue; item.uncapped_value = v.doubleValue;
                item.has_formatted = true; item.has_uncapped = true;
            }
        }
        if (status != ERROR_SUCCESS ||
            (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) ||
            !finite_number(v.doubleValue) || v.doubleValue < 0.0 || v.doubleValue > 100.0) {
            if (d) {
                *d = item;
                d->stage = status != ERROR_SUCCESS ? QG_D_PDH_FORMAT :
                    ((v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA)
                     ? QG_D_PDH_CSTATUS : QG_D_PDH_RANGE);
            }
            /* Unvisited disks have no raw snapshot for this collection. Do
               not mislabel their older cached data as the next interval's
               previous sample. Their next diagnostic will mark it absent. */
            if (d && !gpu && i + 1u < p->count)
                memset(p->raw + i + 1u, 0, (p->count - i - 1u) * sizeof(*p->raw));
            /* Collection succeeded. The next interval is still independently
               formatted/status/range checked by PDH; do not discard it merely
               because this interval failed. The engine resets idle on this
               unknown sample, so a later valid interval earns no back-credit. */
            return false;
        }
        if (gpu) {
            n = (p->groups[p->items[i].group] += v.doubleValue);
            if (!finite_number(n)) { return diagnostic_fail(d, QG_D_PDH_AGGREGATE, 0); }
        } else n = 100.0 - v.doubleValue;
        if (!i || n > maximum) { if (d) *d = item; }
        if (n > maximum) maximum = n;
    }
    if (!ready) return diagnostic_fail(d, QG_D_BASELINE, 0);
    /* Sum processes on the SAME physical engine, never unrelated engines.
       Overlap/rounding can make a sum exceed 100: treating it as busy is safe. */
    *value = maximum > 100.0 ? 100.0 : maximum;
    return true;
}

static int net_compare(const void *a, const void *b)
{
    uint64_t x = ((const QgNet *)a)->luid, y = ((const QgNet *)b)->luid;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static bool net_sample(QgMetrics *m, double *value, QgMetricDiagnostic *d)
{
    MIB_IF_TABLE2 *table = NULL;
    size_t i, count = 0, active = 0;
    uint64_t now;
    double bytes = 0.0;
    bool valid = m->net_ready;
    ULONG status;
    if (d) d->stage = valid ? QG_D_NET_TOPOLOGY : QG_D_BASELINE;
    m->net_ready = false;
    status = GetIfTable2(&table);
    if (status != NO_ERROR || !table) {
        if (table) FreeMibTable(table);
        return diagnostic_fail(d, QG_D_NET_QUERY, (uint32_t)status);
    }
    if (table->NumEntries > QG_MAX_ITEMS ||
        !reserve((void **)&m->next_net, &m->next_net_cap,
                 table->NumEntries, sizeof(QgNet))) { FreeMibTable(table); return diagnostic_fail(d, QG_D_LIMIT, 0); }
    for (i = 0; i < table->NumEntries; ++i) {
        MIB_IF_ROW2 *row = &table->Table[i];
        QgNet *n;
        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK ||
            row->InterfaceAndOperStatusFlags.FilterInterface) continue;
        n = &m->next_net[count++];
        n->luid = row->InterfaceLuid.Value;
        n->rx = row->InOctets; n->tx = row->OutOctets;
        n->type = row->Type; n->oper = row->OperStatus;
        n->admin = row->AdminStatus; n->media = row->MediaConnectState;
        n->active = row->OperStatus == IfOperStatusUp;
        if (n->active) ++active;
    }
    FreeMibTable(table); /* This happens on every return path after allocation. */
    now = GetTickCount64();
    if (d) d->item_count = (uint32_t)count;
    if (!count || !active) return diagnostic_fail(d, QG_D_NET_EMPTY, 0);
    qsort(m->next_net, count, sizeof(QgNet), net_compare);
    if (count != m->net_count || now <= m->net_tick) valid = false;
    for (i = 0; i < count; ++i) {
        QgNet *n = &m->next_net[i];
        if (!n->luid || (i && m->next_net[i - 1u].luid == n->luid))
            return diagnostic_fail(d, QG_D_NET_TOPOLOGY, 0);
        if (valid) {
            QgNet *old = &m->net[i];
            if (n->luid != old->luid || n->type != old->type || n->oper != old->oper ||
                n->admin != old->admin || n->media != old->media ||
                n->rx < old->rx || n->tx < old->tx) {
                if (d) d->stage = (n->rx < old->rx || n->tx < old->tx) ? QG_D_NET_COUNTERS : QG_D_NET_TOPOLOGY;
                valid = false;
            }
            else if (n->active) bytes += (double)(n->rx - old->rx) + (double)(n->tx - old->tx);
        }
    }
    if (valid) {
        *value = bytes * (1000.0 / 1024.0) / (double)(now - m->net_tick);
        valid = finite_number(*value) && *value >= 0.0;
        if (d) { d->has_formatted = true; d->formatted_value = *value;
            d->stage = valid ? QG_D_OK : QG_D_NET_RANGE; }
    }
    {
        QgNet *swap = m->net;
        size_t cap = m->net_cap;
        m->net = m->next_net; m->net_cap = m->next_net_cap;
        m->next_net = swap; m->next_net_cap = cap;
    }
    m->net_count = count; m->net_tick = now; m->net_ready = true;
    return valid;
}

/* Return false for an unknown enumeration/username/state, separately from a
   successfully determined veto. Service sessions with an empty username do
   not keep the machine awake merely because a WinStation exists. */
static bool session_veto(bool *veto, QgDiagnostics *d)
{
    WTS_SESSION_INFOW *sessions = NULL;
    DWORD count = 0, i;
    bool valid = true;
    *veto = false;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        if (d) { d->session_status = GetLastError(); d->session_reason = 1; }
        if (sessions) WTSFreeMemory(sessions);
        *veto = true;
        return false;
    }
    if (count > QG_MAX_ITEMS || (count && !sessions)) {
        if (d) d->session_reason = 2;
        valid = false;
    }
    for (i = 0; valid && i < count; ++i) {
        wchar_t *user = NULL;
        DWORD bytes = 0;
        size_t j, length;
        switch (sessions[i].State) {
        case WTSDisconnected: case WTSListen: case WTSIdle: break;
        case WTSActive: case WTSConnected: case WTSConnectQuery: case WTSShadow:
            if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId,
                                            WTSUserName, &user, &bytes)) {
                if (d) { d->session_status = GetLastError(); d->session_reason = 3; }
                valid = false;
            } else if (!user || bytes < sizeof(wchar_t) || bytes > QG_MAX_BYTES || bytes % sizeof(wchar_t)) {
                if (d) d->session_reason = 4;
                valid = false;
            } else {
                length = bytes / sizeof(wchar_t);
                for (j = 0; j < length && user[j]; ++j) {}
                if (j == length) { if (d) d->session_reason = 4; valid = false; }
                else if (j) *veto = true;
            }
            if (user) WTSFreeMemory(user);
            break;
        default: if (d) d->session_reason = 5; valid = false; break;
        }
    }
    if (sessions) WTSFreeMemory(sessions);
    if (!valid) *veto = true;
    return valid;
}

static void clear_baselines(QgMetrics *m)
{
    m->cpu_ready = false; m->net_ready = false;
    m->gpu.baseline = false; m->disk.baseline = false;
}

QgMetrics *qg_metrics_create(uint32_t mask)
{
    QgMetrics *m;
    if (!mask || (mask & ~QG_ALL)) return NULL;
    m = (QgMetrics *)calloc(1, sizeof(*m));
    if (m) m->mask = mask;
    return m;
}

void qg_metrics_destroy(QgMetrics *m)
{
    QgPdh *sets[2];
    size_t i;
    if (!m) return;
    sets[0] = &m->gpu; sets[1] = &m->disk;
    for (i = 0; i < 2u; ++i) {
        QgPdh *p = sets[i];
        pdh_close(p);
        free(p->paths); free(p->next_paths);
        free(p->items); free(p->next_items); free(p->groups); free(p->raw);
    }
    free(m->net); free(m->next_net); free(m);
}

void qg_metrics_sample_diagnostic(QgMetrics *m, bool veto_active, QgSample *out, QgDiagnostics *d)
{
    ULONGLONG unbiased = 0, end_unbiased = 0;
    uint64_t tick, after, dt = 0, du = 0;
    bool clock_ok, start_ok, continuous = false, sessions_ok = true;
    if (d) memset(d, 0, sizeof(*d));
    if (!out) return;
    memset(out, 0, sizeof(*out));
    tick = GetTickCount64();
    out->at_ms = tick;
    out->discontinuity = true;
    if (!m) { out->veto = veto_active; return; }
    clock_ok = QueryUnbiasedInterruptTime(&unbiased) != FALSE;
    start_ok = clock_ok;
    if (d) { d->clock_start_ok = clock_ok; if (!clock_ok) d->clock_status = GetLastError(); }
    after = GetTickCount64();
    if (m->clock_ready && tick > m->tick && unbiased >= m->unbiased) {
        dt = tick - m->tick; du = (unbiased - m->unbiased) / 10000u;
    }
    if (clock_ok && m->clock_ready && tick > m->tick && unbiased >= m->unbiased &&
        after >= tick && after - tick <= QG_CLOCK_TOLERANCE_MS) {
        continuous = dt >= du ? dt - du <= QG_CLOCK_TOLERANCE_MS
                              : du - dt <= QG_CLOCK_TOLERANCE_MS;
    }
    if (d) {
        d->elapsed_tick_ms = dt; d->elapsed_awake_ms = du;
        d->continuous_before = continuous;
        if (!clock_ok) d->clock_reason = 2;
        else if (!m->clock_ready) d->clock_reason = 1;
        else if (tick <= m->tick || unbiased < m->unbiased || after < tick) d->clock_reason = 3;
        else if (after - tick > QG_CLOCK_TOLERANCE_MS) d->clock_reason = 4;
        else if (!continuous) d->clock_reason = 5;
    }
    m->clock_ready = clock_ok;
    m->tick = tick; m->unbiased = unbiased;
    if (!continuous) clear_baselines(m);
    if ((m->mask & QG_CPU) && cpu_sample(m, &out->value[0], d ? &d->metric[0] : NULL)) out->valid |= QG_CPU;
    if ((m->mask & QG_GPU) && pdh_sample(&m->gpu, true, &out->value[1], d ? &d->metric[1] : NULL)) out->valid |= QG_GPU;
    if ((m->mask & QG_DISK) && pdh_sample(&m->disk, false, &out->value[2], d ? &d->metric[2] : NULL)) out->valid |= QG_DISK;
    if ((m->mask & QG_NET) && net_sample(m, &out->value[3], d ? &d->metric[3] : NULL)) out->valid |= QG_NET;
    if (d) {
        d->provider_valid_mask = out->valid;
        for (unsigned i = 0; i < QG_METRICS; ++i)
            if (out->valid & (1u << i)) d->metric[i].stage = QG_D_OK;
        d->sessions_checked = veto_active;
    }
    if (veto_active) sessions_ok = session_veto(&out->veto, d);
    if (d) d->sessions_ok = sessions_ok;
    /* Also detect a suspend inside a potentially blocking provider call. */
    clock_ok = QueryUnbiasedInterruptTime(&end_unbiased) != FALSE;
    if (d) { d->clock_end_ok = clock_ok; if (!clock_ok) d->clock_status = GetLastError(); }
    after = GetTickCount64();
    if (!clock_ok || after < tick || end_unbiased < unbiased) {
        if (d) d->clock_reason = clock_ok ? 7u : 6u;
        continuous = false;
        clear_baselines(m);
    } else if (start_ok) {
        dt = after - tick; du = (end_unbiased - unbiased) / 10000u;
        if (d) { d->collection_tick_ms = dt; d->collection_awake_ms = du; }
        if (dt >= du ? dt - du > QG_CLOCK_TOLERANCE_MS : du - dt > QG_CLOCK_TOLERANCE_MS) {
            if (d) d->clock_reason = 8;
            continuous = false;
            clear_baselines(m);
        }
    }
    if (d) d->continuous_after = continuous;
    if (!continuous || !sessions_ok || out->valid != m->mask) {
        out->valid = 0; /* Never let a caller accidentally use a partial snapshot. */
        return;
    }
    out->discontinuity = false;
}

void qg_metrics_sample(QgMetrics *m, bool veto_active, QgSample *out)
{
    qg_metrics_sample_diagnostic(m, veto_active, out, NULL);
}
