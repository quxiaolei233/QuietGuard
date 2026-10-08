/* Portable deterministic safety tests. No platform API, sleeping, I/O to files,
 * startup changes, or shutdown requests. Compile with QG_TEST_MAIN for a harness. */
#include "qg.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned long checks, failures;
static const char *test_name;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; if (failures <= 24) \
    fprintf(stderr, "FAIL %s:%d: %s\n", test_name, __LINE__, #x); } } while (0)

static QgConfig enabled_config(void) {
    QgConfig c;
    qg_defaults(&c);
    c.enabled = 1;
    c.idle_seconds = 60;
    c.sample_seconds = 2;
    c.warning_seconds = 10;
    c.veto_active_session = 0;
    return c;
}
static QgSample low_sample(uint64_t at) {
    QgSample s;
    memset(&s, 0, sizeof(s));
    s.at_ms = at;
    s.valid = QG_ALL;
    return s;
}
static void sample_at(QgEngine *e, uint64_t at) {
    QgSample s = low_sample(at);
    qg_engine_sample(e, &s);
}
static void ready_engine(QgEngine *e, const QgConfig *c, uint64_t base) {
    uint64_t idle_end = base + (uint64_t)c->idle_seconds * 1000;
    uint64_t step = (uint64_t)c->sample_seconds * 1000;
    qg_engine_init(e, c);
    sample_at(e, base);
    for (uint64_t at = base + step; at <= idle_end; at += step) sample_at(e, at);
    CHECK(e->phase == QG_WARNING);
    uint64_t warning_end = idle_end + (uint64_t)c->warning_seconds * 1000;
    if (step > 2000) step = 2000;
    for (uint64_t at = idle_end + step; at <= warning_end; at += step) sample_at(e, at);
    CHECK(e->phase == QG_READY);
}
static bool configs_equal(const QgConfig *a, const QgConfig *b) {
    if (a->mask != b->mask || a->idle_seconds != b->idle_seconds ||
        a->sample_seconds != b->sample_seconds || a->warning_seconds != b->warning_seconds ||
        a->enabled != b->enabled || a->veto_active_session != b->veto_active_session) return false;
    for (unsigned i = 0; i < QG_METRICS; ++i)
        if (fabs(a->threshold[i] - b->threshold[i]) > 0.00000051) return false;
    return true;
}
static uint32_t random_u32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}

static void test_configuration_bounds(void) {
    test_name = "configuration bounds";
    QgConfig c, x;
    qg_defaults(&c);
    CHECK(qg_config_valid(&c));
    CHECK(c.enabled == 0);
    CHECK(c.mask == QG_ALL);
    CHECK(c.veto_active_session == 1);
    CHECK(c.idle_seconds == 10800 && c.sample_seconds == 60 && c.warning_seconds == 60);
    CHECK(!qg_config_valid(NULL));
    for (uint32_t mask = 0; mask <= 31; ++mask) {
        x = c; x.mask = mask;
        CHECK(qg_config_valid(&x) == (mask > 0 && mask <= QG_ALL));
    }
    x = c; x.mask = UINT32_MAX; CHECK(!qg_config_valid(&x));
    const uint32_t invalid_idle[] = {0, 1, 59, 604801, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid_idle)/sizeof(invalid_idle[0]); ++i) {
        x = c; x.idle_seconds = invalid_idle[i]; CHECK(!qg_config_valid(&x));
    }
    x = c; x.idle_seconds = 60; x.sample_seconds = 30; CHECK(qg_config_valid(&x));
    x.sample_seconds = 31; CHECK(!qg_config_valid(&x));
    x = c; x.idle_seconds = 604800; x.sample_seconds = 600; CHECK(qg_config_valid(&x));
    x.sample_seconds = 601; CHECK(!qg_config_valid(&x));
    x.sample_seconds = 0; CHECK(!qg_config_valid(&x));
    x.sample_seconds = UINT32_MAX; CHECK(!qg_config_valid(&x));
    x = c; x.sample_seconds = 1; CHECK(qg_config_valid(&x));
    const uint32_t warning[] = {0, 9, 10, 3600, 3601, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(warning)/sizeof(warning[0]); ++i) {
        x = c; x.warning_seconds = warning[i];
        CHECK(qg_config_valid(&x) == (warning[i] >= 10 && warning[i] <= 3600));
    }
    for (uint32_t n = 0; n <= 2; ++n) {
        x = c; x.enabled = n; CHECK(qg_config_valid(&x) == (n <= 1));
        x = c; x.veto_active_session = n; CHECK(qg_config_valid(&x) == (n <= 1));
    }
    x = c; x.enabled = UINT32_MAX; CHECK(!qg_config_valid(&x));
    x = c; x.veto_active_session = UINT32_MAX; CHECK(!qg_config_valid(&x));
    const double invalid_threshold[] = {0, -0.001, -1, 0.000999, NAN, INFINITY, -INFINITY, 1.1e9};
    for (unsigned m = 0; m < QG_METRICS; ++m) {
        for (unsigned i = 0; i < sizeof(invalid_threshold)/sizeof(invalid_threshold[0]); ++i) {
            x = c; x.threshold[m] = invalid_threshold[i]; CHECK(!qg_config_valid(&x));
        }
        x = c; x.threshold[m] = 0.001; CHECK(qg_config_valid(&x));
        x.threshold[m] = m == 3 ? 1e9 : 100; CHECK(qg_config_valid(&x));
        x.threshold[m] += 0.01; CHECK(!qg_config_valid(&x));
        /* Disabled metrics still have to carry valid configuration values. */
        x = c; x.mask = m == 0 ? QG_NET : QG_CPU;
        x.threshold[m] = NAN; CHECK(!qg_config_valid(&x));
    }
}

