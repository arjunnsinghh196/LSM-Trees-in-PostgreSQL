#include "postgres.h"
#include "access/attnum.h"
#include "utils/relcache.h"
#include "access/reloptions.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "utils/rel.h"
#include "nodes/makefuncs.h"
#include "catalog/dependency.h"
#include "catalog/pg_operator.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/storage.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/builtins.h"
#include "utils/index_selfuncs.h"
#include "utils/rel.h"
#include "miscadmin.h"
#include "tcop/utility.h"
#include "postmaster/bgworker.h"
#include "pgstat.h"
#include "executor/executor.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"

#include "lsm3.h"
#include "bloom.h" /* bloom injection: separate Bloom helper module. */

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

PG_FUNCTION_INFO_V1(lsm3_handler);
PG_FUNCTION_INFO_V1(lsm3_btree_wrapper);
PG_FUNCTION_INFO_V1(lsm3_get_merge_count);
PG_FUNCTION_INFO_V1(lsm3_start_merge);
PG_FUNCTION_INFO_V1(lsm3_wait_merge_completion);
PG_FUNCTION_INFO_V1(lsm3_top_index_size);

extern void	_PG_init(void);
extern void	_PG_fini(void);

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * Export the background worker entry point because RegisterDynamicBackgroundWorker() resolves it by name.
 */
extern PGDLLEXPORT void lsm3_merger_main(Datum arg);

/* Lsm3 dictionary (hashtable with control data for all indexes) */
static HTAB*          Lsm3Dict;
static LWLock*        Lsm3DictLock;
static List*          Lsm3ReleasedLocks;
static List*          Lsm3Entries;
static bool           Lsm3InsideCopy;

/* Kind of relation optioms for Lsm3 index */
static relopt_kind    Lsm3ReloptKind;

/* Lsm3 kooks */
static ProcessUtility_hook_type PreviousProcessUtilityHook = NULL;
static shmem_startup_hook_type  PreviousShmemStartupHook = NULL;
#if PG_VERSION_NUM>=140000
static shmem_request_hook_type  PreviousShmemRequestHook = NULL;
#endif
static ExecutorFinish_hook_type PreviousExecutorFinish = NULL;

/* Lsm3 GUCs */
static int Lsm3MaxIndexes;
static int Lsm3TopIndexSize;

/* Background worker termination flag */
static volatile bool Lsm3Cancel;

/* bloom injection: Bloom implementation has been moved to bloom.c/bloom.h. */

static void
lsm3_shmem_request(void)
{
#if PG_VERSION_NUM>=140000
	if (PreviousShmemRequestHook)
		PreviousShmemRequestHook();
#endif

	RequestAddinShmemSpace(hash_estimate_size(Lsm3MaxIndexes, sizeof(Lsm3DictEntry)));
	RequestNamedLWLockTranche("lsm3", 1);
}

static void
lsm3_shmem_startup(void)
{
	HASHCTL info;

	if (PreviousShmemStartupHook)
	{
		PreviousShmemStartupHook();
    }
	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(Oid);
	info.entrysize = sizeof(Lsm3DictEntry);
	Lsm3Dict = ShmemInitHash("lsm3 hash",
							 Lsm3MaxIndexes, Lsm3MaxIndexes,
							 &info,
							 HASH_ELEM | HASH_BLOBS);
	Lsm3DictLock = &(GetNamedLWLockTranche("lsm3"))->lock;
}

/* Initialize Lsm3 control data entry */
static void
lsm3_init_entry(Lsm3DictEntry* entry, Relation index)
{
	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * Initialize fixed-capacity metadata for configurable levels and multiple immutable runs per level.
	 */
	Lsm3Options* opts = index->rd_options ? (Lsm3Options*)index->rd_options : NULL;

	SpinLockInit(&entry->spinlock);
	entry->active_index = 0;
	entry->merger = NULL;
	entry->merge_in_progress = false;
	entry->start_merge = false;
	entry->n_merges = 0;
	entry->n_inserts = 0;

	for (int i = 0; i < LSM3_NUM_TOP_INDEXES; i++)
	{
		entry->top[i] = InvalidOid;
		entry->access_count[i] = 0;
	}

	for (int level_no = 0; level_no < LSM3_MAX_LEVELS; level_no++)
	{
		entry->level_run_count[level_no] = 0;
		for (int run_no = 0; run_no < LSM3_MAX_RUNS_PER_LEVEL; run_no++)
		{
			entry->level[level_no][run_no] = InvalidOid;

			/* bloom injection: initialize per-run generation counters used to invalidate backend-local filters. */
			entry->level_bloom_generation[level_no][run_no] = 0;
		}
	}

	entry->num_levels = opts ? opts->num_levels : LSM3_DEFAULT_LEVELS;
	if (entry->num_levels < 1)
		entry->num_levels = 1;
	if (entry->num_levels > LSM3_MAX_LEVELS)
		entry->num_levels = LSM3_MAX_LEVELS;

	entry->runs_per_level = opts ? opts->runs_per_level : LSM3_DEFAULT_RUNS_PER_LEVEL;
	if (entry->runs_per_level < 1)
		entry->runs_per_level = 1;
	if (entry->runs_per_level > LSM3_MAX_RUNS_PER_LEVEL)
		entry->runs_per_level = LSM3_MAX_RUNS_PER_LEVEL;

	entry->level_size_ratio = opts ? opts->level_size_ratio : LSM3_DEFAULT_LEVEL_SIZE_RATIO;
	if (entry->level_size_ratio < 2)
		entry->level_size_ratio = LSM3_DEFAULT_LEVEL_SIZE_RATIO;

	entry->heap = index->rd_index->indrelid;
	entry->db_id = MyDatabaseId;
	entry->user_id = GetUserId();
	entry->top_index_size = opts ? opts->top_index_size : 0;
}

/* Get B-Tree index size (number of blocks) */
static BlockNumber
lsm3_get_index_size(Oid relid)
{
       Relation index = index_open(relid, AccessShareLock);
       BlockNumber size = RelationGetNumberOfBlocks(index);
	   index_close(index, AccessShareLock);
       return size;
}


/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * Component helpers map logical scan component numbers to physical index OIDs.
 * Layout: 0=top0, 1=top1, then level0_run0..level0_runR, level1_run0.., and last=base.
 */
static inline int
lsm3_level_component(Lsm3DictEntry* entry, int level_no, int run_no)
{
	return LSM3_NUM_TOP_INDEXES + (level_no * entry->runs_per_level) + run_no;
}

static inline int
lsm3_num_components(Lsm3DictEntry* entry)
{
	return LSM3_NUM_TOP_INDEXES + (entry->num_levels * entry->runs_per_level) + 1;
}

static inline int
lsm3_base_component(Lsm3DictEntry* entry)
{
	return LSM3_NUM_TOP_INDEXES + (entry->num_levels * entry->runs_per_level);
}

static Oid
lsm3_component_oid(Lsm3DictEntry* entry, int component)
{
	int level_slot;
	int level_no;
	int run_no;

	if (component < LSM3_NUM_TOP_INDEXES)
		return entry->top[component];

	if (component == lsm3_base_component(entry))
		return entry->base;

	level_slot = component - LSM3_NUM_TOP_INDEXES;
	level_no = level_slot / entry->runs_per_level;
	run_no = level_slot % entry->runs_per_level;

	Assert(level_no >= 0 && level_no < entry->num_levels);
	Assert(run_no >= 0 && run_no < entry->runs_per_level);

	return entry->level[level_no][run_no];
}

/* bloom injection: map a logical component slot back to its level/run coordinates. */
static bool
lsm3_component_level_run(Lsm3DictEntry* entry, int component, int *level_no, int *run_no)
{
	int level_slot;

	if (component < LSM3_NUM_TOP_INDEXES || component == lsm3_base_component(entry))
		return false;

	level_slot = component - LSM3_NUM_TOP_INDEXES;
	*level_no = level_slot / entry->runs_per_level;
	*run_no = level_slot % entry->runs_per_level;

	return *level_no >= 0 && *level_no < entry->num_levels &&
		   *run_no >= 0 && *run_no < entry->runs_per_level;
}

