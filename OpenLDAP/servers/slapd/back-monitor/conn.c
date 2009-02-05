/* conn.c - deal with connection subsystem */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-monitor/conn.c,v 1.43.2.4 2004/03/18 00:56:29 kurt Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2001-2004 The OpenLDAP Foundation.
 * Portions Copyright 2001-2003 Pierangelo Masarati.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by Pierangelo Masarati for inclusion
 * in OpenLDAP Software.
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#include "slap.h"
#include "lutil.h"
#include "back-monitor.h"

#define CONN_CN_PREFIX	"Connection"

int
monitor_subsys_conn_init(
	BackendDB		*be
)
{
	struct monitorinfo	*mi;
	
	Entry			*e, *e_tmp, *e_conn;
	struct monitorentrypriv	*mp;
	char			buf[ BACKMONITOR_BUFSIZE ];
	struct berval		bv;

	assert( be != NULL );

	mi = ( struct monitorinfo * )be->be_private;

	if ( monitor_cache_get( mi,
			&monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn, &e_conn ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, CRIT,
			"monitor_subsys_conn_init: "
			"unable to get entry '%s'\n",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"monitor_subsys_conn_init: "
			"unable to get entry '%s'\n%s%s",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val, 
			"", "" );
#endif
		return( -1 );
	}

	e_tmp = NULL;

	/*
	 * Total conns
	 */
	snprintf( buf, sizeof( buf ),
		"dn: cn=Total,%s\n"
		"objectClass: %s\n"
		"structuralObjectClass: %s\n"
		"cn: Total\n"
		"createTimestamp: %s\n"
		"modifyTimestamp: %s\n",
		monitor_subsys[SLAPD_MONITOR_CONN].mss_dn.bv_val,
		mi->mi_oc_monitorCounterObject->soc_cname.bv_val,
		mi->mi_oc_monitorCounterObject->soc_cname.bv_val,
		mi->mi_startTime.bv_val,
		mi->mi_startTime.bv_val );
	
	e = str2entry( buf );
	if ( e == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, CRIT,
			"monitor_subsys_conn_init: "
			"unable to create entry 'cn=Total,%s'\n",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"monitor_subsys_conn_init: "
			"unable to create entry 'cn=Total,%s'\n%s%s",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val,
			"", "" );
#endif
		return( -1 );
	}
	
	bv.bv_val = "0";
	bv.bv_len = 1;
	attr_merge_one( e, mi->mi_ad_monitorCounter, &bv, NULL );
	
	mp = ( struct monitorentrypriv * )ch_calloc( sizeof( struct monitorentrypriv ), 1 );
	e->e_private = ( void * )mp;
	mp->mp_next = e_tmp;
	mp->mp_children = NULL;
	mp->mp_info = &monitor_subsys[SLAPD_MONITOR_CONN];
	mp->mp_flags = monitor_subsys[SLAPD_MONITOR_CONN].mss_flags \
		| MONITOR_F_SUB | MONITOR_F_PERSISTENT;
	mp->mp_flags &= ~MONITOR_F_VOLATILE_CH;

	if ( monitor_cache_add( mi, e ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, CRIT,
			"monitor_subsys_conn_init: "
			"unable to add entry 'cn=Total,%s'\n",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"monitor_subsys_conn_init: "
			"unable to add entry 'cn=Total,%s'\n%s%s",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val,
			"", "" );
#endif
		return( -1 );
	}
	
	e_tmp = e;

	/*
	 * Current conns
	 */
	snprintf( buf, sizeof( buf ),
		"dn: cn=Current,%s\n"
		"objectClass: %s\n"
		"structuralObjectClass: %s\n"
		"cn: Current\n"
		"createTimestamp: %s\n"
		"modifyTimestamp: %s\n",
		monitor_subsys[SLAPD_MONITOR_CONN].mss_dn.bv_val,
		mi->mi_oc_monitorCounterObject->soc_cname.bv_val,
		mi->mi_oc_monitorCounterObject->soc_cname.bv_val,
		mi->mi_startTime.bv_val,
		mi->mi_startTime.bv_val );
	
	e = str2entry( buf );
	if ( e == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, CRIT,
			"monitor_subsys_conn_init: "
			"unable to create entry 'cn=Current,%s'\n",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"monitor_subsys_conn_init: "
			"unable to create entry 'cn=Current,%s'\n%s%s",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val,
			"", "" );
