/* Compile this translation unit alone: it includes the current production
   source, so assertions cannot drift onto a copied implementation. */
#define QG_METRICS_MOCK 1
#include "../../src/metrics.c"
#include "../../src/core.c" /* Exercise real provider-to-engine idle accounting. */

static void advance(void)
{
    tick += 1000;
    awake += 10000000;
    cpu_idle += 80;
    cpu_kernel += 100;
    cpu_user += 100;
}

static void unknown(const QgSample *s)
{
    assert(s->valid == 0);
    assert(s->discontinuity);
}

static void check_helpers(void)
{
    uint32_t key[4];
    void *buffer = NULL;
    size_t capacity = 0;
    QgSample sample;
    assert(gpu_key(L"pid_4_luid_0xAB_0xCD_phys_2_eng_3_engtype_Compute_1", key));
    assert(key[0] == 171 && key[1] == 205 && key[2] == 2 && key[3] == 3);
    assert(!gpu_key(L"pid_4_luid_0x100000000_0xCD_phys_2_eng_3_engtype_Compute", key));
    assert(!gpu_key(L"pid_4_luid_0xAB_0xCD_phys_2_eng_3_engtype_", key));
    assert(!finite_number(NAN) && !finite_number(INFINITY) && finite_number(DBL_MAX));
    assert(!reserve(&buffer, &capacity, QG_MAX_BYTES + 1u, 1u));
    assert(!buffer && !capacity);
    assert(!qg_metrics_create(0) && !qg_metrics_create(16));
    qg_metrics_sample(NULL, true, &sample);
    unknown(&sample);
    assert(sample.veto);
    qg_metrics_sample(NULL, true, NULL);
    qg_metrics_destroy(NULL);
}

static void check_cpu_and_clocks(void)
{
    QgSample s;
    QgMetrics *m = qg_metrics_create(QG_CPU);
    assert(m);
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_CPU && s.value[0] == 60.0 && !s.discontinuity);
    assert(!session_calls); /* Session enumeration is truly optional. */
    assert(!net_allocs && !pdh_opens); /* Unselected providers never run. */

    groups = 2;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    groups = 1;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_CPU);

    tick += 10000; /* Suspended time is absent from the unbiased clock. */
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_CPU);

    clock_error = true;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    clock_error = false;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_CPU);

    cpu_idle = 0; /* A regressed counter resets the baseline. */
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_CPU);

    processors = 8;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_CPU);

    cpu_error = true;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    cpu_error = false;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_CPU);
    qg_metrics_destroy(m);
}

static void check_gpu(void)
{
    QgSample s;
    size_t opens;
    QgMetrics *m;
    gpu_names[0] = L"\\GPU Engine(pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_3D)\\Utilization Percentage";
    gpu_names[1] = L"\\GPU Engine(pid_2_luid_0x0_0x1_phys_0_eng_0_engtype_3D)\\Utilization Percentage";
    gpu_names[2] = L"\\GPU Engine(pid_2_luid_0x0_0x1_phys_0_eng_1_engtype_Copy)\\Utilization Percentage";
    gpu_count = 3;
    gpu_values[0] = 30;
    gpu_values[1] = 40;
    gpu_values[2] = 60;
    m = qg_metrics_create(QG_GPU);
    assert(m);
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    /* 30 + 40 on engine 0 beats 60 on engine 1; neither 60 nor 130 is right. */
    assert(s.valid == QG_GPU && s.value[1] == 70);
    opens = pdh_opens;
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_GPU && pdh_opens == opens);

    gpu_count = 2;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_GPU && s.value[1] == 70);
    gpu_values[0] = 70;
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.value[1] == 100 && s.valid == QG_GPU);

    gpu_values[0] = NAN;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    gpu_values[0] = 30;
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_GPU);

    gpu_status[0] = 99;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    gpu_status[0] = PDH_CSTATUS_NEW_DATA;
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_GPU);

    suspend_in_collect = true;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_GPU);

    pdh_error = true;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    pdh_error = false;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_GPU);

    gpu_count = 0;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    qg_metrics_destroy(m);

    /* A good CPU result cannot compensate for a missing selected GPU. */
    m = qg_metrics_create(QG_CPU | QG_GPU);
    assert(m);
    qg_metrics_sample(m, false, &s);
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    qg_metrics_destroy(m);
}

