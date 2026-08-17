# LSM Trees in PostgreSQL

A fork of the [`lsm3`](https://github.com/postgrespro/lsm3) PostgreSQL extension that extends it from a fixed three-index design into a **multi-level, multi-run LSM tree with per-component Bloom filters**.

The upstream extension implements an LSM tree using ordinary Postgres B-tree indexes: two mutable "top" indexes absorb inserts, and on overflow the inactive one is merged into a single large base index. That design gives fast inserts, but every point lookup still has to probe all three indexes, and there is no intermediate structure between the small tops and the one enormous base.

This fork changes both of those things.

---

## What this fork adds

### 1. Multi-level, multi-run compaction

Upstream layout is fixed: `top0`, `top1`, `base`. Here the layout becomes

```
top0, top1                          mutable L0 buffer (unchanged)
level[0][0..R-1]                    immutable B-tree runs
level[1][0..R-1]                    immutable B-tree runs
...                                 up to LSM3_MAX_LEVELS
base                                final index
```

with up to **8 levels × 8 runs per level** (`LSM3_MAX_LEVELS`, `LSM3_MAX_RUNS_PER_LEVEL`). Shared-memory arrays stay fixed-capacity; the number of levels and runs actually used is a runtime property of each index.

A top flush now writes into a *new run* at level 0 rather than merging straight into the base. Only when a level's runs are exhausted does tiered compaction cascade the level upward. That is what makes the structure a real LSM tree rather than a two-stage buffer.

Three new reloptions control it:

| Option | Default | Range | Meaning |
|---|---|---|---|
| `num_levels` | 3 | 1–8 | Number of intermediate levels |
| `runs_per_level` | 4 | 1–8 | Immutable runs each level holds before compacting |
| `level_size_ratio` | 4 | 2–INT_MAX | Size multiplier between levels |

Setting `num_levels = 1, runs_per_level = 1` reproduces upstream behaviour, which is what `lsm3vsourlsm.sql` uses to compare the two designs without reinstalling the original extension.

### 2. Bloom filters for negative point lookups

More runs means more indexes to probe, which makes point lookups — especially *misses* — progressively more expensive. `bloom.c` / `bloom.h` is a new module that offsets this.

- **Scope.** Filters are built only for **immutable occupied level runs**. Never for the mutable tops, never for the base, never for range scans. An immutable component is the only kind whose contents can be summarised safely.
- **Size and hashing.** 64 KB per component (`LSM3_BLOOM_BYTES`), 4 hash probes (`LSM3_BLOOM_HASHES`). Hashing is FNV-1a over the raw Datum bytes, with a splitmix64 mixer deriving the additional independent-looking hash values.
- **Caching and invalidation.** Filters live in a backend-local hash table keyed by physical component OID. Each cache entry records the shared **generation counter** and block count observed when it was built. `lsm3.c` bumps `level_bloom_generation[level][run]` whenever a run is rewritten or truncated, so a stale filter is detected even when the relation size does not change — the case a size check alone would miss.
- **Key restriction.** Only equality probes on the **first index key**, and only where that column is a fixed-size by-value type. Anything else falls through to a normal scan.
- **Semantics.** `lsm3_bloom_might_contain_relation()` returns false *only* when absence is proven. A true return means "maybe present" or "Bloom not applicable", so a false positive costs a wasted probe and never a wrong answer.

Controlled per index by the `bloom_enabled` reloption (default `true`).

---

## Building and installing

Requires PostgreSQL development headers (`pg_config` on `PATH`) and a C toolchain.

```bash
make
sudo make install
```

If the LLVM bitcode step causes trouble, skip it:

```bash
make with_llvm=no
sudo make install with_llvm=no
```

`restart.sh` bundles the full rebuild-and-restart cycle used during development.

The extension allocates shared memory, so it **must** be preloaded. Add to `postgresql.conf` (or use the supplied `lsm3.conf`):

```
shared_preload_libraries = 'lsm3'
lsm3.top_index_size = 1MB
```

Then restart Postgres.

---

## Usage

```sql
CREATE EXTENSION lsm3;

CREATE TABLE t (id integer, val text);

-- default: 3 levels, 4 runs per level, Bloom enabled
CREATE INDEX idx ON t USING lsm3(id);

-- explicit configuration
CREATE INDEX idx ON t USING lsm3(id)
    WITH (num_levels = 2,
          runs_per_level = 8,
          level_size_ratio = 1000,
          bloom_enabled = true);

-- upstream-equivalent layout, Bloom off
CREATE INDEX idx ON t USING lsm3(id)
    WITH (num_levels = 1, runs_per_level = 1, bloom_enabled = false);
```

`lsm3_start_merge()` triggers compaction manually, which is how the test scripts get deterministic layouts instead of waiting on the background merger.

### Configuration parameters

| Parameter | Kind | Default | Meaning |
|---|---|---|---|
| `lsm3.max_indexes` | GUC | 1024 | Maximum number of Lsm3 indexes |
| `lsm3.top_index_size` | GUC | 64 MB | Top index size before flush |
| `top_index_size` | reloption | — | Per-index override of the GUC |
| `num_levels` | reloption | 3 | Intermediate levels |
| `runs_per_level` | reloption | 4 | Runs per level |
| `level_size_ratio` | reloption | 4 | Inter-level size multiplier |
| `bloom_enabled` | reloption | true | Bloom filters on equality probes |
| `unique` | reloption | false | Search optimisation, not a constraint |

Each Lsm3 index spawns its own background merge worker, so `max_worker_processes` needs to be large enough for the number of indexes in use.

### Inherited limitations

Carried over from upstream: no parallel index scan, no array keys, and the index cannot be declared `UNIQUE`. The `unique` reloption only tells the scan it may stop after the first match in the active top — it enforces nothing.

---

## Tests and benchmarks

| Script | What it does |
|---|---|
| `sql/test.sql` + `expected/test.out` | Standard `pg_regress` suite — run with `make installcheck` |
| `step_test.sql` | Verifies multi-run levels: that separate runs accumulate in a level, and that they compact upward when `runs_per_level` is reached. Asserts on physical index sizes and row counts. |
| `bench_test.sql` | Three-way comparison — plain B-tree vs multi-run LSM3 vs LSM3 + Bloom. 7 batches × 50,000 rows. Separates insert timing from manual compaction timing. |
| `lsm3vsourlsm.sql` | Upstream-equivalent configuration vs multi-run configuration, using only reloptions so no reinstall is needed. |
| `bloom_test.sql`, `curr_bloom.sql` | Bloom filter behaviour in isolation |
| `foreground_insert.sql` | Insert path under foreground merging |
| `automatic_test.sql` | Automatic (non-manual) compaction path |

Run any of them with:

```bash
psql -f bench_test.sql
```

### What to expect from the benchmarks

- A plain B-tree usually wins on simple point lookups — it has exactly one physical index to search.
- LSM **without** Bloom is slower on negative point lookups, because a miss means probing every run.
- LSM **with** Bloom pays a warmup cost to build filters, after which cached negative probes should beat the no-Bloom configuration.

The benefit is deliberately made visible by using many immutable runs and repeated negative probes; on a shallow layout with mostly positive lookups, Bloom filters have little to offer.

---

## Results

Fill in from your own runs.

| Workload | B-tree | LSM3 (no Bloom) | LSM3 + Bloom |
|---|---|---|---|
| Bulk insert, 350k rows | — | — | — |
| Positive point lookups | — | — | — |
| Negative point lookups (cold) | — | — | — |
| Negative point lookups (warm) | — | — | — |
| Range scan | — | — | — |

---

## Repository layout

```
lsm3.c            access method: insert path, scan/bitmap scan, compaction, bgworker
lsm3.h            shared control structures, level/run arrays, generation counters, options
bloom.c           Bloom module: hashing, filter build, backend-local cache, key extraction
bloom.h           Bloom interface and tuning constants
lsm3--1.0.sql     SQL definitions for the extension
lsm3.control      extension control file
lsm3.conf         preload configuration used by the regression harness
Makefile          PGXS build
restart.sh        rebuild + reinstall + restart Postgres + open psql
META.json         PGXN metadata
sql/, expected/   pg_regress test and expected output
*.sql             benchmark and manual test scripts
```

---

## Attribution and licence

Forked from **[postgrespro/lsm3](https://github.com/postgrespro/lsm3)** by Konstantin Knizhnik (Postgres Professional). The upstream design — LSM over standard B-tree indexes, dual top indexes for non-blocking merges, merged scans across components, background merge worker — is theirs. The multi-level/multi-run compaction, the Bloom filter module, and the benchmark suite are the additions in this fork.

Released under the **PostgreSQL License**, as upstream. See `LICENSE`.

Portions Copyright (c) 2015–2020, Postgres Professional
Portions Copyright (c) 1996–2017, PostgreSQL Global Development Group
Portions Copyright (c) 1994, The Regents of the University of California