#endif
		return( -1 );
	}
	
	bv.bv_val = "0";
	bv.bv_len = 1;
	attr_merge_one( e, mi->mi_ad_monitorCounter, &bv, NULL );
	
	mp = ( struct monitorentrypriv * )ch_calloc( sizeof( struct monitorentrypriv ), 1 );
	e->e_private = ( void * )mp;
	mp->mp_next = e_tmp;
	mp->mp_children = NULL;
	mp->mp_info = &monitor_subsys[SLAPD_MONITOR_CONN];
	mp->mp_flags = monitor_subsys[SLAPD_MONITOR_CONN].mss_flags \
		| MONITOR_F_SUB | MONITOR_F_PERSISTENT;
	mp->mp_flags &= ~MONITOR_F_VOLATILE_CH;

	if ( monitor_cache_add( mi, e ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, CRIT,
			"monitor_subsys_conn_init: "
			"unable to add entry 'cn=Current,%s'\n",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"monitor_subsys_conn_init: "
			"unable to add entry 'cn=Current,%s'\n%s%s",
			monitor_subsys[SLAPD_MONITOR_CONN].mss_ndn.bv_val,
			"", "" );
#endif
		return( -1 );
	}
	
	e_tmp = e;

	mp = ( struct monitorentrypriv * )e_conn->e_private;
	mp->mp_children = e_tmp;

	monitor_cache_release( mi, e_conn );

	return( 0 );
}

int
monitor_subsys_conn_update(
	Operation		*op,
	Entry                   *e
)
{
	struct monitorinfo *mi = (struct monitorinfo *)op->o_bd->be_private;
	long 		n = -1;

	assert( mi );
	assert( e );
	
	if ( strncasecmp( e->e_ndn, "cn=total", 
				sizeof("cn=total")-1 ) == 0 ) {
		n = connections_nextid();

	} else if ( strncasecmp( e->e_ndn, "cn=current", 
				sizeof("cn=current")-1 ) == 0 ) {
		Connection	*c;
		int		connindex;

		for ( n = 0, c = connection_first( &connindex );
				c != NULL;
				n++, c = connection_next( c, &connindex ) ) {
			/* No Op */ ;
		}
		connection_done(c);
	}

	if ( n != -1 ) {
		Attribute	*a;
		char		buf[] = "+9223372036854775807L";

		a = attr_find( e->e_attrs, mi->mi_ad_monitorCounter );
		if ( a == NULL ) {
			return( -1 );
		}

		snprintf( buf, sizeof( buf ), "%ld", n );
		free( a->a_vals[ 0 ].bv_val );
		ber_str2bv( buf, 0, 1, a->a_vals );
	}

	return( 0 );
}

