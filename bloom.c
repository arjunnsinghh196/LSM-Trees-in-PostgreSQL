#include "postgres.h"

#include "bloom.h"

#include "access/nbtree.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "storage/block.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

/*
 * bloom injection:
 * Backend-local cache entry for one physical immutable LSM component.
 * The cache is safe because lsm3.c passes a generation counter that is bumped
 * whenever the corresponding level-run component is rewritten or truncated.
 */
typedef struct Lsm3BloomCacheEntry
{
	Oid relid;                 /* hash key: physical component OID */
	uint64 generation;         /* shared generation observed when this filter was built */
	BlockNumber nblocks;       /* physical size observed when this filter was built */
	bool valid;
	uint8 bits[LSM3_BLOOM_BYTES];
} Lsm3BloomCacheEntry;

/* bloom injection: per-backend cache; correctness is protected by size+generation validation. */
static HTAB *Lsm3BloomCache = NULL;

/* bloom injection: small FNV-1a hash over raw Datum bytes for by-value first-column keys. */
static uint64
lsm3_bloom_hash_bytes(const void *key, Size len)
{
	uint64 hash = UINT64CONST(14695981039346656037);
	const unsigned char *p = (const unsigned char *) key;

	for (Size i = 0; i < len; i++)
	{
		hash ^= p[i];
		hash *= UINT64CONST(1099511628211);
	}
	return hash;
}

/* bloom injection: splitmix-style mixer gives independent-looking derived hash values. */
static uint64
lsm3_bloom_mix64(uint64 x)
{
	x += UINT64CONST(0x9E3779B97F4A7C15);
	x = (x ^ (x >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	x = (x ^ (x >> 27)) * UINT64CONST(0x94D049BB133111EB);
	return x ^ (x >> 31);
}

/* bloom injection: obtain/create the backend-local Bloom cache. */
static HTAB *
lsm3_bloom_cache(void)
{
	HASHCTL ctl;
	MemoryContext old_context;

	if (Lsm3BloomCache)
		return Lsm3BloomCache;

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(Lsm3BloomCacheEntry);

	old_context = MemoryContextSwitchTo(TopMemoryContext);
	Lsm3BloomCache = hash_create("lsm3 bloom cache", 128, &ctl, HASH_ELEM | HASH_BLOBS);
	MemoryContextSwitchTo(old_context);

	return Lsm3BloomCache;
}

/* bloom injection: only by-value first-key datums are supported in this prototype. */
static bool
lsm3_bloom_first_key_supported(Relation index)
{
	TupleDesc desc = RelationGetDescr(index);
	Form_pg_attribute attr;
	int16 typlen;
	bool typbyval;

	if (desc == NULL || desc->natts < 1)
		return false;

	attr = TupleDescAttr(desc, 0);
	if (attr->attisdropped)
		return false;

	get_typlenbyval(attr->atttypid, &typlen, &typbyval);
	return typbyval && typlen > 0 && typlen <= (int16) sizeof(Datum);
}

/* bloom injection: equality-only extraction from PostgreSQL scan keys. */
bool
lsm3_bloom_extract_equality_key(IndexScanDesc scan, Datum *key)
{
	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey skey = &scan->keyData[i];

		if (skey->sk_attno == 1 &&
			skey->sk_strategy == BTEqualStrategyNumber &&
			(skey->sk_flags & SK_ISNULL) == 0)
		{
			*key = skey->sk_argument;
			return true;
		}
	}
	return false;
}

/* bloom injection: add a raw by-value Datum payload to a complete component Bloom filter. */
static void
lsm3_bloom_add_datum(Lsm3BloomCacheEntry *filter, Datum datum)
{
	uint64 value = (uint64) datum;
	uint64 base = lsm3_bloom_hash_bytes(&value, sizeof(value));
	uint64 total_bits = (uint64) LSM3_BLOOM_BYTES * 8;

	for (int i = 0; i < LSM3_BLOOM_HASHES; i++)
	{
		uint64 h = lsm3_bloom_mix64(base + (uint64) i);
		uint64 bit = h % total_bits;
		filter->bits[bit / 8] |= (uint8) (1U << (bit % 8));
	}
}

/* bloom injection: check a raw by-value Datum payload against a complete component Bloom filter. */
static bool
lsm3_bloom_check_datum(Lsm3BloomCacheEntry *filter, Datum datum)
{
	uint64 value = (uint64) datum;
	uint64 base = lsm3_bloom_hash_bytes(&value, sizeof(value));
	uint64 total_bits = (uint64) LSM3_BLOOM_BYTES * 8;

	for (int i = 0; i < LSM3_BLOOM_HASHES; i++)
	{
		uint64 h = lsm3_bloom_mix64(base + (uint64) i);
		uint64 bit = h % total_bits;
		if ((filter->bits[bit / 8] & (uint8) (1U << (bit % 8))) == 0)
			return false;
	}
	return true;
}

/* bloom injection: build/rebuild a complete Bloom filter for one immutable level-run component. */
static Lsm3BloomCacheEntry *
lsm3_bloom_get_or_build(Relation index, Oid heap_oid, uint64 generation)
{
	HTAB *cache;
	Lsm3BloomCacheEntry *filter;
	Relation heap;
	IndexScanDesc scan;
	BlockNumber nblocks;
	Oid relid;
	bool found;
	bool ok;

	if (!lsm3_bloom_first_key_supported(index))
		return NULL;

	relid = RelationGetRelid(index);
	nblocks = RelationGetNumberOfBlocks(index);
	cache = lsm3_bloom_cache();
	filter = (Lsm3BloomCacheEntry *) hash_search(cache, &relid, HASH_ENTER, &found);

	if (found && filter->valid && filter->nblocks == nblocks && filter->generation == generation)
		return filter;

	filter->relid = relid;
	filter->nblocks = nblocks;
	filter->generation = generation;
	filter->valid = false;
	MemSet(filter->bits, 0, sizeof(filter->bits));

	heap = table_open(heap_oid, AccessShareLock);
	scan = index_beginscan(heap, index, SnapshotAny, 0, 0);
	scan->xs_want_itup = true;
	btrescan(scan, NULL, 0, 0, 0);

	for (ok = _bt_first(scan, ForwardScanDirection); ok; ok = _bt_next(scan, ForwardScanDirection))
	{
		bool isnull;
		Datum datum = index_getattr(scan->xs_itup, 1, RelationGetDescr(index), &isnull);
		if (!isnull)
			lsm3_bloom_add_datum(filter, datum);
	}

	index_endscan(scan);
	table_close(heap, AccessShareLock);

	filter->valid = true;
	return filter;
}

/*
 * bloom injection:
 * Public membership API used by lsm3.c.  This function is conservative: it
 * returns false only for a guaranteed Bloom miss; it returns true if Bloom is
 * unsupported, absent, stale, or a possible hit.
 */
bool
lsm3_bloom_might_contain_relation(Relation index_rel, Oid heap_oid, uint64 generation, Datum key)
{
	Lsm3BloomCacheEntry *filter;

	filter = lsm3_bloom_get_or_build(index_rel, heap_oid, generation);
	if (filter == NULL)
		return true;

	if (!lsm3_bloom_check_datum(filter, key))
	{
		elog(DEBUG1, "Lsm3 bloom injection: skipping %s for absent equality key",
			 RelationGetRelationName(index_rel));
		return false;
	}

	return true;
}
