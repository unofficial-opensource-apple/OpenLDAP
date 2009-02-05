/* result.c - routines to send ldap results, errors, and referrals */
/* $OpenLDAP: pkg/ldap/servers/slapd/result.c,v 1.130 2002/02/09 04:14:17 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>
#include <ac/errno.h>
#include <ac/signal.h>
#include <ac/string.h>
#include <ac/ctype.h>
#include <ac/time.h>
#include <ac/unistd.h>

#include "slap.h"

static char *v2ref( BerVarray ref, const char *text )
{
	size_t len = 0, i = 0;
	char *v2;

	if(ref == NULL) {
		if (text) {
			return ch_strdup(text);
		} else {
			return NULL;
		}
	}
	
	if ( text != NULL ) {
		len = strlen( text );
		if (text[len-1] != '\n') {
		    i = 1;
		}
	}

	v2 = ch_malloc( len+i+sizeof("Referral:") );
	if( text != NULL ) {
		strcpy(v2, text);
		if( i ) {
			v2[len++] = '\n';
		}
	}
	strcpy( v2+len, "Referral:" );
	len += sizeof("Referral:");

	for( i=0; ref[i].bv_val != NULL; i++ ) {
		v2 = ch_realloc( v2, len + ref[i].bv_len + 1 );
		v2[len-1] = '\n';
		AC_MEMCPY(&v2[len], ref[i].bv_val, ref[i].bv_len );
		len += ref[i].bv_len;
		if (ref[i].bv_val[ref[i].bv_len-1] != '/') {
			++len;
		}
	}

	v2[len-1] = '\0';
	return v2;
}

static ber_tag_t req2res( ber_tag_t tag )
{
	switch( tag ) {
	case LDAP_REQ_ADD:
	case LDAP_REQ_BIND:
	case LDAP_REQ_COMPARE:
	case LDAP_REQ_EXTENDED:
	case LDAP_REQ_MODIFY:
	case LDAP_REQ_MODRDN:
		tag++;
		break;

	case LDAP_REQ_DELETE:
		tag = LDAP_RES_DELETE;
		break;

	case LDAP_REQ_ABANDON:
	case LDAP_REQ_UNBIND:
		tag = LBER_SEQUENCE;
		break;

	case LDAP_REQ_SEARCH:
		tag = LDAP_RES_SEARCH_RESULT;
		break;

	default:
		tag = LBER_SEQUENCE;
	}

	return tag;
}

static long send_ldap_ber(
	Connection *conn,
	BerElement *ber )
{
	ber_len_t bytes;

	ber_get_option( ber, LBER_OPT_BER_BYTES_TO_WRITE, &bytes );

	/* write only one pdu at a time - wait til it's our turn */
	ldap_pvt_thread_mutex_lock( &conn->c_write_mutex );

	/* lock the connection */ 
	ldap_pvt_thread_mutex_lock( &conn->c_mutex );

	/* write the pdu */
	while( 1 ) {
		int err;
		ber_socket_t	sd;

		if ( connection_state_closing( conn ) ) {
			ldap_pvt_thread_mutex_unlock( &conn->c_mutex );
			ldap_pvt_thread_mutex_unlock( &conn->c_write_mutex );

			return 0;
		}

		if ( ber_flush( conn->c_sb, ber, 0 ) == 0 ) {
			break;
		}

		err = errno;

		/*
		 * we got an error.  if it's ewouldblock, we need to
		 * wait on the socket being writable.  otherwise, figure
		 * it's a hard error and return.
		 */

#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "send_ldap_ber: conn %d  ber_flush failed err=%d (%s)\n",
			   conn ? conn->c_connid : 0, err, sock_errstr(err) ));
#else
		Debug( LDAP_DEBUG_CONNS, "ber_flush failed errno=%d reason=\"%s\"\n",
		    err, sock_errstr(err), 0 );
#endif

		if ( err != EWOULDBLOCK && err != EAGAIN ) {
			connection_closing( conn );

			ldap_pvt_thread_mutex_unlock( &conn->c_mutex );
			ldap_pvt_thread_mutex_unlock( &conn->c_write_mutex );

			return( -1 );
		}

		/* wait for socket to be write-ready */
		conn->c_writewaiter = 1;
		ber_sockbuf_ctrl( conn->c_sb, LBER_SB_OPT_GET_FD, &sd );
		slapd_set_write( sd, 1 );

		ldap_pvt_thread_cond_wait( &conn->c_write_cv, &conn->c_mutex );
		conn->c_writewaiter = 0;
	}

	ldap_pvt_thread_mutex_unlock( &conn->c_mutex );
	ldap_pvt_thread_mutex_unlock( &conn->c_write_mutex );

	return bytes;
}