static int
conn_create(
	struct monitorinfo	*mi,
	Connection		*c,
	Entry			**ep
)
{
	struct monitorentrypriv *mp;
	struct tm		*ltm;
	char			buf[ BACKMONITOR_BUFSIZE ];
	char			buf2[ LDAP_LUTIL_GENTIME_BUFSIZE ];
	char			buf3[ LDAP_LUTIL_GENTIME_BUFSIZE ];

	struct berval           bv;

	Entry			*e;

	struct tm	*ctm;
	char		ctmbuf[ LDAP_LUTIL_GENTIME_BUFSIZE ];
	struct tm	*mtm;
	char		mtmbuf[ LDAP_LUTIL_GENTIME_BUFSIZE ];
#ifdef HAVE_GMTIME_R
	struct tm	tm_buf;
#endif /* HAVE_GMTIME_R */

	assert( c != NULL );
	assert( ep != NULL );

#ifndef HAVE_GMTIME_R
	ldap_pvt_thread_mutex_lock( &gmtime_mutex );
#endif
#ifdef HACK_LOCAL_TIME
# ifdef HAVE_LOCALTIME_R
	ctm = localtime_r( &c->c_starttime, &tm_buf );
	lutil_localtime( ctmbuf, sizeof( ctmbuf ), ctm, -timezone );
	mtm = localtime_r( &c->c_activitytime, &tm_buf );
	lutil_localtime( mtmbuf, sizeof( mtmbuf ), mtm, -timezone );
# else
	ctm = localtime( &c->c_starttime );
	lutil_localtime( ctmbuf, sizeof( ctmbuf ), ctm, -timezone );
	mtm = localtime( &c->c_activitytime );
	lutil_localtime( mtmbuf, sizeof( mtmbuf ), mtm, -timezone );
# endif /* HAVE_LOCALTIME_R */
#else /* !HACK_LOCAL_TIME */
# ifdef HAVE_GMTIME_R
	ctm = gmtime_r( &c->c_starttime, &tm_buf );
	lutil_gentime( ctmbuf, sizeof( ctmbuf ), ctm );
	mtm = gmtime_r( &c->c_activitytime, &tm_buf );
	lutil_gentime( mtmbuf, sizeof( mtmbuf ), mtm );
# else
	ctm = gmtime( &c->c_starttime );
	lutil_gentime( ctmbuf, sizeof( ctmbuf ), ctm );
	mtm = gmtime( &c->c_activitytime );
	lutil_gentime( mtmbuf, sizeof( mtmbuf ), mtm );
# endif /* HAVE_GMTIME_R */
#endif /* !HACK_LOCAL_TIME */
#ifndef HAVE_GMTIME_R
	ldap_pvt_thread_mutex_unlock( &gmtime_mutex );
#endif

	snprintf( buf, sizeof( buf ),
		"dn: cn=" CONN_CN_PREFIX " %ld,%s\n"
		"objectClass: %s\n"
		"structuralObjectClass: %s\n"
		"cn: " CONN_CN_PREFIX " %ld\n"
		"createTimestamp: %s\n"
		"modifyTimestamp: %s\n",
		c->c_connid, monitor_subsys[SLAPD_MONITOR_CONN].mss_dn.bv_val,
		mi->mi_oc_monitorConnection->soc_cname.bv_val,
		mi->mi_oc_monitorConnection->soc_cname.bv_val,
		c->c_connid,
		ctmbuf, mtmbuf );
		
	e = str2entry( buf );

	if ( e == NULL) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, CRIT,
			"monitor_subsys_conn_create: "
			"unable to create entry "
			"'cn=" CONN_CN_PREFIX " %ld,%s' entry\n",
			c->c_connid, monitor_subsys[SLAPD_MONITOR_CONN].mss_dn.bv_val, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"monitor_subsys_conn_create: "
			"unable to create entry "
			"'cn=" CONN_CN_PREFIX " %ld,%s' entry\n",
			c->c_connid, 
			monitor_subsys[SLAPD_MONITOR_CONN].mss_dn.bv_val, 0 );
#endif
		return( -1 );
	}

#ifndef HAVE_GMTIME_R
	ldap_pvt_thread_mutex_lock( &gmtime_mutex );
#endif

#ifdef HAVE_GMTIME_R
	ltm = gmtime_r( &c->c_starttime, &tm_buf );
#else
	ltm = gmtime( &c->c_starttime );
#endif
	lutil_gentime( buf2, sizeof( buf2 ), ltm );

#ifdef HAVE_GMTIME_R
	ltm = gmtime_r( &c->c_activitytime, &tm_buf );
#else
	ltm = gmtime( &c->c_activitytime );
#endif
	lutil_gentime( buf3, sizeof( buf3 ), ltm );

#ifndef HAVE_GMTIME_R
	ldap_pvt_thread_mutex_unlock( &gmtime_mutex );
