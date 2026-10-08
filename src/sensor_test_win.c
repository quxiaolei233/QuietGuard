/* Isolated diagnostic entry. No production initialization is reachable here. */
#ifdef QG_SENSOR_MOCK
#ifdef _WIN32
#error QG_SENSOR_MOCK is host-test-only
#endif
#include "../tests/sensor_mock/win32_stubs.h"
#else
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include "qg.h"
#include <psapi.h>
#include <tlhelp32.h>
#endif
#include "safe_cli.h"
#include "metric_diagnostics.h"
#include "build_identity.h"
#include <float.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SENSOR_MUTEX L"Local\\QuietGuard.SensorTest.v1"
#define CLEANUP_GRACE_MS 2000u
#define OUTPUT_TIMEOUT_MS 5000u
#define REPORT_CAP (1024u * 1024u)

typedef struct {
    bool valid_cpu, valid_memory, valid_handles, valid_threads, valid_io, valid_timing;
    uint64_t capture_started_tick_ms, capture_ended_tick_ms;
    uint64_t cpu_ticks, private_bytes, working_set_bytes, peak_commit_bytes, peak_working_set_bytes;
    uint64_t handles, threads, read_operations, write_operations, other_operations;
    uint64_t read_bytes, write_bytes, other_bytes;
} Resources;
typedef struct {
    QgSample sample;
    QgDiagnostics diagnostics;
    Resources process;
    uint64_t elapsed_ms, collector_ms;
    QgPhase phase;
    bool warmup;
} Record;
typedef struct {
    HANDLE go, stop, done;
    uint32_t mask;
    QgEngine engine;
    Record record;
    volatile LONG create_ok, destroyed, stage;
} Collector;
typedef struct {
    HANDLE output;
    const char *buffer;
    DWORD length;
    DWORD error;
} Writer;
typedef struct { char *data; size_t length; bool failed; } Json;
static PVOID volatile console_cancel;
static volatile LONG console_users;

static uint64_t ticks(FILETIME t)
{
    return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

static void resources(Resources *r)
{
    HANDLE process = GetCurrentProcess(), snap;
    FILETIME created, exited, kernel, user;
    PROCESS_MEMORY_COUNTERS_EX memory;
    IO_COUNTERS io;
    THREADENTRY32 thread;
    DWORD handles = 0;
    memset(r, 0, sizeof(*r));
    r->capture_started_tick_ms = GetTickCount64();
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        r->valid_cpu = true; r->cpu_ticks = ticks(kernel) + ticks(user);
    }
    memset(&memory, 0, sizeof(memory)); memory.cb = (DWORD)sizeof(memory);
    if (GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&memory, (DWORD)sizeof(memory))) {
        r->valid_memory = true;
        r->private_bytes = (uint64_t)memory.PrivateUsage;
        r->working_set_bytes = (uint64_t)memory.WorkingSetSize;
        r->peak_commit_bytes = (uint64_t)memory.PeakPagefileUsage;
        r->peak_working_set_bytes = (uint64_t)memory.PeakWorkingSetSize;
    }
    if (GetProcessHandleCount(process, &handles)) {
        r->valid_handles = true; r->handles = handles;
    }
    if (GetProcessIoCounters(process, &io)) {
        r->valid_io = true;
        r->read_operations = io.ReadOperationCount; r->write_operations = io.WriteOperationCount;
        r->other_operations = io.OtherOperationCount;
        r->read_bytes = io.ReadTransferCount; r->write_bytes = io.WriteTransferCount;
        r->other_bytes = io.OtherTransferCount;
    }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        r->capture_ended_tick_ms = GetTickCount64(); r->valid_timing = true; return;
    }
    memset(&thread, 0, sizeof(thread)); thread.dwSize = (DWORD)sizeof(thread);
    if (Thread32First(snap, &thread)) {
        do {
            if (thread.th32OwnerProcessID == GetCurrentProcessId()) ++r->threads;
            thread.dwSize = (DWORD)sizeof(thread);
        } while (Thread32Next(snap, &thread));
        r->valid_threads = GetLastError() == ERROR_NO_MORE_FILES && r->threads != 0;
    }
    CloseHandle(snap);
    r->capture_ended_tick_ms = GetTickCount64(); r->valid_timing = true;
}