static void
send_ldap_response(
    Connection	*conn,
    Operation	*op,
	ber_tag_t	tag,
	ber_int_t	msgid,
    ber_int_t	err,
    const char	*matched,
    const char	*text,
	BerVarray	ref,
	const char	*resoid,
	struct berval	*resdata,
	struct berval	*sasldata,
	LDAPControl **ctrls
)
{
	char berbuf[256];
	BerElement	*ber = (BerElement *)berbuf;
	int		rc;
	long	bytes;

	if (op->o_callback && op->o_callback->sc_response) {
		op->o_callback->sc_response( conn, op, tag, msgid, err, matched,
			text, ref, resoid, resdata, sasldata, ctrls );
		return;
	}
		
	assert( ctrls == NULL ); /* ctrls not implemented */

	ber_init_w_nullc( ber, LBER_USE_DER );

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		   "send_ldap_response: conn %d	 msgid=%ld tag=%ld err=%ld\n",
		   conn ? conn->c_connid : 0, (long)msgid, (long)tag, (long)err ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"send_ldap_response: msgid=%ld tag=%ld err=%ld\n",
		(long) msgid, (long) tag, (long) err );
#endif

	if( ref ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ARGS,
			   "send_ldap_response: conn %d  ref=\"%s\"\n",
			   conn ? conn->c_connid : 0,
			   ref[0].bv_val ? ref[0].bv_val : "NULL" ));
#else
		Debug( LDAP_DEBUG_ARGS, "send_ldap_response: ref=\"%s\"\n",
			ref[0].bv_val ? ref[0].bv_val : "NULL",
			NULL, NULL );
#endif

	}

#ifdef LDAP_CONNECTIONLESS
	if (conn->c_is_udp) {
	    rc = ber_write(ber, (char *)&op->o_peeraddr, sizeof(struct sockaddr), 0);
	    if (rc != sizeof(struct sockaddr)) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "send_ldap_response: conn %d  ber_write failed\n",
			   conn ? conn->c_connid : 0 ));
#else
		Debug( LDAP_DEBUG_ANY, "ber_write failed\n", 0, 0, 0 );
#endif
		ber_free_buf( ber );
		return;
	    }
	}
	if (conn->c_is_udp && op->o_protocol == LDAP_VERSION2) {
	    rc = ber_printf( ber, "{is{t{ess",
		msgid, "", tag, err,
		matched == NULL ? "" : matched,
		text == NULL ? "" : text );
	} else
#endif
	{
	    rc = ber_printf( ber, "{it{ess",
		msgid, tag, err,
		matched == NULL ? "" : matched,
		text == NULL ? "" : text );
	}

	if( rc != -1 ) {
		if ( ref != NULL ) {
			assert( err == LDAP_REFERRAL );
			rc = ber_printf( ber, "t{W}",
				LDAP_TAG_REFERRAL, ref );
		} else {
			assert( err != LDAP_REFERRAL );
		}
	}

	if( rc != -1 && sasldata != NULL ) {
		rc = ber_printf( ber, "tO",
			LDAP_TAG_SASL_RES_CREDS, sasldata );
	}

	if( rc != -1 && resoid != NULL ) {
		rc = ber_printf( ber, "ts",
			LDAP_TAG_EXOP_RES_OID, resoid );
	}

	if( rc != -1 && resdata != NULL ) {
		rc = ber_printf( ber, "tO",
			LDAP_TAG_EXOP_RES_VALUE, resdata );
	}

	if( rc != -1 ) {
		rc = ber_printf( ber, "N}N}" );
	}
#ifdef LDAP_CONNECTIONLESS
	if( conn->c_is_udp && op->o_protocol == LDAP_VERSION2 && rc != -1 ) {
		rc = ber_printf( ber, "N}" );
	}
#endif

	if ( rc == -1 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "send_ldap_response: conn %d  ber_printf failed\n",
			   conn ? conn->c_connid : 0 ));
#else
		Debug( LDAP_DEBUG_ANY, "ber_printf failed\n", 0, 0, 0 );
#endif

		ber_free_buf( ber );
		return;
	}

	/* send BER */
	bytes = send_ldap_ber( conn, ber );
	ber_free_buf( ber );

	if ( bytes < 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "send_ldap_response: conn %d ber write failed\n",
			   conn ? conn->c_connid : 0 ));