static void disk_fixture(void)
{
    memset(disk_status, 0, sizeof(disk_status));
    memset(disk_format_error, 0, sizeof(disk_format_error));
    memset(disk_raw_error, 0, sizeof(disk_raw_error));
    memset(disk_raw, 0, sizeof(disk_raw));
    memset(disk_types, 0, sizeof(disk_types));
    disk_names[0] = L"\\PhysicalDisk(_Total)\\% Idle Time";
    disk_names[1] = L"\\PhysicalDisk(0 C:)\\% Idle Time";
    disk_names[2] = L"\\PhysicalDisk(1 D:)\\% Idle Time";
    disk_count = 3;
    disk_values[0] = NAN; /* Even invalid aggregate data must be ignored. */
    disk_status[0] = PDH_CSTATUS_INVALID_DATA;
    disk_format_error[0] = PDH_INVALID_DATA;
    disk_values[1] = 90;
    disk_values[2] = 25;
    for (size_t i = 1; i < disk_count; ++i) {
        disk_types[i] = PERF_PRECISION_100NS_TIMER;
        disk_raw[i].CStatus = PDH_CSTATUS_NEW_DATA;
        disk_raw[i].TimeStamp = ft(100000000u + i);
        disk_raw[i].FirstValue = (LONGLONG)(900000u + i);
        disk_raw[i].SecondValue = (LONGLONG)(1000000u + i);
        disk_raw[i].MultiCount = 1u;
    }
}

static void next_sample(QgMetrics *m, QgSample *s)
{
    advance();
    qg_metrics_sample(m, false, s);
}

static QgMetrics *disk_ready(void)
{
    QgSample s;
    QgMetrics *m = qg_metrics_create(QG_DISK);
    assert(m);
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    next_sample(m, &s);
    assert(s.valid == QG_DISK);
    return m;
}

static void disk_recovers(QgMetrics *m)
{
    QgSample s;
    next_sample(m, &s);
    unknown(&s); /* A failed interval cannot contribute idle credit. */
    next_sample(m, &s);
    assert(s.valid == QG_DISK && !s.discontinuity);
}

static void disk_accepts_next(QgMetrics *m)
{
    QgSample s;
    next_sample(m, &s);
    /* PDH independently validates the next collected/formatted interval. */
    assert(s.valid == QG_DISK && !s.discontinuity);
}

static void check_disk(void)
{
    QgSample s;
    QgMetrics *m;
    size_t opens;
    disk_fixture();
    m = disk_ready();
    next_sample(m, &s);
    assert(s.valid == QG_DISK && s.value[2] == 75);
    opens = pdh_opens;
    /* Reordering paths alone must not rebuild the query or lose continuity. */
    disk_names[1] = L"\\PhysicalDisk(1 D:)\\% Idle Time";
    disk_names[2] = L"\\PhysicalDisk(0 C:)\\% Idle Time";
    disk_values[1] = 25;
    disk_values[2] = 90;
    next_sample(m, &s);
    assert(s.valid == QG_DISK && s.value[2] == 75 && pdh_opens == opens);
    disk_count = 2;
    next_sample(m, &s);
    unknown(&s);
    next_sample(m, &s);
    assert(s.valid == QG_DISK && s.value[2] == 75);
    disk_count = 3;
    next_sample(m, &s);
    unknown(&s);
    next_sample(m, &s);
    assert(s.valid == QG_DISK && s.value[2] == 75);
    disk_names[2] = L"\\PhysicalDisk(2 E:)\\% Idle Time";
    next_sample(m, &s);
    unknown(&s); /* Same count, different physical instance. */
    next_sample(m, &s);
    assert(s.valid == QG_DISK);
    disk_count = 1;
    next_sample(m, &s);
    unknown(&s); /* _Total alone is not a physical-disk fallback. */
    disk_count = 0;
    next_sample(m, &s);
    unknown(&s); /* No physical disks is unknown, never fabricated zero. */
    disk_fixture();
    disk_recovers(m);
    disk_names[2] = disk_names[1];
    next_sample(m, &s);
    unknown(&s); /* Duplicate paths are not two valid physical disks. */
    disk_fixture();
    disk_recovers(m);
    qg_metrics_destroy(m);
}