static const char *const keys[] = {
    "version", "mask", "idle_seconds", "sample_seconds", "warning_seconds",
    "cpu", "gpu", "disk", "network", "enabled", "veto_active_session"
};
static const char *const values[] = {"1", "15", "60", "2", "10", "5", "5", "5", "32", "1", "0"};
static size_t document(char *out, size_t cap, int replace, const char *key,
                       const char *value, int omit, bool crlf) {
    size_t used = 0;
    for (int i = 0; i < 11; ++i) {
        if (i == omit) continue;
        int n = snprintf(out + used, cap - used, "%s=%s%s", i == replace && key ? key : keys[i],
                         i == replace && value ? value : values[i], crlf ? "\r\n" : "\n");
        if (n < 0 || (size_t)n >= cap - used) return 0;
        used += (size_t)n;
    }
    return used;
}
static bool parse_preserving_failure(const char *s, size_t n) {
    QgConfig out, before;
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    bool ok = qg_config_parse(s, n, &out);
    if (!ok) CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    else CHECK(qg_config_valid(&out));
    return ok;
}
static void test_parser(void) {
    test_name = "strict parser";
    char text[QG_CONFIG_MAX + 64], altered[QG_CONFIG_MAX + 64];
    QgConfig c, parsed;
    qg_defaults(&c);
    size_t n = qg_config_format(&c, text, sizeof(text));
    CHECK(n > 0 && n < QG_CONFIG_MAX);
    CHECK(qg_config_parse(text, n, &parsed));
    CHECK(configs_equal(&c, &parsed));
    CHECK(!qg_config_parse(NULL, n, &parsed));
    CHECK(!qg_config_parse(text, n, NULL));
    CHECK(!parse_preserving_failure(text, 0));
    CHECK(!parse_preserving_failure("", 1));
    CHECK(qg_config_format(NULL, text, sizeof(text)) == 0);
    CHECK(qg_config_format(&c, NULL, sizeof(text)) == 0);
    CHECK(qg_config_format(&c, text, 0) == 0);
    CHECK(qg_config_format(&c, text, 1) == 0);
    CHECK(qg_config_format(&c, text, n) == 0);
    CHECK(qg_config_format(&c, text, n + 1) == n);
    CHECK(text[n] == '\0');
    QgConfig bad = c; bad.mask = 0;
    CHECK(qg_config_format(&bad, text, sizeof(text)) == 0);

    n = document(text, sizeof(text), -1, NULL, NULL, -1, false);
    CHECK(parse_preserving_failure(text, n));
    CHECK(parse_preserving_failure(text, n - 1)); /* final LF is optional */
    for (size_t cut = 0; cut + 1 < n; ++cut) CHECK(!parse_preserving_failure(text, cut));
    CHECK(!parse_preserving_failure(text, n + 1)); /* NUL never part of a file */
    size_t crlf = document(altered, sizeof(altered), -1, NULL, NULL, -1, true);
    CHECK(parse_preserving_failure(altered, crlf));
    CHECK(parse_preserving_failure(altered, crlf - 2));
    for (size_t i = 0; i < n; ++i) {
        memcpy(altered, text, n);
        altered[i] = '\0';
        CHECK(!parse_preserving_failure(altered, n));
    }
    for (int i = 0; i < 11; ++i) {
        size_t len = document(altered, sizeof(altered), -1, NULL, NULL, i, false);
        CHECK(!parse_preserving_failure(altered, len));
        len = document(altered, sizeof(altered), -1, NULL, NULL, -1, false);
        int extra = snprintf(altered + len, sizeof(altered) - len, "%s=%s\n", keys[i], values[i]);
        CHECK(extra > 0);
        CHECK(!parse_preserving_failure(altered, len + (size_t)extra));
        len = document(altered, sizeof(altered), i, "unknown", NULL, -1, false);
        CHECK(!parse_preserving_failure(altered, len));
    }
    const char *invalid_all[] = {"", "+1", "-1", "1e0", "1E0", " 1", "1 ", "1\t", "\t1", "0x1", "nan", "NaN", "inf", "Infinity", "1=1", "1\r1", "1\n1", "1,0", ".1", "1.", "1..0", "1000000001", "999999999999999999999", "0000000000000000000001"};
    for (int k = 0; k < 11; ++k) {
        for (unsigned i = 0; i < sizeof(invalid_all)/sizeof(invalid_all[0]); ++i) {
            size_t len = document(altered, sizeof(altered), k, NULL, invalid_all[i], -1, false);
            CHECK(!parse_preserving_failure(altered, len));
        }
        if (k < 5 || k >= 9) {
            size_t len = document(altered, sizeof(altered), k, NULL, "1.000000", -1, false);
            CHECK(!parse_preserving_failure(altered, len));
        } else {
            size_t len = document(altered, sizeof(altered), k, NULL, "1.0000001", -1, false);
            CHECK(!parse_preserving_failure(altered, len));
            len = document(altered, sizeof(altered), k, NULL, "0.001000", -1, false);
            CHECK(parse_preserving_failure(altered, len));
        }
        size_t len = document(altered, sizeof(altered), k, " version", NULL, -1, false);
        CHECK(!parse_preserving_failure(altered, len));
    }
    struct Invalid { int k; const char *v; } invalid[] = {
        {0,"0"}, {0,"2"}, {1,"0"}, {1,"16"}, {1,"4294967295"},
        {2,"0"}, {2,"59"}, {2,"604801"}, {3,"0"}, {3,"31"}, {3,"601"},
        {4,"0"}, {4,"9"}, {4,"3601"}, {5,"0"}, {5,"100.000001"},
        {6,"0.000999"}, {6,"101"}, {7,"101"}, {8,"1000000000.000001"},
        {9,"2"}, {10,"2"}
    };
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        size_t len = document(altered, sizeof(altered), invalid[i].k, NULL, invalid[i].v, -1, false);
        CHECK(!parse_preserving_failure(altered, len));
    }
    const char *extra[] = {"\n", "garbage", "unknown=1\n", "# comment\n", " \n"};
    for (unsigned i = 0; i < sizeof(extra)/sizeof(extra[0]); ++i) {
        memcpy(altered, text, n); memcpy(altered + n, extra[i], strlen(extra[i]));
        CHECK(!parse_preserving_failure(altered, n + strlen(extra[i])));
    }
    memcpy(altered + 1, text, n); altered[0] = '\n';
    CHECK(!parse_preserving_failure(altered, n + 1));
    memcpy(altered + 3, text, n); altered[0] = (char)0xef; altered[1] = (char)0xbb; altered[2] = (char)0xbf;
    CHECK(!parse_preserving_failure(altered, n + 3));
    memset(altered, 'x', sizeof(altered));
    CHECK(!parse_preserving_failure(altered, QG_CONFIG_MAX - 1));
    CHECK(!parse_preserving_failure(altered, QG_CONFIG_MAX));
    CHECK(!parse_preserving_failure(altered, sizeof(altered)));
    /* Length is authoritative: unrelated bytes beyond the supplied span are ignored. */
    memcpy(altered, text, n); altered[n] = 'X';
    CHECK(parse_preserving_failure(altered, n));
}