#else
		Debug( LDAP_DEBUG_ANY,
			"send_ldap_response: ber write failed\n",
			0, 0, 0 );
#endif

		return;
	}

	ldap_pvt_thread_mutex_lock( &num_sent_mutex );
	num_bytes_sent += bytes;
	num_pdu_sent++;
	ldap_pvt_thread_mutex_unlock( &num_sent_mutex );
	return;
}


void
send_ldap_disconnect(
    Connection	*conn,
    Operation	*op,
    ber_int_t	err,
    const char	*text
)
{
	ber_tag_t tag;
	ber_int_t msgid;
	char *reqoid;

#define LDAP_UNSOLICITED_ERROR(e) \
	(  (e) == LDAP_PROTOCOL_ERROR \
	|| (e) == LDAP_STRONG_AUTH_REQUIRED \
	|| (e) == LDAP_UNAVAILABLE )

	assert( LDAP_UNSOLICITED_ERROR( err ) );

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		   "send_ldap_disconnect: conn %d  %d:%s\n",
		   conn ? conn->c_connid : 0, err, text ? text : "" ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"send_ldap_disconnect %d:%s\n",
		err, text ? text : "", NULL );
#endif


	if ( op->o_protocol < LDAP_VERSION3 ) {
		reqoid = NULL;
		tag = req2res( op->o_tag );
		msgid = (tag != LBER_SEQUENCE) ? op->o_msgid : 0;

	} else {
		reqoid = LDAP_NOTICE_DISCONNECT;
		tag = LDAP_RES_EXTENDED;
		msgid = 0;
	}

	send_ldap_response( conn, op, tag, msgid,
		err, NULL, text, NULL,
		reqoid, NULL, NULL, NULL );

	Statslog( LDAP_DEBUG_STATS,
	    "conn=%ld op=%ld DISCONNECT tag=%lu err=%ld text=%s\n",
		(long) op->o_connid, (long) op->o_opid,
		(unsigned long) tag, (long) err, text ? text : "" );
}

void
send_ldap_result(
    Connection	*conn,
    Operation	*op,
    ber_int_t	err,
    const char	*matched,
    const char	*text,
	BerVarray ref,
	LDAPControl **ctrls
)
{
	ber_tag_t tag;
	ber_int_t msgid;
	char *tmp = NULL;

	assert( !LDAP_API_ERROR( err ) );

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		   "send_ldap_result : conn %ld	  op=%ld p=%d\n",
		   (long)op->o_connid, (long)op->o_opid, op->o_protocol ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"send_ldap_result: conn=%ld op=%ld p=%d\n",
		(long) op->o_connid, (long) op->o_opid, op->o_protocol );
#endif

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ARGS,
		   "send_ldap_result: conn=%ld err=%d matched=\"%s\" text=\"%s\"\n",
		   (long)op->o_connid, err, matched ? matched : "", text ? text : "" ));
#else
	Debug( LDAP_DEBUG_ARGS,
		"send_ldap_result: err=%d matched=\"%s\" text=\"%s\"\n",
		err, matched ?	matched : "", text ? text : "" );
#endif


	if( ref ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ARGS,
			"send_ldap_result: referral=\"%s\"\n",
			ref[0].bv_val ? ref[0].bv_val : "NULL" ));
#else
		Debug( LDAP_DEBUG_ARGS,
			"send_ldap_result: referral=\"%s\"\n",
			ref[0].bv_val ? ref[0].bv_val : "NULL",
			NULL, NULL );
#endif
	}

	assert( err != LDAP_PARTIAL_RESULTS );

	if ( err == LDAP_REFERRAL ) {
		if( ref == NULL ) {
			err = LDAP_NO_SUCH_OBJECT;
		} else if ( op->o_protocol < LDAP_VERSION3 ) {
			err = LDAP_PARTIAL_RESULTS;
		}
	}

	if ( op->o_protocol < LDAP_VERSION3 ) {
		tmp = v2ref( ref, text );
		text = tmp;
		ref = NULL;
	}

	tag = req2res( op->o_tag );
	msgid = (tag != LBER_SEQUENCE) ? op->o_msgid : 0;

	send_ldap_response( conn, op, tag, msgid,
		err, matched, text, ref,
		NULL, NULL, NULL, ctrls );

	Statslog( LDAP_DEBUG_STATS,
	    "conn=%ld op=%ld RESULT tag=%lu err=%ld text=%s\n",
		(long) op->o_connid, (long) op->o_opid,
		(unsigned long) tag, (long) err, text ? text : "" );

	if( tmp != NULL ) {
		ch_free(tmp);
	}
}