static void check_disk_range_and_status(void)
{
    /* 100 + 2^-46 is exactly nextafter(100, +infinity) in binary64. */
    const double rejected[] = {100.0 + 0x1p-46, 100.01, 1000.0, -0.01,
                               NAN, INFINITY, -INFINITY};
    QgSample s;
    QgMetrics *m;
    size_t raw_before = pdh_raw_calls, capped_before = pdh_capped_calls[1];
    disk_fixture();
    m = disk_ready();
    disk_values[1] = 100;
    disk_values[2] = 100;
    next_sample(m, &s);
    assert(s.valid == QG_DISK && s.value[2] == 0);
    disk_values[1] = 0;
    next_sample(m, &s);
    assert(s.valid == QG_DISK && s.value[2] == 100);
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        disk_values[1] = rejected[i];
        next_sample(m, &s);
        unknown(&s); /* One bad disk invalidates otherwise good disks. */
        disk_values[1] = 100;
        disk_accepts_next(m);
    }
    /* API success and counter validity are separate contracts:
       https://learn.microsoft.com/en-us/windows/win32/perfctrs/checking-pdh-interface-return-values */
    disk_status[1] = PDH_CSTATUS_NO_INSTANCE;
    next_sample(m, &s);
    unknown(&s);
    disk_status[1] = PDH_CSTATUS_NEW_DATA;
    disk_accepts_next(m);
    disk_format_error[1] = PDH_INVALID_DATA;
    next_sample(m, &s);
    unknown(&s); /* Numeric output and CStatus look valid despite API error. */
    disk_format_error[1] = ERROR_SUCCESS;
    disk_accepts_next(m);
    assert(pdh_raw_calls == raw_before); /* Ordinary sampling has no diagnostics cost. */
    assert(pdh_capped_calls[1] == capped_before); /* Do not conceal idle overshoot. */
    qg_metrics_destroy(m);
}

static void check_disk_api_failures(void)
{
    PDH_STATUS *faults[] = {&pdh_open_error, &pdh_localize_error,
        &pdh_info_size_error, &pdh_info_error, &pdh_expand_size_error[1],
        &pdh_expand_error[1], &pdh_parse_error, &pdh_add_error,
        &pdh_collect_error[1]};
    QgSample s;
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        QgMetrics *m;
        disk_fixture();
        m = qg_metrics_create(QG_DISK);
        assert(m);
        *faults[i] = 123;
        qg_metrics_sample(m, false, &s);
        unknown(&s);
        *faults[i] = ERROR_SUCCESS;
        disk_recovers(m);
        qg_metrics_destroy(m);
    }
    disk_fixture();
    {
        QgMetrics *m = disk_ready();
        /* Second expansion call can race the size call; no partial list may
           be used. https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhexpandwildcardpathw */
        pdh_expand_error[1] = PDH_MORE_DATA;
        next_sample(m, &s);
        unknown(&s);
        pdh_expand_error[1] = ERROR_SUCCESS;
        disk_recovers(m);
        pdh_collect_error[1] = 123;
        next_sample(m, &s);
        unknown(&s);
        pdh_collect_error[1] = ERROR_SUCCESS;
        disk_recovers(m);
        qg_metrics_destroy(m);
    }
}

static void next_diagnostic(QgMetrics *m, QgSample *s, QgDiagnostics *d)
{
    advance();
    qg_metrics_sample_diagnostic(m, false, s, d);
}

static void check_raw_evidence(const QgRawDiagnostic *r, size_t index)
{
    assert(r->present && r->api_status == ERROR_SUCCESS);
    assert(r->counter_status == disk_raw[index].CStatus);
    assert(r->counter_type == disk_types[index]);
    assert(r->first == disk_raw[index].FirstValue);
    assert(r->second == disk_raw[index].SecondValue);
    assert(r->timestamp == file_ticks(disk_raw[index].TimeStamp));
    assert(r->multi_count == disk_raw[index].MultiCount);
}