static void test_parser_roundtrip_and_fuzz(void) {
    test_name = "parser roundtrip and deterministic fuzz";
    uint32_t rng = UINT32_C(0x17b21acd);
    char text[QG_CONFIG_MAX + 64], stable[QG_CONFIG_MAX + 64], again[QG_CONFIG_MAX + 64];
    for (unsigned trial = 0; trial < 3000; ++trial) {
        QgConfig a = enabled_config(), b, c;
        a.mask = 1 + random_u32(&rng) % 15;
        a.sample_seconds = 1 + random_u32(&rng) % 600;
        a.idle_seconds = 1200 + random_u32(&rng) % (604800 - 1199);
        a.warning_seconds = 10 + random_u32(&rng) % 3591;
        a.enabled = random_u32(&rng) & 1;
        a.veto_active_session = random_u32(&rng) & 1;
        for (unsigned k = 0; k < QG_METRICS; ++k)
            a.threshold[k] = (1000 + random_u32(&rng) % 99999001) / 1000000.0;
        a.threshold[3] = (double)(1 + random_u32(&rng) % 1000000000);
        if (trial == 0) for (unsigned k = 0; k < QG_METRICS; ++k) a.threshold[k] = 0.001;
        if (trial == 1) for (unsigned k = 0; k < QG_METRICS; ++k) a.threshold[k] = k == 3 ? 1e9 : 100;
        size_t n = qg_config_format(&a, text, sizeof(text));
        CHECK(n > 0);
        CHECK(qg_config_parse(text, n, &b));
        CHECK(configs_equal(&a, &b));
        size_t m = qg_config_format(&b, again, sizeof(again));
        CHECK(n == m && memcmp(text, again, n) == 0);
        CHECK(qg_config_parse(again, m, &c));
        CHECK(configs_equal(&b, &c));
    }
    /* Required keys may appear in any order; canonical output remains stable. */
    for (unsigned trial = 0; trial < 256; ++trial) {
        unsigned order[11];
        for (unsigned i = 0; i < 11; ++i) order[i] = i;
        for (unsigned i = 10; i > 0; --i) {
            unsigned j = random_u32(&rng) % (i + 1), t = order[j]; order[j] = order[i]; order[i] = t;
        }
        size_t used = 0;
        for (unsigned i = 0; i < 11; ++i) {
            int m = snprintf(text + used, sizeof(text) - used, "%s=%s\n", keys[order[i]], values[order[i]]);
            CHECK(m > 0); used += (size_t)m;
        }
        QgConfig parsed;
        CHECK(qg_config_parse(text, used, &parsed));
        QgConfig expected = enabled_config();
        CHECK(configs_equal(&expected, &parsed));
    }
    size_t base = document(stable, sizeof(stable), -1, NULL, NULL, -1, false);
    for (unsigned trial = 0; trial < 10000; ++trial) {
        size_t n;
        if (trial & 1) {
            n = random_u32(&rng) % sizeof(text);
            for (size_t i = 0; i < n; ++i) text[i] = (char)(random_u32(&rng) & 255);
        } else {
            memcpy(text, stable, base); n = base;
            unsigned mutations = 1 + random_u32(&rng) % 8;
            for (unsigned i = 0; i < mutations; ++i) text[random_u32(&rng) % n] = (char)(random_u32(&rng) & 255);
        }
        QgConfig result;
        if (parse_preserving_failure(text, n)) {
            CHECK(qg_config_parse(text, n, &result));
            size_t m = qg_config_format(&result, again, sizeof(again));
            CHECK(m > 0);
            QgConfig canonical;
            CHECK(qg_config_parse(again, m, &canonical));
            CHECK(configs_equal(&result, &canonical));
        }
    }
}