void
send_ldap_sasl(
    Connection	*conn,
    Operation	*op,
    ber_int_t	err,
    const char	*matched,
    const char	*text,
	BerVarray ref,
	LDAPControl **ctrls,
	struct berval *cred
)
{
	ber_tag_t tag;
	ber_int_t msgid;

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		   "send_ldap_sasl: conn %d err=%ld len=%ld\n",
		   op->o_connid, (long)err, cred ? cred->bv_len : -1 ));
#else
	Debug( LDAP_DEBUG_TRACE, "send_ldap_sasl: err=%ld len=%ld\n",
		(long) err, cred ? cred->bv_len : -1, NULL );
#endif


	tag = req2res( op->o_tag );
	msgid = (tag != LBER_SEQUENCE) ? op->o_msgid : 0;

	send_ldap_response( conn, op, tag, msgid,
		err, matched, text, ref,
		NULL, NULL, cred, ctrls	 );
}

void
send_ldap_extended(
    Connection	*conn,
    Operation	*op,
    ber_int_t	err,
    const char	*matched,
    const char	*text,
    BerVarray	refs,
    const char		*rspoid,
	struct berval *rspdata,
	LDAPControl **ctrls
)
{
	ber_tag_t tag;
	ber_int_t msgid;

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		   "send_ldap_extended: conn %d	 err=%ld oid=%s len=%ld\n",
		   op->o_connid, (long)err, rspoid ? rspoid : "",
		   rspdata != NULL ? (long)rspdata->bv_len : (long)0 ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"send_ldap_extended err=%ld oid=%s len=%ld\n",
		(long) err,
		rspoid ? rspoid : "",
		rspdata != NULL ? (long) rspdata->bv_len : (long) 0 );
#endif


	tag = req2res( op->o_tag );
	msgid = (tag != LBER_SEQUENCE) ? op->o_msgid : 0;

	send_ldap_response( conn, op, tag, msgid,
		err, matched, text, refs,
		rspoid, rspdata, NULL, ctrls );
}


void
send_search_result(
    Connection	*conn,
    Operation	*op,
    ber_int_t	err,
    const char	*matched,
	const char	*text,
    BerVarray	refs,
	LDAPControl **ctrls,
    int		nentries
)
{
	ber_tag_t tag;
	ber_int_t msgid;
	char *tmp = NULL;

	assert( !LDAP_API_ERROR( err ) );

	if (op->o_callback && op->o_callback->sc_sresult) {
		op->o_callback->sc_sresult(conn, op, err, matched, text, refs,
			ctrls, nentries);
		return;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		   "send_search_result: conn %d err=%d matched=\"%s\"\n",
		   op->o_connid, err, matched ? matched : "",
		   text ? text : "" ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"send_search_result: err=%d matched=\"%s\" text=\"%s\"\n",
		err, matched ?	matched : "", text ? text : "" );
#endif


	assert( err != LDAP_PARTIAL_RESULTS );

	if( op->o_protocol < LDAP_VERSION3 ) {
		/* send references in search results */
		if( err == LDAP_REFERRAL ) {
			err = LDAP_PARTIAL_RESULTS;
		}

		tmp = v2ref( refs, text );
		text = tmp;
		refs = NULL;

	} else {
		/* don't send references in search results */
		assert( refs == NULL );
		refs = NULL;

		if( err == LDAP_REFERRAL ) {
			err = LDAP_SUCCESS;
		}
	}

	tag = req2res( op->o_tag );
	msgid = (tag != LBER_SEQUENCE) ? op->o_msgid : 0;

	send_ldap_response( conn, op, tag, msgid,
		err, matched, text, refs,
		NULL, NULL, NULL, ctrls );

	Statslog( LDAP_DEBUG_STATS,
	    "conn=%ld op=%ld SEARCH RESULT tag=%lu err=%ld text=%s\n",
		(long) op->o_connid, (long) op->o_opid,
		(unsigned long) tag, (long) err, text ? text : "" );

	if (tmp != NULL) {
	    ch_free(tmp);
	}
}

static struct berval AllUser = { sizeof(LDAP_ALL_USER_ATTRIBUTES)-1,
	LDAP_ALL_USER_ATTRIBUTES };
static struct berval AllOper = { sizeof(LDAP_ALL_OPERATIONAL_ATTRIBUTES)-1,
	LDAP_ALL_OPERATIONAL_ATTRIBUTES };

