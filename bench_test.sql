\echo '============================================================'
\echo 'STRONGER BENCHMARK: BTREE vs MULTI-RUN LSM3 vs LSM3+BLOOM'
\echo '============================================================'
\echo 'Purpose:'
\echo '  1. Compare standard PostgreSQL B-tree with current multi-run LSM3.'
\echo '  2. Compare LSM3 with Bloom disabled vs Bloom enabled on same layout.'
\echo '  3. Make Bloom benefit more visible using many immutable runs and repeated negative probes.'
\echo ''
\echo 'Expected high-level result:'
\echo '  - B-tree is usually fastest for simple point lookups because it has one physical index.'
\echo '  - LSM without Bloom is slower for negative point lookups because it checks many runs.'
\echo '  - LSM with Bloom has a warmup/build-filter cost, then cached negative probes should be faster than LSM without Bloom.'
\echo '  - LSM insert-only timings are separate from manual compaction timings.'
\echo ''

\timing on

SET client_min_messages = warning;
SET enable_seqscan = off;
SET synchronous_commit = off;

DROP TABLE IF EXISTS bench_btree_strong CASCADE;
DROP TABLE IF EXISTS bench_lsm_nobloom_strong CASCADE;
DROP TABLE IF EXISTS bench_lsm_bloom_strong CASCADE;
DROP TABLE IF EXISTS bench_results_strong;

CREATE TEMP TABLE bench_results_strong
(
    system_name text,
    phase text,
    seconds double precision,
    total_result_count bigint,
    runs integer
);

\echo ''
\echo 'Benchmark parameters'
\echo '  batches          = 7'
\echo '  rows_per_batch   = 50000'
\echo '  total rows/table = 350000'
\echo '  runs_per_level   = 8, so 7 manual flushes should stay as 7 level0 runs and not compact upward.'
\echo '  negative_loop    = 5000 repeated absent-key point probes.'
\echo '  positive_loop    = 2000 repeated present-key point probes.'
\echo ''

\echo '============================================================'
\echo 'CREATE TABLES AND INDEXES'
\echo '============================================================'

CREATE TABLE bench_btree_strong
(
    id integer,
    payload text
);
CREATE INDEX bench_btree_strong_idx ON bench_btree_strong(id);

CREATE TABLE bench_lsm_nobloom_strong
(
    id integer,
    payload text
);
CREATE INDEX bench_lsm_nobloom_strong_idx
ON bench_lsm_nobloom_strong
USING lsm3(id)
WITH
(
    num_levels = 2,
    runs_per_level = 8,
    level_size_ratio = 1000,
    top_index_size = 1048576,
    bloom_enabled = false
);

CREATE TABLE bench_lsm_bloom_strong
(
    id integer,
    payload text
);
CREATE INDEX bench_lsm_bloom_strong_idx
ON bench_lsm_bloom_strong
USING lsm3(id)
WITH
(
    num_levels = 2,
    runs_per_level = 8,
    level_size_ratio = 1000,
    top_index_size = 1048576,
    bloom_enabled = true
);

\echo ''
\echo '============================================================'
\echo 'LOAD STANDARD BTREE TABLE'
\echo '============================================================'
\echo 'Expected: one timing row per batch. This is normal PostgreSQL B-tree insertion.'

DO $$
DECLARE
    batches int := 7;
    rows_per_batch int := 50000;
    b int;
    start_ts timestamptz;
    elapsed double precision;
BEGIN
    FOR b IN 0..batches-1 LOOP
        start_ts := clock_timestamp();
        INSERT INTO bench_btree_strong
        SELECT b * rows_per_batch + g,
               md5((b * rows_per_batch + g)::text)
        FROM generate_series(1, rows_per_batch) AS g;
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);

        INSERT INTO bench_results_strong
        VALUES ('btree', 'load_batch', elapsed, rows_per_batch, b + 1);
    END LOOP;
END $$;

ANALYZE bench_btree_strong;

\echo ''
\echo '============================================================'
\echo 'LOAD LSM WITHOUT BLOOM'
\echo '============================================================'
\echo 'Expected: after each batch, manual merge creates one new level0 run.'
\echo 'This intentionally creates many immutable runs to expose LSM read amplification.'

DO $$
DECLARE
    batches int := 7;
    rows_per_batch int := 50000;
    b int;
    start_ts timestamptz;
    elapsed double precision;
