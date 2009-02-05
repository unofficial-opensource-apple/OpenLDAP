/* $OpenLDAP: pkg/ldap/servers/slapd/controls.c,v 1.30.2.1 2002/02/18 18:49:56 kurt Exp $ */
/* 
 * Copyright 1999-2002 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted only
 * as authorized by the OpenLDAP Public License.  A copy of this
 * license is available at http://www.OpenLDAP.org/license.html or
 * in file LICENSE in the top-level directory of the distribution.
 */
#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"

#include "../../libraries/liblber/lber-int.h"

#define SLAP_CTRL_FRONTEND	0x80000000U

#define SLAP_CTRL_OPFLAGS	0x0000FFFFU
#define SLAP_CTRL_ABANDON	0x00000001U
#define SLAP_CTRL_ADD		0x00002002U
#define SLAP_CTRL_BIND		0x00000004U
#define SLAP_CTRL_COMPARE	0x00001008U
#define SLAP_CTRL_DELETE	0x00002010U
#define SLAP_CTRL_MODIFY	0x00002020U
#define SLAP_CTRL_RENAME	0x00002040U
#define SLAP_CTRL_SEARCH	0x00001080U
#define SLAP_CTRL_UNBIND	0x00000100U

#define SLAP_CTRL_INTROGATE	(SLAP_CTRL_COMPARE|SLAP_CTRL_SEARCH)
#define SLAP_CTRL_UPDATE \
	(SLAP_CTRL_ADD|SLAP_CTRL_DELETE|SLAP_CTRL_MODIFY|SLAP_CTRL_RENAME)
#define SLAP_CTRL_ACCESS	(SLAP_CTRL_INTROGATE|SLAP_CTRL_UPDATE)

typedef int (SLAP_CTRL_PARSE_FN) LDAP_P((
	Connection *conn,
	Operation *op,
	LDAPControl *ctrl,
	const char **text ));

static SLAP_CTRL_PARSE_FN parseManageDSAit;
static SLAP_CTRL_PARSE_FN parseSubentries;
static SLAP_CTRL_PARSE_FN parseNoOp;

static struct slap_control {
	char *sc_oid;
	slap_mask_t sc_mask;
	char **sc_extendedops;
	SLAP_CTRL_PARSE_FN *sc_parse;

} supportedControls[] = {
	{ LDAP_CONTROL_MANAGEDSAIT,
		SLAP_CTRL_ACCESS, NULL,
		parseManageDSAit },
#ifdef LDAP_CONTROL_SUBENTRIES
	{ LDAP_CONTROL_SUBENTRIES,
		SLAP_CTRL_SEARCH, NULL,
		parseSubentries },
#endif
#ifdef LDAP_CONTROL_NOOP
	{ LDAP_CONTROL_NOOP,
		SLAP_CTRL_UPDATE, NULL,
		parseNoOp },
#endif
	{ NULL }
};

char *
get_supported_ctrl(int index)
{
	return supportedControls[index].sc_oid;
}

static struct slap_control *
find_ctrl( const char *oid )
{
	int i;
	for( i=0; supportedControls[i].sc_oid; i++ ) {
		if( strcmp( oid, supportedControls[i].sc_oid ) == 0 ) {
			return &supportedControls[i];
		}
	}
	return NULL;
}

int get_ctrls(
	Connection *conn,
	Operation *op,
	int sendres )
{
	int nctrls;
	ber_tag_t tag;
	ber_len_t len;
	char *opaque;
	BerElement *ber = op->o_ber;
	struct slap_control *sc;
	int rc = LDAP_SUCCESS;
	const char *errmsg = NULL;

	len = ber_pvt_ber_remaining(ber);

	if( len == 0) {
		/* no controls */
		rc = LDAP_SUCCESS;
		return rc;
	}

	if(( tag = ber_peek_tag( ber, &len )) != LDAP_TAG_CONTROLS ) {
		if( tag == LBER_ERROR ) {
			rc = SLAPD_DISCONNECT;
			errmsg = "unexpected data in PDU";
		}

		goto return_results;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		"get_ctrls: conn %d\n", conn->c_connid ));
#else
	Debug( LDAP_DEBUG_TRACE, "=> get_ctrls\n", 0, 0, 0 );
#endif
	if( op->o_protocol < LDAP_VERSION3 ) {
		rc = SLAPD_DISCONNECT;
		errmsg = "controls require LDAPv3";
		goto return_results;
	}

	/* one for first control, one for termination */
	op->o_ctrls = ch_malloc( 2 * sizeof(LDAPControl *) );

#if 0
	if( op->ctrls == NULL ) {
		rc = LDAP_NO_MEMORY;
		errmsg = "no memory";
		goto return_results;
	}
