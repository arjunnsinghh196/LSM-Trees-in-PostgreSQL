\echo '============================================================'
\echo 'LSM3 MULTI-RUN LEVELS TEST'
\echo '============================================================'
\echo 'This test verifies multiple immutable B-tree runs inside each LSM level.'
\echo 'It uses manual lsm3_start_merge() only as a deterministic compaction trigger.'
\echo ''
\echo 'Expected summary:'
\echo '  TEST A: level0_run0, level0_run1, level0_run2 receive data; deeper levels/base stay tiny.'
\echo '  TEST B: with runs_per_level = 2, two level0 runs compact into one level1 run.'
\echo '============================================================'

DROP TABLE IF EXISTS lsm3_multirun_accum_test CASCADE;
DROP TABLE IF EXISTS lsm3_multirun_compact_test CASCADE;

-- =====================================================================
-- TEST A: Accumulate multiple runs in the same level
-- =====================================================================
-- Expected physical state after three manual top compactions:
--   lsm3_multirun_accum_idx_level0_run0 > 8192 bytes
--   lsm3_multirun_accum_idx_level0_run1 > 8192 bytes
--   lsm3_multirun_accum_idx_level0_run2 > 8192 bytes
--   lsm3_multirun_accum_idx_level0_run3 <= 16384 bytes, effectively empty
--   all level1/level2 runs <= 16384 bytes, effectively empty
--   base index <= 16384 bytes, effectively empty
--
-- Logical correctness expected:
--   id = 10 returns one row
--   id = 29999 returns one row
--   count(id BETWEEN 1000 AND 1100) = 101
--   count(*) = 30000
-- =====================================================================

\echo ''
\echo 'TEST A: accumulate multiple runs in the same level'
\echo 'Expected: level0_run0, level0_run1, and level0_run2 receive data; later levels/base stay tiny.'

CREATE TABLE lsm3_multirun_accum_test
(
    id integer,
    payload text
);

CREATE INDEX lsm3_multirun_accum_idx
ON lsm3_multirun_accum_test
USING lsm3(id)
WITH (
    num_levels = 3,
    runs_per_level = 4,
    level_size_ratio = 100,
    top_index_size = 32
);

-- First batch -> top -> level0_run0
INSERT INTO lsm3_multirun_accum_test
SELECT g, md5(g::text)
FROM generate_series(1, 10000) AS g;

SELECT lsm3_start_merge('lsm3_multirun_accum_idx'::regclass::oid);
SELECT lsm3_wait_merge_completion('lsm3_multirun_accum_idx'::regclass::oid);

-- Second batch -> top -> level0_run1
INSERT INTO lsm3_multirun_accum_test
SELECT g, md5(g::text)
FROM generate_series(10001, 20000) AS g;

SELECT lsm3_start_merge('lsm3_multirun_accum_idx'::regclass::oid);
SELECT lsm3_wait_merge_completion('lsm3_multirun_accum_idx'::regclass::oid);

-- Third batch -> top -> level0_run2
INSERT INTO lsm3_multirun_accum_test
SELECT g, md5(g::text)
FROM generate_series(20001, 30000) AS g;

SELECT lsm3_start_merge('lsm3_multirun_accum_idx'::regclass::oid);
SELECT lsm3_wait_merge_completion('lsm3_multirun_accum_idx'::regclass::oid);

ANALYZE lsm3_multirun_accum_test;

SET enable_seqscan = off;

\echo ''
\echo 'TEST A correctness checks'
\echo 'Expected: point lookups return one row; range count = 101; total count = 30000.'

SELECT * FROM lsm3_multirun_accum_test WHERE id = 10;
SELECT * FROM lsm3_multirun_accum_test WHERE id = 29999;

SELECT count(*) AS expected_101
FROM lsm3_multirun_accum_test
WHERE id BETWEEN 1000 AND 1100;

SELECT count(*) AS expected_30000
FROM lsm3_multirun_accum_test;

\echo ''
\echo 'TEST A physical component sizes'
\echo 'Expected: level0_run0, level0_run1, level0_run2 > 8192 bytes. level0_run3 and deeper runs should usually be 8192 bytes.'

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'lsm3_multirun_accum_idx%'
ORDER BY c.relname;

INSERT INTO lsm3_multirun_accum_test
SELECT g, md5(g::text)
FROM generate_series(400000,400005 ) AS g;

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'lsm3_multirun_accum_idx%'
ORDER BY c.relname;

\echo ''
\echo 'TEST A pass/fail structural check'
\echo 'Expected: all columns should be PASS.'

WITH sizes AS (
    SELECT
        c.relname,
        pg_relation_size(c.oid) AS bytes
    FROM pg_class c
    WHERE c.relname LIKE 'lsm3_multirun_accum_idx%'
)
SELECT
    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_accum_idx_level0_run0') > 8192
         THEN 'PASS' ELSE 'FAIL' END AS run0_received_data,

    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_accum_idx_level0_run1') > 8192
         THEN 'PASS' ELSE 'FAIL' END AS run1_received_data,

    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_accum_idx_level0_run2') > 8192
         THEN 'PASS' ELSE 'FAIL' END AS run2_received_data,

    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_accum_idx_level0_run3') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS run3_empty,

    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_accum_idx') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS base_empty,

    CASE WHEN coalesce(max(bytes) FILTER (WHERE relname LIKE 'lsm3_multirun_accum_idx_level1_run%'), 0) <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS level1_empty,

    CASE WHEN coalesce(max(bytes) FILTER (WHERE relname LIKE 'lsm3_multirun_accum_idx_level2_run%'), 0) <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS level2_empty