BEGIN
    FOR b IN 0..batches-1 LOOP
        start_ts := clock_timestamp();
        INSERT INTO bench_lsm_nobloom_strong
        SELECT b * rows_per_batch + g,
               md5((b * rows_per_batch + g)::text)
        FROM generate_series(1, rows_per_batch) AS g;
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);

        INSERT INTO bench_results_strong
        VALUES ('lsm-no-bloom', 'load_batch_insert_only', elapsed, rows_per_batch, b + 1);

        start_ts := clock_timestamp();
        PERFORM lsm3_start_merge('bench_lsm_nobloom_strong_idx'::regclass::oid);
        PERFORM lsm3_wait_merge_completion('bench_lsm_nobloom_strong_idx'::regclass::oid);
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);

        INSERT INTO bench_results_strong
        VALUES ('lsm-no-bloom', 'manual_compaction', elapsed, (b + 1) * rows_per_batch, b + 1);
    END LOOP;
END $$;

ANALYZE bench_lsm_nobloom_strong;

\echo ''
\echo '============================================================'
\echo 'LOAD LSM WITH BLOOM'
\echo '============================================================'
\echo 'Expected: same physical LSM shape as no-bloom, but immutable runs can be skipped for absent equality probes.'

DO $$
DECLARE
    batches int := 7;
    rows_per_batch int := 50000;
    b int;
    start_ts timestamptz;
    elapsed double precision;
BEGIN
    FOR b IN 0..batches-1 LOOP
        start_ts := clock_timestamp();
        INSERT INTO bench_lsm_bloom_strong
        SELECT b * rows_per_batch + g,
               md5((b * rows_per_batch + g)::text)
        FROM generate_series(1, rows_per_batch) AS g;
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);

        INSERT INTO bench_results_strong
        VALUES ('lsm-bloom', 'load_batch_insert_only', elapsed, rows_per_batch, b + 1);

        start_ts := clock_timestamp();
        PERFORM lsm3_start_merge('bench_lsm_bloom_strong_idx'::regclass::oid);
        PERFORM lsm3_wait_merge_completion('bench_lsm_bloom_strong_idx'::regclass::oid);
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);

        INSERT INTO bench_results_strong
        VALUES ('lsm-bloom', 'manual_compaction', elapsed, (b + 1) * rows_per_batch, b + 1);
    END LOOP;
END $$;

ANALYZE bench_lsm_bloom_strong;

\echo ''
\echo '============================================================'
\echo 'VERIFY LSM PHYSICAL LAYOUT'
\echo '============================================================'
\echo 'Expected for both LSM tables:'
\echo '  level0_run0 through level0_run6 should be larger than 8192 bytes.'
\echo '  level0_run7 should usually be 8192 bytes.'
\echo '  level1 runs and base should usually be 8192 bytes because runs_per_level=8 and only 7 runs were created.'

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'bench_lsm_nobloom_strong_idx%'
ORDER BY c.relname;

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'bench_lsm_bloom_strong_idx%'
ORDER BY c.relname;

\echo ''
\echo 'Structural pass/fail check for LSM Bloom layout'
\echo 'Expected: first seven level0 runs occupied; run7 empty; base empty.'

WITH sizes AS (
    SELECT c.relname, pg_relation_size(c.oid) AS bytes
    FROM pg_class c
    WHERE c.relname LIKE 'bench_lsm_bloom_strong_idx%'
)
SELECT
    CASE WHEN count(*) FILTER (WHERE relname ~ 'level0_run[0-6]$' AND bytes > 8192) = 7
         THEN 'PASS' ELSE 'FAIL' END AS seven_level0_runs_occupied,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_bloom_strong_idx_level0_run7') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS level0_run7_empty,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_bloom_strong_idx') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS base_empty;

\echo ''
\echo '============================================================'
\echo 'NEGATIVE EQUALITY PROBE BENCHMARK'
\echo '============================================================'
\echo 'Expected:'
\echo '  - btree negative loop should be fast because there is one B-tree.'
\echo '  - lsm-no-bloom negative loop should be slower because it searches many immutable runs.'
\echo '  - lsm-bloom warmup may be slower because it lazily builds Bloom filters.'
\echo '  - lsm-bloom cached loop should be faster than lsm-no-bloom because filters are reused.'

DO $$
DECLARE
    negative_loop int := 5000;
    i int;
    c bigint;
    total bigint;
    start_ts timestamptz;
    elapsed double precision;