/* Only this thread owns provider state, including provider destruction. */
static DWORD WINAPI collect_thread(LPVOID arg)
{
    Collector *c = (Collector *)arg;
    HANDLE waits[2] = { c->stop, c->go };
    QgMetrics *metrics;
    InterlockedExchange(&c->stage, 1);
    metrics = qg_metrics_create(c->mask);
    InterlockedExchange(&c->create_ok, metrics != NULL);
    SetEvent(c->done);
    if (!metrics) return 0;
    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        uint64_t before;
        if (wait != WAIT_OBJECT_0 + 1u) break;
        memset(&c->record, 0, sizeof(c->record));
        InterlockedExchange(&c->stage, 2);
        before = GetTickCount64();
        /* Keep normal session-enumeration overhead and fail-closed semantics. */
        qg_metrics_sample_diagnostic(metrics, true, &c->record.sample, &c->record.diagnostics);
        c->record.collector_ms = GetTickCount64() - before;
        qg_engine_sample(&c->engine, &c->record.sample);
        c->record.phase = c->engine.phase;
        resources(&c->record.process);
        InterlockedExchange(&c->stage, 0);
        SetEvent(c->done);
    }
    InterlockedExchange(&c->stage, 3);
    qg_metrics_destroy(metrics);
    InterlockedExchange(&c->destroyed, 1);
    InterlockedExchange(&c->stage, 4);
    return 0;
}

static BOOL WINAPI console_handler(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT)
        return FALSE;
    InterlockedIncrement(&console_users);
    {
        HANDLE cancel = (HANDLE)InterlockedCompareExchangePointer(&console_cancel, NULL, NULL);
        if (cancel) SetEvent(cancel);
    }
    InterlockedDecrement(&console_users);
    return TRUE;
}

static void add(Json *j, const char *fmt, ...)
{
    int n;
    va_list args;
    if (j->failed) return;
    va_start(args, fmt);
    n = vsnprintf(j->data + j->length, REPORT_CAP - j->length, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= REPORT_CAP - j->length) { j->failed = true; return; }
    j->length += (size_t)n;
}

static void quoted(Json *j, const char *s)
{
    add(j, "\"");
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') add(j, "\\%c", (int)c);
        else if (c < 32u || c > 126u) add(j, "\\u%04x", (unsigned)c);
        else add(j, "%c", (int)c);
    }
    add(j, "\"");
}

static void integer(Json *j, const char *key, bool valid, uint64_t n)
{
    add(j, ",\"%s\":", key);
    if (valid) add(j, "%" PRIu64, n); else add(j, "null");
}
static const char *boolean(bool b) { return b ? "true" : "false"; }

static bool finite_value(double n)
{
    return n == n && n >= -DBL_MAX && n <= DBL_MAX;
}

/* Keep provider instance names bounded and valid JSON on UTF-16 Windows and
   UTF-32 host tests. Never read beyond the fixed diagnostic field. */
static void quoted_instance(Json *j, const wchar_t *s)
{
    add(j, "\"");
    for (size_t i = 0; i < QG_DIAG_NAME_CAP && s[i]; ++i) {
        uint32_t c = (uint32_t)s[i];
        if (c >= 0xd800u && c <= 0xdbffu) {
            uint32_t low = i + 1u < QG_DIAG_NAME_CAP ? (uint32_t)s[i + 1u] : 0u;
            if (low >= 0xdc00u && low <= 0xdfffu) {
                add(j, "\\u%04x\\u%04x", (unsigned)c, (unsigned)low); ++i;
            } else add(j, "\\ufffd");
        } else if ((c >= 0xdc00u && c <= 0xdfffu) || c > 0x10ffffu) add(j, "\\ufffd");
        else if (c > 0xffffu) {
            c -= 0x10000u;
            add(j, "\\u%04x\\u%04x", (unsigned)(0xd800u + (c >> 10)),
                (unsigned)(0xdc00u + (c & 0x3ffu)));
        } else if (c == '"' || c == '\\') add(j, "\\%c", (int)c);
        else if (c < 32u || c > 126u) add(j, "\\u%04x", (unsigned)c);
        else add(j, "%c", (int)c);
    }
    add(j, "\"");
}

