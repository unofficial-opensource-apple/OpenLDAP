/* $OpenLDAP: pkg/ldap/servers/slapd/back-perl/bind.c,v 1.9 2002/02/02 09:10:35 hyc Exp $ */
/*
 *	 Copyright 1999, John C. Quillan, All rights reserved.
 *
 *	 Redistribution and use in source and binary forms are permitted only
 *	 as authorized by the OpenLDAP Public License.	A copy of this
 *	 license is available at http://www.OpenLDAP.org/license.html or
 *	 in file LICENSE in the top-level directory of the distribution.
 */

#include "portable.h"
/* init.c - initialize shell backend */
	
#include <stdio.h>
/*	#include <ac/types.h>
	#include <ac/socket.h>
*/

#include <EXTERN.h>
#include <perl.h>

#include "slap.h"
#include "perl_back.h"


/**********************************************************
 *
 * Bind
 *
 **********************************************************/
int
perl_back_bind(
	Backend *be,
	Connection *conn,
	Operation *op,
	struct berval *dn,
	struct berval *ndn,
	int method,
	struct berval *cred,
	struct berval *edn
)
{
	int return_code;
	int count;

	PerlBackend *perl_back = (PerlBackend *) be->be_private;

	ldap_pvt_thread_mutex_lock( &perl_interpreter_mutex );	

	{
		dSP; ENTER; SAVETMPS;

		PUSHMARK(sp);
		XPUSHs( perl_back->pb_obj_ref );
		XPUSHs(sv_2mortal(newSVpv( dn->bv_val , 0)));
		XPUSHs(sv_2mortal(newSVpv( cred->bv_val , cred->bv_len)));
		PUTBACK;

		count = perl_call_method("bind", G_SCALAR);

		SPAGAIN;

		if (count != 1) {
			croak("Big trouble in back_bind\n");
		}

		return_code = POPi;
							 

		PUTBACK; FREETMPS; LEAVE;
	}

	ldap_pvt_thread_mutex_unlock( &perl_interpreter_mutex );	

	Debug( LDAP_DEBUG_ANY, "Perl BIND\n", 0, 0, 0 );

	return ( return_code );
}