BEGIN
    total := 0;
    start_ts := clock_timestamp();
    FOR i IN 1..negative_loop LOOP
        SELECT count(*) INTO c
        FROM bench_btree_strong
        WHERE id = 1000000000 + i;
        total := total + c;
    END LOOP;
    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);
    INSERT INTO bench_results_strong VALUES ('btree', 'negative_loop', elapsed, total, negative_loop);

    total := 0;
    start_ts := clock_timestamp();
    FOR i IN 1..negative_loop LOOP
        SELECT count(*) INTO c
        FROM bench_lsm_nobloom_strong
        WHERE id = 1000000000 + i;
        total := total + c;
    END LOOP;
    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);
    INSERT INTO bench_results_strong VALUES ('lsm-no-bloom', 'negative_loop', elapsed, total, negative_loop);

    /* Bloom warmup: this first absent-key loop builds backend-local Bloom filters. */
    total := 0;
    start_ts := clock_timestamp();
    FOR i IN 1..negative_loop LOOP
        SELECT count(*) INTO c
        FROM bench_lsm_bloom_strong
        WHERE id = 1100000000 + i;
        total := total + c;
    END LOOP;
    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);
    INSERT INTO bench_results_strong VALUES ('lsm-bloom', 'negative_loop_warmup_build_filters', elapsed, total, negative_loop);

    /* Cached Bloom loop: filters should now be built and reused. */
    total := 0;
    start_ts := clock_timestamp();
    FOR i IN 1..negative_loop LOOP
        SELECT count(*) INTO c
        FROM bench_lsm_bloom_strong
        WHERE id = 1200000000 + i;
        total := total + c;
    END LOOP;
    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);
    INSERT INTO bench_results_strong VALUES ('lsm-bloom', 'negative_loop_cached_1', elapsed, total, negative_loop);

    total := 0;
    start_ts := clock_timestamp();
    FOR i IN 1..negative_loop LOOP
        SELECT count(*) INTO c
        FROM bench_lsm_bloom_strong
        WHERE id = 1300000000 + i;
        total := total + c;
    END LOOP;
    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);
    INSERT INTO bench_results_strong VALUES ('lsm-bloom', 'negative_loop_cached_2', elapsed, total, negative_loop);
END $$;

\echo ''
\echo '============================================================'
\echo 'POSITIVE EQUALITY PROBE BENCHMARK'
\echo '============================================================'
\echo 'Expected:'
\echo '  Bloom may help slightly by skipping runs that do not contain the key, but positive probes still need to scan the run that contains the key.'
\echo '  Therefore Bloom speedup is expected to be much clearer on negative probes than positive probes.'

DO $$
DECLARE
    positive_loop int := 2000;
    total_rows int := 350000;
    i int;
    probe_id int;
    c bigint;
    total bigint;
    start_ts timestamptz;
    elapsed double precision;
BEGIN
    total := 0;
    start_ts := clock_timestamp();
    FOR i IN 1..positive_loop LOOP
        probe_id := ((i * 131) % total_rows) + 1;
        SELECT count(*) INTO c FROM bench_btree_strong WHERE id = probe_id;
        total := total + c;
    END LOOP;
    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);
    INSERT INTO bench_results_strong VALUES ('btree', 'positive_loop', elapsed, total, positive_loop);

    total := 0;
    start_ts := clock_timestamp();
    FOR i IN 1..positive_loop LOOP
        probe_id := ((i * 131) % total_rows) + 1;
        SELECT count(*) INTO c FROM bench_lsm_nobloom_strong WHERE id = probe_id;
        total := total + c;
    END LOOP;
    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);
    INSERT INTO bench_results_strong VALUES ('lsm-no-bloom', 'positive_loop', elapsed, total, positive_loop);

    total := 0;
    start_ts := clock_timestamp();
    FOR i IN 1..positive_loop LOOP
        probe_id := ((i * 131) % total_rows) + 1;
        SELECT count(*) INTO c FROM bench_lsm_bloom_strong WHERE id = probe_id;
        total := total + c;
    END LOOP;
    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);
    INSERT INTO bench_results_strong VALUES ('lsm-bloom', 'positive_loop', elapsed, total, positive_loop);
END $$;

\echo ''
\echo '============================================================'
\echo 'SUMMARY RESULTS'
\echo '============================================================'
\echo 'Interpretation guide:'
\echo '  Compare lsm-no-bloom negative_loop with lsm-bloom negative_loop_cached_1/2.'
\echo '  The cached Bloom loops are the cleanest Bloom speedup numbers.'
\echo '  Compare btree with LSM carefully: B-tree has one component; LSM has many runs and background/manual compaction.'

SELECT
    system_name,
    phase,
    avg(seconds)::numeric(12,6) AS avg_seconds,
    min(seconds)::numeric(12,6) AS min_seconds,
    max(seconds)::numeric(12,6) AS max_seconds,
    sum(total_result_count) AS total_result_count,
    count(*) AS timing_rows,
    max(runs) AS loop_or_batch_count
FROM bench_results_strong
GROUP BY system_name, phase
ORDER BY system_name, phase;

\echo ''
\echo 'Raw timing rows'
SELECT * FROM bench_results_strong ORDER BY system_name, phase, runs;

RESET synchronous_commit;
RESET enable_seqscan;
RESET client_min_messages;

\echo '============================================================'
\echo 'DONE: STRONGER BTREE vs LSM vs BLOOM BENCHMARK'
\echo '============================================================'