static const char *diagnostic_stage(QgDiagnosticStage stage)
{
    static const char *const names[] = {
        "disabled", "ok", "baseline", "cpu_groups", "cpu_query", "cpu_counters",
        "pdh_open", "pdh_localize", "pdh_info_size", "pdh_info", "pdh_path",
        "pdh_expand_size", "pdh_expand", "pdh_parse", "pdh_empty", "limit",
        "pdh_duplicate", "pdh_add", "pdh_collect", "pdh_format", "pdh_cstatus",
        "pdh_range", "pdh_aggregate", "net_query", "net_topology", "net_counters",
        "net_empty", "net_range", "allocation"
    };
    unsigned index = (unsigned)stage;
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown";
}

static void diagnostic_number(Json *j, const char *key, bool present, double n)
{
    add(j, ",\"%s\":", key);
    if (present && finite_value(n)) add(j, "%.17g", n); else add(j, "null");
}

static void write_raw_diagnostic(Json *j, const QgRawDiagnostic *r)
{
    add(j, "{\"present\":%s,\"api_status\":%" PRIu32 ",\"counter_status\":%" PRIu32
        ",\"counter_type\":%" PRIu32 ",\"first\":", boolean(r->present),
        r->api_status, r->counter_status, r->counter_type);
    if (r->present) add(j, "%" PRId64, r->first); else add(j, "null");
    add(j, ",\"second\":");
    if (r->present) add(j, "%" PRId64, r->second); else add(j, "null");
    integer(j, "timestamp", r->present, r->timestamp);
    integer(j, "multi_count", r->present, r->multi_count);
    add(j, "}");
}

static void write_diagnostics(Json *j, const QgDiagnostics *d)
{
    static const char *const names[QG_METRICS] = { "cpu", "gpu", "disk", "network" };
    add(j, "{\"provider_valid_mask\":%" PRIu32 ",\"clock\":{\"start_ok\":%s,\"end_ok\":%s,"
        "\"status\":%" PRIu32 ",\"reason_code\":%" PRIu32 ",\"continuous_before\":%s,\"continuous_after\":%s,"
        "\"elapsed_tick_ms\":%" PRIu64 ",\"elapsed_awake_ms\":%" PRIu64
        ",\"collection_tick_ms\":%" PRIu64 ",\"collection_awake_ms\":%" PRIu64 "},"
        "\"sessions\":{\"checked\":%s,\"ok\":%s,\"status\":%" PRIu32 ",\"reason_code\":%" PRIu32 "},\"metrics\":[",
        d->provider_valid_mask, boolean(d->clock_start_ok), boolean(d->clock_end_ok), d->clock_status, d->clock_reason,
        boolean(d->continuous_before), boolean(d->continuous_after), d->elapsed_tick_ms,
        d->elapsed_awake_ms, d->collection_tick_ms, d->collection_awake_ms,
        boolean(d->sessions_checked), boolean(d->sessions_ok), d->session_status, d->session_reason);
    for (unsigned i = 0; i < QG_METRICS; ++i) {
        const QgMetricDiagnostic *m = &d->metric[i];
        add(j, "%s{\"metric\":\"%s\",\"stage\":\"%s\",\"stage_code\":%u,"
            "\"api_status\":%" PRIu32 ",\"counter_status\":%" PRIu32
            ",\"item_count\":%" PRIu32 ",\"item_index\":%" PRIu32
            ",\"has_formatted\":%s,\"has_uncapped\":%s",
            i ? "," : "", names[i], diagnostic_stage(m->stage), (unsigned)m->stage,
            m->api_status, m->counter_status, m->item_count, m->item_index,
            boolean(m->has_formatted), boolean(m->has_uncapped));
        diagnostic_number(j, "formatted_value", m->has_formatted, m->formatted_value);
        diagnostic_number(j, "uncapped_value", m->has_uncapped, m->uncapped_value);
        add(j, ",\"uncapped_api_status\":%" PRIu32 ",\"uncapped_counter_status\":%" PRIu32
            ",\"topology_changed\":%s,\"name_truncated\":%s,\"instance\":",
            m->uncapped_api_status, m->uncapped_counter_status, boolean(m->topology_changed),
            boolean(m->name_truncated || m->instance[QG_DIAG_NAME_CAP - 1u] != 0));
        quoted_instance(j, m->instance);
        add(j, ",\"raw_previous\":"); write_raw_diagnostic(j, &m->raw_previous);
        add(j, ",\"raw_current\":"); write_raw_diagnostic(j, &m->raw_current);
        add(j, "}");
    }
    add(j, "]}");
}