int
send_search_entry(
    Backend	*be,
    Connection	*conn,
    Operation	*op,
    Entry	*e,
    AttributeName	*attrs,
    int		attrsonly,
	LDAPControl **ctrls
)
{
	char		berbuf[256];
	BerElement	*ber = (BerElement *)berbuf;
	Attribute	*a, *aa;
	int		i, rc=-1, bytes;
	char		*edn;
	int		userattrs;
	int		opattrs;
	static AccessControlState acl_state_init = ACL_STATE_INIT;
	AccessControlState acl_state;

	AttributeDescription *ad_entry = slap_schema.si_ad_entry;

	if (op->o_callback && op->o_callback->sc_sendentry) {
		return op->o_callback->sc_sendentry( be, conn, op, e, attrs,
			attrsonly, ctrls );
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		   "send_search_entry: conn %d	dn=\"%s\"%s\n",
		   op->o_connid, e->e_dn,
		   attrsonly ? " (attrsOnly)" : "" ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"=> send_search_entry: dn=\"%s\"%s\n",
		e->e_dn, attrsonly ? " (attrsOnly)" : "", 0 );
#endif

	if ( ! access_allowed( be, conn, op, e,
		ad_entry, NULL, ACL_READ, NULL ) )
	{
#ifdef NEW_LOGGING
		LDAP_LOG(( "acl", LDAP_LEVEL_INFO,
			   "send_search_entry: conn %d access to entry (%s) not allowed\n",
			   op->o_connid, e->e_dn ));
#else
		Debug( LDAP_DEBUG_ACL,
			"send_search_entry: access to entry not allowed\n",
		    0, 0, 0 );
#endif

		return( 1 );
	}

	edn = e->e_ndn;

	ber_init_w_nullc( ber, LBER_USE_DER );

#ifdef LDAP_CONNECTIONLESS
	if (conn->c_is_udp) {
	    rc = ber_write(ber, (char *)&op->o_peeraddr, sizeof(struct sockaddr), 0);
	    if (rc != sizeof(struct sockaddr)) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "send_search_entry: conn %d  ber_printf failed\n",
			   conn ? conn->c_connid : 0 ));
#else
		Debug( LDAP_DEBUG_ANY, "ber_printf failed\n", 0, 0, 0 );
#endif
		ber_free_buf( ber );
		return;
	    }
	}
	if (conn->c_is_udp && op->o_protocol == LDAP_VERSION2) {
	    rc = ber_printf( ber, "{is{t{O{" /*}}}*/,
		op->o_msgid, "", LDAP_RES_SEARCH_ENTRY, &e->e_name );
	} else
#endif
	{
	    rc = ber_printf( ber, "{it{O{" /*}}}*/, op->o_msgid,
		LDAP_RES_SEARCH_ENTRY, &e->e_name );
	}

	if ( rc == -1 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "send_search_entry: conn %d  ber_printf failed\n",
			   op->o_connid ));
#else
		Debug( LDAP_DEBUG_ANY, "ber_printf failed\n", 0, 0, 0 );
#endif

		ber_free_buf( ber );
		send_ldap_result( conn, op, LDAP_OTHER,
		    NULL, "encoding DN error", NULL, NULL );
		goto error_return;
	}

	/* check for special all user attributes ("*") type */
	userattrs = ( attrs == NULL ) ? 1
		: an_find( attrs, &AllUser );

	/* check for special all operational attributes ("+") type */
	opattrs = ( attrs == NULL ) ? 0
		: an_find( attrs, &AllOper );

	for ( a = e->e_attrs; a != NULL; a = a->a_next ) {
		AttributeDescription *desc = a->a_desc;

		if ( attrs == NULL ) {
			/* all attrs request, skip operational attributes */
			if( is_at_operational( desc->ad_type ) ) {
				continue;
			}

		} else {
			/* specific attrs requested */
			if ( is_at_operational( desc->ad_type ) ) {
				if( !opattrs && !ad_inlist( desc, attrs ) ) {
					continue;
				}

			} else {
				if (!userattrs && !ad_inlist( desc, attrs ) ) {
					continue;
				}
			}
		}

		acl_state = acl_state_init;

		if ( ! access_allowed( be, conn, op, e, desc, NULL,
			ACL_READ, &acl_state ) )
		{
#ifdef NEW_LOGGING
			LDAP_LOG(( "acl", LDAP_LEVEL_INFO, "send_search_entry: "
				"conn %d  access to attribute %s not allowed\n",
				op->o_connid, desc->ad_cname.bv_val ));
#else
			Debug( LDAP_DEBUG_ACL, "acl: "
				"access to attribute %s not allowed\n",
			    desc->ad_cname.bv_val, 0, 0 );
#endif
			continue;
		}

		if (( rc = ber_printf( ber, "{O[" /*]}*/ , &desc->ad_cname )) == -1 ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
				"send_search_entry: conn %d  ber_printf failed\n",
				op->o_connid ));