#endif

	op->o_ctrls[nctrls=0] = NULL;

	/* step through each element */
	for( tag = ber_first_element( ber, &len, &opaque );
		tag != LBER_ERROR;
		tag = ber_next_element( ber, &len, opaque ) )
	{
		LDAPControl *c;
		LDAPControl **tctrls;

		c = ch_calloc( 1, sizeof(LDAPControl) );

#if 0
		if( c == NULL ) {
			ldap_controls_free(op->o_ctrls);
			op->o_ctrls = NULL;

			rc = LDAP_NO_MEMORY;
			errmsg = "no memory";
			goto return_results;
		}
#endif

		/* allocate pointer space for current controls (nctrls)
		 * + this control + extra NULL
		 */
		tctrls = ch_realloc( op->o_ctrls,
			(nctrls+2) * sizeof(LDAPControl *));

#if 0
		if( tctrls == NULL ) {
			ch_free( c );
			ldap_controls_free(op->o_ctrls);
			op->o_ctrls = NULL;

			rc = LDAP_NO_MEMORY;
			errmsg = "no memory";
			goto return_results;
		}
#endif
		op->o_ctrls = tctrls;

		op->o_ctrls[nctrls++] = c;
		op->o_ctrls[nctrls] = NULL;

		tag = ber_scanf( ber, "{a" /*}*/, &c->ldctl_oid );

		if( tag == LBER_ERROR ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
				"get_ctrls: conn %d get OID failed.\n",
				conn->c_connid ));
#else
			Debug( LDAP_DEBUG_TRACE, "=> get_ctrls: get oid failed.\n",
				0, 0, 0 );
#endif
			ldap_controls_free( op->o_ctrls );
			op->o_ctrls = NULL;
			rc = SLAPD_DISCONNECT;
			errmsg = "decoding controls error";
			goto return_results;
		}

		tag = ber_peek_tag( ber, &len );

		if( tag == LBER_BOOLEAN ) {
			ber_int_t crit;
			tag = ber_scanf( ber, "b", &crit );

			if( tag == LBER_ERROR ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
					"get_ctrls: conn %d get crit failed.\n",
					conn->c_connid ));
#else
				Debug( LDAP_DEBUG_TRACE, "=> get_ctrls: get crit failed.\n",
					0, 0, 0 );
#endif
				ldap_controls_free( op->o_ctrls );
				op->o_ctrls = NULL;
				rc = SLAPD_DISCONNECT;
				errmsg = "decoding controls error";
				goto return_results;
			}

			c->ldctl_iscritical = (crit != 0);
			tag = ber_peek_tag( ber, &len );
		}

		if( tag == LBER_OCTETSTRING ) {
			tag = ber_scanf( ber, "o", &c->ldctl_value );

			if( tag == LBER_ERROR ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "operation", LDAP_LEVEL_INFO, "get_ctrls: conn %d: "
					"%s (%scritical): get value failed.\n",
					conn->c_connid,
					c->ldctl_oid ? c->ldctl_oid : "(NULL)",
					c->ldctl_iscritical ? "" : "non" ));
#else
				Debug( LDAP_DEBUG_TRACE, "=> get_ctrls: conn %d: "
					"%s (%scritical): get value failed.\n",
					conn->c_connid,
					c->ldctl_oid ? c->ldctl_oid : "(NULL)",
					c->ldctl_iscritical ? "" : "non" );
#endif
				ldap_controls_free( op->o_ctrls );
				op->o_ctrls = NULL;
				rc = SLAPD_DISCONNECT;
				errmsg = "decoding controls error";
				goto return_results;
			}
		}

#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
			"get_ctrls: conn %d oid=\"%s\" (%scritical)\n",
			conn->c_connid,
			c->ldctl_oid ? c->ldctl_oid : "(NULL)",
			c->ldctl_iscritical ? "" : "non" ));
#else
		Debug( LDAP_DEBUG_TRACE, "=> get_ctrls: oid=\"%s\" (%scritical)\n",
			c->ldctl_oid ? c->ldctl_oid : "(NULL)",
			c->ldctl_iscritical ? "" : "non",
			0 );