/* bloom injection: use Bloom only for immutable occupied level runs, never for tops/base/unoccupied slots. */
static bool
lsm3_bloom_component_eligible(Lsm3ScanOpaque *so, int component, int *level_no, int *run_no)
{
	if (!so->bloom_enabled)
		return false;

	if (!lsm3_component_level_run(so->entry, component, level_no, run_no))
		return false;

	if (*run_no >= so->entry->level_run_count[*level_no])
		return false;

	return so->component_index[component] != NULL;
}

/*
 * bloom injection:
 * lsm3.c decides whether a component is eligible; bloom.c owns the actual
 * backend-local Bloom cache, build, hash, and membership-check code.
 */
static bool
lsm3_bloom_might_contain_component(Lsm3ScanOpaque *so, int component, Datum key)
{
	int level_no;
	int run_no;
	uint64 generation;

	if (!lsm3_bloom_component_eligible(so, component, &level_no, &run_no))
		return true;

	generation = so->entry->level_bloom_generation[level_no][run_no];
	return lsm3_bloom_might_contain_relation(so->component_index[component],
										  so->entry->heap,
										  generation,
										  key);
}

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * A freshly built empty auxiliary B-tree has only its metapage. Once it receives real tuples it grows beyond one page.
 * This helper is used after restart to reconstruct the occupied-run prefix conservatively.
 */
static bool
lsm3_run_has_data(Oid index_oid)
{
	return OidIsValid(index_oid) && lsm3_get_index_size(index_oid) > 1;
}

/* Lookup or create Lsm3 control data for this index */
static Lsm3DictEntry*
lsm3_get_entry(Relation index)
{
	Lsm3DictEntry* entry;
	bool found = true;
	LWLockAcquire(Lsm3DictLock, LW_SHARED);
	entry = (Lsm3DictEntry*)hash_search(Lsm3Dict, &RelationGetRelid(index), HASH_FIND, &found);
	if (entry == NULL)
	{
		/* We need exclusive lock to create new entry */
		LWLockRelease(Lsm3DictLock);
		LWLockAcquire(Lsm3DictLock, LW_EXCLUSIVE);
		entry = (Lsm3DictEntry*)hash_search(Lsm3Dict, &RelationGetRelid(index), HASH_ENTER, &found);
	}
	if (!found)
	{
		char* relname = RelationGetRelationName(index);
		lsm3_init_entry(entry, index);

		/*
		 * LSM3 MULTI-RUN LEVEL CHANGE:
		 * Reconstruct top indexes and all configured level-run indexes from catalog names after restart/cache miss.
		 * level_run_count[] is reconstructed from non-empty prefix runs.
		 */
		for (int i = 0; i < LSM3_NUM_TOP_INDEXES; i++)
		{
			char* topidxname = psprintf("%s_top%d", relname, i);
			entry->top[i] = get_relname_relid(topidxname, RelationGetNamespace(index));
			if (entry->top[i] == InvalidOid)
			{
				elog(ERROR, "Lsm3: failed to lookup %s index", topidxname);
			}
		}

		for (int level_no = 0; level_no < entry->num_levels; level_no++)
		{
			entry->level_run_count[level_no] = 0;
			for (int run_no = 0; run_no < entry->runs_per_level; run_no++)
			{
				char* levelidxname = psprintf("%s_level%d_run%d", relname, level_no, run_no);
				entry->level[level_no][run_no] = get_relname_relid(levelidxname, RelationGetNamespace(index));
				if (entry->level[level_no][run_no] == InvalidOid)
				{
					elog(ERROR, "Lsm3: failed to lookup %s index", levelidxname);
				}

				if (lsm3_run_has_data(entry->level[level_no][run_no]))
					entry->level_run_count[level_no] = run_no + 1;
			}
		}

		entry->active_index = lsm3_get_index_size(entry->top[0]) >= lsm3_get_index_size(entry->top[1]) ? 0 : 1;
	}
	LWLockRelease(Lsm3DictLock);
	return entry;
}


/* Launch merger bgworker */
static void
lsm3_launch_bgworker(Lsm3DictEntry* entry)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	pid_t bgw_pid;

	MemSet(&worker, 0, sizeof(worker));
	snprintf(worker.bgw_name, sizeof(worker.bgw_name), "lsm3-merger-%d", entry->base);
	snprintf(worker.bgw_type, sizeof(worker.bgw_type), "lsm3-merger-%d", entry->base);
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	strcpy(worker.bgw_function_name, "lsm3_merger_main");
	strcpy(worker.bgw_library_name, "lsm3");
	worker.bgw_main_arg = PointerGetDatum(entry);
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
	{
		elog(ERROR, "Lsm3: failed to start background worker");
	}
	if (WaitForBackgroundWorkerStartup(handle, &bgw_pid) != BGWH_STARTED)
	{
		elog(ERROR, "Lsm3: startup of background worker is failed");
	}
	entry->merger = BackendPidGetProc(bgw_pid);
	for (int n_attempts = 0; entry->merger == NULL || n_attempts < 100; n_attempts++)
	{
		pg_usleep(10000); /* wait background worker to be registered in procarray */
		entry->merger = BackendPidGetProc(bgw_pid);
	}
	if (entry->merger == NULL)
	{
		elog(ERROR, "Lsm3: background worker %d is crashed", bgw_pid);
	}
}

/* Cancel merger bgwroker */
static void
lsm3_merge_cancel(int sig)
{
	Lsm3Cancel = true;
	SetLatch(MyLatch);
}

/* Truncate top index */
static void
lsm3_truncate_index(Oid index_oid, Oid heap_oid)
{
	Relation index = index_open(index_oid, AccessExclusiveLock);
	Relation heap = table_open(heap_oid, AccessShareLock); /* heap is actually not used, because we will not load data to top indexes */
	IndexInfo* indexInfo = BuildDummyIndexInfo(index);
	RelationTruncate(index, 0);
	elog(LOG, "Lsm3: truncate index %s", RelationGetRelationName(index));
	index_build(heap, index, indexInfo, true, false);
	index_close(index, AccessExclusiveLock);
	table_close(heap, AccessShareLock);
}

#if PG_VERSION_NUM>=140000
#define INSERT_FLAGS UNIQUE_CHECK_NO, false
#else
#define INSERT_FLAGS false
#endif

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * Merge any source component into any destination component.
 * The old code only used this as "top index -> base index"; cascading compaction now also uses it for level[i] -> level[i+1].
 */
static void
lsm3_merge_indexes(Oid dst_oid, Oid src_oid, Oid heap_oid)
{
	Relation src_index = index_open(src_oid, AccessShareLock);
	Relation heap = table_open(heap_oid, AccessShareLock);
	Relation dst_index = index_open(dst_oid, RowExclusiveLock);
	IndexScanDesc scan;
	bool ok;
	Oid  save_am = dst_index->rd_rel->relam;

	elog(LOG, "Lsm3: merge component %s (%d blocks) into %s (%d blocks)",
		 RelationGetRelationName(src_index), RelationGetNumberOfBlocks(src_index),
		 RelationGetRelationName(dst_index), RelationGetNumberOfBlocks(dst_index));

	dst_index->rd_rel->relam = BTREE_AM_OID;
	scan = index_beginscan(heap, src_index, SnapshotAny, 0, 0);
	scan->xs_want_itup = true;
	btrescan(scan, NULL, 0, 0, 0);
	for (ok = _bt_first(scan, ForwardScanDirection); ok; ok = _bt_next(scan, ForwardScanDirection))
	{
		IndexTuple itup = scan->xs_itup;
		if (BTreeTupleIsPosting(itup))
		{
			/* Some dirty coding here related with handling of posting items (index deduplication).
			 * If index tuple is posting item, we need to transfer it to normal index tuple.
			 * Posting list is representing by index tuple with INDEX_ALT_TID_MASK bit set in t_info and
			 * BT_IS_POSTING bit in TID offset, following by array of TIDs.
			 * We need to store right TID (taken from xs_heaptid) and correct index tuple length
			 * (not including size of TIDs array), clearing INDEX_ALT_TID_MASK.
			 * For efficiency reasons let's do it in place, saving and restoring original values after insertion is done.
			 */
			ItemPointerData save_tid = itup->t_tid;
			unsigned short save_info = itup->t_info;
			itup->t_info = (save_info & ~(INDEX_SIZE_MASK | INDEX_ALT_TID_MASK)) + BTreeTupleGetPostingOffset(itup);
			itup->t_tid = scan->xs_heaptid;
			_bt_doinsert(dst_index, itup, INSERT_FLAGS, heap); /* lsm3 index is not unique so need not to check duplicates */
			itup->t_tid = save_tid;
			itup->t_info = save_info;
		}
		else
		{
			_bt_doinsert(dst_index, itup, INSERT_FLAGS, heap); /* lsm3 index is not unique so need not to check duplicates */
		}
	}
	index_endscan(scan);
	dst_index->rd_rel->relam = save_am;
	index_close(src_index, AccessShareLock);
	index_close(dst_index, RowExclusiveLock);
	table_close(heap, AccessShareLock);
}

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * Compute the total size of all occupied runs in a level in KB.
 */