static void write_resources(Json *j, const Resources *r)
{
    add(j, "{\"valid_cpu\":%s,\"valid_memory\":%s,\"valid_handles\":%s,\"valid_threads\":%s,\"valid_io\":%s,\"cpu_ms\":",
        boolean(r->valid_cpu), boolean(r->valid_memory), boolean(r->valid_handles),
        boolean(r->valid_threads), boolean(r->valid_io));
    if (r->valid_cpu) add(j, "%.4f", (double)r->cpu_ticks / 10000.0); else add(j, "null");
    add(j, ",\"valid_timing\":%s", boolean(r->valid_timing));
    integer(j, "capture_started_tick_ms", r->valid_timing, r->capture_started_tick_ms);
    integer(j, "capture_ended_tick_ms", r->valid_timing, r->capture_ended_tick_ms);
    integer(j, "private_bytes", r->valid_memory, r->private_bytes);
    integer(j, "working_set_bytes", r->valid_memory, r->working_set_bytes);
    integer(j, "peak_commit_bytes", r->valid_memory, r->peak_commit_bytes);
    integer(j, "peak_working_set_bytes", r->valid_memory, r->peak_working_set_bytes);
    integer(j, "handles", r->valid_handles, r->handles);
    integer(j, "threads", r->valid_threads, r->threads);
    integer(j, "read_operations", r->valid_io, r->read_operations);
    integer(j, "write_operations", r->valid_io, r->write_operations);
    integer(j, "other_operations", r->valid_io, r->other_operations);
    integer(j, "read_bytes", r->valid_io, r->read_bytes);
    integer(j, "write_bytes", r->valid_io, r->write_bytes);
    integer(j, "other_bytes", r->valid_io, r->other_bytes);
    add(j, "}");
}

static void write_record(Json *j, const Record *r, unsigned index)
{
    add(j, "%s{\"index\":%u,\"elapsed_ms\":%" PRIu64 ",\"warmup\":%s,\"at_ms\":%" PRIu64
        ",\"valid_mask\":%u,\"values\":[", index ? "," : "", index, r->elapsed_ms, boolean(r->warmup), r->sample.at_ms, r->sample.valid);
    for (unsigned metric = 0; metric < 4u; ++metric) {
        double value = r->sample.value[metric];
        add(j, "%s", metric ? "," : "");
        if ((r->sample.valid & (1u << metric)) && finite_value(value))
            add(j, "%.9g", value);
        else add(j, "null");
    }
    add(j, "],\"veto\":%s,\"discontinuity\":%s,\"collector_ms\":%" PRIu64 ",\"phase\":%u,\"process\":",
        boolean(r->sample.veto), boolean(r->sample.discontinuity), r->collector_ms, (unsigned)r->phase);
    write_resources(j, &r->process);
    add(j, ",\"diagnostics\":"); write_diagnostics(j, &r->diagnostics);
    add(j, "}");
}

/* A final synchronous output operation is isolated too: stdout may be a full
   pipe or a stalled device. The controller never blocks on WriteFile itself. */
static DWORD WINAPI write_thread(LPVOID arg)
{
    Writer *w = (Writer *)arg;
    DWORD at = 0;
    do {
        DWORD done = 0;
        if (!WriteFile(w->output, w->buffer + at, w->length - at, &done, NULL)) {
            w->error = GetLastError(); if (!w->error) w->error = ERROR_WRITE_FAULT;
            return 0;
        }
        if (done > w->length - at || (!done && at < w->length)) {
            w->error = ERROR_WRITE_FAULT; return 0;
        }
        at += done;
    } while (at < w->length);
    return 0;
}

/* Self-only fail-stop. Never terminate a worker and then free its live state. */
static void self_stop(UINT code)
{
    TerminateProcess(GetCurrentProcess(), code);
    ExitProcess(code); /* fallback if the self-termination request failed */
}

