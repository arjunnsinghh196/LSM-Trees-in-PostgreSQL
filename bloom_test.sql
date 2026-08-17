\echo '============================================================'
\echo 'LSM3 BLOOM FILTER TEST'
\echo '============================================================'
\echo 'This test uses the previous multi-run LSM implementation plus Bloom filters.'
\echo 'Bloom filters are backend-local and are used only for equality lookups on occupied immutable level runs.'
\echo 'Expected: negative equality lookup prints DEBUG1 bloom-skip messages and returns 0 rows.'

DROP TABLE IF EXISTS lsm3_bloom_test CASCADE;

CREATE TABLE lsm3_bloom_test
(
    id integer,
    payload text
);

\echo ''
\echo 'Step 1: create LSM3 index with Bloom enabled'
\echo 'Expected: CREATE INDEX succeeds; bloom_enabled is accepted as a reloption.'

CREATE INDEX lsm3_bloom_idx
ON lsm3_bloom_test
USING lsm3(id)
WITH (
    num_levels = 2,
    runs_per_level = 4,
    level_size_ratio = 100,
    top_index_size = 1024,
    bloom_enabled = true
);

\echo ''
\echo 'Step 2: insert data and manually compact top -> level0_run0'
\echo 'Expected: level0_run0 becomes non-empty; top indexes become tiny after merge.'

INSERT INTO lsm3_bloom_test
SELECT g, md5(g::text)
FROM generate_series(1, 20000) AS g;

SELECT lsm3_start_merge('lsm3_bloom_idx'::regclass::oid);
SELECT lsm3_wait_merge_completion('lsm3_bloom_idx'::regclass::oid);

ANALYZE lsm3_bloom_test;

\echo ''
\echo 'Step 3: show physical component sizes'
\echo 'Expected: lsm3_bloom_idx_level0_run0 > 8192 bytes; other level runs are usually 8192 bytes.'

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'lsm3_bloom_idx%'
ORDER BY c.relname;

\echo ''
\echo 'Step 4: enable DEBUG1 logs so Bloom skip messages are visible'
\echo 'Expected during the negative lookup: DEBUG line like:'
\echo '  Lsm3 bloom injection: skipping lsm3_bloom_idx_level0_run0 for absent equality key'

SET enable_seqscan = off;
SET client_min_messages = debug1;

\echo ''
\echo 'Step 5: negative equality lookup'
\echo 'Expected: count = 0 and at least one Bloom skip DEBUG message for occupied level0_run0.'

SELECT count(*) AS expected_zero
FROM lsm3_bloom_test
WHERE id = 999999;

\echo ''
\echo 'Step 6: positive equality lookup'
\echo 'Expected: exactly one row; Bloom must not skip the component containing the key.'

SELECT *
FROM lsm3_bloom_test
WHERE id = 10;

\echo ''
\echo 'Step 7: range query correctness'
\echo 'Expected: count = 101. Bloom is not used for range predicates.'

SELECT count(*) AS expected_101
FROM lsm3_bloom_test
WHERE id BETWEEN 1000 AND 1100;

RESET client_min_messages;
RESET enable_seqscan;


SET enable_seqscan = off;
SET client_min_messages = debug1;

EXPLAIN ANALYZE
SELECT count(*)
FROM lsm3_bloom_test
WHERE id = 999999;

EXPLAIN ANALYZE
SELECT *
FROM lsm3_bloom_test
WHERE id = 999999;

SELECT count(*)
FROM lsm3_bloom_test
WHERE id IN (900001, 900002, 900003, 900004, 900005,
             900006, 900007, 900008, 900009, 900010);

RESET client_min_messages;
RESET enable_seqscan;

\echo '============================================================'
\echo 'DONE'
\echo '============================================================'