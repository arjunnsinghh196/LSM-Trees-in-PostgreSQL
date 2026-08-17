\echo '============================================================'
\echo 'CURRENT IMPLEMENTATION BENCHMARK: BTREE vs MULTI-RUN LSM3, BLOOM OFF/ON'
\echo '============================================================'
\echo 'Purpose:'
\echo '  1. Compare standard PostgreSQL B-tree with the current LSM3 implementation.'
\echo '  2. Compare current LSM3 with bloom_enabled=false vs bloom_enabled=true.'
\echo '  3. Create multiple immutable level0 runs so Bloom has real components to skip.'
\echo ''
\echo 'Expected high-level result:'
\echo '  - lsm-bloom and lsm-no-bloom should have similar load/merge time.'
\echo '  - lsm-bloom first negative probe may include Bloom build cost.'
\echo '  - lsm-bloom cached negative probes should be faster than lsm-no-bloom.'
\echo '  - standard btree can still be competitive/faster for point lookups because it has only one tree.'
\echo '  - the advantage of Bloom is reducing LSM read amplification across many runs.'
\echo ''

\timing on
SET jit = off;
SET enable_seqscan = off;
SET client_min_messages = warning;

DROP TABLE IF EXISTS bench_btree CASCADE;
DROP TABLE IF EXISTS bench_lsm_nobloom CASCADE;
DROP TABLE IF EXISTS bench_lsm_bloom CASCADE;
DROP TABLE IF EXISTS bench_results CASCADE;
DROP FUNCTION IF EXISTS bench_negative_probe(text, text, text, integer, integer);
DROP FUNCTION IF EXISTS bench_positive_probe(text, text, text, integer);

CREATE TABLE bench_results
(
    system_name text,
    phase text,
    seconds numeric,
    result_count bigint,
    notes text,
    created_at timestamptz DEFAULT clock_timestamp()
);

CREATE OR REPLACE FUNCTION bench_negative_probe(
    _system_name text,
    _table_name text,
    _phase text,
    _start_key integer,
    _num_keys integer
)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    t0 timestamptz;
    elapsed numeric;
    c bigint;
BEGIN
    t0 := clock_timestamp();

    EXECUTE format(
        'SELECT count(*)
         FROM generate_series($1, $2) AS g(id)
         WHERE EXISTS (SELECT 1 FROM %I AS t WHERE t.id = g.id)',
         _table_name
    )
    INTO c
    USING _start_key, _start_key + _num_keys - 1;

    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - t0);

    INSERT INTO bench_results(system_name, phase, seconds, result_count, notes)
    VALUES (_system_name, _phase, elapsed, c,
            format('%s absent equality probes, expected result_count=0', _num_keys));
END;
$$;

CREATE OR REPLACE FUNCTION bench_positive_probe(
    _system_name text,
    _table_name text,
    _phase text,
    _step integer
)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    t0 timestamptz;
    elapsed numeric;
    c bigint;
BEGIN
    t0 := clock_timestamp();

    EXECUTE format(
        'SELECT count(*)
         FROM generate_series(1, 90000, $1) AS g(id)
         WHERE EXISTS (SELECT 1 FROM %I AS t WHERE t.id = g.id)',
         _table_name
    )
    INTO c
    USING _step;

    elapsed := EXTRACT(EPOCH FROM clock_timestamp() - t0);

    INSERT INTO bench_results(system_name, phase, seconds, result_count, notes)
    VALUES (_system_name, _phase, elapsed, c,
            format('positive probes every %s keys; expected result_count about 90000/step', _step));
END;
$$;

\echo ''
\echo 'Step 1: create tables and indexes'
\echo 'Expected:'
\echo '  - btree table uses standard PostgreSQL btree index.'
\echo '  - lsm-no-bloom uses multi-run LSM3 with Bloom disabled.'
\echo '  - lsm-bloom uses same multi-run LSM3 with Bloom enabled.'
\echo ''

CREATE TABLE bench_btree
(
    id integer,
    payload text
);
CREATE INDEX bench_btree_idx ON bench_btree USING btree(id);

CREATE TABLE bench_lsm_nobloom
(
    id integer,
    payload text
);
CREATE INDEX bench_lsm_nobloom_idx
ON bench_lsm_nobloom
USING lsm3(id)
WITH (
    num_levels = 3,
    runs_per_level = 8,
    level_size_ratio = 100,
    top_index_size = 64,
    bloom_enabled = false
);