static bool bounded_write(HANDLE output, const char *buffer, DWORD length)
{
    Writer w = { output, buffer, length, ERROR_SUCCESS };
    HANDLE thread = CreateThread(NULL, 0, write_thread, &w, 0, NULL);
    DWORD result;
    if (!thread) return false;
    result = WaitForSingleObject(thread, OUTPUT_TIMEOUT_MS);
    if (result != WAIT_OBJECT_0) {
        CancelSynchronousIo(thread);
        if (WaitForSingleObject(thread, CLEANUP_GRACE_MS) != WAIT_OBJECT_0) self_stop(11u);
        CloseHandle(thread);
        return false;
    }
    CloseHandle(thread);
    return w.error == ERROR_SUCCESS;
}

static bool local_file(HANDLE file)
{
    wchar_t final[1024], root[4];
    DWORD n, drive;
    if (GetFileType(file) != FILE_TYPE_DISK) return false;
    n = GetFinalPathNameByHandleW(file, final, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n < 7u || n >= 1024u || wcsncmp(final, L"\\\\?\\", 4) != 0 ||
        !((final[4] >= L'A' && final[4] <= L'Z') || (final[4] >= L'a' && final[4] <= L'z')) ||
        final[5] != L':' || final[6] != L'\\') return false;
    root[0] = final[4]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    drive = GetDriveTypeW(root);
    return drive == DRIVE_FIXED || drive == DRIVE_REMOVABLE || drive == DRIVE_RAMDISK;
}

/* Lock every ancestor against write/delete while validating and creating the
   output. Reject reparse points rather than following junctions or symlinks;
   reject remote drive roots before any file creation. */
static HANDLE output_file(const wchar_t *path)
{
    HANDLE parents[QG_SAFE_OUTPUT_CAP / 2u + 1u], output = NULL;
    wchar_t part[QG_SAFE_OUTPUT_CAP], root[4];
    size_t count = 0, length = wcslen(path);
    DWORD drive;
    root[0] = path[0]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    drive = GetDriveTypeW(root);
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK) return NULL;
    memcpy(part, path, (length + 1u) * sizeof(*part));
    for (size_t i = 0; i < length; ++i) if (part[i] == L'/') part[i] = L'\\';
    for (size_t i = 2u; i < length; ++i) {
        BY_HANDLE_FILE_INFORMATION info;
        wchar_t saved;
        HANDLE directory;
        if (part[i] != L'\\') continue;
        saved = part[i == 2u ? 3u : i];
        part[i == 2u ? 3u : i] = 0;
        directory = CreateFileW(part, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        part[i == 2u ? 3u : i] = saved;
        if (directory == INVALID_HANDLE_VALUE) goto done;
        parents[count++] = directory;
        if (!GetFileInformationByHandle(directory, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !local_file(directory)) goto done;
    }
    output = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) output = NULL;
    if (output && !local_file(output)) { CloseHandle(output); output = NULL; }
done:
    while (count) CloseHandle(parents[--count]);
    return output;
}

typedef struct { const QgSafeOptions *options; HANDLE output; } OutputOpen;
static DWORD WINAPI open_thread(LPVOID arg)
{
    OutputOpen *task = (OutputOpen *)arg;
    HANDLE output;
    if (task->options->output[0]) {
        output = output_file(task->options->output);
        if (!output) return 0;
    } else {
        HANDLE source = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD type;
        if (!source || source == INVALID_HANDLE_VALUE) return 0;
        type = GetFileType(source);
        if (type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE && type != FILE_TYPE_CHAR) return 0;
        if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &output,
                             GENERIC_WRITE, FALSE, 0)) return 0;
    }
    {
        Writer probe = { output, "", 0, ERROR_SUCCESS };
        write_thread(&probe);
        if (probe.error) { CloseHandle(output); return 0; }
    }
    task->output = output;
    return 0;
}

