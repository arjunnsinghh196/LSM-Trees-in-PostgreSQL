/*
 * It is too expensive to check index size at each insert because it requires traverse of all index file segments and calling lseek for each.
 * But we do not need precise size, so it is enough to do it at each n-th insert. The lagest B-Tree key size is abut 2kb,
 * so with N=64K in the worst case error will be less than 128Mb and for 32-bit key just 1Mb.
 */
#define LSM3_CHECK_TOP_INDEX_SIZE_PERIOD (64*1024) /* should be power of two */

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * Keep shared-memory arrays fixed-size, but allow each Lsm3 index to use a runtime number of levels
 * and a runtime number of immutable B-tree runs inside each level.
 */
#define LSM3_NUM_TOP_INDEXES 2
#define LSM3_MAX_LEVELS 8
#define LSM3_DEFAULT_LEVELS 3
#define LSM3_DEFAULT_LEVEL_SIZE_RATIO 4
#define LSM3_MAX_RUNS_PER_LEVEL 8
#define LSM3_DEFAULT_RUNS_PER_LEVEL 4
#define LSM3_MAX_COMPONENTS (LSM3_NUM_TOP_INDEXES + (LSM3_MAX_LEVELS * LSM3_MAX_RUNS_PER_LEVEL) + 1)

/*
 * bloom injection:
 * Backend-local Bloom filters are keyed by physical component OID and invalidated using
 * shared generation counters whenever level-run components are rewritten/truncated.
 */

/*
 * Control structure for Lsm3 index located in shared memory
 */
typedef struct
{
	Oid base;   /* Oid of base index */
	Oid heap;   /* Oid of indexed relation */

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * top[] is still the two-entry mutable L0 buffer.
	 * level[level][run] stores immutable runs for each configured middle level.
	 */
	Oid top[LSM3_NUM_TOP_INDEXES];
	Oid level[LSM3_MAX_LEVELS][LSM3_MAX_RUNS_PER_LEVEL];

	int access_count[LSM3_NUM_TOP_INDEXES]; /* Access counter for top indexes */
	int active_index; /* Index used for insert */

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * num_levels and runs_per_level are runtime limits within fixed-capacity shared-memory arrays.
	 * level_run_count[level] is the prefix count of currently occupied runs in that level.
	 */
	int num_levels;
	int runs_per_level;
	int level_run_count[LSM3_MAX_LEVELS];
	int level_size_ratio;

	/*
	 * bloom injection:
	 * Generation is bumped whenever a level run is rewritten/truncated so backend-local
	 * Bloom cache entries can detect stale filters even if the relation size stays one page.
	 */
	uint64 level_bloom_generation[LSM3_MAX_LEVELS][LSM3_MAX_RUNS_PER_LEVEL];

	uint64 n_merges;  /* Number of performed top-level merge requests since database open */
	uint64 n_inserts; /* Number of performed inserts since database open  */
	volatile bool start_merge; /* Start top -> level run -> cascading compaction */
	volatile bool merge_in_progress; /* Overflow/manual request initiated a multi-run compaction */
	PGPROC* merger;   /* Merger background worker */
	Oid     db_id;    /* user ID (for background worker) */
	Oid     user_id;  /* database Id (for background worker) */
	Oid     am_id;    /* Lsm3 AM Oid */
	int     top_index_size; /* Size of top index */
	slock_t spinlock; /* Spinlock to synchronize access */
} Lsm3DictEntry;

/*
 * Opaque part of index scan descriptor
 */
typedef struct
{
	Lsm3DictEntry* entry;      /* Lsm3 control structure */

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * component_index/scan/eof are now sized for top0, top1, all level runs, and base.
	 * ncomponents tells the scan path how many fixed-capacity slots are active.
	 */
	Relation 	   component_index[LSM3_MAX_COMPONENTS]; /* Opened auxiliary index relations; base uses scan relation itself */
	SortSupport    sortKeys;   /* Context for comparing index tuples */
	IndexScanDesc  scan[LSM3_MAX_COMPONENTS];    /* Scan descriptors for all active components */
	bool           eof[LSM3_MAX_COMPONENTS];     /* Indicators that end of index was reached */
	int            ncomponents; /* Number of active scan components: two tops + active level runs + base */

	bool           unique;     /* Whether index is "unique" and we can stop scan after locating first occurrence */

	/*
	 * bloom injection:
	 * Cached from reloptions at scan start. Bloom filters are used only for equality probes
	 * on immutable occupied level runs, never for active tops or range scans.
	 */
	bool           bloom_enabled;

	int            curr_index; /* Index from which last tuple was selected (or -1 if none) */
} Lsm3ScanOpaque;

/* Lsm3 index options */
typedef struct
{
	BTOptions   nbt_opts;       /* Standard B-Tree options */
	int         top_index_size; /* Size of top index (overrode lsm3.top_index_size GUC */

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * These reloptions make the number of levels and runs per level configurable while preserving fixed shared-memory capacity.
	 */
	int         num_levels;
	int         runs_per_level;
	int         level_size_ratio;

	/*
	 * bloom injection:
	 * Enables/disables backend-local per-component Bloom filters for equality point lookups.
	 */
	bool        bloom_enabled;

	bool        unique;			/* Index may not contain duplicates. We prohibit unique constraint for Lsm3 index
                                 * because it can not be enforced. But presence of this index option allows to optimize
								 * index lookup: if key is found in active top index, do not search older components.
                                 */
} Lsm3Options;
