/* $OpenLDAP: pkg/ldap/servers/slapd/search.c,v 1.86 2002/01/17 18:33:48 ando Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/* Portions
 * Copyright (c) 1995 Regents of the University of Michigan.
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

#include <ac/string.h>
#include <ac/socket.h>

#include "ldap_pvt.h"
#include "slap.h"

int
do_search(
    Connection	*conn,	/* where to send results */
    Operation	*op	/* info about the op to which we're responding */
) {
	ber_int_t	scope, deref, attrsonly;
	ber_int_t	sizelimit, timelimit;
	struct berval base = { 0, NULL };
	struct berval pbase = { 0, NULL };
	struct berval nbase = { 0, NULL };
	struct berval	fstr = { 0, NULL };
	Filter		*filter = NULL;
	AttributeName	*an = NULL;
	ber_len_t	siz, off, i;
	Backend		*be;
	int			rc;
	const char	*text;
	int			manageDSAit;

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		"do_search: conn %d\n", conn->c_connid ));
#else
	Debug( LDAP_DEBUG_TRACE, "do_search\n", 0, 0, 0 );
#endif

	/*
	 * Parse the search request.  It looks like this:
	 *
	 *	SearchRequest := [APPLICATION 3] SEQUENCE {
	 *		baseObject	DistinguishedName,
	 *		scope		ENUMERATED {
	 *			baseObject	(0),
	 *			singleLevel	(1),
	 *			wholeSubtree	(2)
	 *		},
	 *		derefAliases	ENUMERATED {
	 *			neverDerefaliases	(0),
	 *			derefInSearching	(1),
	 *			derefFindingBaseObj	(2),
	 *			alwaysDerefAliases	(3)
	 *		},
	 *		sizelimit	INTEGER (0 .. 65535),
	 *		timelimit	INTEGER (0 .. 65535),
	 *		attrsOnly	BOOLEAN,
	 *		filter		Filter,
	 *		attributes	SEQUENCE OF AttributeType
	 *	}
	 */

	/* baseObject, scope, derefAliases, sizelimit, timelimit, attrsOnly */
	if ( ber_scanf( op->o_ber, "{miiiib" /*}*/,
		&base, &scope, &deref, &sizelimit,
	    &timelimit, &attrsonly ) == LBER_ERROR )
	{
		send_ldap_disconnect( conn, op,
			LDAP_PROTOCOL_ERROR, "decoding error" );
		rc = SLAPD_DISCONNECT;
		goto return_results;
	}

	switch( scope ) {
	case LDAP_SCOPE_BASE:
	case LDAP_SCOPE_ONELEVEL:
	case LDAP_SCOPE_SUBTREE:
		break;
	default:
		send_ldap_result( conn, op, rc = LDAP_PROTOCOL_ERROR,
			NULL, "invalid scope", NULL, NULL );
		goto return_results;
	}

	switch( deref ) {
	case LDAP_DEREF_NEVER:
	case LDAP_DEREF_FINDING:
	case LDAP_DEREF_SEARCHING:
	case LDAP_DEREF_ALWAYS:
		break;
	default:
		send_ldap_result( conn, op, rc = LDAP_PROTOCOL_ERROR,
			NULL, "invalid deref", NULL, NULL );
		goto return_results;
	}

	rc = dnPrettyNormal( NULL, &base, &pbase, &nbase );
	if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
			"do_search: conn %d  invalid dn (%s)\n",
			conn->c_connid, base.bv_val ));
#else
		Debug( LDAP_DEBUG_ANY,
			"do_search: invalid dn (%s)\n", base.bv_val, 0, 0 );
#endif
		send_ldap_result( conn, op, rc = LDAP_INVALID_DN_SYNTAX, NULL,
		    "invalid DN", NULL, NULL );
		goto return_results;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ARGS,
		"do_search \"%s\" %d %d %d %d %d\n", base.bv_val, scope,
		deref, sizelimit, timelimit, attrsonly ));
#else
	Debug( LDAP_DEBUG_ARGS, "SRCH \"%s\" %d %d",
		base.bv_val, scope, deref );
	Debug( LDAP_DEBUG_ARGS, "    %d %d %d\n",
		sizelimit, timelimit, attrsonly);
#endif

	/* filter - returns a "normalized" version */
	rc = get_filter( conn, op->o_ber, &filter, &fstr, &text );
	if( rc != LDAP_SUCCESS ) {
		if( rc == SLAPD_DISCONNECT ) {
			send_ldap_disconnect( conn, op,
				LDAP_PROTOCOL_ERROR, text );
		} else {
			send_ldap_result( conn, op, rc,
				NULL, text, NULL, NULL );
		}
		goto return_results;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ARGS,
		"do_search: conn %d	filter: %s\n", conn->c_connid, fstr.bv_val ));
#else
	Debug( LDAP_DEBUG_ARGS, "    filter: %s\n", fstr.bv_val, 0, 0 );
#endif

	/* attributes */
	siz = sizeof(AttributeName);
	off = 0;
	if ( ber_scanf( op->o_ber, "{M}}", &an, &siz, off ) == LBER_ERROR ) {
		send_ldap_disconnect( conn, op,
			LDAP_PROTOCOL_ERROR, "decoding attrs error" );
		rc = SLAPD_DISCONNECT;
		goto return_results;
	}
	for ( i=0; i<siz; i++ ) {
		an[i].an_desc = NULL;
		an[i].an_oc = NULL;
		slap_bv2ad(&an[i].an_name, &an[i].an_desc, &text);
	}

	if( (rc = get_ctrls( conn, op, 1 )) != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
			"do_search: conn %d  get_ctrls failed (%d)\n",
			conn->c_connid, rc ));
