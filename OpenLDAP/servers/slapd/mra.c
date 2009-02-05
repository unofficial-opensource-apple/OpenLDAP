/* mra.c - routines for dealing with extensible matching rule assertions */
/* $OpenLDAP: pkg/ldap/servers/slapd/mra.c,v 1.31.2.3 2004/04/12 18:13:21 kurt Exp $ */
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

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"

void
mra_free(
	Operation *op,
	MatchingRuleAssertion *mra,
	int	freeit
)
{
	/* op->o_tmpfree( mra->ma_value.bv_val, op->o_tmpmemctx ); */
	ch_free( mra->ma_value.bv_val );
	if ( freeit ) {
		op->o_tmpfree( (char *) mra, op->o_tmpmemctx );
	}
}

int
get_mra(
	Operation *op,
	BerElement	*ber,
	MatchingRuleAssertion	**mra,
	const char **text
)
{
	int rc;
	ber_tag_t tag, rtag;
	ber_len_t length;
	struct berval type = BER_BVNULL;
	struct berval value = BER_BVNULL;
	struct berval rule_text = BER_BVNULL;
	MatchingRuleAssertion ma;

	memset( &ma, 0, sizeof ma);

	rtag = ber_scanf( ber, "{t" /*"}"*/, &tag );

	if( rtag == LBER_ERROR ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, ERR, 
			"get_mra: ber_scanf (\"{t\") failure\n", 0, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY, "  get_mra ber_scanf\n", 0, 0, 0 );
#endif

		*text = "Error parsing matching rule assertion";
		return SLAPD_DISCONNECT;
	}

	if ( tag == LDAP_FILTER_EXT_OID ) {
		rtag = ber_scanf( ber, "m", &rule_text );
		if ( rtag == LBER_ERROR ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "get_mra: ber_scanf(\"o\") failure.\n", 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_ANY, "  get_mra ber_scanf for mr\n", 0, 0, 0 );
#endif

			*text = "Error parsing matching rule in matching rule assertion";
			return SLAPD_DISCONNECT;
		}

		rtag = ber_scanf( ber, "t", &tag );
		if( rtag == LBER_ERROR ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "get_mra: ber_scanf (\"t\") failure\n", 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_ANY, "  get_mra ber_scanf\n", 0, 0, 0 );
#endif

			*text = "Error parsing matching rule assertion";
			return SLAPD_DISCONNECT;
		}
	}

	if ( tag == LDAP_FILTER_EXT_TYPE ) {
		rtag = ber_scanf( ber, "m", &type );
		if ( rtag == LBER_ERROR ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "get_mra: ber_scanf (\"o\") failure.\n", 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_ANY, "  get_mra ber_scanf for ad\n", 0, 0, 0 );
#endif

			*text = "Error parsing attribute description in matching rule assertion";
			return SLAPD_DISCONNECT;
		}

		rtag = ber_scanf( ber, "t", &tag );
		if( rtag == LBER_ERROR ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR,
			   "get_mra: ber_scanf (\"t\") failure.\n", 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_ANY, "  get_mra ber_scanf\n", 0, 0, 0 );
#endif

			*text = "Error parsing matching rule assertion";
			return SLAPD_DISCONNECT;
		}
	}

	if ( tag != LDAP_FILTER_EXT_VALUE ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, ERR, 
			"get_mra: ber_scanf missing value\n", 0, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY, "  get_mra ber_scanf missing value\n", 0, 0, 0 );
#endif

		*text = "Missing value in matching rule assertion";
		return SLAPD_DISCONNECT;
	}

	rtag = ber_scanf( ber, "m", &value );

	if( rtag == LBER_ERROR ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, ERR, 
			"get_mra: ber_scanf (\"o\") failure.\n", 0, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY, "  get_mra ber_scanf\n", 0, 0, 0 );
#endif

		*text = "Error decoding value in matching rule assertion";
		return SLAPD_DISCONNECT;
	}

	tag = ber_peek_tag( ber, &length );

	if ( tag == LDAP_FILTER_EXT_DNATTRS ) {
		rtag = ber_scanf( ber, /*"{"*/ "b}", &ma.ma_dnattrs );
	} else {
		rtag = ber_scanf( ber, /*"{"*/ "}" );
	}

	if( rtag == LBER_ERROR ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, ERR, "get_mra: ber_scanf failure\n", 0, 0, 0);
#else
		Debug( LDAP_DEBUG_ANY, "  get_mra ber_scanf\n", 0, 0, 0 );
#endif

		*text = "Error decoding dnattrs matching rule assertion";
		return SLAPD_DISCONNECT;
	}

	if( type.bv_val != NULL ) {
		rc = slap_bv2ad( &type, &ma.ma_desc, text );
		if( rc != LDAP_SUCCESS ) {
			return rc;
		}
	}

	if( rule_text.bv_val != NULL ) {
		ma.ma_rule = mr_bvfind( &rule_text );
		if( ma.ma_rule == NULL ) {
			*text = "matching rule not recognized";
			return LDAP_INAPPROPRIATE_MATCHING;
		}
	}

	if ( ma.ma_rule == NULL ) {
		/*
		 * Need either type or rule ...
		 */
		if ( ma.ma_desc == NULL ) {
			*text = "no matching rule or type";
			return LDAP_INAPPROPRIATE_MATCHING;
		}

		if ( ma.ma_desc->ad_type->sat_equality != NULL &&
			ma.ma_desc->ad_type->sat_equality->smr_usage & SLAP_MR_EXT )
		{
			/* no matching rule was provided, use the attribute's
			   equality rule if it supports extensible matching. */
			ma.ma_rule = ma.ma_desc->ad_type->sat_equality;

		} else {
			*text = "no appropriate rule to use for type";
			return LDAP_INAPPROPRIATE_MATCHING;
		}
	}

	if ( ma.ma_desc != NULL ) {
		if( !mr_usable_with_at( ma.ma_rule, ma.ma_desc->ad_type ) ) {
			*text = "matching rule use with this attribute not appropriate";
			return LDAP_INAPPROPRIATE_MATCHING;
		}

	}

	/*
	 * Normalize per matching rule
	 */
	rc = asserted_value_validate_normalize( ma.ma_desc,
		ma.ma_rule,
		SLAP_MR_EXT|SLAP_MR_VALUE_OF_ASSERTION_SYNTAX,
		&value, &ma.ma_value, text, op->o_tmpmemctx );

	if( rc != LDAP_SUCCESS ) {
		return rc;
	}

	length = sizeof(ma);
	/* Append rule_text to end of struct */
	if (rule_text.bv_val) length += rule_text.bv_len + 1;
	*mra = op->o_tmpalloc( length, op->o_tmpmemctx );
	**mra = ma;
	if (rule_text.bv_val) {
		(*mra)->ma_rule_text.bv_len = rule_text.bv_len;
		(*mra)->ma_rule_text.bv_val = (char *)(*mra+1);
		AC_MEMCPY((*mra)->ma_rule_text.bv_val, rule_text.bv_val,
			rule_text.bv_len+1);
	}

	return LDAP_SUCCESS;
}