static void check_disk_diagnostics(void)
{
    const double rejected[] = {100.0 + 0x1p-46, 1000.0, -1.0, NAN, INFINITY};
    QgSample s;
    QgDiagnostics d;
    QgMetrics *m;
    QgRawDiagnostic previous;
    size_t raw_before = pdh_raw_calls;
    disk_fixture();
    m = qg_metrics_create(QG_DISK);
    assert(m);
    memset(&d, 0xff, sizeof(d));
    qg_metrics_sample_diagnostic(m, false, &s, &d);
    unknown(&s);
    assert(d.provider_valid_mask == 0 && d.metric[2].stage == QG_D_BASELINE);
    assert(d.metric[2].topology_changed && d.metric[2].item_count == 2);
    assert(!d.metric[2].has_formatted && !d.metric[2].has_uncapped);
    assert(!d.metric[2].raw_previous.present);
    check_raw_evidence(&d.metric[2].raw_current, 1);
    assert(pdh_raw_calls == raw_before + 2); /* Both disks primed on warmup. */
    assert(d.metric[0].stage == QG_D_DISABLED && d.metric[1].stage == QG_D_DISABLED);
    assert(d.metric[3].stage == QG_D_DISABLED);
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_DISK && s.value[2] == 75);
    assert(d.provider_valid_mask == QG_DISK && d.metric[2].stage == QG_D_OK);
    assert(!d.metric[2].topology_changed && d.metric[2].item_index == 1);
    assert(!wcscmp(d.metric[2].instance, L"1 D:"));
    assert(d.metric[2].has_formatted && d.metric[2].formatted_value == 25);
    assert(d.metric[2].has_uncapped && d.metric[2].uncapped_value == 25);
    check_raw_evidence(&d.metric[2].raw_previous, 2);
    check_raw_evidence(&d.metric[2].raw_current, 2);
    assert(d.clock_start_ok && d.clock_end_ok && d.continuous_before && d.continuous_after);
    assert(d.elapsed_tick_ms == 1000 && d.elapsed_awake_ms == 1000);

    previous = d.metric[2].raw_current;
    disk_raw[2].FirstValue += 100;
    disk_raw[2].SecondValue += 1000;
    disk_raw[2].TimeStamp = ft(previous.timestamp + 10000000u);
    next_diagnostic(m, &s, &d);
    assert(d.metric[2].raw_previous.first == previous.first);
    assert(d.metric[2].raw_previous.second == previous.second);
    assert(d.metric[2].raw_previous.timestamp == previous.timestamp);
    check_raw_evidence(&d.metric[2].raw_current, 2);

    /* Raw diagnostics are auxiliary; their own failures cannot authorize or
       invalidate otherwise trustworthy formatted sampling. */
    disk_raw_error[2] = 123;
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_DISK && s.value[2] == 75 && d.metric[2].stage == QG_D_OK);
    assert(!d.metric[2].raw_current.present && d.metric[2].raw_current.api_status == 123);
    disk_raw_error[2] = ERROR_SUCCESS;
    disk_raw[2].CStatus = PDH_CSTATUS_INVALID_DATA;
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_DISK && !d.metric[2].raw_current.present);
    assert(d.metric[2].raw_current.api_status == ERROR_SUCCESS);
    assert(d.metric[2].raw_current.counter_status == PDH_CSTATUS_INVALID_DATA);
    disk_raw[2].CStatus = PDH_CSTATUS_VALID_DATA;
    next_diagnostic(m, &s, &d);
    assert(!d.metric[2].raw_previous.present);
    check_raw_evidence(&d.metric[2].raw_current, 2);

    disk_values[1] = 100;
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        disk_values[2] = rejected[i];
        next_diagnostic(m, &s, &d);
        unknown(&s);
        assert(d.provider_valid_mask == 0 && d.metric[2].stage == QG_D_PDH_RANGE);
        assert(d.metric[2].api_status == ERROR_SUCCESS);
        assert(d.metric[2].counter_status == PDH_CSTATUS_VALID_DATA);
        assert(d.metric[2].item_index == 1 && !wcscmp(d.metric[2].instance, L"1 D:"));
        assert(d.metric[2].has_formatted && d.metric[2].has_uncapped);
        assert(isnan(rejected[i]) ? isnan(d.metric[2].uncapped_value)
                                 : d.metric[2].uncapped_value == rejected[i]);
        disk_values[2] = 25;
        next_diagnostic(m, &s, &d);
        assert(s.valid == QG_DISK && d.metric[2].stage == QG_D_OK);
    }
    disk_status[2] = PDH_CSTATUS_NO_INSTANCE;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    assert(d.metric[2].stage == QG_D_PDH_CSTATUS);
    assert(d.metric[2].api_status == ERROR_SUCCESS);
    assert(d.metric[2].counter_status == PDH_CSTATUS_NO_INSTANCE);
    disk_status[2] = PDH_CSTATUS_NEW_DATA;
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_DISK);
    disk_format_error[2] = PDH_INVALID_DATA;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    assert(d.metric[2].stage == QG_D_PDH_FORMAT);
    assert(d.metric[2].api_status == (uint32_t)PDH_INVALID_DATA);
    assert(d.metric[2].item_index == 1 && !wcscmp(d.metric[2].instance, L"1 D:"));
    /* A later disk's API failure must not inherit earlier disk values. */
    assert(!d.metric[2].has_formatted && !d.metric[2].has_uncapped);
    disk_format_error[2] = ERROR_SUCCESS;
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_DISK);

    {
        int64_t recovered_interval_raw;
        /* If an early disk failure skips later raw reads, a later disk must
           not label a two-interval-old raw value as the previous sample. */
        disk_raw[2].FirstValue += 1000;
        disk_values[1] = 100.01;
        next_diagnostic(m, &s, &d);
        unknown(&s);
        assert(d.metric[2].item_index == 0);
        disk_values[1] = 100;
        disk_raw[2].FirstValue += 1000;
        next_diagnostic(m, &s, &d);
        assert(s.valid == QG_DISK && d.metric[2].item_index == 1);
        assert(!d.metric[2].raw_previous.present);
        recovered_interval_raw = d.metric[2].raw_current.first;
        disk_raw[2].FirstValue += 1000;
        next_diagnostic(m, &s, &d);
        assert(s.valid == QG_DISK && d.metric[2].raw_previous.present);
        assert(d.metric[2].raw_previous.first == recovered_interval_raw);
    }

    raw_before = pdh_raw_calls;
    next_sample(m, &s);
    assert(s.valid == QG_DISK && pdh_raw_calls == raw_before);
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_DISK && !d.metric[2].raw_previous.present);
    check_raw_evidence(&d.metric[2].raw_current, 2);
    disk_count = 2;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    assert(d.metric[2].topology_changed && d.metric[2].stage == QG_D_BASELINE);
    assert(!d.metric[2].raw_previous.present);
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_DISK && d.metric[2].raw_previous.present);
    /* Replacement at the same count must not attribute the old disk's raw
       pair to the new physical instance. */
    disk_names[1] = L"\\PhysicalDisk(7 G:)\\% Idle Time";
    disk_raw[1].FirstValue = 777;
    disk_raw[1].SecondValue = 888;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    assert(d.metric[2].topology_changed && !d.metric[2].raw_previous.present);
    assert(!wcscmp(d.metric[2].instance, L"7 G:"));
    check_raw_evidence(&d.metric[2].raw_current, 1);
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_DISK && !d.metric[2].topology_changed);
    check_raw_evidence(&d.metric[2].raw_previous, 1);
    qg_metrics_destroy(m);
}