static int64
lsm3_level_size_kb(Lsm3DictEntry* entry, int level_no)
{
	int64 total_kb = 0;

	for (int run_no = 0; run_no < entry->level_run_count[level_no]; run_no++)
	{
		Oid run_oid = entry->level[level_no][run_no];
		if (OidIsValid(run_oid))
			total_kb += (int64)lsm3_get_index_size(run_oid) * (BLCKSZ / 1024);
	}
	return total_kb;
}

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * Compute the size limit for a whole level in KB.
 * Formula: level0 = top_size * ratio, level1 = top_size * ratio^2, ...
 */
static int64
lsm3_level_threshold_kb(Lsm3DictEntry* entry, int level_no)
{
	int64 threshold = entry->top_index_size ? entry->top_index_size : Lsm3TopIndexSize;

	for (int i = 0; i <= level_no; i++)
	{
		if (threshold > PG_INT64_MAX / entry->level_size_ratio)
			return PG_INT64_MAX;
		threshold *= entry->level_size_ratio;
	}
	return threshold;
}

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * A level should compact when it has filled all available run slots or when its total size exceeds its threshold.
 */
static bool
lsm3_level_should_compact(Lsm3DictEntry* entry, int level_no)
{
	int run_count = entry->level_run_count[level_no];
	int64 threshold_kb = lsm3_level_threshold_kb(entry, level_no);
	int64 size_kb = lsm3_level_size_kb(entry, level_no);

	return run_count >= entry->runs_per_level || size_kb > threshold_kb;
}

static void lsm3_compact_level(Lsm3DictEntry* entry, int level_no);

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * Add a source component as a new immutable run in level_no. If the destination level is full,
 * compact it first to make a free run slot. After adding, compact the level if it crosses its
 * run-count or size threshold.
 */
static void
lsm3_add_component_to_level(Lsm3DictEntry* entry, int level_no, Oid src_oid, const char *src_desc)
{
	Oid dst_oid;
	int dst_run;

	if (level_no >= entry->num_levels)
	{
		elog(LOG, "Lsm3: merge %s directly into base", src_desc);
		lsm3_merge_indexes(entry->base, src_oid, entry->heap);
		lsm3_truncate_index(src_oid, entry->heap);
		return;
	}

	if (entry->level_run_count[level_no] >= entry->runs_per_level)
	{
		elog(LOG, "Lsm3: level%d has no free run slot; compacting before adding %s", level_no, src_desc);
		lsm3_compact_level(entry, level_no);
	}

	Assert(entry->level_run_count[level_no] < entry->runs_per_level);
	dst_run = entry->level_run_count[level_no];
	dst_oid = entry->level[level_no][dst_run];

	if (!OidIsValid(dst_oid))
		elog(ERROR, "Lsm3: invalid destination run level%d_run%d", level_no, dst_run);

	pgstat_report_activity(STATE_RUNNING, "merging component into level run");
	elog(LOG, "Lsm3: merge %s into level%d_run%d", src_desc, level_no, dst_run);
	lsm3_merge_indexes(dst_oid, src_oid, entry->heap);

	pgstat_report_activity(STATE_RUNNING, "truncate source component");
	lsm3_truncate_index(src_oid, entry->heap);

	/* bloom injection: destination run content changed, so invalidate cached Bloom filters. */
	entry->level_bloom_generation[level_no][dst_run] += 1;

	entry->level_run_count[level_no] += 1;

	if (lsm3_level_should_compact(entry, level_no))
	{
		elog(LOG, "Lsm3: level%d now has %d runs and " INT64_FORMAT " KB; compacting", level_no,
			 entry->level_run_count[level_no], lsm3_level_size_kb(entry, level_no));
		lsm3_compact_level(entry, level_no);
	}
}

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * Tiered compaction for one level. All occupied runs in level_no are merged into one new run
 * of the next level, or into base if level_no is the last configured level. Source runs are
 * truncated and their run count is reset to zero.
 */
static void
lsm3_compact_level(Lsm3DictEntry* entry, int level_no)
{
	int run_count;
	Oid dst_oid;
	int dst_run;

	if (level_no < 0 || level_no >= entry->num_levels)
		return;

	run_count = entry->level_run_count[level_no];
	if (run_count <= 0)
		return;

	pgstat_report_activity(STATE_RUNNING, "tiered level compaction");

	if (level_no + 1 < entry->num_levels)
	{
		/* Make sure the next level has a free run slot before we produce a new run there. */
		if (entry->level_run_count[level_no + 1] >= entry->runs_per_level)
		{
			elog(LOG, "Lsm3: compact next level%d first because it is full", level_no + 1);
			lsm3_compact_level(entry, level_no + 1);
		}

		Assert(entry->level_run_count[level_no + 1] < entry->runs_per_level);
		dst_run = entry->level_run_count[level_no + 1];
		dst_oid = entry->level[level_no + 1][dst_run];
		if (!OidIsValid(dst_oid))
			elog(ERROR, "Lsm3: invalid destination run level%d_run%d", level_no + 1, dst_run);

		elog(LOG, "Lsm3: compact %d runs from level%d into level%d_run%d", run_count, level_no, level_no + 1, dst_run);
	}
	else
	{
		dst_run = -1;
		dst_oid = entry->base;
		elog(LOG, "Lsm3: compact %d runs from final level%d into base", run_count, level_no);
	}

	for (int run_no = 0; run_no < run_count; run_no++)
	{
		Oid src_oid = entry->level[level_no][run_no];
		if (!OidIsValid(src_oid))
			continue;

		lsm3_merge_indexes(dst_oid, src_oid, entry->heap);
		lsm3_truncate_index(src_oid, entry->heap);

		/* bloom injection: source run was truncated, so invalidate cached Bloom filters. */
		entry->level_bloom_generation[level_no][run_no] += 1;
	}

	entry->level_run_count[level_no] = 0;

	if (level_no + 1 < entry->num_levels)
	{
		/* bloom injection: destination run content changed, so invalidate cached Bloom filters. */
		entry->level_bloom_generation[level_no + 1][dst_run] += 1;

		entry->level_run_count[level_no + 1] += 1;
		if (lsm3_level_should_compact(entry, level_no + 1))
			lsm3_compact_level(entry, level_no + 1);
	}
}

/*
 * LSM3 MULTI-RUN LEVEL CHANGE:
 * Compact the inactive top index into a new run of level0. Further cascading happens only when
 * a level fills its run slots or exceeds its configured total-size threshold.
 */
static void
lsm3_compact_from_top(Lsm3DictEntry* entry, int top_index)
{
	char src_desc[64];

	snprintf(src_desc, sizeof(src_desc), "top%d", top_index);
	lsm3_add_component_to_level(entry, 0, entry->top[top_index], src_desc);
}

/* Lsm3 index options.
 */
