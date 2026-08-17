\echo '============================================================'
\echo 'LSM3 MULTI-LEVEL COMPACTION TEST'
\echo '============================================================'
\echo 'This test uses manual lsm3_start_merge() so compaction is deterministic.'
\echo 'It verifies: top -> level0, cascading level -> next level/base, and query correctness.'

DROP TABLE IF EXISTS lsm3_level_hold_test CASCADE;
DROP TABLE IF EXISTS lsm3_level_cascade_test CASCADE;

-- ============================================================
-- TEST A: top -> level0, no cascade beyond level0
-- ============================================================

\echo ''
\echo 'TEST A: top -> level0 without cascading further'
\echo 'Expected: level0 becomes larger than one empty page; level1, level2, and base stay tiny.'

CREATE TABLE lsm3_level_hold_test
(
    id integer,
    payload text
);

CREATE INDEX lsm3_level_hold_idx
ON lsm3_level_hold_test
USING lsm3(id)
WITH (
    num_levels = 3,
    level_size_ratio = 100,
    top_index_size = 32
);

INSERT INTO lsm3_level_hold_test
SELECT g, md5(g::text)
FROM generate_series(1, 20000) AS g;

-- Trigger manual compaction: old active top should be merged into level0.
SELECT lsm3_start_merge('lsm3_level_hold_idx'::regclass::oid);
SELECT lsm3_wait_merge_completion('lsm3_level_hold_idx'::regclass::oid);

ANALYZE lsm3_level_hold_test;
SET enable_seqscan = off;

\echo ''
\echo 'TEST A correctness checks'
\echo 'Expected: both point queries return exactly one row; range count = 101.'

SELECT * FROM lsm3_level_hold_test WHERE id = 10;
SELECT * FROM lsm3_level_hold_test WHERE id = 19999;
SELECT count(*) AS expected_101
FROM lsm3_level_hold_test
WHERE id BETWEEN 1000 AND 1100;

\echo ''
\echo 'TEST A physical component sizes'
\echo 'Expected: lsm3_level_hold_idx_level0 > 8192 bytes. level1, level2, base should usually be 8192 bytes.'

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'lsm3_level_hold_idx%'
ORDER BY c.relname;

\echo ''
\echo 'TEST A pass/fail structural check'

WITH sizes AS (
    SELECT
        pg_relation_size('lsm3_level_hold_idx'::regclass) AS base_bytes,
        pg_relation_size('lsm3_level_hold_idx_level0'::regclass) AS level0_bytes,
        pg_relation_size('lsm3_level_hold_idx_level1'::regclass) AS level1_bytes,
        pg_relation_size('lsm3_level_hold_idx_level2'::regclass) AS level2_bytes
)
SELECT
    CASE WHEN level0_bytes > 8192 THEN 'PASS' ELSE 'FAIL' END AS level0_received_data,
    CASE WHEN level1_bytes = 8192 THEN 'PASS' ELSE 'CHECK' END AS level1_not_used_yet,
    CASE WHEN level2_bytes = 8192 THEN 'PASS' ELSE 'CHECK' END AS level2_not_used_yet,
    CASE WHEN base_bytes = 8192 THEN 'PASS' ELSE 'CHECK' END AS base_not_used_yet
FROM sizes;

SELECT lsm3_get_merge_count('lsm3_level_hold_idx'::regclass::oid) AS test_a_merge_count;

RESET enable_seqscan;

-- ============================================================
-- TEST B: top -> level0 -> level1 -> level2 -> base
-- ============================================================

\echo ''
\echo 'TEST B: full cascading compaction to base'
\echo 'Expected: low thresholds force level0, level1, and level2 to compact upward; base becomes non-empty.'

CREATE TABLE lsm3_level_cascade_test
(
    id integer,
    payload text
);

CREATE INDEX lsm3_level_cascade_idx
ON lsm3_level_cascade_test
USING lsm3(id)
WITH (
    num_levels = 3,
    level_size_ratio = 2,
    top_index_size = 32
);

INSERT INTO lsm3_level_cascade_test
SELECT g, md5(g::text)
FROM generate_series(1, 60000) AS g;

-- Trigger manual compaction. With small thresholds, level0 should cascade all the way to base.
SELECT lsm3_start_merge('lsm3_level_cascade_idx'::regclass::oid);
SELECT lsm3_wait_merge_completion('lsm3_level_cascade_idx'::regclass::oid);

ANALYZE lsm3_level_cascade_test;
SET enable_seqscan = off;

\echo ''
\echo 'TEST B correctness checks'
\echo 'Expected: both point queries return exactly one row; range count = 101.'

SELECT * FROM lsm3_level_cascade_test WHERE id = 10;
SELECT * FROM lsm3_level_cascade_test WHERE id = 59999;
SELECT count(*) AS expected_101
FROM lsm3_level_cascade_test
WHERE id BETWEEN 1000 AND 1100;

\echo ''
\echo 'TEST B physical component sizes'
\echo 'Expected: base index > 8192 bytes. level0, level1, level2 should usually be back to 8192 bytes after truncation.'

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'lsm3_level_cascade_idx%'
ORDER BY c.relname;

\echo ''
\echo 'TEST B pass/fail structural check'

WITH sizes AS (
    SELECT
        pg_relation_size('lsm3_level_cascade_idx'::regclass) AS base_bytes,
        pg_relation_size('lsm3_level_cascade_idx_level0'::regclass) AS level0_bytes,
        pg_relation_size('lsm3_level_cascade_idx_level1'::regclass) AS level1_bytes,
        pg_relation_size('lsm3_level_cascade_idx_level2'::regclass) AS level2_bytes
)
SELECT
    CASE WHEN base_bytes > 8192 THEN 'PASS' ELSE 'FAIL' END AS base_received_data,
    CASE WHEN level0_bytes = 8192 THEN 'PASS' ELSE 'CHECK' END AS level0_truncated,
    CASE WHEN level1_bytes = 8192 THEN 'PASS' ELSE 'CHECK' END AS level1_truncated,
    CASE WHEN level2_bytes = 8192 THEN 'PASS' ELSE 'CHECK' END AS level2_truncated
FROM sizes;

SELECT lsm3_get_merge_count('lsm3_level_cascade_idx'::regclass::oid) AS test_b_merge_count;

RESET enable_seqscan;

\echo ''
\echo '============================================================'
\echo 'DONE'
\echo '============================================================'