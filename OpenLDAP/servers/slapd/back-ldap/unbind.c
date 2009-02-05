/* unbind.c - ldap backend unbind function */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-ldap/unbind.c,v 1.18.2.3 2004/01/01 18:16:37 kurt Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1999-2004 The OpenLDAP Foundation.
 * Portions Copyright 1999-2003 Howard Chu.
 * Portions Copyright 2000-2003 Pierangelo Masarati.
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
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by the Howard Chu for inclusion
 * in OpenLDAP Software and subsequently enhanced by Pierangelo
 * Masarati.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>
#include <ac/string.h>

#include "slap.h"
#include "back-ldap.h"

int
ldap_back_conn_destroy(
    Backend		*be,
    Connection		*conn
)
{
	struct ldapinfo	*li = (struct ldapinfo *) be->be_private;
	struct ldapconn *lc, lc_curr;

#ifdef NEW_LOGGING
	LDAP_LOG( BACK_LDAP, INFO,
		"ldap_back_conn_destroy: fetching conn %ld\n", conn->c_connid, 0, 0 );
#else /* !NEW_LOGGING */
	Debug( LDAP_DEBUG_TRACE,
		"=>ldap_back_conn_destroy: fetching conn %ld\n",
		conn->c_connid, 0, 0 );
#endif /* !NEW_LOGGING */

	lc_curr.conn = conn;
	lc_curr.local_dn = conn->c_ndn;
	
	ldap_pvt_thread_mutex_lock( &li->conn_mutex );
	lc = avl_delete( &li->conntree, (caddr_t)&lc_curr, ldap_back_conn_cmp );
	ldap_pvt_thread_mutex_unlock( &li->conn_mutex );

	if (lc) {
#ifdef NEW_LOGGING
		LDAP_LOG( BACK_LDAP, DETAIL1, 
			"ldap_back_conn_destroy: destroying conn %ld\n", 
			conn->c_connid, 0, 0 );
#else /* !NEW_LOGGING */
		Debug( LDAP_DEBUG_TRACE,
			"=>ldap_back_conn_destroy: destroying conn %ld\n",
			lc->conn->c_connid, 0, 0 );
#endif

#ifdef ENABLE_REWRITE
		/*
		 * Cleanup rewrite session
		 */
		rewrite_session_delete( li->rwmap.rwm_rw, conn );
#endif /* ENABLE_REWRITE */

		/*
		 * Needs a test because the handler may be corrupted,
		 * and calling ldap_unbind on a corrupted header results
		 * in a segmentation fault
		 */
		ldap_back_conn_free( lc );
	}

	/* no response to unbind */

	return 0;
}
