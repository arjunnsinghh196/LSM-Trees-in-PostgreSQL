#ifndef LSM3_BLOOM_H
#define LSM3_BLOOM_H

#include "postgres.h"
#include "access/relscan.h"
#include "utils/rel.h"
#include "access/genam.h"  /* bloom injection: defines IndexScanDesc for split Bloom module. */
#include "access/skey.h"
/*
 * bloom injection:
 * Separate Bloom helper module for LSM3.  The main access method decides which
 * physical component is safe to skip; this module owns hashing, cache storage,
 * complete-filter rebuilds, and equality-key extraction.
 */
#define LSM3_BLOOM_BYTES (64 * 1024)
#define LSM3_BLOOM_HASHES 4

/* bloom injection: extract an equality probe on the first index key, if present. */
extern bool lsm3_bloom_extract_equality_key(IndexScanDesc scan, Datum *key);

/*
 * bloom injection:
 * Returns false only when the complete Bloom filter for index_rel proves the key
 * is absent.  A true return means "maybe present" or "Bloom unsupported".
 */
extern bool lsm3_bloom_might_contain_relation(Relation index_rel,
											 Oid heap_oid,
											 uint64 generation,
											 Datum key);

#endif /* LSM3_BLOOM_H */