#else
		Debug( LDAP_DEBUG_ANY, "do_search: get_ctrls failed\n", 0, 0, 0 );
#endif

		goto return_results;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ARGS,
		"do_search: conn %d	attrs:", conn->c_connid ));
#else
	Debug( LDAP_DEBUG_ARGS, "    attrs:", 0, 0, 0 );
#endif

	if ( siz != 0 ) {
		for ( i = 0; i<siz; i++ ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_ARGS,
				"do_search:	   %s", an[i].an_name.bv_val ));
#else
			Debug( LDAP_DEBUG_ARGS, " %s", an[i].an_name.bv_val, 0, 0 );
#endif
		}
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ARGS, "\n" ));
#else
	Debug( LDAP_DEBUG_ARGS, "\n", 0, 0, 0 );
#endif

	Statslog( LDAP_DEBUG_STATS,
	    "conn=%ld op=%d SRCH base=\"%s\" scope=%d filter=\"%s\"\n",
	    op->o_connid, op->o_opid, pbase.bv_val, scope, fstr.bv_val );

	manageDSAit = get_manageDSAit( op );

	if ( scope == LDAP_SCOPE_BASE ) {
		Entry *entry = NULL;

		if ( nbase.bv_len == 0 ) {
#ifdef LDAP_CONNECTIONLESS
			/* Ignore LDAPv2 CLDAP Root DSE queries */
			if (op->o_protocol==LDAP_VERSION2 && conn->c_is_udp) {
				goto return_results;
			}
#endif
			/* check restrictions */
			rc = backend_check_restrictions( NULL, conn, op, NULL, &text ) ;
			if( rc != LDAP_SUCCESS ) {
				send_ldap_result( conn, op, rc,
					NULL, text, NULL, NULL );
				goto return_results;
			}

			rc = root_dse_info( conn, &entry, &text );
		}

#if defined( SLAPD_SCHEMA_DN )
		else if ( strcasecmp( nbase.bv_val, SLAPD_SCHEMA_DN ) == 0 ) {
			/* check restrictions */
			rc = backend_check_restrictions( NULL, conn, op, NULL, &text ) ;
			if( rc != LDAP_SUCCESS ) {
				send_ldap_result( conn, op, rc,
					NULL, text, NULL, NULL );
				goto return_results;
			}

			rc = schema_info( &entry, &text );
		}
#endif

		if( rc != LDAP_SUCCESS ) {
			send_ldap_result( conn, op, rc,
				NULL, text, NULL, NULL );
			goto return_results;

		} else if ( entry != NULL ) {
			rc = test_filter( NULL, conn, op,
				entry, filter );

			if( rc == LDAP_COMPARE_TRUE ) {
				send_search_entry( NULL, conn, op,
					entry, an, attrsonly, NULL );
			}
			entry_free( entry );

			send_ldap_result( conn, op, LDAP_SUCCESS,
				NULL, NULL, NULL, NULL );

			goto return_results;
		}
	}

	if( !nbase.bv_len && default_search_nbase.bv_len ) {
		ch_free( pbase.bv_val );
		ch_free( nbase.bv_val );

		ber_dupbv( &pbase, &default_search_base );
		ber_dupbv( &nbase, &default_search_nbase );
	}

	/*
	 * We could be serving multiple database backends.  Select the
	 * appropriate one, or send a referral to our "referral server"
	 * if we don't hold it.
	 */
	if ( (be = select_backend( &nbase, manageDSAit, 1 )) == NULL ) {
		BerVarray ref = referral_rewrite( default_referral,
			NULL, &pbase, scope );

		send_ldap_result( conn, op, rc = LDAP_REFERRAL,
			NULL, NULL, ref ? ref : default_referral, NULL );

		ber_bvarray_free( ref );
		goto return_results;
	}

	/* check restrictions */
	rc = backend_check_restrictions( be, conn, op, NULL, &text ) ;
	if( rc != LDAP_SUCCESS ) {
		send_ldap_result( conn, op, rc,
			NULL, text, NULL, NULL );
		goto return_results;
	}

	/* check for referrals */
	rc = backend_check_referrals( be, conn, op, &pbase, &nbase );
	if ( rc != LDAP_SUCCESS ) {
		goto return_results;
	}

	/* deref the base if needed */
	suffix_alias( be, &nbase );

	/* actually do the search and send the result(s) */
	if ( be->be_search ) {
		(*be->be_search)( be, conn, op, &pbase, &nbase,
			scope, deref, sizelimit,
		    timelimit, filter, &fstr, an, attrsonly );
	} else {
		send_ldap_result( conn, op, rc = LDAP_UNWILLING_TO_PERFORM,
			NULL, "operation not supported within namingContext",
			NULL, NULL );
	}

return_results:;
	if( pbase.bv_val != NULL) free( pbase.bv_val );
	if( nbase.bv_val != NULL) free( nbase.bv_val );

	if( fstr.bv_val != NULL) free( fstr.bv_val );
	if( filter != NULL) filter_free( filter );
	if( an != NULL ) free( an );

	return rc;
}