CREATE TABLE bench_lsm_bloom
(
    id integer,
    payload text
);
CREATE INDEX bench_lsm_bloom_idx
ON bench_lsm_bloom
USING lsm3(id)
WITH (
    num_levels = 3,
    runs_per_level = 8,
    level_size_ratio = 100,
    top_index_size = 64,
    bloom_enabled = true
);

\echo ''
\echo 'Step 2: load standard B-tree table'
\echo 'Expected: one growing PostgreSQL B-tree index.'
\echo ''

DO $$
DECLARE
    batch integer;
    batch_size integer := 15000;
    batches integer := 6;
    from_id integer;
    to_id integer;
    t0 timestamptz;
    elapsed numeric;
BEGIN
    FOR batch IN 0..batches-1 LOOP
        from_id := batch * batch_size + 1;
        to_id := (batch + 1) * batch_size;

        t0 := clock_timestamp();
        INSERT INTO bench_btree
        SELECT g, md5(g::text)
        FROM generate_series(from_id, to_id) AS g;
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - t0);

        INSERT INTO bench_results(system_name, phase, seconds, result_count, notes)
        VALUES ('btree', 'load_batch', elapsed, batch_size,
                format('batch %s inserted into standard btree table', batch + 1));
    END LOOP;
END;
$$;

\echo ''
\echo 'Step 3: load current LSM3 with Bloom disabled'
\echo 'Expected: every batch is inserted into top, then manually compacted into a new level0 run.'
\echo 'This creates multiple level0 runs and measures insert time separately from compaction time.'
\echo ''

DO $$
DECLARE
    batch integer;
    batch_size integer := 15000;
    batches integer := 6;
    from_id integer;
    to_id integer;
    t0 timestamptz;
    elapsed numeric;
BEGIN
    FOR batch IN 0..batches-1 LOOP
        from_id := batch * batch_size + 1;
        to_id := (batch + 1) * batch_size;

        t0 := clock_timestamp();
        INSERT INTO bench_lsm_nobloom
        SELECT g, md5(g::text)
        FROM generate_series(from_id, to_id) AS g;
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - t0);
        INSERT INTO bench_results(system_name, phase, seconds, result_count, notes)
        VALUES ('lsm-no-bloom', 'load_batch_insert_only', elapsed, batch_size,
                format('batch %s inserted into active top', batch + 1));

        t0 := clock_timestamp();
        PERFORM lsm3_start_merge('bench_lsm_nobloom_idx'::regclass::oid);
        PERFORM lsm3_wait_merge_completion('bench_lsm_nobloom_idx'::regclass::oid);
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - t0);
        INSERT INTO bench_results(system_name, phase, seconds, result_count, notes)
        VALUES ('lsm-no-bloom', 'manual_compaction', elapsed, batch_size,
                format('batch %s top -> level0 run; deterministic compaction trigger', batch + 1));
    END LOOP;
END;
$$;

\echo ''
\echo 'Step 4: load current LSM3 with Bloom enabled'
\echo 'Expected: same physical LSM layout as no-bloom. Bloom filters are lazy, so load/merge time should be close to no-bloom.'
\echo ''

DO $$
DECLARE
    batch integer;
    batch_size integer := 15000;
    batches integer := 6;
    from_id integer;
    to_id integer;
    t0 timestamptz;
    elapsed numeric;
BEGIN
    FOR batch IN 0..batches-1 LOOP
        from_id := batch * batch_size + 1;
        to_id := (batch + 1) * batch_size;

        t0 := clock_timestamp();
        INSERT INTO bench_lsm_bloom
        SELECT g, md5(g::text)
        FROM generate_series(from_id, to_id) AS g;
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - t0);
        INSERT INTO bench_results(system_name, phase, seconds, result_count, notes)
        VALUES ('lsm-bloom', 'load_batch_insert_only', elapsed, batch_size,
                format('batch %s inserted into active top', batch + 1));

        t0 := clock_timestamp();
        PERFORM lsm3_start_merge('bench_lsm_bloom_idx'::regclass::oid);
        PERFORM lsm3_wait_merge_completion('bench_lsm_bloom_idx'::regclass::oid);
        elapsed := EXTRACT(EPOCH FROM clock_timestamp() - t0);
        INSERT INTO bench_results(system_name, phase, seconds, result_count, notes)
        VALUES ('lsm-bloom', 'manual_compaction', elapsed, batch_size,
                format('batch %s top -> level0 run; deterministic compaction trigger', batch + 1));
    END LOOP;
END;
$$;

ANALYZE bench_btree;
ANALYZE bench_lsm_nobloom;
ANALYZE bench_lsm_bloom;