static void check_disk_diagnostic_faults(void)
{
    const struct {
        PDH_STATUS *fault;
        QgDiagnosticStage stage;
    } cases[] = {
        {&pdh_open_error, QG_D_PDH_OPEN}, {&pdh_localize_error, QG_D_PDH_LOCALIZE},
        {&pdh_info_size_error, QG_D_PDH_INFO_SIZE}, {&pdh_info_error, QG_D_PDH_INFO},
        {&pdh_expand_size_error[1], QG_D_PDH_EXPAND_SIZE},
        {&pdh_expand_error[1], QG_D_PDH_EXPAND}, {&pdh_parse_error, QG_D_PDH_PARSE},
        {&pdh_add_error, QG_D_PDH_ADD}, {&pdh_collect_error[1], QG_D_PDH_COLLECT}
    };
    QgSample s;
    QgDiagnostics d;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        QgMetrics *m;
        size_t raw_before = pdh_raw_calls;
        disk_fixture();
        m = qg_metrics_create(QG_DISK);
        assert(m);
        *cases[i].fault = 123;
        qg_metrics_sample_diagnostic(m, false, &s, &d);
        unknown(&s);
        assert(d.metric[2].stage == cases[i].stage && d.metric[2].api_status == 123);
        assert(!d.metric[2].has_formatted && !d.metric[2].raw_current.present);
        assert(pdh_raw_calls == raw_before);
        *cases[i].fault = ERROR_SUCCESS;
        next_diagnostic(m, &s, &d);
        unknown(&s);
        assert(d.metric[2].stage == QG_D_BASELINE);
        next_diagnostic(m, &s, &d);
        assert(s.valid == QG_DISK && d.metric[2].stage == QG_D_OK);
        qg_metrics_destroy(m);
    }
    for (size_t count = 0; count < 2; ++count) {
        QgMetrics *m;
        disk_fixture();
        disk_count = count;
        m = qg_metrics_create(QG_DISK);
        assert(m);
        qg_metrics_sample_diagnostic(m, false, &s, &d);
        unknown(&s);
        assert(d.metric[2].stage == QG_D_PDH_EMPTY && d.metric[2].item_count == 0);
        qg_metrics_destroy(m);
    }
}