static void test_metrics_and_fail_closed(void) {
    test_name = "metric AND and invalid readings";
    QgConfig c = enabled_config();
    QgEngine e;
    for (uint32_t mask = 1; mask <= QG_ALL; ++mask) {
        c.mask = mask;
        qg_engine_init(&e, &c);
        CHECK(e.phase == QG_UNKNOWN && !e.have_idle && !e.have_last);
        QgSample s = low_sample(1000);
        s.valid = mask;
        for (unsigned i = 0; i < QG_METRICS; ++i) if (!(mask & (1u << i))) s.value[i] = NAN;
        qg_engine_sample(&e, &s);
        CHECK(e.phase == QG_IDLE && e.have_idle && e.idle_since == 1000);
        CHECK(!qg_engine_can_commit(&e, e.epoch, 1000));
        for (unsigned metric = 0; metric < QG_METRICS; ++metric) {
            if (!(mask & (1u << metric))) continue;
            qg_engine_init(&e, &c); sample_at(&e, 1000);
            s = low_sample(3000); s.value[metric] = c.threshold[metric];
            uint64_t epoch = e.epoch;
            qg_engine_sample(&e, &s);
            CHECK(e.phase == QG_BUSY && !e.have_idle && e.epoch != epoch);
            sample_at(&e, 5000); CHECK(e.phase == QG_IDLE && e.idle_since == 5000);
            qg_engine_init(&e, &c);
            s = low_sample(1000); s.value[metric] = c.threshold[metric] - 0.000001;
            qg_engine_sample(&e, &s); CHECK(e.phase == QG_IDLE);
            s.at_ms = 3000; s.value[metric] = c.threshold[metric] + 0.000001;
            qg_engine_sample(&e, &s); CHECK(e.phase == QG_BUSY);
            qg_engine_init(&e, &c); sample_at(&e, 1000);
            s = low_sample(3000); s.valid &= ~(1u << metric);
            qg_engine_sample(&e, &s); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
            const double malformed[] = {NAN, INFINITY, -INFINITY, -0.000001, -1};
            for (unsigned j = 0; j < sizeof(malformed)/sizeof(malformed[0]); ++j) {
                qg_engine_init(&e, &c); sample_at(&e, 1000);
                s = low_sample(3000); s.value[metric] = malformed[j];
                qg_engine_sample(&e, &s); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
            }
            if (metric < 3) {
                qg_engine_init(&e, &c); sample_at(&e, 1000);
                s = low_sample(3000); s.value[metric] = 100.000001;
                qg_engine_sample(&e, &s); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
            }
        }
        qg_engine_init(&e, &c); sample_at(&e, 1000);
        s = low_sample(3000); s.valid = 0;
        qg_engine_sample(&e, &s); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
        sample_at(&e, 5000); CHECK(e.phase == QG_IDLE && e.idle_since == 5000);
        s = low_sample(7000); s.veto = true;
        qg_engine_sample(&e, &s); CHECK(e.phase == QG_BUSY && !e.have_idle);
    }
    c = enabled_config(); c.mask = QG_NET; c.threshold[3] = 1000000;
    qg_engine_init(&e, &c);
    QgSample s = low_sample(0); s.value[3] = 999999;
    qg_engine_sample(&e, &s); CHECK(e.phase == QG_IDLE); /* network is not a percent */
    qg_engine_init(&e, &c); s = low_sample(0);
    for (unsigned i = 0; i < QG_METRICS; ++i) s.value[i] = -0.0;
    qg_engine_sample(&e, &s); CHECK(e.phase == QG_IDLE);
    c = enabled_config(); qg_engine_init(&e, &c);
    s = low_sample(0); s.valid = 0; qg_engine_sample(&e, &s);
    CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
    sample_at(&e, 2000); CHECK(e.phase == QG_IDLE && e.idle_since == 2000);
}