static bytea *
lsm3_options(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(BTOptions, fillfactor)},
		{"vacuum_cleanup_index_scale_factor", RELOPT_TYPE_REAL,
		offsetof(BTOptions, vacuum_cleanup_index_scale_factor)},
		{"deduplicate_items", RELOPT_TYPE_BOOL,
		 offsetof(BTOptions, deduplicate_items)},
		{"top_index_size", RELOPT_TYPE_INT, offsetof(Lsm3Options, top_index_size)},

		/*
		 * LSM3 MULTI-RUN LEVEL CHANGE:
		 * Parse runtime level/run configuration from reloptions and store it in Lsm3Options.
		 */
		{"num_levels", RELOPT_TYPE_INT, offsetof(Lsm3Options, num_levels)},
		{"runs_per_level", RELOPT_TYPE_INT, offsetof(Lsm3Options, runs_per_level)},
		{"level_size_ratio", RELOPT_TYPE_INT, offsetof(Lsm3Options, level_size_ratio)},

		/* bloom injection: parse reloption controlling equality-lookup Bloom skipping. */
		{"bloom_enabled", RELOPT_TYPE_BOOL, offsetof(Lsm3Options, bloom_enabled)},

		{"unique", RELOPT_TYPE_BOOL, offsetof(Lsm3Options, unique)}
	};
	return (bytea *) build_reloptions(reloptions, validate, Lsm3ReloptKind,
									  sizeof(Lsm3Options), tab, lengthof(tab));
}


/* Main function of merger bgwroker */
PGDLLEXPORT void
lsm3_merger_main(Datum arg)
{
	Lsm3DictEntry* entry = (Lsm3DictEntry*)DatumGetPointer(arg);
	char	   *appname;

	pqsignal(SIGINT,  lsm3_merge_cancel);
	pqsignal(SIGQUIT, lsm3_merge_cancel);
	pqsignal(SIGTERM, lsm3_merge_cancel);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnectionByOid(entry->db_id, entry->user_id, 0);

	appname = psprintf("lsm3 merger for %d", entry->base);
	pgstat_report_appname(appname);
	pfree(appname);

	while (!Lsm3Cancel)
	{
		int merge_index= -1;
		int wr;
		pgstat_report_activity(STATE_IDLE, "waiting");
		wr = WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, -1L, PG_WAIT_EXTENSION);

		if ((wr & WL_POSTMASTER_DEATH) || Lsm3Cancel)
		{
			break;
		}

		ResetLatch(MyLatch);

		/* Check if merge is requested under spinlock */
		SpinLockAcquire(&entry->spinlock);
		if (entry->start_merge)
		{
			merge_index = 1 - entry->active_index; /* at this moment active index should already be swapped */
			entry->start_merge = false;
		}
		SpinLockRelease(&entry->spinlock);

		if (merge_index >= 0)
		{
			/*
			 * LSM3 MULTI-RUN LEVEL CHANGE:
			 * The worker now performs top -> level0 followed by level cascading.
			 * The old implementation did only top -> base.
			 */
			StartTransactionCommand();
			{
				lsm3_compact_from_top(entry, merge_index);
			}
			CommitTransactionCommand();

			SpinLockAcquire(&entry->spinlock);
			entry->merge_in_progress = false; /* mark multi-level compaction as completed */
			SpinLockRelease(&entry->spinlock);
		}
	}
	entry->merger = NULL;
}

/* Build index tuple comparator context */
static SortSupport
lsm3_build_sortkeys(Relation index)
{
	int	keysz = IndexRelationGetNumberOfKeyAttributes(index);
	SortSupport	sortKeys = (SortSupport) palloc0(keysz * sizeof(SortSupportData));
	BTScanInsert inskey = _bt_mkscankey(index, NULL);
	Oid          save_am = index->rd_rel->relam;

	index->rd_rel->relam = BTREE_AM_OID;

	for (int i = 0; i < keysz; i++)
	{
		SortSupport sortKey = &sortKeys[i];
		ScanKey		scanKey = &inskey->scankeys[i];
		int16		strategy;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = scanKey->sk_collation;
		sortKey->ssup_nulls_first =
			(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
		sortKey->ssup_attno = scanKey->sk_attno;
		/* Abbreviation is not supported here */
		sortKey->abbreviate = false;

		Assert(sortKey->ssup_attno != 0);

		strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
			BTGreaterStrategyNumber : BTLessStrategyNumber;

		PrepareSortSupportFromIndexRel(index, strategy, sortKey);
	}
	index->rd_rel->relam = save_am;
	return sortKeys;
}

/* Compare index tuples */
static int
lsm3_compare_index_tuples(IndexScanDesc scan1, IndexScanDesc scan2, SortSupport sortKeys)
{
	int n_keys = IndexRelationGetNumberOfKeyAttributes(scan1->indexRelation);

	for (int i = 1; i <= n_keys; i++)
	{
		Datum	datum[2];
		bool	isNull[2];
		int 	result;

		datum[0] = index_getattr(scan1->xs_itup, i, scan1->xs_itupdesc, &isNull[0]);
		datum[1] = index_getattr(scan2->xs_itup, i, scan2->xs_itupdesc, &isNull[1]);
		result = ApplySortComparator(datum[0], isNull[0],
									 datum[1], isNull[1],
									 &sortKeys[i - 1]);
		if (result != 0)
		{
			return result;
		}
	}
	return ItemPointerCompare(&scan1->xs_heaptid, &scan2->xs_heaptid);
}

/*
 * Lsm3 access methods implementation
 */

static IndexBuildResult *
lsm3_build(Relation heap, Relation index, IndexInfo *indexInfo)
{
	bool found;
	Lsm3DictEntry* entry;
	LWLockAcquire(Lsm3DictLock, LW_EXCLUSIVE);
	elog(LOG, "lsm3_build %s", index->rd_rel->relname.data);
	entry = hash_search(Lsm3Dict, &RelationGetRelid(index), HASH_ENTER, &found); /* Setting Lsm3Entry indicates to utility hook that Lsm3 index was created */
	if (!found)
	{
		lsm3_init_entry(entry, index);
	}
	{
		MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);
		Lsm3Entries = lappend(Lsm3Entries, entry);
		MemoryContextSwitchTo(old_context);
	}
	entry->am_id = index->rd_rel->relam;
	index->rd_rel->relam = BTREE_AM_OID;
	LWLockRelease(Lsm3DictLock); /* Release lock set by lsm3_build */
	return btbuild(heap, index, indexInfo);
}

/*
 * Grab previously release self locks (to let merger to proceed).
 */
static void
lsm3_reacquire_locks(void)
{
	if (Lsm3ReleasedLocks)
	{
		ListCell* cell;
		foreach (cell, Lsm3ReleasedLocks)
		{
			Oid indexOid = lfirst_oid(cell);
			LockRelationOid(indexOid, RowExclusiveLock);
		}
		list_free(Lsm3ReleasedLocks);
		Lsm3ReleasedLocks = NULL;
	}
}

/*
 * Insert in active top index; on overflow swap active indexes and initiate multi-level compaction.
 * LSM3 MULTI-RUN LEVEL CHANGE: the merge target is now level0 first, not base directly.
 */