static void check_diagnostic_masks(void)
{
    QgSample s;
    QgDiagnostics d;
    QgMetrics *m;
    disk_fixture();
    gpu_count = 1;
    gpu_values[0] = 10;
    gpu_status[0] = PDH_CSTATUS_VALID_DATA;
    gpu_types[0] = PERF_100NSEC_TIMER;
    memset(&net_table, 0, sizeof(net_table));
    net_table.NumEntries = 1;
    net_table.Table[0].InterfaceLuid.Value = 11;
    net_table.Table[0].OperStatus = IfOperStatusUp;
    net_table.Table[0].Type = 6;
    m = qg_metrics_create(QG_ALL);
    assert(m);
    qg_metrics_sample_diagnostic(m, false, &s, &d);
    unknown(&s);
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_ALL && d.provider_valid_mask == QG_ALL);
    disk_values[2] = 100.0 + 0x1p-46;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    assert(d.provider_valid_mask == (QG_CPU | QG_GPU | QG_NET));
    assert(d.metric[0].stage == QG_D_OK && d.metric[1].stage == QG_D_OK);
    assert(d.metric[2].stage == QG_D_PDH_RANGE && d.metric[3].stage == QG_D_OK);
    disk_values[2] = 25;
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_ALL && d.provider_valid_mask == QG_ALL);

    cpu_error = true;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    assert(d.metric[0].stage == QG_D_CPU_QUERY && d.metric[0].api_status == mock_last_error);
    assert(d.provider_valid_mask == (QG_GPU | QG_DISK | QG_NET));
    cpu_error = false;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_ALL);
    wts_error = true;
    advance();
    qg_metrics_sample_diagnostic(m, true, &s, &d);
    unknown(&s);
    assert(d.provider_valid_mask == QG_ALL && d.sessions_checked && !d.sessions_ok);
    assert(d.session_status == mock_last_error && s.veto);
    assert(d.session_reason == 1);
    wts_error = false;
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_ALL && !d.sessions_checked && d.session_status == 0);
    suspend_in_collect = true;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    assert(d.provider_valid_mask == QG_ALL && d.continuous_before && !d.continuous_after);
    assert(d.clock_reason == 8 && d.collection_tick_ms == 10000 && d.collection_awake_ms == 0);
    qg_metrics_destroy(m);
}

