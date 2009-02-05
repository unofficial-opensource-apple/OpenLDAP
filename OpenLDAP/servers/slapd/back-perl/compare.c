/* $OpenLDAP: pkg/ldap/servers/slapd/back-perl/compare.c,v 1.10 2002/02/02 09:37:26 hyc Exp $ */
/*
 *	 Copyright 1999, John C. Quillan, All rights reserved.
 *
 *	 Redistribution and use in source and binary forms are permitted only
 *	 as authorized by the OpenLDAP Public License.	A copy of this
 *	 license is available at http://www.OpenLDAP.org/license.html or
 *	 in file LICENSE in the top-level directory of the distribution.
 */

#include "portable.h"

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
 * Compare
 *
 **********************************************************/

perl_back_compare(
	Backend	*be,
	Connection	*conn,
	Operation	*op,
	struct berval	*dn,
	struct berval	*ndn,
	AttributeAssertion		*ava
)
{
	int return_code;
	int count;
	char *avastr, *ptr;

	PerlBackend *perl_back = (PerlBackend *)be->be_private;

	avastr = ch_malloc( ava->aa_desc->ad_cname.bv_len + 1 +
		ava->aa_value.bv_len + 1 );
	
	slap_strcopy( slap_strcopy( slap_strcopy( avastr,
		ava->aa_desc->ad_cname.bv_val ), "=" ),
		ava->aa_value.bv_val );

	ldap_pvt_thread_mutex_lock( &perl_interpreter_mutex );	

	{
		dSP; ENTER; SAVETMPS;

		PUSHMARK(sp);
		XPUSHs( perl_back->pb_obj_ref );
		XPUSHs(sv_2mortal(newSVpv( dn->bv_val , 0)));
		XPUSHs(sv_2mortal(newSVpv( avastr , 0)));
		PUTBACK;

		count = perl_call_method("compare", G_SCALAR);

		SPAGAIN;

		if (count != 1) {
			croak("Big trouble in back_compare\n");
		}

		return_code = POPi;
							 
		PUTBACK; FREETMPS; LEAVE;
	}

	ldap_pvt_thread_mutex_unlock( &perl_interpreter_mutex );	

	ch_free( avastr );

	send_ldap_result( conn, op, return_code ? LDAP_COMPARE_TRUE :
		LDAP_COMPARE_FALSE, NULL, NULL, NULL, NULL );

	Debug( LDAP_DEBUG_ANY, "Perl COMPARE\n", 0, 0, 0 );

	return (0);
}