#endif

		sc = find_ctrl( c->ldctl_oid );
		if( sc != NULL ) {
			/* recognized control */
			slap_mask_t tagmask;
			switch( op->o_tag ) {
			case LDAP_REQ_ADD:
				tagmask = SLAP_CTRL_ADD;
				break;
			case LDAP_REQ_BIND:
				tagmask = SLAP_CTRL_BIND;
				break;
			case LDAP_REQ_COMPARE:
				tagmask = SLAP_CTRL_COMPARE;
				break;
			case LDAP_REQ_DELETE:
				tagmask = SLAP_CTRL_DELETE;
				break;
			case LDAP_REQ_MODIFY:
				tagmask = SLAP_CTRL_MODIFY;
				break;
			case LDAP_REQ_RENAME:
				tagmask = SLAP_CTRL_RENAME;
				break;
			case LDAP_REQ_SEARCH:
				tagmask = SLAP_CTRL_SEARCH;
				break;
			case LDAP_REQ_UNBIND:
				tagmask = SLAP_CTRL_UNBIND;
				break;
			case LDAP_REQ_EXTENDED:
				/* FIXME: check list of extended operations */
				tagmask = ~0U;
				break;
			default:
				rc = LDAP_OTHER;
				errmsg = "controls internal error";
				goto return_results;
			}

			if (( sc->sc_mask & tagmask ) == tagmask ) {
				/* available extension */

				if( !sc->sc_parse ) {
					rc = LDAP_OTHER;
					errmsg = "not yet implemented";
					goto return_results;
				}

				rc = sc->sc_parse( conn, op, c, &errmsg );

				if( rc != LDAP_SUCCESS ) goto return_results;

				if( sc->sc_mask & SLAP_CTRL_FRONTEND ) {
					/* kludge to disable backend_control() check */
					c->ldctl_iscritical = 0;
				}

			} else if( c->ldctl_iscritical ) {
				/* unavailable CRITICAL control */
				rc = LDAP_UNAVAILABLE_CRITICAL_EXTENSION;
				errmsg = "critical extension is unavailable";
				goto return_results;
			}

		} else if( c->ldctl_iscritical ) {
			/* unrecognized CRITICAL control */
			rc = LDAP_UNAVAILABLE_CRITICAL_EXTENSION;
			errmsg = "critical extension is not recognized";
			goto return_results;
		}
	}

return_results:
#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_RESULTS,
		"get_ctrls: conn=%d	n=%d rc=%d err=%s\n",
		conn->c_connid, nctrls, rc, errmsg ? errmsg : "" ));
#else
	Debug( LDAP_DEBUG_TRACE, "<= get_ctrls: n=%d rc=%d err=%s\n",
		nctrls, rc, errmsg ? errmsg : "");
#endif

	if( sendres && rc != LDAP_SUCCESS ) {
		if( rc == SLAPD_DISCONNECT ) {
			send_ldap_disconnect( conn, op, LDAP_PROTOCOL_ERROR, errmsg );
		} else {
			send_ldap_result( conn, op, rc,
				NULL, errmsg, NULL, NULL );
		}
	}

	return rc;
}

static int parseManageDSAit (
	Connection *conn,
	Operation *op,
	LDAPControl *ctrl,
	const char **text )
{
	if ( op->o_managedsait != SLAP_NO_CONTROL ) {
		*text = "manageDSAit control specified multiple times";
		return LDAP_PROTOCOL_ERROR;
	}

	if ( ctrl->ldctl_value.bv_len ) {
		*text = "manageDSAit control value not empty";
		return LDAP_PROTOCOL_ERROR;
	}

	op->o_managedsait = ctrl->ldctl_iscritical
		? SLAP_CRITICAL_CONTROL
		: SLAP_NONCRITICAL_CONTROL;

	return LDAP_SUCCESS;
}

#ifdef LDAP_CONTROL_SUBENTRIES
static int parseSubentries (
	Connection *conn,
	Operation *op,
	LDAPControl *ctrl,
	const char **text )
{
	if ( op->o_subentries != SLAP_NO_CONTROL ) {
		*text = "subentries control specified multiple times";
		return LDAP_PROTOCOL_ERROR;
	}

	/* FIXME: should use BER library */
	if( ( ctrl->ldctl_value.bv_len != 3 )
		&& ( ctrl->ldctl_value.bv_val[0] != 0x01 )
		&& ( ctrl->ldctl_value.bv_val[1] != 0x01 ))
	{
		*text = "subentries control value encoding is bogus";
		return LDAP_PROTOCOL_ERROR;
	}

	op->o_subentries = ctrl->ldctl_iscritical
		? SLAP_CRITICAL_CONTROL
		: SLAP_NONCRITICAL_CONTROL;

	op->o_subentries_visibility = (ctrl->ldctl_value.bv_val[2] != 0x00);

	return LDAP_SUCCESS;
}
#endif

#ifdef LDAP_CONTROL_NOOP
static int parseNoOp (
	Connection *conn,
	Operation *op,
	LDAPControl *ctrl,
	const char **text )
{
	if ( op->o_noop != SLAP_NO_CONTROL ) {
		*text = "noop control specified multiple times";
		return LDAP_PROTOCOL_ERROR;
	}

	if ( ctrl->ldctl_value.bv_len ) {
		*text = "noop control value not empty";
		return LDAP_PROTOCOL_ERROR;
	}

	op->o_noop = ctrl->ldctl_iscritical
		? SLAP_CRITICAL_CONTROL
		: SLAP_NONCRITICAL_CONTROL;

	return LDAP_SUCCESS;
}
#endif

