\echo '============================================================'
\echo 'FOREGROUND INSERT BENCHMARK: BTREE vs CURRENT MULTI-RUN LSM3'
\echo '============================================================'
\echo 'Purpose:'
\echo '  Test whether foreground insert batches into the LSM top component can be faster than inserting into a continuously growing PostgreSQL B-tree.'
\echo '  This benchmark deliberately separates foreground insert time from manual compaction time.'
\echo ''
\echo 'Important interpretation:'
\echo '  - LSM insert_only time excludes the following manual compaction/reset cost.'
\echo '  - The LSM top component is still a PostgreSQL B-tree, so native B-tree may still be faster.'
\echo '  - If LSM insert_only is faster, the valid claim is foreground top-component insertion is faster under this workload, not total end-to-end ingest.'
\echo '  - If LSM insert_only is not faster, the valid claim is that this prototype mainly defers/organizes compaction rather than beating native B-tree insert path.'
\echo ''
\echo 'Benchmark shape:'
\echo '  batches        = 7'
\echo '  rows_per_batch = 50000'
\echo '  total rows     = 350000'
\echo '  runs_per_level = 8, so 7 manual flushes fit in level0 without upward compaction.'
\echo '  IDs are pseudo-randomized to avoid the native B-tree getting only rightmost-page append behavior.'
\echo ''

\timing on

SET client_min_messages = warning;
SET synchronous_commit = off;
SET enable_seqscan = off;

DROP TABLE IF EXISTS bench_insert_btree_random CASCADE;
DROP TABLE IF EXISTS bench_insert_lsm_random CASCADE;
DROP TABLE IF EXISTS bench_insert_results_random;

CREATE TEMP TABLE bench_insert_results_random
(
    system_name text,
    phase text,
    seconds double precision,
    rows_done bigint,
    batch_no integer
);

\echo '============================================================'
\echo 'CREATE TABLES AND INDEXES'
\echo '============================================================'

CREATE TABLE bench_insert_btree_random
(
    id integer,
    payload text
);
CREATE INDEX bench_insert_btree_random_idx ON bench_insert_btree_random(id);

CREATE TABLE bench_insert_lsm_random
(
    id integer,
    payload text
);
CREATE INDEX bench_insert_lsm_random_idx
ON bench_insert_lsm_random
USING lsm3(id)
WITH
(
    num_levels = 2,
    runs_per_level = 8,
    level_size_ratio = 1000,
    top_index_size = 1048576,
    bloom_enabled = false
);

\echo ''
\echo '============================================================'
\echo 'LOAD BTREE'
\echo '============================================================'
\echo 'Expected: B-tree has one growing physical index. Randomized key order can cause more page-split/random-write behavior than ordered inserts.'

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
        INSERT INTO bench_insert_btree_random
        SELECT ((gid::bigint * 48271) % 2147483647)::integer AS id,
               md5(gid::text) AS payload
        FROM generate_series(b * rows_per_batch + 1, (b + 1) * rows_per_batch) AS gid;
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);

        INSERT INTO bench_insert_results_random
        VALUES ('btree', 'load_batch', elapsed, rows_per_batch, b + 1);
    END LOOP;
END $$;

ANALYZE bench_insert_btree_random;

\echo ''
\echo '============================================================'
\echo 'LOAD CURRENT MULTI-RUN LSM3'
\echo '============================================================'
\echo 'Expected: insert_only measures only foreground insertion into active top. manual_compaction_reset_top is measured separately.'
\echo 'After each batch we manually compact so the next insert batch starts with a small/empty top component.'

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
        INSERT INTO bench_insert_lsm_random
        SELECT ((gid::bigint * 48271) % 2147483647)::integer AS id,
               md5(gid::text) AS payload
        FROM generate_series(b * rows_per_batch + 1, (b + 1) * rows_per_batch) AS gid;
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);

        INSERT INTO bench_insert_results_random
        VALUES ('current-multirun-lsm3', 'load_batch_insert_only', elapsed, rows_per_batch, b + 1);

        start_ts := clock_timestamp();
        PERFORM lsm3_start_merge('bench_insert_lsm_random_idx'::regclass::oid);
        PERFORM lsm3_wait_merge_completion('bench_insert_lsm_random_idx'::regclass::oid);
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - start_ts);

        INSERT INTO bench_insert_results_random
        VALUES ('current-multirun-lsm3', 'manual_compaction_reset_top', elapsed, (b + 1) * rows_per_batch, b + 1);
    END LOOP;