#endif /* HAVE_GMTIME_R */

	/* monitored info */
	sprintf( buf,
		"%ld : %ld "
		": %ld/%ld/%ld/%ld "
		": %ld/%ld/%ld "
		": %s%s%s%s%s%s "
		": %s : %s : %s "
		": %s : %s : %s : %s",
		c->c_connid,
		(long) c->c_protocol,
		c->c_n_ops_received, c->c_n_ops_executing,
		c->c_n_ops_pending, c->c_n_ops_completed,
		
		/* add low-level counters here */
		c->c_n_get, c->c_n_read, c->c_n_write,
		
		c->c_currentber ? "r" : "",
		c->c_writewaiter ? "w" : "",
		LDAP_STAILQ_EMPTY( &c->c_ops ) ? "" : "x",
		LDAP_STAILQ_EMPTY( &c->c_pending_ops ) ? "" : "p",
		connection_state2str( c->c_conn_state ),
		c->c_sasl_bind_in_progress ? "S" : "",
		
		c->c_dn.bv_len ? c->c_dn.bv_val : SLAPD_ANONYMOUS,
		
		c->c_listener_url.bv_val,
		c->c_peer_domain.bv_val,
		c->c_peer_name.bv_val,
		c->c_sock_name.bv_val,
		
		buf2,
		buf3
		);

	bv.bv_val = buf;
	bv.bv_len = strlen( buf );
	attr_merge_one( e, mi->mi_ad_monitoredInfo, &bv, NULL );

	/* connection number */
	snprintf( buf, sizeof( buf ), "%ld", c->c_connid );
	bv.bv_val = buf;
	bv.bv_len = strlen( buf );
	attr_merge_one( e, mi->mi_ad_monitorConnectionNumber, &bv, NULL );

	/* authz DN */
	attr_merge_one( e, mi->mi_ad_monitorConnectionAuthzDN,
			&c->c_dn, &c->c_ndn );

	/* local address */
	attr_merge_one( e, mi->mi_ad_monitorConnectionLocalAddress,
			&c->c_sock_name, NULL );

	/* peer address */
	attr_merge_one( e, mi->mi_ad_monitorConnectionPeerAddress,
			&c->c_peer_name, NULL );

	mp = ( struct monitorentrypriv * )ch_calloc( sizeof( struct monitorentrypriv ), 1 );
	e->e_private = ( void * )mp;
	mp->mp_info = &monitor_subsys[ SLAPD_MONITOR_CONN ];
	mp->mp_children = NULL;
	mp->mp_flags = MONITOR_F_SUB | MONITOR_F_VOLATILE;

	*ep = e;

	return( 0 );
}

int 
monitor_subsys_conn_create( 
	Operation		*op,
	struct berval		*ndn,
	Entry 			*e_parent,
	Entry			**ep
)
{
	struct monitorinfo *mi = (struct monitorinfo *)op->o_bd->be_private;
	Connection		*c;
	int			connindex;
	struct monitorentrypriv *mp;

	assert( mi != NULL );
	assert( e_parent != NULL );
	assert( ep != NULL );

	*ep = NULL;

	if ( ndn == NULL ) {
		Entry *e, *e_tmp = NULL;

		/* create all the children of e_parent */
		for ( c = connection_first( &connindex );
				c != NULL;
				c = connection_next( c, &connindex )) {
			if ( conn_create( mi, c, &e ) || e == NULL ) {
				connection_done(c);
				for ( ; e_tmp != NULL; ) {
					mp = ( struct monitorentrypriv * )e_tmp->e_private;
					e = mp->mp_next;

					ch_free( mp );
					e_tmp->e_private = NULL;
					entry_free( e_tmp );

					e_tmp = e;
				}
				return( -1 );
			}
			mp = ( struct monitorentrypriv * )e->e_private;
			mp->mp_next = e_tmp;
			e_tmp = e;
		}
		connection_done(c);

		*ep = e;

	} else {
		LDAPRDN		values = NULL;
		const char	*text = NULL;
		unsigned long 	connid;
	       
		/* create exactly the required entry */

		if ( ldap_bv2rdn( ndn, &values, (char **)&text,
			LDAP_DN_FORMAT_LDAP ) )
		{
			return( -1 );
		}
		
		assert( values );
		assert( values[ 0 ] );

		connid = atol( values[ 0 ]->la_value.bv_val
				+ sizeof( CONN_CN_PREFIX ) );

		ldap_rdnfree( values );

		for ( c = connection_first( &connindex );
				c != NULL;
				c = connection_next( c, &connindex )) {
			if ( c->c_connid == connid ) {
				if ( conn_create( mi, c, ep ) || *ep == NULL ) {
					connection_done( c );
					return( -1 );
				}

				break;
			}
		}
		
		connection_done(c);
	
	}

	return( 0 );
}

