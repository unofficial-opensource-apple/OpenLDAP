/* dbcache.c - manage cache of open databases */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-bdb/dbcache.c,v 1.15 2002/01/04 20:17:49 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/errno.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>
#include <sys/stat.h>

#include "slap.h"
#include "back-bdb.h"
#include "lutil_hash.h"

/* Pass-thru hash function. Since the indexer is already giving us hash
 * values as keys, we don't need BDB to re-hash them.
 */
#if LUTIL_HASH_BYTES == 4
static u_int32_t
bdb_db_hash(
	DB *db,
	const void *bytes,
	u_int32_t length
)
{
	u_int32_t *ret = (u_int32_t *)bytes;
	return *ret;
}
#endif

int
bdb_db_cache(
	Backend	*be,
	const char *name,
	DB **dbout )
{
	int i;
	int rc;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	struct bdb_db_info *db;
	char *file;

	*dbout = NULL;

	for( i=BDB_NDB; bdb->bi_databases[i]; i++ ) {
		if( !strcmp( bdb->bi_databases[i]->bdi_name, name) ) {
			*dbout = bdb->bi_databases[i]->bdi_db;
			return 0;
		}
	}

	ldap_pvt_thread_mutex_lock( &bdb->bi_database_mutex );

	/* check again! may have been added by another thread */
	for( i=BDB_NDB; bdb->bi_databases[i]; i++ ) {
		if( !strcmp( bdb->bi_databases[i]->bdi_name, name) ) {
			*dbout = bdb->bi_databases[i]->bdi_db;
			ldap_pvt_thread_mutex_unlock( &bdb->bi_database_mutex );
			return 0;
		}
	}

	if( i >= BDB_INDICES ) {
		ldap_pvt_thread_mutex_unlock( &bdb->bi_database_mutex );
		return -1;
	}

	db = (struct bdb_db_info *) ch_calloc(1, sizeof(struct bdb_db_info));

	db->bdi_name = ch_strdup( name );

	rc = db_create( &db->bdi_db, bdb->bi_dbenv, 0 );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_ANY,
			"bdb_db_cache: db_create(%s) failed: %s (%d)\n",
			bdb->bi_dbenv_home, db_strerror(rc), rc );
		ldap_pvt_thread_mutex_unlock( &bdb->bi_database_mutex );
		return rc;
	}

	rc = db->bdi_db->set_pagesize( db->bdi_db, BDB_PAGESIZE );
#if LUTIL_HASH_BYTES == 4
	rc = db->bdi_db->set_h_hash( db->bdi_db, bdb_db_hash );
#endif
#ifdef BDB_IDL_MULTI
	rc = db->bdi_db->set_flags( db->bdi_db, DB_DUP | DB_DUPSORT );
	rc = db->bdi_db->set_dup_compare( db->bdi_db, bdb_bt_compare );
#endif

	file = ch_malloc( strlen( name ) + sizeof(BDB_SUFFIX) );
	sprintf( file, "%s" BDB_SUFFIX, name );

	rc = db->bdi_db->open( db->bdi_db,
		file, name,
		DB_HASH, bdb->bi_db_opflags | DB_CREATE | DB_THREAD,
		bdb->bi_dbenv_mode );

	ch_free( file );

	if( rc != 0 ) {
		Debug( LDAP_DEBUG_ANY,
			"bdb_db_cache: db_open(%s) failed: %s (%d)\n",
			name, db_strerror(rc), rc );
		ldap_pvt_thread_mutex_unlock( &bdb->bi_database_mutex );
		return rc;
	}

	bdb->bi_databases[i+1] = NULL;
	bdb->bi_databases[i] = db;
	bdb->bi_ndatabases = i+1;

	*dbout = db->bdi_db;

	ldap_pvt_thread_mutex_unlock( &bdb->bi_database_mutex );
	return 0;
}
