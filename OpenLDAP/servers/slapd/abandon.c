/* abandon.c - decode and handle an ldap abandon operation */
/* $OpenLDAP: pkg/ldap/servers/slapd/abandon.c,v 1.36.2.4 2004/07/16 19:28:38 kurt Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1998-2004 The OpenLDAP Foundation.
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
/* Portions Copyright (c) 1995 Regents of the University of Michigan.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of Michigan at Ann Arbor. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 */

#include "portable.h"

#include <stdio.h>
#include <ac/socket.h>

#include "slap.h"

int
do_abandon( Operation *op, SlapReply *rs )
{
	ber_int_t	id;
	Operation	*o;
	int		i;

#ifdef NEW_LOGGING
	LDAP_LOG( OPERATION, ENTRY, "conn: %d do_abandon\n", op->o_connid, 0, 0);
#else
	Debug( LDAP_DEBUG_TRACE, "do_abandon\n", 0, 0, 0 );
#endif

	/*
	 * Parse the abandon request.  It looks like this:
	 *
	 *	AbandonRequest := MessageID
	 */

	if ( ber_scanf( op->o_ber, "i", &id ) == LBER_ERROR ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, ERR, 
			"conn: %d do_abandon: ber_scanf failed\n", op->o_connid, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY, "do_abandon: ber_scanf failed\n", 0, 0 ,0 );
#endif
		send_ldap_discon( op, rs,
			LDAP_PROTOCOL_ERROR, "decoding error" );
		return -1;
	}

	if( get_ctrls( op, rs, 0 ) != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_ANY, "do_abandon: get_ctrls failed\n", 0, 0 ,0 );
		return rs->sr_err;
	} 

#ifdef NEW_LOGGING
	LDAP_LOG( OPERATION, ARGS, "do_abandon: conn: %d  id=%ld\n", 
		op->o_connid, (long) id, 0 );
#else
	Debug( LDAP_DEBUG_ARGS, "do_abandon: id=%ld\n", (long) id, 0 ,0 );
#endif

	if( id <= 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, ERR, 
			"do_abandon: conn: %d bad msgid %ld\n", 
			op->o_connid, (long) id, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"do_abandon: bad msgid %ld\n", (long) id, 0, 0 );
#endif
		return LDAP_SUCCESS;
	}

	ldap_pvt_thread_mutex_lock( &op->o_conn->c_mutex );
	/*
	 * find the operation being abandoned and set the o_abandon
	 * flag.  It's up to the backend to periodically check this
	 * flag and abort the operation at a convenient time.
	 */

	LDAP_STAILQ_FOREACH( o, &op->o_conn->c_ops, o_next ) {
		if ( o->o_msgid == id ) {
			o->o_abandon = 1;
			goto done;
		}
	}

	LDAP_STAILQ_FOREACH( o, &op->o_conn->c_pending_ops, o_next ) {
		if ( o->o_msgid == id ) {
			LDAP_STAILQ_REMOVE( &op->o_conn->c_pending_ops,
				o, slap_op, o_next );
			LDAP_STAILQ_NEXT(o, o_next) = NULL;
			op->o_conn->c_n_ops_pending--;
			slap_op_free( o );
			goto done;
		}
	}

done:

	op->orn_msgid = id;
	for ( i = 0; i < nbackends; i++ ) {
		op->o_bd = &backends[i];

		if( op->o_bd->be_abandon ) op->o_bd->be_abandon( op, rs );
	}

	ldap_pvt_thread_mutex_unlock( &op->o_conn->c_mutex );

#ifdef NEW_LOGGING
	LDAP_LOG( OPERATION, ENTRY, 
		"do_abandon: conn: %d op=%ld %sfound\n",
		op->o_connid, (long)id, o ? "" : "not " );
#else
	Debug( LDAP_DEBUG_TRACE, "do_abandon: op=%ld %sfound\n",
		(long) id, o ? "" : "not ", 0 );
#endif
	return LDAP_SUCCESS;
}