static void test_time_and_reset(void) {
    test_name = "timeline, sleep, regression and reset";
    QgConfig c = enabled_config(); QgEngine e;
    qg_engine_init(&e, &c); sample_at(&e, 0);
    CHECK(e.have_idle && e.idle_since == 0 && e.phase == QG_IDLE);
    for (uint64_t at = 2000; at < 60000; at += 2000) sample_at(&e, at);
    CHECK(e.phase == QG_IDLE);
    sample_at(&e, 59999); CHECK(e.phase == QG_IDLE);
    sample_at(&e, 60000); CHECK(e.phase == QG_WARNING && e.warning_since == 60000);
    for (uint64_t at = 62000; at < 70000; at += 2000) sample_at(&e, at);
    sample_at(&e, 69999); CHECK(e.phase == QG_WARNING);
    sample_at(&e, 70000); CHECK(e.phase == QG_READY);
    CHECK(qg_engine_can_commit(&e, e.epoch, 70000));
    CHECK(qg_engine_can_commit(&e, e.epoch, 71999));
    CHECK(qg_engine_can_commit(&e, e.epoch, 72000));
    CHECK(!qg_engine_can_commit(&e, e.epoch, 72001));
    CHECK(!qg_engine_can_commit(&e, e.epoch, 69999));
    CHECK(!qg_engine_can_commit(&e, e.epoch - 1, 70000));
    /* Clock observations do not advance the state; elapsed time alone never commits. */
    CHECK(!qg_engine_can_commit(&e, e.epoch, UINT64_MAX));
    uint64_t epoch = e.epoch;
    sample_at(&e, 70000); CHECK(e.phase == QG_UNKNOWN && e.epoch != epoch && !e.have_idle);
    sample_at(&e, 72000); CHECK(e.phase == QG_IDLE && e.idle_since == 72000);
    sample_at(&e, 71999); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
    sample_at(&e, 73999); CHECK(e.phase == QG_IDLE && e.idle_since == 73999);
    QgSample s = low_sample(75999); s.discontinuity = true;
    qg_engine_sample(&e, &s); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
    sample_at(&e, 77999); CHECK(e.phase == QG_IDLE && e.idle_since == 77999);
    sample_at(&e, 1000000000); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
    sample_at(&e, 1000002000); CHECK(e.phase == QG_IDLE && e.idle_since == 1000002000);
    /* Exact normal scheduling tolerance: max(2 seconds, 10% of interval). */
    const uint32_t intervals[] = {1, 2, 10, 60, 600};
    for (unsigned i = 0; i < sizeof(intervals)/sizeof(intervals[0]); ++i) {
        c = enabled_config(); c.sample_seconds = intervals[i]; c.idle_seconds = 1200;
        uint64_t step = (uint64_t)c.sample_seconds * 1000;
        uint64_t slack = step / 10; if (slack < 2000) slack = 2000;
        qg_engine_init(&e, &c); sample_at(&e, 1000); sample_at(&e, 1000 + step + slack);
        CHECK(e.phase == QG_IDLE && e.idle_since == 1000);
        qg_engine_init(&e, &c); sample_at(&e, 1000); sample_at(&e, 1001 + step + slack);
        CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
    }
    c = enabled_config(); c.sample_seconds = 30;
    ready_engine(&e, &c, 1000);
    sample_at(&e, e.last_ms + 4001); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
    qg_engine_init(&e, &c); sample_at(&e, 1000); sample_at(&e, 31000); sample_at(&e, 61000);
    CHECK(e.phase == QG_WARNING);
    sample_at(&e, 91000); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
    c = enabled_config();
    uint64_t base = UINT64_MAX - 80000;
    ready_engine(&e, &c, base);
    CHECK(qg_engine_can_commit(&e, e.epoch, base + 70000));
    for (unsigned i = 0; i < 4; ++i) sample_at(&e, e.last_ms + 2000);
    sample_at(&e, UINT64_MAX - 1000);
    CHECK(e.phase == QG_READY);
    CHECK(qg_engine_can_commit(&e, e.epoch, UINT64_MAX));
    /* Unsigned subtraction alone could mistake clock wrap for a fresh sample. */
    CHECK(!qg_engine_can_commit(&e, e.epoch, 0));
    CHECK(!qg_engine_can_commit(&e, e.epoch, 999));
    sample_at(&e, 1); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
    qg_engine_reset(&e, QG_UNKNOWN);
    CHECK(!e.have_last && !e.have_idle && e.last_ms == 0 && e.idle_since == 0 && e.warning_since == 0);
}

