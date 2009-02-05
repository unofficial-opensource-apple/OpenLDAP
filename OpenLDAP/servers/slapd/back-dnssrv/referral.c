/* referral.c - DNS SRV backend referral handler */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-dnssrv/referral.c,v 1.11.2.2 2004/01/01 18:16:36 kurt Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2000-2004 The OpenLDAP Foundation.
 * Portions Copyright 2000-2003 Kurt D. Zeilenga.
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
 * This work was originally developed by Kurt D. Zeilenga for inclusion
 * in OpenLDAP Software.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "external.h"

int
dnssrv_back_referrals(
    Operation	*op,
    SlapReply	*rs )
{
	int i;
	int rc = LDAP_OTHER;
	char *domain = NULL;
	char *hostlist = NULL;
	char **hosts = NULL;
	BerVarray urls = NULL;

	if( op->o_req_dn.bv_len == 0 ) {
		rs->sr_text = "DNS SRV operation upon null (empty) DN disallowed";
		return LDAP_UNWILLING_TO_PERFORM;
	}

	if( get_manageDSAit( op ) ) {
		if( op->o_tag == LDAP_REQ_SEARCH ) {
			return LDAP_SUCCESS;
		}

		rs->sr_text = "DNS SRV problem processing manageDSAit control";
		return LDAP_OTHER;
	} 

	if( ldap_dn2domain( op->o_req_dn.bv_val, &domain ) || domain == NULL ) {
		rs->sr_err = LDAP_REFERRAL;
		rs->sr_ref = default_referral;
		send_ldap_result( op, rs );
		return LDAP_REFERRAL;
	}

	Debug( LDAP_DEBUG_TRACE, "DNSSRV: dn=\"%s\" -> domain=\"%s\"\n",
		op->o_req_dn.bv_val, domain, 0 );

	if( ( rc = ldap_domain2hostlist( domain, &hostlist ) ) ) {
		Debug( LDAP_DEBUG_TRACE,
			"DNSSRV: domain2hostlist(%s) returned %d\n",
			domain, rc, 0 );
		rs->sr_text = "no DNS SRV RR available for DN";
		rc = LDAP_NO_SUCH_OBJECT;
		goto done;
	}

	hosts = ldap_str2charray( hostlist, " " );

	if( hosts == NULL ) {
		Debug( LDAP_DEBUG_TRACE, "DNSSRV: str2charrary error\n", 0, 0, 0 );
		rs->sr_text = "problem processing DNS SRV records for DN";
		goto done;
	}

	for( i=0; hosts[i] != NULL; i++) {
		struct berval url;

		url.bv_len = sizeof("ldap://")-1 + strlen(hosts[i]);
		url.bv_val = ch_malloc( url.bv_len + 1 );

		strcpy( url.bv_val, "ldap://" );
		strcpy( &url.bv_val[sizeof("ldap://")-1], hosts[i] );

		if ( ber_bvarray_add( &urls, &url ) < 0 ) {
			free( url.bv_val );
			rs->sr_text = "problem processing DNS SRV records for DN";
			goto done;
		}
	}

	Statslog( LDAP_DEBUG_STATS,
	    "conn=%lu op=%lu DNSSRV p=%d dn=\"%s\" url=\"%s\"\n",
	    op->o_connid, op->o_opid, op->o_protocol,
		op->o_req_dn.bv_val, urls[0].bv_val );

	Debug( LDAP_DEBUG_TRACE, "DNSSRV: dn=\"%s\" -> url=\"%s\"\n",
		op->o_req_dn.bv_val, urls[0].bv_val, 0 );

	rs->sr_ref = urls;
	send_ldap_error( op, rs, LDAP_REFERRAL,
		"DNS SRV generated referrals" );

done:
	if( domain != NULL ) ch_free( domain );
	if( hostlist != NULL ) ch_free( hostlist );
	if( hosts != NULL ) ldap_charray_free( hosts );
	ber_bvarray_free( urls );
	return rc;
}