static bool
lsm3_insert(Relation rel, Datum *values, bool *isnull,
			ItemPointer ht_ctid, Relation heapRel,
			IndexUniqueCheck checkUnique,
#if PG_VERSION_NUM>=140000
			bool indexUnchanged,
#endif
			IndexInfo *indexInfo)
{
	Lsm3DictEntry* entry = lsm3_get_entry(rel);

	int active_index;
	uint64 n_merges; /* used to check if merge was initiated by somebody else */
	Relation index;
	Oid  save_am;
	BlockNumber top_blocks;
	bool overflow;
	int top_index_size = entry->top_index_size ? entry->top_index_size : Lsm3TopIndexSize;
	bool is_initialized = true;

	/* Obtain current active index and increment access counter under spinlock */
	SpinLockAcquire(&entry->spinlock);
	active_index = entry->active_index;
	if (entry->top[active_index])
		entry->access_count[active_index] += 1;
	else
		is_initialized = false;
	n_merges = entry->n_merges;
	SpinLockRelease(&entry->spinlock);

	if (!is_initialized)
	{
		bool res;
		save_am = rel->rd_rel->relam;
		rel->rd_rel->relam = BTREE_AM_OID;
		res = btinsert(rel, values, isnull, ht_ctid, heapRel, checkUnique,
#if PG_VERSION_NUM>=140000
			 indexUnchanged,
#endif
			 indexInfo);
		rel->rd_rel->relam = save_am;
		return res;
	}
	/* Do insert in top index */
	index = index_open(entry->top[active_index], RowExclusiveLock);

	/*
	 * LSM3 CLEANUP CHANGE:
	 * Save the original AM before temporarily treating the auxiliary relation as a B-Tree.
	 * We also read the block count before index_close(), because the Relation pointer is not valid after close.
	 */
	save_am = index->rd_rel->relam;
	index->rd_rel->relam = BTREE_AM_OID;

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * Removed checkpoint Bloom insertion; inserts now only update the active top index.
	 */

	btinsert(index, values, isnull, ht_ctid, heapRel, checkUnique,
#if PG_VERSION_NUM>=140000
			 indexUnchanged,
#endif
			 indexInfo);
	top_blocks = RelationGetNumberOfBlocks(index);
	index->rd_rel->relam = save_am;
	index_close(index, RowExclusiveLock);

	overflow = !entry->merge_in_progress /* do not check for overflow if merge was already initiated */
 		&& (entry->n_inserts % LSM3_CHECK_TOP_INDEX_SIZE_PERIOD) == 0 /* perform check only each N-th insert  */
		&& top_blocks*(BLCKSZ/1024) > top_index_size;

	SpinLockAcquire(&entry->spinlock);
	/* If multi-level compaction was not initiated before by somebody else, then schedule it */
	if (overflow && !entry->merge_in_progress && entry->n_merges == n_merges)
	{
		Assert(entry->active_index == active_index);
		entry->merge_in_progress = true;
		entry->active_index ^= 1; /* swap top indexes */
		entry->n_merges += 1;
		/*
		 * LSM3 MULTI-RUN LEVEL CHANGE:
		 * Bloom reset removed; later Bloom filters should be maintained per LSM component.
		 */
	}
	Assert(entry->access_count[active_index] > 0);
	entry->access_count[active_index] -= 1;
	entry->n_inserts += 1;
	if (entry->merge_in_progress)
	{
		LOCKTAG		tag;
		SET_LOCKTAG_RELATION(tag,
							 MyDatabaseId,
							 entry->top[1-active_index]);
		/* Holding lock on non-ative index prevent merger bgworker from truncation this index */
		if (LockHeldByMe(&tag, RowExclusiveLock))
		{
			/* Copy locks all indexes and hold this locks until end of copy.
			 * We can not just release lock, because otherwise CopyFrom produces
			 * "you don't own a lock of type" warning.
			 * So just try to periodically release this lock and let merger grab it.
			 */
			if (!Lsm3InsideCopy ||
				(entry->n_inserts % LSM3_CHECK_TOP_INDEX_SIZE_PERIOD) == 0) /* release lock only each N-th insert  */

			{
				LockRelease(&tag, RowExclusiveLock, false);
				Lsm3ReleasedLocks = lappend_oid(Lsm3ReleasedLocks, entry->top[1-active_index]);
			}
		}

		/* If all inserts in previous active index are completed then we can start merge */
		if (entry->active_index != active_index && entry->access_count[active_index] == 0)
		{
			entry->start_merge = true;
			if (entry->merger == NULL) /* lazy start of bgworker */
			{
				lsm3_launch_bgworker(entry);
			}
			SetLatch(&entry->merger->procLatch);
		}
	}
	SpinLockRelease(&entry->spinlock);

	/* We have to require released locks because othervise CopyFrom will produce warning */
	if (Lsm3InsideCopy && Lsm3ReleasedLocks)
	{
		pg_usleep(1); /* give merge thread a chance to grab the lock before we require it */
		lsm3_reacquire_locks();
	}
	return false;
}

static IndexScanDesc
lsm3_beginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	Lsm3ScanOpaque* so;
	int i;
	int base_component;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	/* get the scan */
	scan = RelationGetIndexScan(rel, nkeys, norderbys);
	scan->xs_itupdesc = RelationGetDescr(rel);

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * Allocate zeroed scan opaque and open every active component: top0, top1, level0..levelN-1, and base.
	 */
	so = (Lsm3ScanOpaque*)palloc0(sizeof(Lsm3ScanOpaque));
	so->entry = lsm3_get_entry(rel);
	so->sortKeys = lsm3_build_sortkeys(rel);
	so->ncomponents = lsm3_num_components(so->entry);
	base_component = lsm3_base_component(so->entry);

	for (i = 0; i < so->ncomponents; i++)
	{
		Oid component_oid = lsm3_component_oid(so->entry, i);

		if (i == base_component)
		{
			/* Base index is the main relation passed to ambeginscan; do not reopen or close it. */
			so->component_index[i] = NULL;
			so->scan[i] = btbeginscan(rel, nkeys, norderbys);
		}
		else if (component_oid != InvalidOid)
		{
			so->component_index[i] = index_open(component_oid, AccessShareLock);
			so->scan[i] = btbeginscan(so->component_index[i], nkeys, norderbys);
		}
		else
		{
			so->component_index[i] = NULL;
			so->scan[i] = NULL;
		}

		if (so->scan[i])
		{
			so->eof[i] = false;
			so->scan[i]->xs_want_itup = true;
			so->scan[i]->parallel_scan = NULL;
		}
	}

	so->unique = rel->rd_options ? ((Lsm3Options*)rel->rd_options)->unique : false;

	/* bloom injection: cache reloption value for the scan. */
	so->bloom_enabled = rel->rd_options ? ((Lsm3Options*)rel->rd_options)->bloom_enabled : true;

	so->curr_index = -1;
	scan->opaque = so;

	return scan;
}

static void
lsm3_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
			ScanKey orderbys, int norderbys)
{
	Lsm3ScanOpaque* so = (Lsm3ScanOpaque*) scan->opaque;

	/*
	 * bloom injection:
	 * PostgreSQL passes the real scan keys to amrescan(), not ambeginscan().
	 * We forward those keys to every internal B-tree scan below, but Bloom key
	 * extraction reads the outer LSM scan descriptor in lsm3_gettuple()/getbitmap().
	 * Therefore we must also copy the latest scan keys into the outer descriptor.
	 */
	if (scankey && nscankeys > 0)
	{
		Assert(scan->numberOfKeys >= nscankeys);
		memcpy(scan->keyData, scankey, nscankeys * sizeof(ScanKeyData));
	}

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * Rescan all active components instead of the old fixed three scans.
	 */
	so->curr_index = -1;
	for (int i = 0; i < so->ncomponents; i++)
	{
		if (so->scan[i])
		{
			btrescan(so->scan[i], scankey, nscankeys, orderbys, norderbys);
			so->eof[i] = false;
		}
	}
}

static void
lsm3_endscan(IndexScanDesc scan)
{
	Lsm3ScanOpaque* so = (Lsm3ScanOpaque*) scan->opaque;

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * End all active component scans and close only auxiliary component relations.
	 */
	for (int i = 0; i < so->ncomponents; i++)
	{
		if (so->scan[i])
		{
			btendscan(so->scan[i]);
			if (so->component_index[i])
			{
				index_close(so->component_index[i], AccessShareLock);
			}
		}
	}
	pfree(so);
}


