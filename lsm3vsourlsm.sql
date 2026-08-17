\echo '============================================================'
\echo 'PARAMETER-ONLY COMPARISON: ORIGINAL-LIKE LSM vs MULTI-RUN LSM'
\echo '============================================================'
\echo 'Purpose:'
\echo '  Compare two configurations of the SAME current implementation.'
\echo '  No reinstall of original LSM3 is needed.'
\echo ''
\echo 'Mode A: original-like compatibility mode'
\echo '  num_levels = 1, runs_per_level = 1, level_size_ratio = 2'
\echo '  Expected behavior: each manual top flush cascades through level0_run0 into base.'
\echo '  Expected final layout: base large, level0_run0 tiny, top0/top1 tiny.'
\echo ''
\echo 'Mode B: improved multi-run mode'
\echo '  num_levels = 2, runs_per_level = 8, level_size_ratio = 1000'
\echo '  Expected behavior: each manual top flush creates a separate level0 run; no upward compaction after only 7 runs.'
\echo '  Expected final layout: level0_run0..level0_run6 large, level0_run7 tiny, base tiny, level1 tiny.'
\echo ''
\echo 'Benchmark shape:'
\echo '  batches        = 7'
\echo '  rows_per_batch = 50000'
\echo '  total rows     = 350000 per table'
\echo '  IDs are pseudo-randomized but deterministic.'
\echo '  top_index_size = 1048576 KB, intentionally huge, so only lsm3_start_merge() causes flushes.'
\echo ''
\echo 'Important fix in this version:'
\echo '  The pseudo-random ORDER BY expression casts to bigint to avoid integer overflow.'
\echo '============================================================'

\set ON_ERROR_STOP on
\timing on

SET client_min_messages = notice;
SET enable_seqscan = off;
SET synchronous_commit = off;

\echo ''
\echo '============================================================'
\echo 'ROBUST CLEANUP OF OLD BENCHMARK OBJECTS'
\echo '============================================================'
\echo 'Expected: no error even if previous failed runs left auxiliary index relations behind.'

DO $$
DECLARE
    r record;
BEGIN
    -- Drop benchmark tables first. CASCADE should remove dependent indexes.
    FOR r IN
        SELECT n.nspname, c.relname
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE c.relkind IN ('r', 'p')
          AND n.nspname = current_schema()
          AND (c.relname LIKE 'bench_lsm_compat%'
               OR c.relname LIKE 'bench_lsm_multirun%')
    LOOP
        EXECUTE format('DROP TABLE IF EXISTS %I.%I CASCADE', r.nspname, r.relname);
    END LOOP;

    -- Drop any leftover indexes/auxiliary indexes explicitly.
    FOR r IN
        SELECT n.nspname, c.relname
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE c.relkind IN ('i', 'I')
          AND n.nspname = current_schema()
          AND (c.relname LIKE 'bench_lsm_compat_idx%'
               OR c.relname LIKE 'bench_lsm_multirun_idx%')
    LOOP
        EXECUTE format('DROP INDEX IF EXISTS %I.%I CASCADE', r.nspname, r.relname);
    END LOOP;

    -- Drop stale sequences, if any were accidentally left by local edits.
    FOR r IN
        SELECT n.nspname, c.relname
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE c.relkind = 'S'
          AND n.nspname = current_schema()
          AND (c.relname LIKE 'bench_lsm_compat%'
               OR c.relname LIKE 'bench_lsm_multirun%')
    LOOP
        EXECUTE format('DROP SEQUENCE IF EXISTS %I.%I CASCADE', r.nspname, r.relname);
    END LOOP;
END $$;

DROP TABLE IF EXISTS bench_param_timing;
CREATE TEMP TABLE bench_param_timing
(
    system_name text,
    phase text,
    seconds double precision,
    rows_done bigint,
    batch_no integer
);

\echo ''
\echo '============================================================'
\echo 'CREATE TABLES AND INDEXES'
\echo '============================================================'