static HANDLE open_output(const QgSafeOptions *o)
{
    OutputOpen task = { o, NULL };
    HANDLE thread = CreateThread(NULL, 0, open_thread, &task, 0, NULL);
    if (!thread) return NULL;
    if (WaitForSingleObject(thread, OUTPUT_TIMEOUT_MS) != WAIT_OBJECT_0) {
        CancelSynchronousIo(thread);
        if (WaitForSingleObject(thread, CLEANUP_GRACE_MS) != WAIT_OBJECT_0) self_stop(4u);
        if (task.output) CloseHandle(task.output);
        CloseHandle(thread); return NULL;
    }
    CloseHandle(thread);
    return task.output;
}

/* Resource enumeration is diagnostic work too and may encounter a stalled
   kernel/provider. Its private buffer is published only after its thread exits. */
static DWORD WINAPI probe_thread(LPVOID arg)
{
    resources((Resources *)arg);
    return 0;
}
static bool bounded_resources(Resources *result, DWORD timeout, HANDLE cancel, bool *cancelled, bool *forced)
{
    Resources *private_result = (Resources *)calloc(1, sizeof(*private_result));
    HANDLE thread;
    memset(result, 0, sizeof(*result));
    if (!private_result) return false;
    thread = CreateThread(NULL, 0, probe_thread, private_result, 0, NULL);
    if (!thread) { free(private_result); return false; }
    {
        HANDLE waits[2] = { cancel, thread };
        DWORD wait = cancel ? WaitForMultipleObjects(2, waits, FALSE, timeout) : WaitForSingleObject(thread, timeout);
        if ((cancel && wait == WAIT_OBJECT_0 + 1u) || (!cancel && wait == WAIT_OBJECT_0)) {
            *result = *private_result;
            CloseHandle(thread); free(private_result); return true;
        }
        if (cancel && wait == WAIT_OBJECT_0) *cancelled = true;
        CancelSynchronousIo(thread);
        if (WaitForSingleObject(thread, CLEANUP_GRACE_MS) != WAIT_OBJECT_0) {
            *forced = true; return false; /* Self teardown retains live state. */
        }
        CloseHandle(thread); free(private_result); return false;
    }
}

/* All waits are bounded by the earlier of provider and run deadlines, and are
   interruptible by the separate named test cancellation event. */
static DWORD await(HANDLE done, HANDLE cancel, HANDLE thread, uint64_t deadline)
{
    HANDLE waits[3] = { cancel, done, thread };
    uint64_t now = GetTickCount64(), remaining;
    if (now >= deadline) return WAIT_TIMEOUT;
    remaining = deadline - now;
    return WaitForMultipleObjects(3, waits, FALSE, (DWORD)remaining);
}