END $$;

ANALYZE bench_insert_lsm_random;

\echo ''
\echo '============================================================'
\echo 'VERIFY CURRENT LSM PHYSICAL LAYOUT'
\echo '============================================================'
\echo 'Expected: level0_run0..level0_run6 should be occupied; level0_run7 should be tiny; top0/top1 should be tiny after last manual compaction; base should be tiny.'

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'bench_insert_lsm_random_idx%'
ORDER BY c.relname;

WITH sizes AS (
    SELECT c.relname, pg_relation_size(c.oid) AS bytes
    FROM pg_class c
    WHERE c.relname LIKE 'bench_insert_lsm_random_idx%'
)
SELECT
    CASE WHEN count(*) FILTER (
        WHERE relname ~ 'bench_insert_lsm_random_idx_level0_run[0-6]$'
          AND bytes > 8192
    ) = 7 THEN 'PASS' ELSE 'FAIL' END AS seven_level0_runs_occupied,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_insert_lsm_random_idx_level0_run7') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS run7_empty,
    CASE WHEN max(bytes) FILTER (WHERE relname = 'bench_insert_lsm_random_idx') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS base_empty
FROM sizes;

\echo ''
\echo '============================================================'
\echo 'SUMMARY RESULTS'
\echo '============================================================'
\echo 'Primary comparison: btree load_batch vs current-multirun-lsm3 load_batch_insert_only.'
\echo 'Do not include manual_compaction_reset_top if the claim is only foreground insert latency.'
\echo 'Include manual_compaction_reset_top for end-to-end ingest cost.'

SELECT
    system_name,
    phase,
    avg(seconds)::numeric(12,6) AS avg_seconds,
    min(seconds)::numeric(12,6) AS min_seconds,
    max(seconds)::numeric(12,6) AS max_seconds,
    sum(rows_done) AS total_rows_or_rows_done_sum,
    count(*) AS timing_rows
FROM bench_insert_results_random
GROUP BY system_name, phase
ORDER BY system_name, phase;

\echo ''
\echo 'Derived comparison: foreground insert-only ratio'
WITH b AS (
    SELECT avg(seconds) AS s
    FROM bench_insert_results_random
    WHERE system_name = 'btree' AND phase = 'load_batch'
), l AS (
    SELECT avg(seconds) AS s
    FROM bench_insert_results_random
    WHERE system_name = 'current-multirun-lsm3' AND phase = 'load_batch_insert_only'
)
SELECT
    b.s::numeric(12,6) AS btree_avg_insert_batch_seconds,
    l.s::numeric(12,6) AS lsm_avg_insert_only_batch_seconds,
    (l.s / b.s)::numeric(12,4) AS lsm_over_btree_ratio,
    CASE WHEN l.s < b.s
         THEN 'LSM foreground insert faster in this workload'
         ELSE 'B-tree foreground insert faster in this workload'
    END AS interpretation
FROM b, l;

\echo ''
\echo 'Raw timing rows'
SELECT * FROM bench_insert_results_random ORDER BY system_name, phase, batch_no;

RESET enable_seqscan;
RESET synchronous_commit;
RESET client_min_messages;

\echo '============================================================'
\echo 'DONE: FOREGROUND INSERT BENCHMARK'
\echo '============================================================'