#else
			Debug( LDAP_DEBUG_ANY, "ber_printf failed\n", 0, 0, 0 );
#endif

			ber_free_buf( ber );
			send_ldap_result( conn, op, LDAP_OTHER,
			    NULL, "encoding description error", NULL, NULL );
			goto error_return;
		}

		if ( ! attrsonly ) {
			for ( i = 0; a->a_vals[i].bv_val != NULL; i++ ) {
				if ( ! access_allowed( be, conn, op, e,
					desc, &a->a_vals[i], ACL_READ, &acl_state ) )
				{
#ifdef NEW_LOGGING
					LDAP_LOG(( "acl", LDAP_LEVEL_INFO,
						"send_search_entry: conn %d "
						"access to attribute %s, value %d not allowed\n",
						op->o_connid, desc->ad_cname.bv_val, i ));
#else
					Debug( LDAP_DEBUG_ACL,
						"acl: access to attribute %s, value %d not allowed\n",
					desc->ad_cname.bv_val, i, 0 );
#endif

					continue;
				}

				if (( rc = ber_printf( ber, "O", &a->a_vals[i] )) == -1 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
						"send_search_entry: conn %d  ber_printf failed.\n",
						op->o_connid ));
#else
					Debug( LDAP_DEBUG_ANY,
					    "ber_printf failed\n", 0, 0, 0 );
#endif

					ber_free_buf( ber );
					send_ldap_result( conn, op, LDAP_OTHER,
						NULL, "encoding values error", NULL, NULL );
					goto error_return;
				}
			}
		}

		if (( rc = ber_printf( ber, /*{[*/ "]N}" )) == -1 ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
				"send_search_entry: conn %d  ber_printf failed\n",
				op->o_connid ));
#else
			Debug( LDAP_DEBUG_ANY, "ber_printf failed\n", 0, 0, 0 );
#endif

			ber_free_buf( ber );
			send_ldap_result( conn, op, LDAP_OTHER,
			    NULL, "encode end error", NULL, NULL );
			goto error_return;
		}
	}

	/* eventually will loop through generated operational attributes */
	/* only have subschemaSubentry implemented */
	aa = backend_operational( be, conn, op, e, attrs, opattrs );
	
	for (a = aa ; a != NULL; a = a->a_next ) {
		AttributeDescription *desc = a->a_desc;

		if ( attrs == NULL ) {
			/* all attrs request, skip operational attributes */
			if( is_at_operational( desc->ad_type ) ) {
				continue;
			}

		} else {
			/* specific attrs requested */
			if( is_at_operational( desc->ad_type ) ) {
				if( !opattrs && !ad_inlist( desc, attrs ) ) {
					continue;
				}
			} else {
				if (!userattrs && !ad_inlist( desc, attrs ) )
				{
					continue;
				}
			}
		}

		acl_state = acl_state_init;

		if ( ! access_allowed( be, conn, op, e,	desc, NULL,
			ACL_READ, &acl_state ) )
		{
#ifdef NEW_LOGGING
			LDAP_LOG(( "acl", LDAP_LEVEL_INFO,
				"send_search_entry: conn %s "
				"access to attribute %s not allowed\n",
				op->o_connid, desc->ad_cname.bv_val ));
#else
			Debug( LDAP_DEBUG_ACL, "acl: access to attribute %s not allowed\n",
			    desc->ad_cname.bv_val, 0, 0 );
#endif

			continue;
		}

		rc = ber_printf( ber, "{O[" /*]}*/ , &desc->ad_cname );
		if ( rc == -1 ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
				"send_search_entry: conn %d  ber_printf failed\n",
				op->o_connid ));
#else
			Debug( LDAP_DEBUG_ANY, "ber_printf failed\n", 0, 0, 0 );