int qg_run_sensor_test(const QgSafeOptions *o)
{
    HANDLE mutex = NULL, output = NULL, cancel = NULL, thread = NULL;
    Collector *c = NULL;
    Record *records = NULL;
    Json json = { NULL, 0, false };
    Resources initial, final;
    FILETIME utc_start, utc_end;
    wchar_t cancel_name[96];
    uint64_t started = 0, ended, duration, interval, timeout, next, provider_deadline;
    unsigned count = 0, valid = 0, steady_valid = 0, valid_union = 0;
    int code = 5;
    const char *reason = "controller_error", *initiating_reason = "controller_error";
    bool forced = false, probe_cancelled = false, console_handler_installed = false, sample_pending = false;
    DWORD wait;
    if (!qg_safe_options_valid(o)) return 2;
    mutex = CreateMutexW(NULL, FALSE, SENSOR_MUTEX);
    if (!mutex) return 5;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 3; }
    output = open_output(o);
    if (!output) { CloseHandle(mutex); return 4; }
    c = (Collector *)calloc(1, sizeof(*c));
    records = (Record *)calloc(QG_SAFE_MAX_SAMPLES, sizeof(*records));
    json.data = (char *)malloc(REPORT_CAP);
    if (!c || !records || !json.data) goto finish_no_report;
    swprintf(cancel_name, sizeof(cancel_name) / sizeof(cancel_name[0]),
             L"Local\\QuietGuard.SensorTest.%lu.Cancel", (unsigned long)GetCurrentProcessId());
    cancel = CreateEventW(NULL, TRUE, FALSE, cancel_name);
    if (!cancel || GetLastError() == ERROR_ALREADY_EXISTS) goto finish_no_report;
    c->go = CreateEventW(NULL, FALSE, FALSE, NULL);
    c->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    c->done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!c->go || !c->stop || !c->done) goto finish_no_report;
    {
        QgConfig config;
        qg_defaults(&config);
        config.mask = o->mask; config.sample_seconds = o->interval_seconds;
        config.enabled = 1; config.veto_active_session = 1;
        qg_engine_init(&c->engine, &config);
    }
    c->mask = o->mask;
    duration = (uint64_t)o->duration_seconds * 1000u;
    interval = (uint64_t)o->interval_seconds * 1000u;
    timeout = (uint64_t)o->timeout_seconds * 1000u;
    InterlockedExchangePointer(&console_cancel, cancel);
    console_handler_installed = SetConsoleCtrlHandler(console_handler, TRUE) != FALSE;
    memset(&initial, 0, sizeof(initial)); memset(&final, 0, sizeof(final));
    GetSystemTimeAsFileTime(&utc_start);
    started = GetTickCount64();
    if (!bounded_resources(&initial, (DWORD)timeout, cancel, &probe_cancelled, &forced)) {
        reason = probe_cancelled ? "cancelled" : "telemetry_error"; code = probe_cancelled ? 8 : 5; goto report;
    }
    thread = CreateThread(NULL, 0, collect_thread, c, 0, NULL);
    if (!thread) goto report;
    provider_deadline = GetTickCount64() + timeout;
    if (provider_deadline > started + duration) provider_deadline = started + duration;
    wait = await(c->done, cancel, thread, provider_deadline);
    if (wait == WAIT_OBJECT_0) { reason = "cancelled"; code = 8; goto cleanup_provider; }
    if (wait == WAIT_TIMEOUT) { reason = "provider_timeout"; code = 7; goto cleanup_provider; }
    if (wait != WAIT_OBJECT_0 + 1u) {
        if (wait == WAIT_OBJECT_0 + 2u && !InterlockedCompareExchange(&c->create_ok, 0, 0)) {
            reason = "provider_create_failed"; code = 6;
        }
        goto cleanup_provider;
    }
    if (!InterlockedCompareExchange(&c->create_ok, 0, 0)) {
        reason = "provider_create_failed"; code = 6; goto cleanup_provider;
    }
    next = started;
    code = 0; reason = "completed";
    while (GetTickCount64() < started + duration) {
        uint64_t now = GetTickCount64(), stop_at;
        if (now < next) {
            stop_at = next < started + duration ? next : started + duration;
            wait = WaitForSingleObject(cancel, (DWORD)(stop_at - now));
            if (wait == WAIT_OBJECT_0) { code = 8; reason = "cancelled"; break; }
            if (wait != WAIT_TIMEOUT) { code = 5; reason = "controller_error"; break; }
            continue;
        }
        if (count >= QG_SAFE_MAX_SAMPLES) { code = 5; reason = "sample_limit"; break; }
        now = GetTickCount64();
        if (now >= started + duration) break;
        provider_deadline = now + timeout;
        stop_at = provider_deadline < started + duration ? provider_deadline : started + duration;
        if (!SetEvent(c->go)) { code = 5; reason = "controller_error"; break; }
        sample_pending = true;
        wait = await(c->done, cancel, thread, stop_at);
        if (wait == WAIT_OBJECT_0) { code = 8; reason = "cancelled"; break; }
        if (wait == WAIT_TIMEOUT) {
            if (provider_deadline <= started + duration) { code = 7; reason = "provider_timeout"; }
            break;
        }
        if (wait != WAIT_OBJECT_0 + 1u) { code = 5; reason = "controller_error"; break; }
        sample_pending = false;
        records[count] = c->record;
        records[count].elapsed_ms = records[count].sample.at_ms >= started ?
            records[count].sample.at_ms - started : 0;
        records[count].warmup = records[count].elapsed_ms < 2u * interval;
        valid_union |= records[count].sample.valid;
        if ((records[count].sample.valid & o->mask) == o->mask) {
            ++valid; if (!records[count].warmup) ++steady_valid;
        }
        ++count;
        /* Skip missed slots, never queue/overlap calls or make catch-up bursts. */
        now = GetTickCount64();
        next = started + ((now - started) / interval + 1u) * interval;
    }
