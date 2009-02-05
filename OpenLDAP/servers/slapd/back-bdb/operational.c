/* operational.c - bdb backend operational attributes function */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-bdb/operational.c,v 1.16.2.4 2004/11/24 04:07:17 hyc Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2000-2004 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "back-bdb.h"
#include "external.h"

/*
 * sets *hasSubordinates to LDAP_COMPARE_TRUE/LDAP_COMPARE_FALSE
 * if the entry has children or not.
 */
int
bdb_hasSubordinates(
	Operation	*op,
	Entry		*e,
	int		*hasSubordinates )
{
	int		rc;
	
	assert( e );

	/* NOTE: this should never happen, but it actually happens
	 * when using back-relay; until we find a better way to
	 * preserve entry's private information while rewriting it,
	 * let's disable the hasSubordinate feature for back-relay.
	 */
	if ( BEI( e ) == NULL ) {
		return LDAP_OTHER;
	}

retry:
	/* FIXME: we can no longer assume the entry's e_private
	 * field is correctly populated; so we need to reacquire
	 * it with reader lock */
	rc = bdb_cache_children( op, NULL, e );
	
	switch( rc ) {
	case DB_LOCK_DEADLOCK:
	case DB_LOCK_NOTGRANTED:
		ldap_pvt_thread_yield();
		goto retry;

	case 0:
		*hasSubordinates = LDAP_COMPARE_TRUE;
		break;

	case DB_NOTFOUND:
		*hasSubordinates = LDAP_COMPARE_FALSE;
		rc = LDAP_SUCCESS;
		break;

	default:
#ifdef NEW_LOGGING
		LDAP_LOG ( OPERATION, ERR, 
			"=> bdb_hasSubordinates: has_children failed: %s (%d)\n",
			db_strerror(rc), rc, 0 );
#else
		Debug(LDAP_DEBUG_ARGS, 
			"<=- " LDAP_XSTRING(bdb_hasSubordinates)
			": has_children failed: %s (%d)\n", 
			db_strerror(rc), rc, 0 );
#endif
		rc = LDAP_OTHER;
	}

	return rc;
}

/*
 * sets the supported operational attributes (if required)
 */
int
bdb_operational(
	Operation	*op,
	SlapReply	*rs,
	int		opattrs,
	Attribute	**a )
{
	Attribute	**aa = a;
	
	assert( rs->sr_entry );

	if ( opattrs || ad_inlist( slap_schema.si_ad_hasSubordinates, rs->sr_attrs ) ) {
		int	hasSubordinates;

		rs->sr_err = bdb_hasSubordinates( op, rs->sr_entry, &hasSubordinates );
		if ( rs->sr_err == LDAP_SUCCESS ) {
			*aa = slap_operational_hasSubordinate( hasSubordinates == LDAP_COMPARE_TRUE );
			if ( *aa != NULL ) {
				aa = &(*aa)->a_next;
			}
		}
	}

	return rs->sr_err;
}