\echo ''
\echo 'Step 5: verify correctness before timing probes'
\echo 'Expected: all counts = 90000.'
\echo ''

SELECT 'btree' AS system_name, count(*) AS expected_90000 FROM bench_btree
UNION ALL
SELECT 'lsm-no-bloom', count(*) FROM bench_lsm_nobloom
UNION ALL
SELECT 'lsm-bloom', count(*) FROM bench_lsm_bloom;

\echo ''
\echo 'Step 6: show physical layout'
\echo 'Expected for current LSM tables:'
\echo '  - level0_run0 through level0_run5 should be non-empty.'
\echo '  - deeper levels/base should usually remain tiny because runs_per_level=8 and level_size_ratio=100.'
\echo ''

SELECT c.relname, pg_size_pretty(pg_relation_size(c.oid)) AS size, pg_relation_size(c.oid) AS bytes
FROM pg_class c
WHERE c.relname LIKE 'bench_btree_idx%'
   OR c.relname LIKE 'bench_lsm_nobloom_idx%'
   OR c.relname LIKE 'bench_lsm_bloom_idx%'
ORDER BY c.relname;

\echo ''
\echo 'Step 7: negative equality-probe benchmark'
\echo 'Expected:'
\echo '  - lsm-bloom negative_probe_warmup may include Bloom build cost.'
\echo '  - lsm-bloom negative_probe_cached_* should be faster than lsm-no-bloom cached probes.'
\echo '  - result_count should be 0 for all negative probes.'
\echo ''

SELECT bench_negative_probe('btree', 'bench_btree', 'negative_probe_1', 100000000, 50000);
SELECT bench_negative_probe('btree', 'bench_btree', 'negative_probe_2', 100000000, 50000);
SELECT bench_negative_probe('btree', 'bench_btree', 'negative_probe_3', 100000000, 50000);

SELECT bench_negative_probe('lsm-no-bloom', 'bench_lsm_nobloom', 'negative_probe_1', 100000000, 50000);
SELECT bench_negative_probe('lsm-no-bloom', 'bench_lsm_nobloom', 'negative_probe_2', 100000000, 50000);
SELECT bench_negative_probe('lsm-no-bloom', 'bench_lsm_nobloom', 'negative_probe_3', 100000000, 50000);

SELECT bench_negative_probe('lsm-bloom', 'bench_lsm_bloom', 'negative_probe_warmup_build_filter', 100000000, 50000);
SELECT bench_negative_probe('lsm-bloom', 'bench_lsm_bloom', 'negative_probe_cached_1', 100000000, 50000);
SELECT bench_negative_probe('lsm-bloom', 'bench_lsm_bloom', 'negative_probe_cached_2', 100000000, 50000);

\echo ''
\echo 'Step 8: positive equality-probe sanity benchmark'
\echo 'Expected:'
\echo '  - Bloom should not help much for positive lookups because matching runs still need to be scanned.'
\echo '  - result_count should be 9000 when step=10 over 1..90000.'
\echo ''

SELECT bench_positive_probe('btree', 'bench_btree', 'positive_probe', 10);
SELECT bench_positive_probe('lsm-no-bloom', 'bench_lsm_nobloom', 'positive_probe', 10);
SELECT bench_positive_probe('lsm-bloom', 'bench_lsm_bloom', 'positive_probe', 10);

\echo ''
\echo 'Step 9: summarized timing results'
\echo 'Interpretation:'
\echo '  - Compare lsm-no-bloom vs lsm-bloom for negative_probe_cached_* to isolate Bloom speedup.'
\echo '  - Compare original LSM3 script vs current LSM3 manual_compaction to isolate multi-level/multi-run compaction effect.'
\echo '  - Compare btree load_batch vs LSM load_batch_insert_only/manual_compaction separately.'
\echo ''

SELECT
    system_name,
    phase,
    round(avg(seconds), 6) AS avg_seconds,
    round(min(seconds), 6) AS min_seconds,
    round(max(seconds), 6) AS max_seconds,
    sum(result_count) AS total_result_count,
    count(*) AS runs
FROM bench_results
GROUP BY system_name, phase
ORDER BY system_name, phase;

\echo ''
\echo 'Raw timing rows'
SELECT system_name, phase, round(seconds, 6) AS seconds, result_count, notes
FROM bench_results
ORDER BY created_at;

RESET enable_seqscan;
RESET client_min_messages;
RESET jit;

\echo '============================================================'
\echo 'DONE: CURRENT IMPLEMENTATION BENCHMARK'
\echo '============================================================'