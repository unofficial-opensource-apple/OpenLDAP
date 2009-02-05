/* back-bdb.h - ldap ldbm back-end header file */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-bdb/idl.h,v 1.11 2002/01/30 23:10:24 kurt Exp $ */
/*
 * Copyright 2000-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#ifndef _BDB_IDL_H_
#define _BDB_IDL_H_

#include <portable.h>

#include "slap.h"

/* IDL sizes - likely should be even bigger
 *   limiting factors: sizeof(ID), thread stack size
 *
 * Note: the -2 stuff is intended to reduce the size
 * just enough to avoiding internal (to malloc) allocation
 * of overly large blocks to provide the requested size. 
 * The stuff needs to be thought out better, doesn't deal
 * well with mixed stack/malloc allocation of IDLs.
 */
#define BDB_IDL_DB_SIZE		((1<<16)-2) /* 32K IDL on disk */
#define BDB_IDL_UM_SIZE		((1<<17)-2) /* 64K IDL in memory */
#define BDB_IDL_UM_SIZEOF	(BDB_IDL_UM_SIZE * sizeof(ID))

#define BDB_IDL_DB_MAX		(BDB_IDL_DB_SIZE-2)
/* #define BDB_IDL_DB_ALLOC	(BDB_IDL_DB_SIZE * sizeof(ID)) */

#define BDB_IDL_UM_MAX			(BDB_IDL_UM_SIZE-2)
/* #define BDB_IDL_UM_ALLOC	(BDB_IDL_UM_SIZE * sizeof(ID)) */

#define BDB_IDL_IS_RANGE(ids)	((ids)[0] == NOID)
#define BDB_IDL_RANGE_SIZE		(3)
#define BDB_IDL_RANGE_SIZEOF	(BDB_IDL_RANGE_SIZE * sizeof(ID))
#define BDB_IDL_SIZEOF(ids)		((BDB_IDL_IS_RANGE(ids) \
	? BDB_IDL_RANGE_SIZE : ((ids)[0]+1)) * sizeof(ID))

#define BDB_IDL_RANGE_FIRST(ids)	((ids)[1])
#define BDB_IDL_RANGE_LAST(ids)		((ids)[2])

#define BDB_IDL_RANGE( ids, f, l ) \
	do { \
		(ids)[0] = NOID; \
		(ids)[1] = (f);  \
		(ids)[2] = (l);  \
	} while(0)

#define BDB_IDL_ZERO(ids) \
	do { \
		(ids)[0] = 0; \
		(ids)[1] = 0; \
		(ids)[2] = 0; \
	} while(0)

#define BDB_IDL_IS_ZERO(ids) ( (ids)[0] == 0 )
#define BDB_IDL_IS_ALL( range, ids ) ( (ids)[0] == NOID \
	&& (ids)[1] <= (range)[1] && (range)[2] <= (ids)[2] )

#define BDB_IDL_CPY( dst, src ) (AC_MEMCPY( dst, src, BDB_IDL_SIZEOF( src ) ))

#define BDB_IDL_ID( bdb, ids, id ) BDB_IDL_RANGE( ids, id, ((bdb)->bi_lastid) )
#define BDB_IDL_ALL( bdb, ids ) BDB_IDL_RANGE( ids, 1, ((bdb)->bi_lastid) )

#define BDB_IDL_FIRST( ids )	( ids[1] )
#define BDB_IDL_LAST( ids )		( BDB_IDL_IS_RANGE(ids) \
	? ids[2] : ids[ids[0]] )

#define BDB_IDL_N( ids )		( BDB_IDL_IS_RANGE(ids) \
	? (ids[2]-ids[1])+1 : ids[0] )

LDAP_BEGIN_DECL
LDAP_END_DECL

#endif