static void check_diagnostic_clock_sessions(void)
{
    QgSample s;
    QgDiagnostics d;
    QgMetrics *m = qg_metrics_create(QG_CPU);
    size_t raw_before = pdh_raw_calls;
    assert(m);
    qg_metrics_sample_diagnostic(m, false, &s, &d);
    unknown(&s);
    assert(d.clock_reason == 1);
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_CPU && d.clock_reason == 0);
    wts_error = true;
    advance();
    qg_metrics_sample_diagnostic(m, true, &s, &d);
    unknown(&s);
    assert(d.provider_valid_mask == QG_CPU && d.metric[0].stage == QG_D_OK);
    assert(d.sessions_checked && !d.sessions_ok && d.session_reason == 1);
    assert(d.session_status == mock_last_error && s.veto);
    wts_error = false;
    session_count = 1;
    session_rows[0].SessionId = 0;
    session_rows[0].State = WTSActive;
    session_users[0] = L"Alice";
    username_error = true;
    advance();
    qg_metrics_sample_diagnostic(m, true, &s, &d);
    unknown(&s);
    assert(d.provider_valid_mask == QG_CPU && d.session_reason == 3);
    assert(d.session_status == mock_last_error && s.veto);
    username_error = false;
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_CPU && !d.sessions_checked && d.session_reason == 0);
    tick += 10000;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    assert(d.clock_reason == 5 && !d.continuous_before && !d.continuous_after);
    assert(d.elapsed_tick_ms == 11000 && d.elapsed_awake_ms == 1000);
    assert(d.provider_valid_mask == 0 && d.metric[0].stage == QG_D_BASELINE);
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_CPU && d.clock_reason == 0);
    clock_fail_on_call = clock_query_calls + 1u;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    assert(!d.clock_start_ok && d.clock_end_ok && d.clock_reason == 2);
    assert(d.clock_status == mock_last_error && d.collection_awake_ms == 0);
    clock_fail_on_call = 0;
    next_diagnostic(m, &s, &d);
    unknown(&s);
    next_diagnostic(m, &s, &d);
    assert(s.valid == QG_CPU && d.clock_reason == 0);
    assert(pdh_raw_calls == raw_before); /* CPU-only diagnostics never touch PDH. */
    qg_metrics_destroy(m);
}

static void check_provider_engine_recovery(void)
{
    /* This is actual metrics.c -> core.c integration, not a model of either.
       A rejected sample cancels the streak even though the provider accepts
       the next independently validated PDH interval without extra warmup. */
    for (unsigned failure = 0; failure < 3u; ++failure) {
        QgConfig c;
        QgEngine e;
        QgSample s;
        QgMetrics *m;
        uint64_t original_start, restarted_at, rejected_at, prior_epoch;
        disk_fixture();
        disk_values[1] = disk_values[2] = 100;
        qg_defaults(&c);
        c.mask = QG_DISK;
        c.enabled = 1;
        c.idle_seconds = 60;
        c.sample_seconds = 1;
        c.warning_seconds = 10;
        c.veto_active_session = 0;
        assert(qg_config_valid(&c));
        qg_engine_init(&e, &c);
        m = qg_metrics_create(QG_DISK);
        assert(m);
        qg_metrics_sample(m, false, &s);
        qg_engine_sample(&e, &s);
        assert(e.phase == QG_UNKNOWN && !e.have_idle);
        next_sample(m, &s);
        assert(s.valid == QG_DISK && s.value[2] == 0);
        qg_engine_sample(&e, &s);
        original_start = s.at_ms;
        assert(e.phase == QG_IDLE && e.idle_since == original_start);
        for (unsigned i = 0; i < 30u; ++i) {
            next_sample(m, &s);
            qg_engine_sample(&e, &s);
            assert(e.phase == QG_IDLE && e.idle_since == original_start);
        }
        if (failure == 0u) disk_values[1] = 100.0 + 0x1p-46;
        if (failure == 1u) disk_status[1] = PDH_CSTATUS_NO_INSTANCE;
        if (failure == 2u) disk_format_error[1] = PDH_INVALID_DATA;
        prior_epoch = e.epoch;
        next_sample(m, &s);
        unknown(&s);
        rejected_at = s.at_ms;
        qg_engine_sample(&e, &s);
        assert(e.phase == QG_UNKNOWN && !e.have_idle && e.idle_since == 0);
        assert(e.epoch != prior_epoch && !qg_engine_can_commit(&e, prior_epoch, s.at_ms));
        disk_values[1] = 100;
        disk_status[1] = PDH_CSTATUS_VALID_DATA;
        disk_format_error[1] = ERROR_SUCCESS;
        next_sample(m, &s);
        assert(s.valid == QG_DISK && !s.discontinuity);
        qg_engine_sample(&e, &s);
        restarted_at = s.at_ms;
        assert(restarted_at == rejected_at + 1000 && restarted_at > original_start);
        assert(e.phase == QG_IDLE && e.have_idle && e.idle_since == restarted_at);
        /* The old deadline passes during this loop, but only a full new
           60-second streak can reach warning. No rejected time is credited. */
        for (unsigned i = 1; i < 60u; ++i) {
            next_sample(m, &s);
            qg_engine_sample(&e, &s);
            assert(e.phase == QG_IDLE && e.idle_since == restarted_at);
            assert(!qg_engine_can_commit(&e, e.epoch, s.at_ms));
        }
        next_sample(m, &s);
        qg_engine_sample(&e, &s);
        assert(e.phase == QG_WARNING && s.at_ms - restarted_at == 60000);
        assert(e.warning_since == s.at_ms);
        for (unsigned i = 1; i <= 10u; ++i) {
            next_sample(m, &s);
            qg_engine_sample(&e, &s);
            assert(e.phase == (i == 10u ? QG_READY : QG_WARNING));
        }
        assert(qg_engine_can_commit(&e, e.epoch, s.at_ms));
        /* Test accounting only: this harness has no power or service APIs. */
        qg_metrics_destroy(m);
    }
}