static void test_cancel_config_and_final_barrier(void) {
    test_name = "cancel/config epochs and fresh final barrier";
    QgConfig c = enabled_config(); QgEngine e;
    ready_engine(&e, &c, 1000);
    uint64_t epoch = e.epoch, now = e.last_ms;
    CHECK(qg_engine_can_commit(&e, epoch, now));
    qg_engine_cancel(&e);
    CHECK(e.phase == QG_PAUSED && !e.cfg.enabled && e.epoch != epoch);
    CHECK(!e.have_idle && !e.have_last && !qg_engine_can_commit(&e, epoch, now));
    sample_at(&e, now + 1); CHECK(e.phase == QG_PAUSED && !e.have_last);
    uint64_t cancelled = e.epoch;
    qg_engine_cancel(&e); CHECK(e.epoch != cancelled && e.phase == QG_PAUSED);
    qg_engine_configure(&e, &c); CHECK(e.phase == QG_UNKNOWN && !e.have_idle);
    CHECK(!qg_engine_can_commit(&e, epoch, now));
    sample_at(&e, now + 2); CHECK(e.phase == QG_IDLE && e.idle_since == now + 2);
    ready_engine(&e, &c, 0); epoch = e.epoch; now = e.last_ms;
    qg_engine_configure(&e, &c); /* Even identical Apply invalidates a queued decision. */
    CHECK(e.epoch != epoch && e.phase == QG_UNKNOWN && !qg_engine_can_commit(&e, epoch, now));
    ready_engine(&e, &c, 0); epoch = e.epoch;
    QgConfig changed = c; changed.threshold[0] += 1;
    qg_engine_configure(&e, &changed); CHECK(e.epoch != epoch && !e.have_idle);
    ready_engine(&e, &c, 0); epoch = e.epoch; changed = c; changed.enabled = 0;
    qg_engine_configure(&e, &changed); CHECK(e.phase == QG_PAUSED && e.epoch != epoch);
    changed = c; changed.mask = 0;
    qg_engine_configure(&e, &changed); CHECK(e.phase == QG_FAULT && !e.cfg.enabled && !e.have_idle);
    sample_at(&e, 100000); CHECK(e.phase == QG_FAULT && !e.have_last);
    qg_engine_configure(&e, NULL); CHECK(e.phase == QG_FAULT && !e.cfg.enabled);
    qg_engine_init(&e, NULL); CHECK(e.phase == QG_FAULT && !e.cfg.enabled);

    /* For every selected metric, a final busy or unknown sample revokes READY. */
    for (unsigned i = 0; i < QG_METRICS; ++i) {
        for (unsigned kind = 0; kind < 5; ++kind) {
            ready_engine(&e, &c, 1000); epoch = e.epoch; now = e.last_ms;
            QgSample final_sample = low_sample(now + 1);
            if (kind == 0) final_sample.value[i] = c.threshold[i];
            if (kind == 1) final_sample.valid &= ~(1u << i);
            if (kind == 2) final_sample.value[i] = NAN;
            if (kind == 3) final_sample.veto = true;
            if (kind == 4) final_sample.discontinuity = true;
            qg_engine_sample(&e, &final_sample);
            CHECK(!qg_engine_can_commit(&e, epoch, now + 1));
            CHECK(!qg_engine_can_commit(&e, e.epoch, now + 1));
            CHECK(!e.have_idle && e.epoch != epoch);
        }
    }
    /* Cancellation order is safe both before and after the final fresh sample. */
    ready_engine(&e, &c, 0); epoch = e.epoch; now = e.last_ms;
    qg_engine_cancel(&e); sample_at(&e, now + 1);
    CHECK(!qg_engine_can_commit(&e, epoch, now + 1) && e.phase == QG_PAUSED);
    ready_engine(&e, &c, 0); epoch = e.epoch; now = e.last_ms;
    sample_at(&e, now + 1); CHECK(qg_engine_can_commit(&e, epoch, now + 1));
    qg_engine_cancel(&e); CHECK(!qg_engine_can_commit(&e, epoch, now + 1));
    /* A requested result is terminal until an explicit reset or reconfiguration. */
    ready_engine(&e, &c, 0); e.phase = QG_REQUESTED; epoch = e.epoch; now = e.last_ms;
    sample_at(&e, now + 100000); CHECK(e.phase == QG_REQUESTED && e.last_ms == now && e.epoch == epoch);
    CHECK(!qg_engine_can_commit(&e, epoch, now));
}

