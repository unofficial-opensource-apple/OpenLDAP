/* bind.c - shell backend bind function */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-shell/bind.c,v 1.14 2002/01/04 20:17:54 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>
#include <ac/string.h>

#include "slap.h"
#include "shell.h"

int
shell_back_bind(
    Backend		*be,
    Connection		*conn,
    Operation		*op,
    struct berval	*dn,
    struct berval	*ndn,
    int			method,
    struct berval	*cred,
    struct berval	*edn
)
{
	struct shellinfo	*si = (struct shellinfo *) be->be_private;
	FILE			*rfp, *wfp;
	int			rc;

	if ( si->si_bind == NULL ) {
		send_ldap_result( conn, op, LDAP_UNWILLING_TO_PERFORM, NULL,
		    "bind not implemented", NULL, NULL );
		return( -1 );
	}

	if ( (op->o_private = (void *) forkandexec( si->si_bind, &rfp, &wfp ))
	    == (void *) -1 ) {
		send_ldap_result( conn, op, LDAP_OPERATIONS_ERROR, NULL,
		    "could not fork/exec", NULL, NULL );
		return( -1 );
	}

	/* write out the request to the bind process */
	fprintf( wfp, "BIND\n" );
	fprintf( wfp, "msgid: %ld\n", (long) op->o_msgid );
	print_suffixes( wfp, be );
	fprintf( wfp, "dn: %s\n", dn->bv_val );
	fprintf( wfp, "method: %d\n", method );
	fprintf( wfp, "credlen: %lu\n", cred->bv_len );
	fprintf( wfp, "cred: %s\n", cred->bv_val ); /* XXX */
	fclose( wfp );

	/* read in the results and send them along */
	rc = read_and_send_results( be, conn, op, rfp, NULL, 0 );
	fclose( rfp );

	return( rc );
}