#endif

			ber_free_buf( ber );
			send_ldap_result( conn, op, LDAP_OTHER,
			    NULL, "encoding description error", NULL, NULL );
			attrs_free( aa );
			goto error_return;
		}

		if ( ! attrsonly ) {
			for ( i = 0; a->a_vals[i].bv_val != NULL; i++ ) {
				if ( ! access_allowed( be, conn, op, e,
					desc, &a->a_vals[i], ACL_READ, &acl_state ) )
				{
#ifdef NEW_LOGGING
					LDAP_LOG(( "acl", LDAP_LEVEL_INFO,
						"send_search_entry: conn %d "
						"access to %s, value %d not allowed\n",
						op->o_connid, desc->ad_cname.bv_val, i ));
#else
					Debug( LDAP_DEBUG_ACL,
						"acl: access to attribute %s, value %d not allowed\n",
					desc->ad_cname.bv_val, i, 0 );
#endif

					continue;
				}

				if (( rc = ber_printf( ber, "O", &a->a_vals[i] )) == -1 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
						   "send_search_entry: conn %d  ber_printf failed\n",
						   op->o_connid ));
#else
					Debug( LDAP_DEBUG_ANY,
					    "ber_printf failed\n", 0, 0, 0 );
#endif

					ber_free_buf( ber );
					send_ldap_result( conn, op, LDAP_OTHER,
						NULL, "encoding values error", NULL, NULL );
					attrs_free( aa );
					goto error_return;
				}
			}
		}

		if (( rc = ber_printf( ber, /*{[*/ "]N}" )) == -1 ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
				   "send_search_entry: conn %d  ber_printf failed\n",
				   op->o_connid ));
#else
			Debug( LDAP_DEBUG_ANY, "ber_printf failed\n", 0, 0, 0 );
#endif

			ber_free_buf( ber );
			send_ldap_result( conn, op, LDAP_OTHER,
			    NULL, "encode end error", NULL, NULL );
			attrs_free( aa );
			goto error_return;
		}
	}

	attrs_free( aa );

	rc = ber_printf( ber, /*{{{*/ "}N}N}" );

#ifdef LDAP_CONNECTIONLESS
	if (conn->c_is_udp && op->o_protocol == LDAP_VERSION2 && rc != -1)
		rc = ber_printf( ber, "}" );
#endif
	if ( rc == -1 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "send_search_entry: conn %d ber_printf failed\n",
			   op->o_connid ));
#else
		Debug( LDAP_DEBUG_ANY, "ber_printf failed\n", 0, 0, 0 );
#endif

		ber_free_buf( ber );
		send_ldap_result( conn, op, LDAP_OTHER,
			NULL, "encode entry end error", NULL, NULL );
		return( 1 );
	}

	bytes = send_ldap_ber( conn, ber );
	ber_free_buf( ber );

	if ( bytes < 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "send_ldap_response: conn %d  ber write failed.\n",
			   op->o_connid ));
#else
		Debug( LDAP_DEBUG_ANY,
			"send_ldap_response: ber write failed\n",
			0, 0, 0 );
#endif

		return -1;
	}

	ldap_pvt_thread_mutex_lock( &num_sent_mutex );
	num_bytes_sent += bytes;
	num_entries_sent++;
	num_pdu_sent++;
	ldap_pvt_thread_mutex_unlock( &num_sent_mutex );

	Statslog( LDAP_DEBUG_STATS2, "conn=%ld op=%ld ENTRY dn=\"%s\"\n",
	    (long) conn->c_connid, (long) op->o_opid, e->e_dn, 0, 0 );

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		   "send_search_entry: conn %d exit.\n",
		   op->o_connid ));
#else
	Debug( LDAP_DEBUG_TRACE, "<= send_search_entry\n", 0, 0, 0 );
#endif

	rc = 0;

error_return:;
	return( rc );
}

int
send_search_reference(
    Backend	*be,
    Connection	*conn,
    Operation	*op,
    Entry	*e,
	BerVarray refs,
	LDAPControl **ctrls,
    BerVarray *v2refs
)
{
	char		berbuf[256];
	BerElement	*ber = (BerElement *)berbuf;
	int rc;
	int bytes;

	AttributeDescription *ad_ref = slap_schema.si_ad_ref;
	AttributeDescription *ad_entry = slap_schema.si_ad_entry;

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		"send_search_reference: conn %d  dn=\"%s\"\n",
		op->o_connid, e->e_dn ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"=> send_search_reference: dn=\"%s\"\n",
		e->e_dn, 0, 0 );