/* Independent temporal oracle: track uninterrupted qualifying observations, not
 * the implementation's reset helpers or phases. A monitor may be stricter on
 * unknown-vs-busy display, but never earlier at warning or ready boundaries. */
typedef struct {
    bool observing, streak, warning;
    uint64_t previous, beginning, warning_at;
} Oracle;
static void oracle_sample(Oracle *m, const QgConfig *c, const QgSample *s) {
    uint64_t interval = (uint64_t)c->sample_seconds * 1000;
    if (m->warning && interval > 2000) interval = 2000;
    uint64_t slack = interval / 10; if (slack < 2000) slack = 2000;
    bool gap = m->observing && (s->at_ms <= m->previous || s->at_ms - m->previous > interval + slack);
    bool good = !gap && !s->discontinuity && !s->veto && (s->valid & c->mask) == c->mask;
    for (unsigned i = 0; i < QG_METRICS; ++i) if (c->mask & (1u << i))
        good = good && isfinite(s->value[i]) && s->value[i] >= 0 &&
               (i == 3 || s->value[i] <= 100) && s->value[i] < c->threshold[i];
    m->observing = true; m->previous = s->at_ms;
    if (!good) { m->streak = false; m->warning = false; return; }
    if (!m->streak) { m->streak = true; m->beginning = s->at_ms; return; }
    if (!m->warning && s->at_ms - m->beginning >= (uint64_t)c->idle_seconds * 1000) {
        m->warning = true; m->warning_at = s->at_ms;
    }
}
static void test_model_traces(void) {
    test_name = "deterministic temporal model traces";
    uint32_t rng = UINT32_C(0x51afe207);
    unsigned ready_count = 0, warning_count = 0, interruption_count = 0;
    for (unsigned trace = 0; trace < 256; ++trace) {
        QgConfig c = enabled_config(); QgEngine e; Oracle oracle;
        c.mask = 1 + random_u32(&rng) % 15;
        c.sample_seconds = 1 + random_u32(&rng) % 3;
        c.warning_seconds = 10 + random_u32(&rng) % 10;
        qg_engine_init(&e, &c); memset(&oracle, 0, sizeof(oracle));
        uint64_t at = (uint64_t)trace * 1000000;
        for (unsigned event = 0; event < 400; ++event) {
            uint32_t roll = random_u32(&rng);
            uint64_t old_epoch = e.epoch;
            /* Start every trace with a known uninterrupted run, then adversity. */
            bool calm = event < 100 || (event >= 220 && event < 320);
            at += (uint64_t)c.sample_seconds * 1000;
            if (oracle.warning && c.sample_seconds > 2) at -= 1000;
            QgSample s = low_sample(at);
            if (!calm) {
                unsigned selected = roll % QG_METRICS;
                switch ((roll >> 4) % 18) {
                case 0: s.value[selected] = c.threshold[selected]; break;
                case 1: s.value[selected] = NAN; break;
                case 2: s.value[selected] = INFINITY; break;
                case 3: s.value[selected] = -0.01; break;
                case 4: s.valid &= ~(1u << selected); break;
                case 5: s.discontinuity = true; break;
                case 6: s.veto = true; break;
                case 7: s.at_ms = e.last_ms; break;
                case 8: s.at_ms = e.last_ms ? e.last_ms - 1 : 0; break;
                case 9: s.at_ms += 7200000; break;
                case 10:
                    qg_engine_cancel(&e); CHECK(!qg_engine_can_commit(&e, old_epoch, at));
                    sample_at(&e, at); CHECK(e.phase == QG_PAUSED);
                    qg_engine_configure(&e, &c); memset(&oracle, 0, sizeof(oracle)); break;
                case 11:
                    c.mask = 1 + random_u32(&rng) % 15;
                    qg_engine_configure(&e, &c); memset(&oracle, 0, sizeof(oracle));
                    CHECK(!qg_engine_can_commit(&e, old_epoch, at)); break;
                default: break;
                }
            }
            at = s.at_ms;
            oracle_sample(&oracle, &c, &s);
            qg_engine_sample(&e, &s);
            bool ready = oracle.warning && at - oracle.warning_at >= (uint64_t)c.warning_seconds * 1000;
            CHECK(e.have_last && e.last_ms == at);
            CHECK(e.have_idle == oracle.streak);
            CHECK((e.phase == QG_WARNING || e.phase == QG_READY) == oracle.warning);
            CHECK((e.phase == QG_READY) == ready);
            if (oracle.streak) CHECK(e.idle_since == oracle.beginning);
            if (oracle.warning) { CHECK(e.warning_since == oracle.warning_at); ++warning_count; }
            if (ready) ++ready_count;
            if (!oracle.streak) ++interruption_count;
            CHECK(qg_engine_can_commit(&e, e.epoch, at) == ready);
            CHECK(qg_engine_can_commit(&e, e.epoch, at + 2000) == ready);
            CHECK(!qg_engine_can_commit(&e, e.epoch, at + 2001));
            CHECK(!qg_engine_can_commit(&e, e.epoch - 1, at));
            if (at) CHECK(!qg_engine_can_commit(&e, e.epoch, at - 1));
            if (ready) {
                CHECK(at >= oracle.beginning);
                CHECK(at - oracle.beginning >= ((uint64_t)c.idle_seconds + c.warning_seconds) * 1000);
            }
        }
    }
    CHECK(ready_count > 1000);
    CHECK(warning_count > ready_count);
    CHECK(interruption_count > 1000);
}

int qg_self_test(void) {
    checks = failures = 0;
    test_configuration_bounds();
    test_parser();
    test_parser_roundtrip_and_fuzz();
    test_metrics_and_fail_closed();
    test_time_and_reset();
    test_cancel_config_and_final_barrier();
    test_model_traces();
    if (failures) {
        fprintf(stderr, "QuietGuard self-test FAILED: %lu / %lu checks; seeds 17b21acd,51afe207\n", failures, checks);
        return 1;
    }
    printf("QuietGuard self-test PASS: %lu checks; 10000 parser fuzz inputs; 102400 model events; seeds 17b21acd,51afe207\n", checks);
    return 0;
}
#ifdef QG_TEST_MAIN
int main(void) { return qg_self_test(); }
#endif