CREATE TABLE bench_lsm_compat
(
    id integer,
    payload text
);

\echo 'Creating original-like LSM index'
\echo 'Expected: CREATE INDEX succeeds. Large top_index_size prevents automatic merges; runs_per_level=1 makes every manual flush cascade to base.'
CREATE INDEX bench_lsm_compat_idx
ON bench_lsm_compat
USING lsm3(id)
WITH (
    num_levels = 1,
    runs_per_level = 1,
    level_size_ratio = 2,
    top_index_size = 1048576,
    bloom_enabled = false
);

CREATE TABLE bench_lsm_multirun
(
    id integer,
    payload text
);

\echo 'Creating improved multi-run LSM index'
\echo 'Expected: CREATE INDEX succeeds. Seven manual flushes become seven independent level0 runs.'
CREATE INDEX bench_lsm_multirun_idx
ON bench_lsm_multirun
USING lsm3(id)
WITH (
    num_levels = 2,
    runs_per_level = 8,
    level_size_ratio = 1000,
    top_index_size = 1048576,
    bloom_enabled = false
);

\echo ''
\echo '============================================================'
\echo 'LOAD ORIGINAL-LIKE MODE'
\echo '============================================================'
\echo 'Expected: after each batch, manual merge pushes data to base, leaving level0_run0 tiny.'

DO $$
DECLARE
    b integer;
    start_id integer;
    t0 timestamptz;
    t1 timestamptz;
    rows_per_batch integer := 50000;
BEGIN
    FOR b IN 1..7 LOOP
        start_id := (b - 1) * rows_per_batch + 1;

        t0 := clock_timestamp();
        INSERT INTO bench_lsm_compat
        SELECT
            start_id + g - 1 AS id,
            md5((start_id + g - 1)::text) AS payload
        FROM generate_series(1, rows_per_batch) AS g
        -- IMPORTANT: cast to bigint before multiplication to avoid integer overflow.
        ORDER BY (((start_id + g - 1)::bigint * 1103515245 + 12345) % 2147483647);
        t1 := clock_timestamp();

        INSERT INTO bench_param_timing
        VALUES ('original-like', 'insert_only', EXTRACT(EPOCH FROM (t1 - t0)), rows_per_batch, b);

        t0 := clock_timestamp();
        PERFORM lsm3_start_merge('bench_lsm_compat_idx'::regclass::oid);
        PERFORM lsm3_wait_merge_completion('bench_lsm_compat_idx'::regclass::oid);
        t1 := clock_timestamp();

        INSERT INTO bench_param_timing
        VALUES ('original-like', 'manual_compaction', EXTRACT(EPOCH FROM (t1 - t0)), b * rows_per_batch, b);
    END LOOP;
END $$;

ANALYZE bench_lsm_compat;

\echo ''
\echo '============================================================'
\echo 'LOAD IMPROVED MULTI-RUN MODE'
\echo '============================================================'
\echo 'Expected: after each batch, manual merge creates a new level0 run; base remains tiny.'

DO $$
DECLARE
    b integer;
    start_id integer;
    t0 timestamptz;
    t1 timestamptz;
    rows_per_batch integer := 50000;
BEGIN
    FOR b IN 1..7 LOOP
        start_id := (b - 1) * rows_per_batch + 1;

        t0 := clock_timestamp();
        INSERT INTO bench_lsm_multirun
        SELECT
            start_id + g - 1 AS id,
            md5((start_id + g - 1)::text) AS payload
        FROM generate_series(1, rows_per_batch) AS g
        -- IMPORTANT: cast to bigint before multiplication to avoid integer overflow.
        ORDER BY (((start_id + g - 1)::bigint * 1103515245 + 12345) % 2147483647);
        t1 := clock_timestamp();

        INSERT INTO bench_param_timing
        VALUES ('multi-run', 'insert_only', EXTRACT(EPOCH FROM (t1 - t0)), rows_per_batch, b);

        t0 := clock_timestamp();
        PERFORM lsm3_start_merge('bench_lsm_multirun_idx'::regclass::oid);
        PERFORM lsm3_wait_merge_completion('bench_lsm_multirun_idx'::regclass::oid);
        t1 := clock_timestamp();

        INSERT INTO bench_param_timing
        VALUES ('multi-run', 'manual_compaction', EXTRACT(EPOCH FROM (t1 - t0)), b * rows_per_batch, b);
    END LOOP;