FROM sizes;

SELECT lsm3_get_merge_count('lsm3_multirun_accum_idx'::regclass::oid) AS test_a_merge_count;

RESET enable_seqscan;

-- =====================================================================
-- TEST B: Run-count-triggered tiered compaction
-- =====================================================================
-- Expected physical state after two manual top compactions with runs_per_level = 2:
--   first merge:  top -> level0_run0
--   second merge: top -> level0_run1
--   level0 now has 2 runs, so it compacts upward:
--       level0_run0 + level0_run1 -> level1_run0
--       level0_run0 and level0_run1 are truncated
--
-- Expected final physical state:
--   level0_run0 <= 16384 bytes, effectively empty
--   level0_run1 <= 16384 bytes, effectively empty
--   level1_run0 > 8192 bytes
--   level1_run1 <= 16384 bytes, effectively empty
--   level2 runs <= 16384 bytes, effectively empty
--   base index <= 16384 bytes, effectively empty
--
-- Logical correctness expected:
--   id = 10 returns one row
--   id = 15999 returns one row
--   count(id BETWEEN 1000 AND 1100) = 101
--   count(*) = 16000
-- =====================================================================

\echo ''
\echo 'TEST B: run-count-triggered tiered compaction'
\echo 'Expected: with runs_per_level = 2, two level0 runs compact into one level1 run.'

CREATE TABLE lsm3_multirun_compact_test
(
    id integer,
    payload text
);

CREATE INDEX lsm3_multirun_compact_idx
ON lsm3_multirun_compact_test
USING lsm3(id)
WITH (
    num_levels = 3,
    runs_per_level = 2,
    level_size_ratio = 10,
    top_index_size = 32
);

-- First batch -> top -> level0_run0
INSERT INTO lsm3_multirun_compact_test
SELECT g, md5(g::text)
FROM generate_series(1, 8000) AS g;
INSERT INTO lsm3_multirun_compact_test
SELECT g, md5(g::text)
FROM generate_series(800001, 1600000) AS g;
INSERT INTO lsm3_multirun_compact_test
SELECT g, md5(g::text)
FROM generate_series(8100001, 16100000) AS g;

SELECT lsm3_start_merge('lsm3_multirun_compact_idx'::regclass::oid);
SELECT lsm3_wait_merge_completion('lsm3_multirun_compact_idx'::regclass::oid);

-- Second batch -> top -> level0_run1, then level0 has two runs and compacts to level1_run0
INSERT INTO lsm3_multirun_compact_test
SELECT g, md5(g::text)
FROM generate_series(8001, 16000) AS g;

INSERT INTO lsm3_multirun_compact_test
SELECT g, md5(g::text)
FROM generate_series(800001, 1600000) AS g;

SELECT lsm3_start_merge('lsm3_multirun_compact_idx'::regclass::oid);
SELECT lsm3_wait_merge_completion('lsm3_multirun_compact_idx'::regclass::oid);

ANALYZE lsm3_multirun_compact_test;

SET enable_seqscan = off;

\echo ''
\echo 'TEST B correctness checks'
\echo 'Expected: point lookups return one row; range count = 101; total count = 16000.'

SELECT * FROM lsm3_multirun_compact_test WHERE id = 10;
SELECT * FROM lsm3_multirun_compact_test WHERE id = 15999;

SELECT count(*) AS expected_101
FROM lsm3_multirun_compact_test
WHERE id BETWEEN 1000 AND 1100;

SELECT count(*) AS expected_16000
FROM lsm3_multirun_compact_test;

\echo ''
\echo 'TEST B physical component sizes'
\echo 'Expected: level0_run0 and level0_run1 tiny after truncation; level1_run0 > 8192 bytes; base remains tiny.'

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'lsm3_multirun_compact_idx%'
ORDER BY c.relname;

\echo ''
\echo 'TEST B pass/fail structural check'
\echo 'Expected: all columns should be PASS.'

WITH sizes AS (
    SELECT
        c.relname,
        pg_relation_size(c.oid) AS bytes
    FROM pg_class c
    WHERE c.relname LIKE 'lsm3_multirun_compact_idx%'
)
SELECT
    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_compact_idx_level0_run0') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS level0_run0_truncated,

    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_compact_idx_level0_run1') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS level0_run1_truncated,

    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_compact_idx_level1_run0') > 8192
         THEN 'PASS' ELSE 'FAIL' END AS level1_run0_received_data,

    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_compact_idx_level1_run1') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS level1_run1_empty,

    CASE WHEN coalesce(max(bytes) FILTER (WHERE relname LIKE 'lsm3_multirun_compact_idx_level2_run%'), 0) <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS level2_empty,

    CASE WHEN max(bytes) FILTER (WHERE relname = 'lsm3_multirun_compact_idx') <= 16384
         THEN 'PASS' ELSE 'FAIL' END AS base_empty
FROM sizes;

SELECT lsm3_get_merge_count('lsm3_multirun_compact_idx'::regclass::oid) AS test_b_merge_count;

RESET enable_seqscan;

\echo ''
\echo '============================================================'
\echo 'DONE'
\echo 'Expected final summary:'
\echo '  TEST A should show three non-empty level0 runs and PASS in all structural columns.'
\echo '  TEST B should show empty level0 runs, non-empty level1_run0, and PASS in all structural columns.'
\echo '============================================================'