#endif


	if ( ! access_allowed( be, conn, op, e,
		ad_entry, NULL, ACL_READ, NULL ) )
	{
#ifdef NEW_LOGGING
		LDAP_LOG(( "acl", LDAP_LEVEL_INFO,
			"send_search_reference: conn %d	access to entry %s not allowed\n",
			op->o_connid, e->e_dn ));
#else
		Debug( LDAP_DEBUG_ACL,
			"send_search_reference: access to entry not allowed\n",
		    0, 0, 0 );
#endif

		return( 1 );
	}

	if ( ! access_allowed( be, conn, op, e,
		ad_ref, NULL, ACL_READ, NULL ) )
	{
#ifdef NEW_LOGGING
		LDAP_LOG(( "acl", LDAP_LEVEL_INFO,
			"send_search_reference: conn %d access to reference not allowed.\n",
			op->o_connid ));
#else
		Debug( LDAP_DEBUG_ACL,
			"send_search_reference: access to reference not allowed\n",
		    0, 0, 0 );
#endif

		return( 1 );
	}

	if( refs == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			"send_search_reference: null ref in (%s).\n",
			op->o_connid, e->e_dn ));
#else
		Debug( LDAP_DEBUG_ANY,
			"send_search_reference: null ref in (%s)\n", 
			e->e_dn, 0, 0 );
#endif

		return( 1 );
	}

	if( op->o_protocol < LDAP_VERSION3 ) {
		/* save the references for the result */
		if( refs[0].bv_val != NULL ) {
			value_add( v2refs, refs );
		}
		return 0;
	}

	ber_init_w_nullc( ber, LBER_USE_DER );

	rc = ber_printf( ber, "{it{W}N}", op->o_msgid,
		LDAP_RES_SEARCH_REFERENCE, refs );

	if ( rc == -1 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			"send_search_reference: conn %d	ber_printf failed.\n",
			op->o_connid ));
#else
		Debug( LDAP_DEBUG_ANY,
			"send_search_reference: ber_printf failed\n", 0, 0, 0 );
#endif

		ber_free_buf( ber );
		send_ldap_result( conn, op, LDAP_OTHER,
			NULL, "encode DN error", NULL, NULL );
		return -1;
	}

	bytes = send_ldap_ber( conn, ber );
	ber_free_buf( ber );

	ldap_pvt_thread_mutex_lock( &num_sent_mutex );
	num_bytes_sent += bytes;
	num_refs_sent++;
	num_pdu_sent++;
	ldap_pvt_thread_mutex_unlock( &num_sent_mutex );

	Statslog( LDAP_DEBUG_STATS2, "conn=%ld op=%ld REF dn=\"%s\"\n",
		(long) conn->c_connid, (long) op->o_opid, e->e_dn, 0, 0 );

#ifdef NEW_LOGGING
	LDAP_LOG(( "operation", LDAP_LEVEL_ENTRY,
		"send_search_reference: conn %d exit.\n", op->o_connid ));
#else
	Debug( LDAP_DEBUG_TRACE, "<= send_search_reference\n", 0, 0, 0 );
#endif

	return 0;
}


int
str2result(
    char	*s,
    int		*code,
    char	**matched,
    char	**info
)
{
	int	rc;
	char	*c;

	*code = LDAP_SUCCESS;
	*matched = NULL;
	*info = NULL;

	if ( strncasecmp( s, "RESULT", 6 ) != 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
			   "str2result: (%s), expecting \"RESULT\"\n", s ));
#else
		Debug( LDAP_DEBUG_ANY, "str2result (%s) expecting \"RESULT\"\n",
		    s, 0, 0 );
#endif


		return( -1 );
	}

	rc = 0;
	while ( (s = strchr( s, '\n' )) != NULL ) {
		*s++ = '\0';
		if ( *s == '\0' ) {
			break;
		}
		if ( (c = strchr( s, ':' )) != NULL ) {
			c++;
		}

		if ( strncasecmp( s, "code", 4 ) == 0 ) {
			if ( c != NULL ) {
				*code = atoi( c );
			}
		} else if ( strncasecmp( s, "matched", 7 ) == 0 ) {
			if ( c != NULL ) {
				*matched = c;
			}
		} else if ( strncasecmp( s, "info", 4 ) == 0 ) {
			if ( c != NULL ) {
				*info = c;
			}
		} else {
#ifdef NEW_LOGGING
			LDAP_LOG(( "operation", LDAP_LEVEL_INFO,
				"str2result: (%s) unknown.\n", s ));
#else
			Debug( LDAP_DEBUG_ANY, "str2result (%s) unknown\n",
			    s, 0, 0 );
#endif

			rc = -1;
		}
	}

	return( rc );
}