END $$;

ANALYZE bench_lsm_multirun;

\echo ''
\echo '============================================================'
\echo 'VERIFY PHYSICAL LAYOUT: ORIGINAL-LIKE MODE'
\echo '============================================================'
\echo 'Expected:'
\echo '  bench_lsm_compat_idx             > 8192 bytes  (base received data)'
\echo '  bench_lsm_compat_idx_level0_run0 <= 16384 bytes after truncation'
\echo '  bench_lsm_compat_idx_top0/top1   <= 16384 bytes after merge'

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = current_schema()
  AND c.relname LIKE 'bench_lsm_compat_idx%'
ORDER BY c.relname;

WITH sizes AS (
    SELECT c.relname, pg_relation_size(c.oid) AS bytes
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = current_schema()
      AND c.relname LIKE 'bench_lsm_compat_idx%'
)
SELECT
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_compat_idx') > 8192
         THEN 'PASS' ELSE 'FAIL' END AS base_received_data,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_compat_idx_level0_run0') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS level0_run0_truncated,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_compat_idx_top0') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS top0_tiny,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_compat_idx_top1') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS top1_tiny
FROM sizes;

\echo ''
\echo '============================================================'
\echo 'VERIFY PHYSICAL LAYOUT: IMPROVED MULTI-RUN MODE'
\echo '============================================================'
\echo 'Expected:'
\echo '  bench_lsm_multirun_idx_level0_run0..run6 > 8192 bytes'
\echo '  bench_lsm_multirun_idx_level0_run7       <= 16384 bytes'
\echo '  bench_lsm_multirun_idx                   <= 16384 bytes (base deferred)'
\echo '  level1 runs                              <= 16384 bytes'

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = current_schema()
  AND c.relname LIKE 'bench_lsm_multirun_idx%'
ORDER BY c.relname;

WITH sizes AS (
    SELECT c.relname, pg_relation_size(c.oid) AS bytes
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = current_schema()
      AND c.relname LIKE 'bench_lsm_multirun_idx%'
)
SELECT
    CASE WHEN count(*) FILTER (
        WHERE relname ~ 'bench_lsm_multirun_idx_level0_run[0-6]$'
          AND bytes > 8192
    ) = 7 THEN 'PASS' ELSE 'FAIL' END AS seven_level0_runs_occupied,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_multirun_idx_level0_run7') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS run7_empty,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_multirun_idx') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS base_deferred,
    CASE WHEN count(*) FILTER (
        WHERE relname ~ 'bench_lsm_multirun_idx_level1_run[0-7]$'
          AND bytes <= 16384
    ) = 8 THEN 'PASS' ELSE 'FAIL' END AS level1_empty,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_multirun_idx_top0') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS top0_tiny,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_lsm_multirun_idx_top1') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS top1_tiny
FROM sizes;

\echo ''
\echo '============================================================'
\echo 'CORRECTNESS CHECKS'
\echo '============================================================'
\echo 'Expected: both tables have 350000 rows and sample point lookups return expected rows.'

SELECT 'original-like' AS system_name, count(*) AS expected_350000
FROM bench_lsm_compat;

SELECT 'multi-run' AS system_name, count(*) AS expected_350000
FROM bench_lsm_multirun;

SELECT 'original-like' AS system_name, count(*) AS expected_3
FROM bench_lsm_compat
WHERE id IN (1, 175000, 350000);

SELECT 'multi-run' AS system_name, count(*) AS expected_3
FROM bench_lsm_multirun
WHERE id IN (1, 175000, 350000);

