/* index.c - routines for dealing with attribute indexes */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-bdb/key.c,v 1.7 2002/01/04 20:17:49 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "back-bdb.h"
#include "idl.h"

/* read a key */
int
bdb_key_read(
	Backend	*be,
	DB *db,
	DB_TXN *txn,
	struct berval *k,
	ID *ids
)
{
	int rc;
	DBT key;

#ifdef NEW_LOGGING
	LDAP_LOG(( "index", LDAP_LEVEL_ENTRY,
		"key_read: enter\n" ));
#else
	Debug( LDAP_DEBUG_TRACE, "=> key_read\n", 0, 0, 0 );
#endif

	DBTzero( &key );
	bv2DBT(k,&key);

	rc = bdb_idl_fetch_key( be, db, txn, &key, ids );

	if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "index", LDAP_LEVEL_ENTRY,
			"bdb_key_read: failed (%d)\n",
			rc ));
#else
		Debug( LDAP_DEBUG_TRACE, "<= bdb_index_read: failed (%d)\n",
			rc, 0, 0 );
#endif
	} else {
#ifdef NEW_LOGGING
		LDAP_LOG(( "index", LDAP_LEVEL_ENTRY,
			"bdb_key_read: %ld candidates\n", (long) BDB_IDL_N(ids) ));
#else
		Debug( LDAP_DEBUG_TRACE, "<= bdb_index_read %ld candidates\n",
			(long) BDB_IDL_N(ids), 0, 0 );
#endif
	}

	return rc;
}

/* Add or remove stuff from index files */
int
bdb_key_change(
	Backend *be,
	DB *db,
	DB_TXN *txn,
	struct berval *k,
	ID id,
	int op
)
{
	int	rc;
	DBT	key;

#ifdef NEW_LOGGING
	LDAP_LOG(( "index", LDAP_LEVEL_ENTRY,
		"key_change: %s ID %lx\n",
		op == SLAP_INDEX_ADD_OP ? "Add" : "Delete", (long) id ));
#else
	Debug( LDAP_DEBUG_TRACE, "=> key_change(%s,%lx)\n",
		op == SLAP_INDEX_ADD_OP ? "ADD":"DELETE", (long) id, 0 );
#endif

	DBTzero( &key );
	bv2DBT(k,&key);

	if (op == SLAP_INDEX_ADD_OP) {
		/* Add values */
		rc = bdb_idl_insert_key( be, db, txn, &key, id );

	} else {
		/* Delete values */
		rc = bdb_idl_delete_key( be, db, txn, &key, id );
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "index", LDAP_LEVEL_ENTRY,
		"key_change: return %d\n", rc ));
#else
	Debug( LDAP_DEBUG_TRACE, "<= key_change %d\n", rc, 0, 0 );
#endif

	return rc;
}
