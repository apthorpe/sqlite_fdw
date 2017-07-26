/*-------------------------------------------------------------------------
 *
 * shippable.c
 *	  Determine which database objects are shippable to a remote server.
 *
 * We need to determine whether particular functions, operators, and indeed
 * data types are shippable to a remote server for execution --- that is,
 * do they exist and have the same behavior remotely as they do locally?
 * Built-in objects are generally considered shippable.  Other objects can
 * be shipped if they are white-listed by the user.
 *
 * Note: there are additional filter rules that prevent shipping mutable
 * functions or functions using nonportable collations.  Those considerations
 * need not be accounted for here.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/postgres_fdw/shippable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_namespace.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "nodes/relation.h"
#include "nodes/execnodes.h"


#include "sqlite_private.h"


/* Hash table for caching the results of shippability lookups */
static HTAB *ShippableCacheHash = NULL;

/*
 * Hash key for shippability lookups.  We include the FDW server OID because
 * decisions may differ per-server.  Otherwise, objects are identified by
 * their (local!) OID and catalog OID.
 */
typedef struct
{
	/* XXX we assume this struct contains no padding bytes */
	Oid			objid;			/* function/operator/type OID */
	Oid			classid;		/* OID of its catalog (pg_proc, etc) */
	Oid			serverid;		/* FDW server we are concerned with */
} ShippableCacheKey;

typedef struct
{
	ShippableCacheKey key;		/* hash key - must be first */
	bool		shippable;
} ShippableCacheEntry;


/*
 * Flush cache entries when pg_foreign_server is updated.
 *
 * We do this because of the possibility of ALTER SERVER being used to change
 * a server's extensions option.  We do not currently bother to check whether
 * objects' extension membership changes once a shippability decision has been
 * made for them, however.
 */
static void
InvalidateShippableCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	ShippableCacheEntry *entry;

	/*
	 * In principle we could flush only cache entries relating to the
	 * pg_foreign_server entry being outdated; but that would be more
	 * complicated, and it's probably not worth the trouble.  So for now, just
	 * flush all entries.
	 */
	hash_seq_init(&status, ShippableCacheHash);
	while ((entry = (ShippableCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (hash_search(ShippableCacheHash,
						(void *) &entry->key,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

/*
 * Initialize the backend-lifespan cache of shippability decisions.
 */
static void
InitializeShippableCache(void)
{
	HASHCTL		ctl;

	/* Create the hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(ShippableCacheKey);
	ctl.entrysize = sizeof(ShippableCacheEntry);
	ShippableCacheHash =
		hash_create("Shippability cache", 256, &ctl, HASH_ELEM | HASH_BLOBS);

	/* Set up invalidation callback on pg_foreign_server. */
	CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
								  InvalidateShippableCacheCallback,
								  (Datum) 0);
}

/*
 * Returns true if given object (operator/function/type) is shippable
 * according to the server options.
 *
 * Right now "shippability" is exclusively a function of whether the object
 * belongs to an extension declared by the user.  In the future we could
 * additionally have a whitelist of functions/operators declared one at a time.
 */
static bool
lookup_shippable(Oid objectId, Oid classId, SqliteFdwRelationInfo *fpinfo)
{
	Oid			extensionOid;

	/*
	 * Is object a member of some extension?  (Note: this is a fairly
	 * expensive lookup, which is why we try to cache the results.)
	 */
	extensionOid = getExtensionOfObject(classId, objectId);

	/* If so, is that extension in fpinfo->shippable_extensions? */
	if (OidIsValid(extensionOid) &&
		list_member_oid(fpinfo->shippable_extensions, extensionOid))
		return true;

	return false;
}

/*
 * Return true if given object is one of PostgreSQL's built-in objects.
 *
 * We use FirstBootstrapObjectId as the cutoff, so that we only consider
 * objects with hand-assigned OIDs to be "built in", not for instance any
 * function or type defined in the information_schema.
 *
 * Our constraints for dealing with types are tighter than they are for
 * functions or operators: we want to accept only types that are in pg_catalog,
 * else deparse_type_name might incorrectly fail to schema-qualify their names.
 * Thus we must exclude information_schema types.
 *
 * XXX there is a problem with this, which is that the set of built-in
 * objects expands over time.  Something that is built-in to us might not
 * be known to the remote server, if it's of an older version.  But keeping
 * track of that would be a huge exercise.
 */
bool
is_builtin(Oid objectId)
{
	return (objectId < FirstBootstrapObjectId);
}

/*
 * is_shippable
 *	   Is this object (function/operator/type) shippable to foreign server?
 */
bool
is_shippable(Oid objectId, Oid classId, SqliteFdwRelationInfo *fpinfo)
{
	ShippableCacheKey key;
	ShippableCacheEntry *entry;

	/* Built-in objects are presumed shippable. */
	if (is_builtin(objectId))
		return true;

	/* Otherwise, give up if user hasn't specified any shippable extensions. */
	if (fpinfo->shippable_extensions == NIL)
		return false;

	/* Initialize cache if first time through. */
	if (!ShippableCacheHash)
		InitializeShippableCache();

	/* Set up cache hash key */
	key.objid = objectId;
	key.classid = classId;
	key.serverid = 0;

	/* See if we already cached the result. */
	entry = (ShippableCacheEntry *)
		hash_search(ShippableCacheHash,
					(void *) &key,
					HASH_FIND,
					NULL);

	if (!entry)
	{
		/* Not found in cache, so perform shippability lookup. */
		bool		shippable = lookup_shippable(objectId, classId, fpinfo);

		/*
		 * Don't create a new hash entry until *after* we have the shippable
		 * result in hand, as the underlying catalog lookups might trigger a
		 * cache invalidation.
		 */
		entry = (ShippableCacheEntry *)
			hash_search(ShippableCacheHash,
						(void *) &key,
						HASH_ENTER,
						NULL);

		entry->shippable = shippable;
	}

	return entry->shippable;
}


static bool
is_inarray(char const *teststr, char const **array, int len)
{
    for(int i = 0; i < len; i++)
        if (strcmp(teststr, array[i]) == 0)
            return true;
    return false;
}


static bool
is_allowed_name(Oid funcid, char const *allowed_names[], size_t len)
{
    HeapTuple proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
    Form_pg_proc procform;
    bool isvalid = false;
    
    if (!HeapTupleIsValid(proctup))
        elog(ERROR, "cache lookup failed for function %u", funcid);
    
    procform = (Form_pg_proc) GETSTRUCT(proctup);
    if (procform->pronamespace == PG_CATALOG_NAMESPACE) {
        isvalid = is_inarray(
            NameStr(procform->proname), 
            allowed_names, 
            len);
    }
    
    ReleaseSysCache(proctup);
    return isvalid;
}


bool
is_shippable_agg(Oid funcid)
{
    char const *allowed_names[] = {"avg", "average", "max", "min", "sum"};
    return is_allowed_name(funcid, allowed_names, 
                           sizeof(allowed_names) / sizeof(allowed_names[0]));
}

    
bool
is_shippable_func(Oid funcid)
{
    char const *allowed_names[] = {"abs", "coalesce"};
    return is_allowed_name(funcid, allowed_names, 
                           sizeof(allowed_names) / sizeof(allowed_names[0]));
}

    
bool
is_shippable_op(Oid funcid)
{
    char const *allowed_names[] = {"avg", "average", "max", "min", "sum"};
    return is_allowed_name(funcid, allowed_names, 
                           sizeof(allowed_names) / sizeof(allowed_names[0]));
}