static bool
lsm3_gettuple(IndexScanDesc scan, ScanDirection dir)
{
	Lsm3ScanOpaque* so = (Lsm3ScanOpaque*) scan->opaque;
	int min = -1;
	int curr = so->curr_index;
	int try_index_order[LSM3_MAX_COMPONENTS];
	int order_count = 0;

	/* bloom injection: Bloom filters are useful only for equality predicates on first index key. */
	Datum bloom_key = (Datum) 0;
	bool bloom_key_valid = lsm3_bloom_extract_equality_key(scan, &bloom_key);

	/* btree indexes are never lossy */
	scan->xs_recheck = false;

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * Build newest-to-oldest component order dynamically:
	 * active top, inactive top, then for each level scan newer runs before older runs, and finally base.
	 */
	try_index_order[order_count++] = so->entry->active_index;
	try_index_order[order_count++] = 1 - so->entry->active_index;
	for (int level = 0; level < so->entry->num_levels; level++)
	{
		/*
		 * LSM3 MULTI-RUN LEVEL CHANGE:
		 * Include every configured run, not just level_run_count[level]. During compaction a destination
		 * run may already contain copied tuples before the run-count metadata is advanced; scanning all
		 * run slots prevents readers from missing tuples during that transient state.
		 */
		for (int run = so->entry->runs_per_level - 1; run >= 0; run--)
		{
			try_index_order[order_count++] = lsm3_level_component(so->entry, level, run);
		}
	}
	try_index_order[order_count++] = lsm3_base_component(so->entry);

	if (curr >= 0) /* lazy advance of current index */
	{
		so->eof[curr] = !_bt_next(so->scan[curr], dir); /* move forward current index */
	}

	for (int j = 0; j < order_count; j++)
	{
		int i = try_index_order[j];
		BTScanOpaque bto;

		if (i >= so->ncomponents || so->scan[i] == NULL)
		{
			continue;
		}

		bto = (BTScanOpaque)so->scan[i]->opaque;
		so->scan[i]->xs_snapshot = scan->xs_snapshot;
		if (!so->eof[i] && !BTScanPosIsValid(bto->currPos))
		{
			/* bloom injection: skip immutable level-run component if Bloom proves key absence. */
			if (bloom_key_valid && !lsm3_bloom_might_contain_component(so, i, bloom_key))
			{
				so->eof[i] = true;
				continue;
			}

			so->eof[i] = !_bt_first(so->scan[i], dir);
			if (!so->eof[i] && so->unique && scan->numberOfKeys == scan->indexRelation->rd_index->indnkeyatts)
			{
				/* If index is marked as unique and we perform lookup using all index keys,
				 * then we can stop after locating first occurrence.
				 * LSM3 MULTI-RUN LEVEL CHANGE: this now skips all remaining dynamic components, not just three indexes.
				 */
				elog(DEBUG1, "Lsm3: lookup %d indexes", j+1);
				while (++j < order_count) /* prevent search of all remaining indexes */
				{
					if (try_index_order[j] < so->ncomponents)
					{
						so->eof[try_index_order[j]] = true;
					}
				}
				min = i;
				break;
			}
		}
		if (!so->eof[i])
		{
			if (min < 0)
			{
				min = i;
			}
			else
			{
				int result = lsm3_compare_index_tuples(so->scan[i], so->scan[min], so->sortKeys);
				if (result == 0)
				{
					/* Duplicate: it can happen during merge when same tid is both in top and base index */
					so->eof[i] = !_bt_next(so->scan[i], dir); /* just skip one of entries */
				}
				else if ((result < 0) == ScanDirectionIsForward(dir))
				{
					min = i;
				}
			}
		}
	}
	if (min < 0) /* all indexes are traversed */
	{
		return false;
	}
	else
	{
		scan->xs_heaptid = so->scan[min]->xs_heaptid; /* copy TID */
		if (scan->xs_want_itup) {
			scan->xs_itup = so->scan[min]->xs_itup;
		}
		so->curr_index = min; /*will be advance at next call of gettuple */
		return true;
	}
}

static int64
lsm3_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	Lsm3ScanOpaque* so = (Lsm3ScanOpaque*)scan->opaque;
	int64 ntids = 0;

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * Bitmap scan now visits every active component instead of two top indexes plus base only.
	 * bloom injection: equality bitmap scans can skip immutable level runs using complete Bloom filters.
	 */
	Datum bloom_key = (Datum) 0;
	bool bloom_key_valid = lsm3_bloom_extract_equality_key(scan, &bloom_key);

	for (int i = 0; i < so->ncomponents; i++)
	{
		if (so->scan[i])
		{
			so->scan[i]->xs_snapshot = scan->xs_snapshot;

			/* bloom injection: skip this component only if absence is guaranteed. */
			if (bloom_key_valid && !lsm3_bloom_might_contain_component(so, i, bloom_key))
				continue;

			ntids += btgetbitmap(so->scan[i], tbm);
		}
	}
	return ntids;
}


Datum
lsm3_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = BTMaxStrategyNumber;
	amroutine->amsupport = BTNProcs;
	amroutine->amoptsprocnum = BTOPTIONS_PROC;
	amroutine->amcanorder = true;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = false;   /* We can't check that index is unique without accessing base index */
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false; /* TODO: not sure if it will work correctly with merge */
	amroutine->amsearchnulls = true;
	amroutine->amstorage = false;
	amroutine->amclusterable = true;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = false; /* TODO: parallel scac is not supported yet */
	amroutine->amcaninclude = true;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amparallelvacuumoptions = 0;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = lsm3_build;
	amroutine->ambuildempty = btbuildempty;
	amroutine->aminsert = lsm3_insert;
	amroutine->ambulkdelete = btbulkdelete;
	amroutine->amvacuumcleanup = btvacuumcleanup;
	amroutine->amcanreturn = btcanreturn;
	amroutine->amcostestimate = btcostestimate;
	amroutine->amoptions = lsm3_options;
	amroutine->amproperty = btproperty;
	amroutine->ambuildphasename = btbuildphasename;
	amroutine->amvalidate = btvalidate;
	amroutine->ambeginscan = lsm3_beginscan;
	amroutine->amrescan = lsm3_rescan;
	amroutine->amgettuple = lsm3_gettuple;
	amroutine->amgetbitmap = lsm3_getbitmap;
	amroutine->amendscan = lsm3_endscan;
	amroutine->ammarkpos = NULL;  /*  When do we need index_markpos? Can we live without it? */
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * Access methods for B-Tree wrapper: actually we aonly want to disable inserts.
 */