static void check_network(void)
{
    QgSample s;
    MIB_IF_ROW2 swap;
    QgMetrics *m;
    net_table.NumEntries = 3;
    net_table.Table[0].InterfaceLuid.Value = 9;
    net_table.Table[0].OperStatus = 1;
    net_table.Table[0].Type = 6;
    net_table.Table[1].InterfaceLuid.Value = 5;
    net_table.Table[1].OperStatus = 1;
    net_table.Table[1].Type = 131; /* A virtual/tunnel adapter is included. */
    net_table.Table[2].Type = 24; /* Loopback is excluded. */
    m = qg_metrics_create(QG_NET);
    assert(m);
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    net_table.Table[0].InOctets += 1024;
    net_table.Table[1].OutOctets += 1024;
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_NET && s.value[3] == 2);

    swap = net_table.Table[0];
    net_table.Table[0] = net_table.Table[1];
    net_table.Table[1] = swap;
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_NET && s.value[3] == 0);
    net_table.Table[0].OperStatus = 2;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_NET);
    net_table.Table[1].InOctets = 0;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_NET);

    net_error = true;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    net_error = false;
    advance();
    qg_metrics_sample(m, false, &s);
    unknown(&s);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_NET);
    qg_metrics_destroy(m);
}

static void check_sessions(void)
{
    QgSample s;
    QgMetrics *m;
    session_count = 1;
    session_rows[0].SessionId = 0;
    session_rows[0].State = WTSActive;
    session_users[0] = L"Alice";
    m = qg_metrics_create(QG_CPU);
    assert(m);
    qg_metrics_sample(m, true, &s);
    unknown(&s);
    assert(s.veto);
    advance();
    qg_metrics_sample(m, true, &s);
    assert(s.veto && s.valid == QG_CPU);
    session_rows[0].State = WTSDisconnected;
    advance();
    qg_metrics_sample(m, true, &s);
    assert(!s.veto && s.valid == QG_CPU);
    session_rows[0].State = WTSConnected;
    session_users[0] = L"";
    advance();
    qg_metrics_sample(m, true, &s);
    assert(!s.veto && s.valid == QG_CPU);
    username_error = true;
    advance();
    qg_metrics_sample(m, true, &s);
    unknown(&s);
    assert(s.veto);
    username_error = false;
    session_rows[0].State = WTSDown;
    advance();
    qg_metrics_sample(m, true, &s);
    unknown(&s);
    assert(s.veto);
    advance();
    qg_metrics_sample(m, false, &s);
    assert(s.valid == QG_CPU && !s.veto);
    qg_metrics_destroy(m);
    assert(!session_veto(&s.veto, NULL) && s.veto);
}

int main(void)
{
    check_helpers();
    check_cpu_and_clocks();
    check_gpu();
    check_disk();
    check_disk_range_and_status();
    check_disk_api_failures();
    assert(!pdh_raw_calls); /* Every ordinary sample above avoided raw reads. */
    check_network();
    check_sessions();
    check_disk_diagnostics();
    check_disk_diagnostic_faults();
    check_diagnostic_masks();
    check_diagnostic_clock_sessions();
    check_provider_engine_recovery();
    assert(net_allocs == net_frees);
    assert(pdh_opens == pdh_closes);
    puts("PASS: CPU, PDH engine aggregation/topology/status, disk range/status/topology/recovery, raw diagnostics and masks, provider-to-engine idle reset, NIC deltas/topology, WTS veto, clock/suspend, cache reuse and allocation cleanup");
    return 0;
}
