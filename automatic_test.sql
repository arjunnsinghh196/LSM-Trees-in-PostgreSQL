\echo '============================================================'
\echo 'AUTOMATIC LSM3 MULTI-LEVEL TEST'
\echo '============================================================'

DROP TABLE IF EXISTS lsm3_auto_middle_test CASCADE;

CREATE TABLE lsm3_auto_middle_test
(
    id integer,
    payload text
);

CREATE INDEX lsm3_auto_middle_idx
ON lsm3_auto_middle_test
USING lsm3(id)
WITH (
    num_levels = 3,
    level_size_ratio = 4,
    top_index_size = 128
);

INSERT INTO lsm3_auto_middle_test
SELECT
    g,
    md5(g::text)
FROM generate_series(1, 800000) AS g;

SELECT lsm3_wait_merge_completion('lsm3_auto_middle_idx'::regclass::oid);

ANALYZE lsm3_auto_middle_test;

SET enable_seqscan = off;

SELECT * FROM lsm3_auto_middle_test WHERE id = 10;
SELECT * FROM lsm3_auto_middle_test WHERE id = 79999;

SELECT count(*) AS expected_101
FROM lsm3_auto_middle_test
WHERE id BETWEEN 1000 AND 1100;

SELECT
    c.relname,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'lsm3_auto_middle_idx%'
ORDER BY c.relname;

SELECT lsm3_get_merge_count('lsm3_auto_middle_idx'::regclass::oid) AS merge_count;

RESET enable_seqscan;