/* We do not need to load data in top top index: just initialize index metadata */
static IndexBuildResult *
lsm3_build_empty(Relation heap, Relation index, IndexInfo *indexInfo)
{
	Page		metapage;

	/* Construct metapage. */
	metapage = (Page) palloc(BLCKSZ);
	_bt_initmetapage(metapage, BTREE_METAPAGE, 0, _bt_allequalimage(index, false));

#if PG_VERSION_NUM>=140000
	RelationGetSmgr(index);
#else
	RelationOpenSmgr(index);
#endif

	/*
	 * Write the page and log it.  It might seem that an immediate sync would
	 * be sufficient to guarantee that the file exists on disk, but recovery
	 * itself might remove it while replaying, for example, an
	 * XLOG_DBASE_CREATE or XLOG_TBLSPC_CREATE record.  Therefore, we need
	 * this even when wal_level=minimal.
	 */
	PageSetChecksumInplace(metapage, BTREE_METAPAGE);
	smgrextend(index->rd_smgr, MAIN_FORKNUM, BTREE_METAPAGE,
			   (char *) metapage, true);
#if PG_VERSION_NUM>=160000
	log_newpage(&index->rd_smgr->smgr_rlocator.locator, MAIN_FORKNUM,
				BTREE_METAPAGE, metapage, true);
#else
	log_newpage(&index->rd_smgr->smgr_rnode.node, MAIN_FORKNUM,
				BTREE_METAPAGE, metapage, true);
#endif
	/*
	 * An immediate sync is required even if we xlog'd the page, because the
	 * write did not go through shared_buffers and therefore a concurrent
	 * checkpoint may have moved the redo pointer past our xlog record.
	 */
	smgrimmedsync(index->rd_smgr, MAIN_FORKNUM);
	RelationCloseSmgr(index);

	return (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
}

static bool
lsm3_dummy_insert(Relation rel, Datum *values, bool *isnull,
				  ItemPointer ht_ctid, Relation heapRel,
				  IndexUniqueCheck checkUnique,
#if PG_VERSION_NUM>=140000
				  bool indexUnchanged,
#endif
				  IndexInfo *indexInfo)
{
	return false;
}

Datum
lsm3_btree_wrapper(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = BTMaxStrategyNumber;
	amroutine->amsupport = BTNProcs;
	amroutine->amoptsprocnum = BTOPTIONS_PROC;
	amroutine->amcanorder = true;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = true;
	amroutine->amsearchnulls = true;
	amroutine->amstorage = false;
	amroutine->amclusterable = true;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = true;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amparallelvacuumoptions = 0;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = lsm3_build_empty;
	amroutine->ambuildempty = btbuildempty;
	amroutine->aminsert = lsm3_dummy_insert;
	amroutine->ambulkdelete = btbulkdelete;
	amroutine->amvacuumcleanup = btvacuumcleanup;
	amroutine->amcanreturn = btcanreturn;
	amroutine->amcostestimate = btcostestimate;
	amroutine->amoptions = lsm3_options;
	amroutine->amproperty = btproperty;
	amroutine->ambuildphasename = btbuildphasename;
	amroutine->amvalidate = btvalidate;
	amroutine->ambeginscan = btbeginscan;
	amroutine->amrescan = btrescan;
	amroutine->amgettuple = btgettuple;
	amroutine->amgetbitmap = btgetbitmap;
	amroutine->amendscan = btendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * Utulity hook handling creation of Lsm3 indexes
 */
static void
lsm3_process_utility(PlannedStmt *plannedStmt,
					 const char *queryString,
#if PG_VERSION_NUM>=140000
					 bool readOnlyTree,
#endif
					 ProcessUtilityContext context,
					 ParamListInfo paramListInfo,
					 QueryEnvironment *queryEnvironment,
					 DestReceiver *destReceiver,
#if PG_VERSION_NUM>=130000
					 QueryCompletion *completionTag
#else
	                 char *completionTag
#endif
	)
{
    Node *parseTree = plannedStmt->utilityStmt;
	DropStmt* drop  = NULL;
	ObjectAddresses *drop_objects = NULL;
	List* drop_oids = NULL;
	ListCell* cell;

	Lsm3Entries = NULL; /* Reset entry to check it after utility statement execution */
	Lsm3InsideCopy = false;
	if (IsA(parseTree, DropStmt))
	{
		drop = (DropStmt*)parseTree;
		if (drop->removeType == OBJECT_INDEX)
		{
			foreach (cell, drop->objects)
			{
				RangeVar* rv = makeRangeVarFromNameList((List *) lfirst(cell));
				Relation index = relation_openrv(rv, ExclusiveLock);
				if (index->rd_indam->ambuild  == lsm3_build)
				{
					Lsm3DictEntry* entry = lsm3_get_entry(index);
					if (drop_objects == NULL)
					{
						drop_objects = new_object_addresses();
					}

					/*
					 * LSM3 MULTI-RUN LEVEL CHANGE:
					 * Drop cleanup must remove top indexes and every configured level-run index.
					 */
					for (int i = 0; i < LSM3_NUM_TOP_INDEXES; i++)
					{
						if (entry->top[i])
						{
							ObjectAddress obj;
							obj.classId = RelationRelationId;
							obj.objectId = entry->top[i];
							obj.objectSubId = 0;
							add_exact_object_address(&obj, drop_objects);
						}
					}
					for (int level_no = 0; level_no < entry->num_levels; level_no++)
					{
						for (int run_no = 0; run_no < entry->runs_per_level; run_no++)
						{
							if (entry->level[level_no][run_no])
							{
								ObjectAddress obj;
								obj.classId = RelationRelationId;
								obj.objectId = entry->level[level_no][run_no];
								obj.objectSubId = 0;
								add_exact_object_address(&obj, drop_objects);
							}
						}
					}

					drop_oids = lappend_oid(drop_oids, RelationGetRelid(index));
				}
				relation_close(index, ExclusiveLock);
			}
		}
	}
	else if (IsA(parseTree, CopyStmt))
	{
		Lsm3InsideCopy = true;
	}

	(PreviousProcessUtilityHook ? PreviousProcessUtilityHook : standard_ProcessUtility)
		(plannedStmt,
		 queryString,
#if PG_VERSION_NUM>=140000
		 readOnlyTree,
#endif
		 context,
		 paramListInfo,
		 queryEnvironment,
		 destReceiver,
		 completionTag);

	if (Lsm3Entries)
	{
		foreach (cell, Lsm3Entries)
		{
			Lsm3DictEntry* entry = (Lsm3DictEntry*)lfirst(cell);
			Oid top_index[LSM3_NUM_TOP_INDEXES];
			Oid level_index[LSM3_MAX_LEVELS][LSM3_MAX_RUNS_PER_LEVEL];

			for (int i = 0; i < LSM3_NUM_TOP_INDEXES; i++)
			{
				top_index[i] = InvalidOid;
			}
			for (int level_no = 0; level_no < LSM3_MAX_LEVELS; level_no++)
			{
				for (int run_no = 0; run_no < LSM3_MAX_RUNS_PER_LEVEL; run_no++)
				{
					level_index[level_no][run_no] = InvalidOid;
				}
			}

			if (IsA(parseTree, IndexStmt)) /* This is Lsm3 creation statement */
			{
				IndexStmt* stmt = (IndexStmt*)parseTree;
				char* originIndexName = stmt->idxname;
				char* originAccessMethod = stmt->accessMethod;

				/*
				 * LSM3 MULTI-RUN LEVEL CHANGE:
				 * Create two mutable top indexes and num_levels * runs_per_level immutable level-run indexes.
				 */
				for (int i = 0; i < LSM3_NUM_TOP_INDEXES; i++)
				{
					if (stmt->concurrent)
					{
						PushActiveSnapshot(GetTransactionSnapshot());
					}
					stmt->accessMethod = "lsm3_btree_wrapper";
					stmt->idxname = psprintf("%s_top%d", get_rel_name(entry->base), i);

					/* PG16 requires 11 arguments (added total_parts as the 6th argument) */
					top_index[i] = DefineIndex(entry->heap,
											   stmt,
											   InvalidOid,
											   InvalidOid,
											   InvalidOid,
											   -1,      /* NEW IN PG16: total_parts */
											   false,
											   false,
											   false,
											   false,
											   true).objectId;
				}

				for (int level_no = 0; level_no < entry->num_levels; level_no++)
				{
					for (int run_no = 0; run_no < entry->runs_per_level; run_no++)
					{
						if (stmt->concurrent)
						{
							PushActiveSnapshot(GetTransactionSnapshot());
						}
						stmt->accessMethod = "lsm3_btree_wrapper";
						stmt->idxname = psprintf("%s_level%d_run%d", get_rel_name(entry->base), level_no, run_no);

						/* PG16 requires 11 arguments (added total_parts as the 6th argument) */
						level_index[level_no][run_no] = DefineIndex(entry->heap,
														 stmt,
														 InvalidOid,
														 InvalidOid,
														 InvalidOid,
														 -1,      /* NEW IN PG16: total_parts */
														 false,
														 false,
														 false,
														 false,
														 true).objectId;
					}
				}

				stmt->accessMethod = originAccessMethod;
				stmt->idxname = originIndexName;
			}
			else
			{
				/*
				 * LSM3 MULTI-RUN LEVEL CHANGE:
				 * For non-creation utility paths, recover all auxiliary index OIDs by name.
				 */
				for (int i = 0; i < LSM3_NUM_TOP_INDEXES; i++)
				{
					top_index[i] = entry->top[i];
					if (top_index[i] == InvalidOid)
					{
						char* topidxname = psprintf("%s_top%d", get_rel_name(entry->base), i);
						top_index[i] = get_relname_relid(topidxname, get_rel_namespace(entry->base));
						if (top_index[i] == InvalidOid)
						{
							elog(ERROR, "Lsm3: failed to lookup %s index", topidxname);
						}
					}
				}
				for (int level_no = 0; level_no < entry->num_levels; level_no++)
				{
					for (int run_no = 0; run_no < entry->runs_per_level; run_no++)
					{
						level_index[level_no][run_no] = entry->level[level_no][run_no];
						if (level_index[level_no][run_no] == InvalidOid)
						{
							char* levelidxname = psprintf("%s_level%d_run%d", get_rel_name(entry->base), level_no, run_no);
							level_index[level_no][run_no] = get_relname_relid(levelidxname, get_rel_namespace(entry->base));
							if (level_index[level_no][run_no] == InvalidOid)
							{
								elog(ERROR, "Lsm3: failed to lookup %s index", levelidxname);
							}
						}
					}
				}
			}
			if (ActiveSnapshotSet())
			{
				PopActiveSnapshot();
			}
			CommitTransactionCommand();
			StartTransactionCommand();

			/*
			 * LSM3 MULTI-RUN LEVEL CHANGE:
			 * Mark every auxiliary index invalid so the planner never chooses top/run indexes directly.
			 */
			for (int i = 0; i < LSM3_NUM_TOP_INDEXES; i++)
			{
				index_set_state_flags(top_index[i], INDEX_DROP_CLEAR_VALID);
			}
			for (int level_no = 0; level_no < entry->num_levels; level_no++)
			{
				for (int run_no = 0; run_no < entry->runs_per_level; run_no++)
				{
					index_set_state_flags(level_index[level_no][run_no], INDEX_DROP_CLEAR_VALID);
				}
			}

			SpinLockAcquire(&entry->spinlock);
			for (int i = 0; i < LSM3_NUM_TOP_INDEXES; i++)
			{
				entry->top[i] = top_index[i];
			}
			for (int level_no = 0; level_no < entry->num_levels; level_no++)
			{
				entry->level_run_count[level_no] = 0;
				for (int run_no = 0; run_no < entry->runs_per_level; run_no++)
				{
					entry->level[level_no][run_no] = level_index[level_no][run_no];
				}
			}
			SpinLockRelease(&entry->spinlock);
			{
				Relation index = index_open(entry->base, AccessShareLock);
				index->rd_rel->relam = entry->am_id;
				index_close(index, AccessShareLock);
			}
		}
		list_free(Lsm3Entries);
		Lsm3Entries = NULL;
	}
	else if (drop_objects)
	{
		performMultipleDeletions(drop_objects, drop->behavior, 0);
		LWLockAcquire(Lsm3DictLock, LW_EXCLUSIVE);
		foreach (cell, drop_oids)
		{
			hash_search(Lsm3Dict, &lfirst_oid(cell), HASH_REMOVE, NULL);
		}
		LWLockRelease(Lsm3DictLock);
	}
}


/*
 * Executor finish hook to reclaim released locks on non-active top indexes
 * to avoid "you don't own a lock of type RowExclusiveLock" warning
 */
static void
lsm3_executor_finish(QueryDesc *queryDesc)
{
	lsm3_reacquire_locks();
	Lsm3InsideCopy = false;
	if (PreviousExecutorFinish)
		PreviousExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);

}


void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "Lsm3: this extension should be loaded via shared_preload_libraries");
	}
	DefineCustomIntVariable("lsm3.top_index_size",
                            "Size of top index B-Tree (kb)",
							NULL,
							&Lsm3TopIndexSize,
							64*1024,
							BLCKSZ/1024,
							INT_MAX,
							PGC_SIGHUP,
							GUC_UNIT_KB,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("lsm3.max_indexes",
                            "Maximal number of Lsm3 indexes.",
							NULL,
							&Lsm3MaxIndexes,
							1024,
							1,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	Lsm3ReloptKind = add_reloption_kind();

	add_bool_reloption(Lsm3ReloptKind, "unique",
					   "Index contains no duplicates",
					   false, AccessExclusiveLock);
	add_int_reloption(Lsm3ReloptKind, "top_index_size",
					  "Size of top index (kb)",
					  0, 0, INT_MAX, AccessExclusiveLock);

	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * num_levels controls the number of middle levels; runs_per_level controls how many immutable B-tree runs
	 * each level may hold before tiered compaction; ratio controls size-based compaction thresholds.
	 */
	add_int_reloption(Lsm3ReloptKind, "num_levels",
					  "Number of intermediate LSM levels",
					  LSM3_DEFAULT_LEVELS, 1, LSM3_MAX_LEVELS, AccessExclusiveLock);
	add_int_reloption(Lsm3ReloptKind, "runs_per_level",
					  "Number of immutable B-tree runs per LSM level",
					  LSM3_DEFAULT_RUNS_PER_LEVEL, 1, LSM3_MAX_RUNS_PER_LEVEL, AccessExclusiveLock);
	add_int_reloption(Lsm3ReloptKind, "level_size_ratio",
					  "Size multiplier between LSM levels",
					  LSM3_DEFAULT_LEVEL_SIZE_RATIO, 2, INT_MAX, AccessExclusiveLock);

	/* bloom injection: opt-in/out switch for backend-local per-component Bloom filters. */
	add_bool_reloption(Lsm3ReloptKind, "bloom_enabled",
					   "Use per-component Bloom filters for equality lookups on immutable level runs",
					   true, AccessExclusiveLock);

	add_int_reloption(Lsm3ReloptKind, "fillfactor",
					  "Packs btree index pages only to this percentage",
					  BTREE_DEFAULT_FILLFACTOR, BTREE_MIN_FILLFACTOR, 100, ShareUpdateExclusiveLock);
	add_real_reloption(Lsm3ReloptKind, "vacuum_cleanup_index_scale_factor",
					  "Packs btree index pages only to this percentage",
					  -1, 0.0, 1e10, ShareUpdateExclusiveLock);
	add_bool_reloption(Lsm3ReloptKind, "deduplicate_items",
					   "Enables \"deduplicate items\" feature for this btree index",
					   true, AccessExclusiveLock);

	PreviousShmemStartupHook = shmem_startup_hook;
	shmem_startup_hook = lsm3_shmem_startup;

	PreviousShmemRequestHook = shmem_request_hook;
	shmem_request_hook = lsm3_shmem_request;

	// lsm3_shmem_request();
	// RequestAddinShmemSpace(hash_estimate_size(Lsm3MaxIndexes, sizeof(Lsm3DictEntry)));
	// RequestNamedLWLockTranche("lsm3", 1);

	PreviousProcessUtilityHook = ProcessUtility_hook;
    ProcessUtility_hook = lsm3_process_utility;

	PreviousExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = lsm3_executor_finish;
}

Datum
lsm3_get_merge_count(PG_FUNCTION_ARGS)
{
	Oid	relid = PG_GETARG_OID(0);
	Relation index = index_open(relid, AccessShareLock);
	Lsm3DictEntry* entry = lsm3_get_entry(index);
	index_close(index, AccessShareLock);
	if (entry == NULL)
		PG_RETURN_NULL();
	else
		PG_RETURN_INT64(entry->n_merges);
}


Datum
lsm3_start_merge(PG_FUNCTION_ARGS)
{
	/*
	 * LSM3 MULTI-RUN LEVEL CHANGE:
	 * Manual merge now triggers the same top -> level0 -> cascade path as automatic overflow.
	 */
	Oid	relid = PG_GETARG_OID(0);
	Relation index = index_open(relid, AccessShareLock);
	Lsm3DictEntry* entry = lsm3_get_entry(index);
	index_close(index, AccessShareLock);

	SpinLockAcquire(&entry->spinlock);
	if (!entry->merge_in_progress)
	{
		entry->merge_in_progress = true;
		entry->active_index ^= 1;
		entry->n_merges += 1;
		if (entry->access_count[1-entry->active_index] == 0)
		{
			entry->start_merge = true;
			if (entry->merger == NULL) /* lazy start of bgworker */
			{
				lsm3_launch_bgworker(entry);
			}
			SetLatch(&entry->merger->procLatch);
		}
	}
	SpinLockRelease(&entry->spinlock);
	PG_RETURN_NULL();
}

Datum
lsm3_wait_merge_completion(PG_FUNCTION_ARGS)
{
	Oid	relid = PG_GETARG_OID(0);
	Relation index = index_open(relid, AccessShareLock);
	Lsm3DictEntry* entry = lsm3_get_entry(index);
	index_close(index, AccessShareLock);

	while (entry->merge_in_progress)
	{
		pg_usleep(1000000); /* one second */
	}
	PG_RETURN_NULL();
}

Datum
lsm3_top_index_size(PG_FUNCTION_ARGS)
{
	Oid	relid = PG_GETARG_OID(0);
	Relation index = index_open(relid, AccessShareLock);
	Lsm3DictEntry* entry = lsm3_get_entry(index);
	index_close(index, AccessShareLock);
	/*
	 * LSM3 CLEANUP CHANGE:
	 * Return the active top index size directly; the previous nested lsm3_get_index_size() call used a block count as an OID.
	 */
	PG_RETURN_INT64((uint64)lsm3_get_index_size(entry->top[entry->active_index]) * BLCKSZ);
}