cleanup_provider:
    SetEvent(c->stop);
    if (WaitForSingleObject(thread, CLEANUP_GRACE_MS) != WAIT_OBJECT_0) {
        forced = true; code = 10;
    }
    if (!code && !steady_valid) { code = 9; reason = "no_valid_samples"; }
report:
    if (!forced && !bounded_resources(&final, (DWORD)timeout, NULL, &probe_cancelled, &forced)) {
        reason = "telemetry_error"; code = 5;
    }
    ended = GetTickCount64();
    GetSystemTimeAsFileTime(&utc_end);
    initiating_reason = reason;
    if (forced) { reason = "forced_cleanup"; code = 10; }
    add(&json, "{\"schema_version\":1,\"kind\":\"quietguard.sensor-test\",\"pid\":%lu,\"build\":{\"version\":",
        (unsigned long)GetCurrentProcessId());
    quoted(&json, QG_VERSION); add(&json, ",\"commit\":"); quoted(&json, QG_BUILD_COMMIT);
    add(&json, ",\"source\":"); quoted(&json, QG_BUILD_SOURCE);
    add(&json, "},\"options\":{\"mask\":%u,\"duration_seconds\":%u,\"interval_seconds\":%u,\"timeout_seconds\":%u},"
        "\"cancellation_event\":\"Local\\\\QuietGuard.SensorTest.%lu.Cancel\","
        "\"outcome\":{\"reason\":\"%s\",\"initiating_reason\":\"%s\",\"exit_code\":%d,\"forced_cleanup\":%s,\"provider_destroyed\":%s,"
        "\"provider_stage\":%ld,\"sample_in_flight_at_stop\":%s,\"console_cancel_handler\":%s},",
        o->mask, o->duration_seconds, o->interval_seconds, o->timeout_seconds,
        (unsigned long)GetCurrentProcessId(), reason, initiating_reason, code, boolean(forced),
        boolean(InterlockedCompareExchange(&c->destroyed, 0, 0) != 0),
        (long)InterlockedCompareExchange(&c->stage, 0, 0), boolean(sample_pending), boolean(console_handler_installed));
    add(&json, "\"timing\":{\"started_tick_ms\":%" PRIu64 ",\"ended_tick_ms\":%" PRIu64 ",\"elapsed_ms\":%" PRIu64
        ",\"started_utc_filetime\":%" PRIu64 ",\"ended_utc_filetime\":%" PRIu64 ",\"warmup_intervals\":2,\"warmup_ms\":%" PRIu64 "},"
        "\"summary\":{\"sample_count\":%u,\"valid_samples\":%u,\"steady_valid_samples\":%u,\"valid_mask_union\":%u},\"process_start\":",
        started, ended, ended - started, ticks(utc_start), ticks(utc_end), 2u * interval, count, valid, steady_valid, valid_union);
    write_resources(&json, &initial); add(&json, ",\"process_end\":"); write_resources(&json, &final);
    add(&json, ",\"scope\":\"diagnostic harness including controller, worker, session veto and self-instrumentation; not GUI/service performance; process I/O is not physical disk I/O\",\"samples\":[");
    for (unsigned i = 0; i < count; ++i) write_record(&json, &records[i], i);
    add(&json, "]}\n");
    if (json.failed || !bounded_write(output, json.data, (DWORD)json.length)) code = 11;
    /* Forced provider cleanup means kernel process teardown, not destroy(). */
    if (forced) self_stop((UINT)code);
finish_no_report:
    InterlockedExchangePointer(&console_cancel, NULL);
    if (console_handler_installed) SetConsoleCtrlHandler(console_handler, FALSE);
    {
        uint64_t until = GetTickCount64() + CLEANUP_GRACE_MS;
        while (InterlockedCompareExchange(&console_users, 0, 0)) {
            if (GetTickCount64() >= until) self_stop((UINT)code);
            Sleep(1);
        }
    }
    if (thread) CloseHandle(thread);
    if (c) {
        if (c->go) CloseHandle(c->go);
        if (c->stop) CloseHandle(c->stop);
        if (c->done) CloseHandle(c->done);
    }
    if (cancel) CloseHandle(cancel);
    if (output) CloseHandle(output);
    if (mutex) CloseHandle(mutex);
    free(c); free(records); free(json.data);
    return code;
}