\echo ''
\echo '============================================================'
\echo 'SUMMARY RESULTS'
\echo '============================================================'
\echo 'Interpretation:'
\echo '  original-like approximates old LSM3 behavior: flushes quickly reach base.'
\echo '  multi-run demonstrates new behavior: flushes become independent level0 runs and base is deferred.'
\echo '  Timing includes lsm3_wait_merge_completion polling, so manual_compaction is observed end-to-end latency, not pure merge CPU time.'

SELECT
    system_name,
    phase,
    round(avg(seconds)::numeric, 6) AS avg_seconds,
    round(min(seconds)::numeric, 6) AS min_seconds,
    round(max(seconds)::numeric, 6) AS max_seconds,
    sum(rows_done) AS total_rows_or_progress_sum,
    count(*) AS timing_rows
FROM bench_param_timing
GROUP BY system_name, phase
ORDER BY system_name, phase;

\echo ''
\echo 'Derived comparison: insert-only and manual-compaction ratios'

WITH s AS (
    SELECT system_name, phase, avg(seconds) AS avg_sec
    FROM bench_param_timing
    GROUP BY system_name, phase
)
SELECT
    round((SELECT avg_sec FROM s WHERE system_name = 'original-like' AND phase = 'insert_only')::numeric, 6) AS original_insert_avg,
    round((SELECT avg_sec FROM s WHERE system_name = 'multi-run' AND phase = 'insert_only')::numeric, 6) AS multirun_insert_avg,
    round(((SELECT avg_sec FROM s WHERE system_name = 'multi-run' AND phase = 'insert_only') /
           NULLIF((SELECT avg_sec FROM s WHERE system_name = 'original-like' AND phase = 'insert_only'), 0))::numeric, 4) AS multirun_insert_over_original,
    round((SELECT avg_sec FROM s WHERE system_name = 'original-like' AND phase = 'manual_compaction')::numeric, 6) AS original_compaction_avg,
    round((SELECT avg_sec FROM s WHERE system_name = 'multi-run' AND phase = 'manual_compaction')::numeric, 6) AS multirun_compaction_avg,
    round(((SELECT avg_sec FROM s WHERE system_name = 'multi-run' AND phase = 'manual_compaction') /
           NULLIF((SELECT avg_sec FROM s WHERE system_name = 'original-like' AND phase = 'manual_compaction'), 0))::numeric, 4) AS multirun_compaction_over_original;

\echo ''
\echo 'Physical-size summary: base vs level0 storage'
\echo 'Expected: original-like stores data mostly in base; multi-run stores data mostly in level0 runs.'

WITH sizes AS (
    SELECT c.relname, pg_relation_size(c.oid) AS bytes
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = current_schema()
      AND (c.relname LIKE 'bench_lsm_compat_idx%'
           OR c.relname LIKE 'bench_lsm_multirun_idx%')
)
SELECT
    'original-like' AS system_name,
    max(bytes) FILTER (WHERE relname = 'bench_lsm_compat_idx') AS base_bytes,
    sum(bytes) FILTER (WHERE relname LIKE 'bench_lsm_compat_idx_level0_run%') AS level0_total_bytes
FROM sizes
UNION ALL
SELECT
    'multi-run' AS system_name,
    max(bytes) FILTER (WHERE relname = 'bench_lsm_multirun_idx') AS base_bytes,
    sum(bytes) FILTER (WHERE relname LIKE 'bench_lsm_multirun_idx_level0_run%') AS level0_total_bytes
FROM sizes;

\echo ''
\echo 'Raw timing rows'
SELECT * FROM bench_param_timing ORDER BY system_name, phase, batch_no;

RESET synchronous_commit;
RESET enable_seqscan;
RESET client_min_messages;

\echo '============================================================'
\echo 'DONE: PARAMETER-ONLY ORIGINAL-LIKE vs MULTI-RUN TEST'
\echo '============================================================'