/* $OpenLDAP: pkg/ldap/servers/slapd/connection.c,v 1.163 2002/01/29 05:06:20 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>
#include <limits.h>

#include <ac/socket.h>
#include <ac/errno.h>
#include <ac/signal.h>
#include <ac/string.h>
#include <ac/time.h>
#include <ac/unistd.h>

#include "ldap_pvt.h"
#include "lutil.h"
#include "slap.h"

/* protected by connections_mutex */
static ldap_pvt_thread_mutex_t connections_mutex;
static Connection *connections = NULL;
static unsigned long conn_nextid = 0;

/* structure state (protected by connections_mutex) */
#define SLAP_C_UNINITIALIZED	0x00	/* MUST BE ZERO (0) */
#define SLAP_C_UNUSED			0x01
#define SLAP_C_USED				0x02

/* connection state (protected by c_mutex ) */
#define SLAP_C_INVALID			0x00	/* MUST BE ZERO (0) */
#define SLAP_C_INACTIVE			0x01	/* zero threads */
#define SLAP_C_ACTIVE			0x02	/* one or more threads */
#define SLAP_C_BINDING			0x03	/* binding */
#define SLAP_C_CLOSING			0x04	/* closing */

const char *
connection_state2str( int state )
{
	switch( state ) {
	case SLAP_C_INVALID:	return "!";		
	case SLAP_C_INACTIVE:	return "|";		
	case SLAP_C_ACTIVE:		return "";			
	case SLAP_C_BINDING:	return "B";
	case SLAP_C_CLOSING:	return "C";			
	}

	return "?";
}

static Connection* connection_get( ber_socket_t s );

static int connection_input( Connection *c );
static void connection_close( Connection *c );

static int connection_op_activate( Connection *conn, Operation *op );
static int connection_resched( Connection *conn );
static void connection_abandon( Connection *conn );
static void connection_destroy( Connection *c );

struct co_arg {
	Connection	*co_conn;
	Operation	*co_op;
};

/*
 * Initialize connection management infrastructure.
 */
int connections_init(void)
{
	assert( connections == NULL );

	if( connections != NULL) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "connections_init:  already initialized.\n" ));
#else
		Debug( LDAP_DEBUG_ANY, "connections_init: already initialized.\n",
			0, 0, 0 );
#endif
		return -1;
	}

	/* should check return of every call */
	ldap_pvt_thread_mutex_init( &connections_mutex );

	connections = (Connection *) calloc( dtblsize, sizeof(Connection) );

	if( connections == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "connections_init: allocation (%d * %ld) of connection array failed\n",
			   dtblsize, (long) sizeof(Connection) ));
#else
		Debug( LDAP_DEBUG_ANY,
			"connections_init: allocation (%d*%ld) of connection array failed\n",
			dtblsize, (long) sizeof(Connection), 0 );
#endif
		return -1;
	}

    assert( connections[0].c_struct_state == SLAP_C_UNINITIALIZED );
    assert( connections[dtblsize-1].c_struct_state == SLAP_C_UNINITIALIZED );

	/*
	 * per entry initialization of the Connection array initialization
	 * will be done by connection_init()
	 */ 

	return 0;
}

/*
 * Destroy connection management infrastructure.
 */
int connections_destroy(void)
{
	ber_socket_t i;

	/* should check return of every call */

	if( connections == NULL) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "connections_destroy: nothing to destroy.\n"));
#else
		Debug( LDAP_DEBUG_ANY, "connections_destroy: nothing to destroy.\n",
			0, 0, 0 );
#endif
		return -1;
	}

	for ( i = 0; i < dtblsize; i++ ) {
		if( connections[i].c_struct_state != SLAP_C_UNINITIALIZED ) {
			ber_sockbuf_free( connections[i].c_sb );
			ldap_pvt_thread_mutex_destroy( &connections[i].c_mutex );
			ldap_pvt_thread_mutex_destroy( &connections[i].c_write_mutex );
			ldap_pvt_thread_cond_destroy( &connections[i].c_write_cv );
		}
	}

	free( connections );
	connections = NULL;

	ldap_pvt_thread_mutex_destroy( &connections_mutex );
	return 0;
}

/*
 * shutdown all connections
 */
int connections_shutdown(void)
{
	ber_socket_t i;

	ldap_pvt_thread_mutex_lock( &connections_mutex );

	for ( i = 0; i < dtblsize; i++ ) {
		if( connections[i].c_struct_state != SLAP_C_USED ) {
			continue;
		}

		ldap_pvt_thread_mutex_lock( &connections[i].c_mutex );

		/* connections_mutex and c_mutex are locked */
		connection_closing( &connections[i] );
		connection_close( &connections[i] );

		ldap_pvt_thread_mutex_unlock( &connections[i].c_mutex );
	}

	ldap_pvt_thread_mutex_unlock( &connections_mutex );

	return 0;
}

/*
 * Timeout idle connections.
 */
int connections_timeout_idle(time_t now)
{
	int i = 0;
	int connindex;
	Connection* c;

	for( c = connection_first( &connindex );
		c != NULL;
		c = connection_next( c, &connindex ) )
	{
		if( difftime( c->c_activitytime+global_idletimeout, now) < 0 ) {
			/* close it */
			connection_closing( c );
			connection_close( c );
			i++;
		}
	}
	connection_done( c );

	return i;
}

static Connection* connection_get( ber_socket_t s )
{
	/* connections_mutex should be locked by caller */

	Connection *c;

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_ENTRY,
		   "connection_get: socket %ld\n", (long)s ));
#else
	Debug( LDAP_DEBUG_ARGS,
		"connection_get(%ld)\n",
		(long) s, 0, 0 );
#endif

	assert( connections != NULL );

	if(s == AC_SOCKET_INVALID) {
		return NULL;
	}

#ifndef HAVE_WINSOCK
	c = &connections[s];

	assert( c->c_struct_state != SLAP_C_UNINITIALIZED );

#else
	c = NULL;
	{
		ber_socket_t i, sd;

		for(i=0; i<dtblsize; i++) {
			ber_sockbuf_ctrl( connections[i].c_sb,
				LBER_SB_OPT_GET_FD, &sd );

			if( connections[i].c_struct_state == SLAP_C_UNINITIALIZED ) {
				assert( connections[i].c_conn_state == SLAP_C_INVALID );
				assert( connections[i].c_sb == 0 );
				break;
			}

			if( connections[i].c_struct_state == SLAP_C_UNUSED ) {
				assert( connections[i].c_conn_state == SLAP_C_INVALID );
				assert( sd == AC_SOCKET_INVALID );
				continue;
			}

			/* state can actually change from used -> unused by resched,
			 * so don't assert details here.
			 */

			if( sd == s ) {
				c = &connections[i];
				break;
			}
		}
	}
#endif

	if( c != NULL ) {
		ber_socket_t	sd;

		ldap_pvt_thread_mutex_lock( &c->c_mutex );

		ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_GET_FD, &sd );
		if( c->c_struct_state != SLAP_C_USED ) {
			/* connection must have been closed due to resched */

			assert( c->c_conn_state == SLAP_C_INVALID );
			assert( sd == AC_SOCKET_INVALID );

#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_ARGS,
				   "connection_get:  connection %d not used\n", s ));
#else
			Debug( LDAP_DEBUG_TRACE,
				"connection_get(%d): connection not used\n",
				s, 0, 0 );
#endif

			ldap_pvt_thread_mutex_unlock( &c->c_mutex );
			return NULL;
		}

#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_RESULTS,
			   "connection_get: get for %d got connid %ld\n",s, c->c_connid ));
#else
		Debug( LDAP_DEBUG_TRACE,
			"connection_get(%d): got connid=%ld\n",
			s, c->c_connid, 0 );
#endif

		c->c_n_get++;

		assert( c->c_struct_state == SLAP_C_USED );
		assert( c->c_conn_state != SLAP_C_INVALID );
		assert( sd != AC_SOCKET_INVALID );

#ifdef SLAPD_MONITOR
		c->c_activitytime = slap_get_time();
#else
		if( global_idletimeout > 0 ) {
			c->c_activitytime = slap_get_time();
		}
#endif
	}

	return c;
}

static void connection_return( Connection *c )
{
	ldap_pvt_thread_mutex_unlock( &c->c_mutex );
}

long connection_init(
	ber_socket_t s,
	const char* url,
	const char* dnsname,
	const char* peername,
	const char* sockname,
	int tls_udp_option,
	slap_ssf_t ssf,
	const char *authid )
{
	unsigned long id;
	Connection *c;

	assert( connections != NULL );

	assert( dnsname != NULL );
	assert( peername != NULL );
	assert( sockname != NULL );

#ifndef HAVE_TLS
	assert( tls_udp_option != 1 );
#endif

	if( s == AC_SOCKET_INVALID ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "connection_init: init of socket %ld invalid.\n", (long)s ));
#else
		Debug( LDAP_DEBUG_ANY,
		       "connection_init(%ld): invalid.\n",
		       (long) s, 0, 0 );
#endif
		return -1;
	}

	assert( s >= 0 );
#ifndef HAVE_WINSOCK
	assert( s < dtblsize );
#endif

	ldap_pvt_thread_mutex_lock( &connections_mutex );

#ifndef HAVE_WINSOCK
	c = &connections[s];

#else
	{
		ber_socket_t	i;

		c = NULL;

	for( i=0; i < dtblsize; i++) {
		ber_socket_t	sd;

	    if( connections[i].c_struct_state == SLAP_C_UNINITIALIZED ) {
		assert( connections[i].c_sb == 0 );
		c = &connections[i];
		break;
	    }

			sd = AC_SOCKET_INVALID;
			if (connections[i].c_sb != NULL)
			ber_sockbuf_ctrl( connections[i].c_sb, LBER_SB_OPT_GET_FD, &sd );
	    
	    if( connections[i].c_struct_state == SLAP_C_UNUSED ) {
		assert( sd == AC_SOCKET_INVALID );
		c = &connections[i];
		break;
	    }

	    assert( connections[i].c_struct_state == SLAP_C_USED );
	    assert( connections[i].c_conn_state != SLAP_C_INVALID );
	    assert( sd != AC_SOCKET_INVALID );
	}

	if( c == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "connection_init: skt %d	 connection table full (%d/%d)\n",
			   s, i, dtblsize ));
#else
		Debug( LDAP_DEBUG_ANY,
				"connection_init(%d): connection table full (%d/%d)\n",
				s, i, dtblsize);
#endif
	    ldap_pvt_thread_mutex_unlock( &connections_mutex );
	    return -1;
	}
    }
#endif

    assert( c != NULL );

	if( c->c_struct_state == SLAP_C_UNINITIALIZED ) {
		c->c_authmech.bv_val = NULL;
		c->c_authmech.bv_len = 0;
		c->c_dn.bv_val = NULL;
		c->c_dn.bv_len = 0;
		c->c_ndn.bv_val = NULL;
		c->c_ndn.bv_len = 0;
		c->c_cdn.bv_val = NULL;
		c->c_cdn.bv_len = 0;
		c->c_groups = NULL;

		c->c_listener_url.bv_val = NULL;
		c->c_listener_url.bv_len = 0;
		c->c_peer_domain.bv_val = NULL;
		c->c_peer_domain.bv_len = 0;
		c->c_peer_name.bv_val = NULL;
		c->c_peer_name.bv_len = 0;
		c->c_sock_name.bv_val = NULL;
		c->c_sock_name.bv_len = 0;

		LDAP_STAILQ_INIT(&c->c_ops);
		LDAP_STAILQ_INIT(&c->c_pending_ops);

		c->c_sasl_bind_mech.bv_val = NULL;
		c->c_sasl_bind_mech.bv_len = 0;
		c->c_sasl_context = NULL;
		c->c_sasl_extra = NULL;

		c->c_sb = ber_sockbuf_alloc( );

		{
			ber_len_t max = sockbuf_max_incoming;
			ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_SET_MAX_INCOMING, &max );
		}

		c->c_currentber = NULL;

		/* should check status of thread calls */
		ldap_pvt_thread_mutex_init( &c->c_mutex );
		ldap_pvt_thread_mutex_init( &c->c_write_mutex );
		ldap_pvt_thread_cond_init( &c->c_write_cv );

		c->c_struct_state = SLAP_C_UNUSED;
	}

    ldap_pvt_thread_mutex_lock( &c->c_mutex );

    assert( c->c_struct_state == SLAP_C_UNUSED );
    assert( c->c_authmech.bv_val == NULL );
    assert( c->c_dn.bv_val == NULL );
    assert( c->c_ndn.bv_val == NULL );
    assert( c->c_cdn.bv_val == NULL );
    assert( c->c_groups == NULL );
    assert( c->c_listener_url.bv_val == NULL );
    assert( c->c_peer_domain.bv_val == NULL );
    assert( c->c_peer_name.bv_val == NULL );
    assert( c->c_sock_name.bv_val == NULL );
    assert( LDAP_STAILQ_EMPTY(&c->c_ops) );
    assert( LDAP_STAILQ_EMPTY(&c->c_pending_ops) );
	assert( c->c_sasl_bind_mech.bv_val == NULL );
	assert( c->c_sasl_context == NULL );
	assert( c->c_sasl_extra == NULL );
	assert( c->c_currentber == NULL );

	ber_str2bv( url, 0, 1, &c->c_listener_url );
	ber_str2bv( dnsname, 0, 1, &c->c_peer_domain );
	ber_str2bv( peername, 0, 1, &c->c_peer_name );
	ber_str2bv( sockname, 0, 1, &c->c_sock_name );

    c->c_n_ops_received = 0;
    c->c_n_ops_executing = 0;
    c->c_n_ops_pending = 0;
    c->c_n_ops_completed = 0;

	c->c_n_get = 0;
	c->c_n_read = 0;
	c->c_n_write = 0;

	/* set to zero until bind, implies LDAP_VERSION3 */
	c->c_protocol = 0;

#ifdef SLAPD_MONITOR
	c->c_activitytime = c->c_starttime = slap_get_time();
#else
	if( global_idletimeout > 0 ) {
		c->c_activitytime = c->c_starttime = slap_get_time();
	}
#endif

#ifdef LDAP_CONNECTIONLESS
	c->c_is_udp = 0;
	if (tls_udp_option == 2)
	{
		c->c_is_udp = 1;
#ifdef LDAP_DEBUG
	ber_sockbuf_add_io( c->c_sb, &ber_sockbuf_io_debug,
		LBER_SBIOD_LEVEL_PROVIDER, (void*)"udp_" );
#endif
	ber_sockbuf_add_io( c->c_sb, &ber_sockbuf_io_udp,
		LBER_SBIOD_LEVEL_PROVIDER, (void *)&s );
	ber_sockbuf_add_io( c->c_sb, &ber_sockbuf_io_readahead,
		LBER_SBIOD_LEVEL_PROVIDER, NULL );
	} else
#endif
	{
#ifdef LDAP_DEBUG
	ber_sockbuf_add_io( c->c_sb, &ber_sockbuf_io_debug,
		LBER_SBIOD_LEVEL_PROVIDER, (void*)"tcp_" );
#endif
	ber_sockbuf_add_io( c->c_sb, &ber_sockbuf_io_tcp,
		LBER_SBIOD_LEVEL_PROVIDER, (void *)&s );
	}

#ifdef LDAP_DEBUG
	ber_sockbuf_add_io( c->c_sb, &ber_sockbuf_io_debug,
		INT_MAX, (void*)"ldap_" );
#endif

	if( ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_SET_NONBLOCK,
		c /* non-NULL */ ) < 0 )
	{
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "connection_init: conn %d  set nonblocking failed\n",
			   c->c_connid ));
#else
		Debug( LDAP_DEBUG_ANY,
			"connection_init(%d, %s): set nonblocking failed\n",
			s, c->c_peer_name, 0 );
#endif
	}

    id = c->c_connid = conn_nextid++;

    c->c_conn_state = SLAP_C_INACTIVE;
    c->c_struct_state = SLAP_C_USED;

	c->c_ssf = c->c_transport_ssf = ssf;
	c->c_tls_ssf = 0;

#ifdef HAVE_TLS
    if ( tls_udp_option == 1 ) {
	    c->c_is_tls = 1;
	    c->c_needs_tls_accept = 1;
    } else {
	    c->c_is_tls = 0;
	    c->c_needs_tls_accept = 0;
    }
#endif

	slap_sasl_open( c );
	slap_sasl_external( c, ssf, authid );

    ldap_pvt_thread_mutex_unlock( &c->c_mutex );
    ldap_pvt_thread_mutex_unlock( &connections_mutex );

    backend_connection_init(c);

    return id;
}

void connection2anonymous( Connection *c )
{
    assert( connections != NULL );
    assert( c != NULL );

	{
		ber_len_t max = sockbuf_max_incoming;
		ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_SET_MAX_INCOMING, &max );
	}

	if(c->c_authmech.bv_val != NULL ) {
		free(c->c_authmech.bv_val);
		c->c_authmech.bv_val = NULL;
	}
	c->c_authmech.bv_len = 0;

    if(c->c_dn.bv_val != NULL) {
	free(c->c_dn.bv_val);
	c->c_dn.bv_val = NULL;
    }
    c->c_dn.bv_len = 0;
    if(c->c_ndn.bv_val != NULL) {
	free(c->c_ndn.bv_val);
	c->c_ndn.bv_val = NULL;
    }
    c->c_ndn.bv_len = 0;

	if(c->c_cdn.bv_val != NULL) {
		free(c->c_cdn.bv_val);
		c->c_cdn.bv_val = NULL;
	}
	c->c_cdn.bv_len = 0;

	c->c_authc_backend = NULL;
	c->c_authz_backend = NULL;
    
    {
	GroupAssertion *g, *n;
	for (g = c->c_groups; g; g=n)
	{
	    n = g->ga_next;
	    free(g);
	}
	c->c_groups = NULL;
    }

}

static void
connection_destroy( Connection *c )
{
	/* note: connections_mutex should be locked by caller */
    ber_socket_t	sd;

    assert( connections != NULL );
    assert( c != NULL );
    assert( c->c_struct_state != SLAP_C_UNUSED );
    assert( c->c_conn_state != SLAP_C_INVALID );
    assert( LDAP_STAILQ_EMPTY(&c->c_ops) );

    backend_connection_destroy(c);

    c->c_protocol = 0;
    c->c_connid = -1;

    c->c_activitytime = c->c_starttime = 0;

	connection2anonymous( c );

	if(c->c_listener_url.bv_val != NULL) {
		free(c->c_listener_url.bv_val);
		c->c_listener_url.bv_val = NULL;
	}
	c->c_listener_url.bv_len = 0;

	if(c->c_peer_domain.bv_val != NULL) {
		free(c->c_peer_domain.bv_val);
		c->c_peer_domain.bv_val = NULL;
	}
	c->c_peer_domain.bv_len = 0;
	if(c->c_peer_name.bv_val != NULL) {
#ifdef LDAP_PF_lOCAL
		/*
		 * If peer was a domain socket, unlink. Mind you,
		 * they may be un-named. Should we leave this to
		 * the client?
		 */
		if (strncmp(c->c_peer_name.bv_val, "PATH=", 5) == 0) {
			char *path = c->c_peer_name.bv_val + 5;
			if (path != '\0') {
				(void)unlink(path);
			}
		}
#endif /* LDAP_PF_LOCAL */

		free(c->c_peer_name.bv_val);
		c->c_peer_name.bv_val = NULL;
	}
	c->c_peer_name.bv_len = 0;
	if(c->c_sock_name.bv_val != NULL) {
		free(c->c_sock_name.bv_val);
		c->c_sock_name.bv_val = NULL;
	}
	c->c_sock_name.bv_len = 0;

	c->c_sasl_bind_in_progress = 0;
	if(c->c_sasl_bind_mech.bv_val != NULL) {
		free(c->c_sasl_bind_mech.bv_val);
		c->c_sasl_bind_mech.bv_val = NULL;
	}
	c->c_sasl_bind_mech.bv_len = 0;

	slap_sasl_close( c );

	if ( c->c_currentber != NULL ) {
		ber_free( c->c_currentber, 1 );
		c->c_currentber = NULL;
	}

	ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_GET_FD, &sd );
	if ( sd != AC_SOCKET_INVALID ) {
		slapd_remove( sd, 0 );

		Statslog( LDAP_DEBUG_STATS,
		    "conn=%ld fd=%d closed\n",
			c->c_connid, sd, 0, 0, 0 );
	}

	ber_sockbuf_free( c->c_sb );

	c->c_sb = ber_sockbuf_alloc( );

	{
		ber_len_t max = sockbuf_max_incoming;
		ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_SET_MAX_INCOMING, &max );
	}

    c->c_conn_state = SLAP_C_INVALID;
    c->c_struct_state = SLAP_C_UNUSED;
}

int connection_state_closing( Connection *c )
{
	/* c_mutex must be locked by caller */

	int state;
	assert( c != NULL );
	assert( c->c_struct_state == SLAP_C_USED );

	state = c->c_conn_state;

	assert( state != SLAP_C_INVALID );

	return state == SLAP_C_CLOSING;
}

static void connection_abandon( Connection *c )
{
	/* c_mutex must be locked by caller */

	Operation *o;

	LDAP_STAILQ_FOREACH(o, &c->c_ops, o_next) {
		ldap_pvt_thread_mutex_lock( &o->o_abandonmutex );
		o->o_abandon = 1;
		ldap_pvt_thread_mutex_unlock( &o->o_abandonmutex );
	}

	/* remove pending operations */
	while ( (o = LDAP_STAILQ_FIRST( &c->c_pending_ops )) != NULL) {
		LDAP_STAILQ_REMOVE_HEAD( &c->c_pending_ops, o_next );
		LDAP_STAILQ_NEXT(o, o_next) = NULL;
		slap_op_free( o );
	}
}

void connection_closing( Connection *c )
{
	assert( connections != NULL );
	assert( c != NULL );
	assert( c->c_struct_state == SLAP_C_USED );
	assert( c->c_conn_state != SLAP_C_INVALID );

	/* c_mutex must be locked by caller */

	if( c->c_conn_state != SLAP_C_CLOSING ) {
		ber_socket_t	sd;

		ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_GET_FD, &sd );
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
			   "connection_closing: conn %d readying socket %d for close.\n",
			   c->c_connid, sd ));
#else
		Debug( LDAP_DEBUG_TRACE,
			"connection_closing: readying conn=%ld sd=%d for close\n",
			c->c_connid, sd, 0 );
#endif
		/* update state to closing */
		c->c_conn_state = SLAP_C_CLOSING;

		/* don't listen on this port anymore */
		slapd_clr_read( sd, 1 );

		/* abandon active operations */
		connection_abandon( c );

		/* wake write blocked operations */
		slapd_clr_write( sd, 1 );
		ldap_pvt_thread_cond_signal( &c->c_write_cv );
	}
}

static void connection_close( Connection *c )
{
	ber_socket_t	sd;

	assert( connections != NULL );
	assert( c != NULL );
	assert( c->c_struct_state == SLAP_C_USED );
	assert( c->c_conn_state == SLAP_C_CLOSING );

	/* note: connections_mutex and c_mutex should be locked by caller */

	ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_GET_FD, &sd );
	if( !LDAP_STAILQ_EMPTY(&c->c_ops) ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
			   "connection_close: conn %d  deferring sd %d\n",
			   c->c_connid, sd ));
#else
		Debug( LDAP_DEBUG_TRACE,
			"connection_close: deferring conn=%ld sd=%d\n",
			c->c_connid, sd, 0 );
#endif
		return;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_RESULTS,
		   "connection_close: conn %d  sd %d\n",
		   c->c_connid, sd ));
#else
	Debug( LDAP_DEBUG_TRACE, "connection_close: conn=%ld sd=%d\n",
		c->c_connid, sd, 0 );
#endif
	connection_destroy( c );
}

unsigned long connections_nextid(void)
{
	unsigned long id;
	assert( connections != NULL );

	ldap_pvt_thread_mutex_lock( &connections_mutex );

	id = conn_nextid;

	ldap_pvt_thread_mutex_unlock( &connections_mutex );

	return id;
}

Connection* connection_first( ber_socket_t *index )
{
	assert( connections != NULL );
	assert( index != NULL );

	ldap_pvt_thread_mutex_lock( &connections_mutex );

	*index = 0;

	return connection_next(NULL, index);
}

Connection* connection_next( Connection *c, ber_socket_t *index )
{
	assert( connections != NULL );
	assert( index != NULL );
	assert( *index <= dtblsize );

	if( c != NULL ) {
		ldap_pvt_thread_mutex_unlock( &c->c_mutex );
	}

	c = NULL;

	for(; *index < dtblsize; (*index)++) {
		if( connections[*index].c_struct_state == SLAP_C_UNINITIALIZED ) {
			assert( connections[*index].c_conn_state == SLAP_C_INVALID );
#ifndef HAVE_WINSOCK
			continue;
#else
			break;
#endif
		}

		if( connections[*index].c_struct_state == SLAP_C_USED ) {
			assert( connections[*index].c_conn_state != SLAP_C_INVALID );
			c = &connections[(*index)++];
			break;
		}

		assert( connections[*index].c_struct_state == SLAP_C_UNUSED );
		assert( connections[*index].c_conn_state == SLAP_C_INVALID );
	}

	if( c != NULL ) {
		ldap_pvt_thread_mutex_lock( &c->c_mutex );
	}

	return c;
}

void connection_done( Connection *c )
{
	assert( connections != NULL );

	if( c != NULL ) {
		ldap_pvt_thread_mutex_unlock( &c->c_mutex );
	}

	ldap_pvt_thread_mutex_unlock( &connections_mutex );
}

/*
 * connection_activity - handle the request operation op on connection
 * conn.  This routine figures out what kind of operation it is and
 * calls the appropriate stub to handle it.
 */

static void *
connection_operation( void *arg_v )
{
	int rc;
	struct co_arg	*arg = arg_v;
	ber_tag_t tag = arg->co_op->o_tag;
	Connection *conn = arg->co_conn;

	ldap_pvt_thread_mutex_lock( &num_ops_mutex );
	num_ops_initiated++;
	ldap_pvt_thread_mutex_unlock( &num_ops_mutex );

	if( conn->c_sasl_bind_in_progress && tag != LDAP_REQ_BIND ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "connection_operation: conn %d  SASL bind in progress (tag=%ld).\n",
			   conn->c_connid, (long)tag ));
#else
		Debug( LDAP_DEBUG_ANY, "connection_operation: "
			"error: SASL bind in progress (tag=%ld).\n",
			(long) tag, 0, 0 );
#endif
		send_ldap_result( conn, arg->co_op,
			rc = LDAP_OPERATIONS_ERROR,
			NULL, "SASL bind in progress", NULL, NULL );
		goto operations_error;
	}

	switch ( tag ) {
	case LDAP_REQ_BIND:
		rc = do_bind( conn, arg->co_op );
		break;

	case LDAP_REQ_UNBIND:
		rc = do_unbind( conn, arg->co_op );
		break;

	case LDAP_REQ_ADD:
		rc = do_add( conn, arg->co_op );
		break;

	case LDAP_REQ_DELETE:
		rc = do_delete( conn, arg->co_op );
		break;

	case LDAP_REQ_MODRDN:
		rc = do_modrdn( conn, arg->co_op );
		break;

	case LDAP_REQ_MODIFY:
		rc = do_modify( conn, arg->co_op );
		break;

	case LDAP_REQ_COMPARE:
		rc = do_compare( conn, arg->co_op );
		break;

	case LDAP_REQ_SEARCH:
		rc = do_search( conn, arg->co_op );
		break;

	case LDAP_REQ_ABANDON:
		rc = do_abandon( conn, arg->co_op );
		break;

	case LDAP_REQ_EXTENDED:
		rc = do_extended( conn, arg->co_op );
		break;

	default:
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "connection_operation: conn %d  unknown LDAP request 0x%lx\n",
			   conn->c_connid, tag ));
#else
		Debug( LDAP_DEBUG_ANY, "unknown LDAP request 0x%lx\n",
		    tag, 0, 0 );
#endif
		arg->co_op->o_tag = LBER_ERROR;
		send_ldap_disconnect( conn, arg->co_op,
			LDAP_PROTOCOL_ERROR, "unknown LDAP request" );
		rc = -1;
		break;
	}

	if( rc == SLAPD_DISCONNECT ) tag = LBER_ERROR;

operations_error:
	ldap_pvt_thread_mutex_lock( &num_ops_mutex );
	num_ops_completed++;
	ldap_pvt_thread_mutex_unlock( &num_ops_mutex );

	ldap_pvt_thread_mutex_lock( &conn->c_mutex );

	conn->c_n_ops_executing--;
	conn->c_n_ops_completed++;

	LDAP_STAILQ_REMOVE( &conn->c_ops, arg->co_op, slap_op, o_next);
	LDAP_STAILQ_NEXT(arg->co_op, o_next) = NULL;
	slap_op_free( arg->co_op );
	arg->co_op = NULL;
	arg->co_conn = NULL;
	free( (char *) arg );
	arg = NULL;

	switch( tag ) {
	case LBER_ERROR:
	case LDAP_REQ_UNBIND:
		/* c_mutex is locked */
		connection_closing( conn );
		break;

	case LDAP_REQ_BIND:
		conn->c_sasl_bind_in_progress =
			rc == LDAP_SASL_BIND_IN_PROGRESS ? 1 : 0;

		if( conn->c_conn_state == SLAP_C_BINDING) {
			conn->c_conn_state = SLAP_C_ACTIVE;
		}
	}

	connection_resched( conn );

	ldap_pvt_thread_mutex_unlock( &conn->c_mutex );

	return NULL;
}

int connection_read(ber_socket_t s)
{
	int rc = 0;
	Connection *c;
	assert( connections != NULL );

	ldap_pvt_thread_mutex_lock( &connections_mutex );

	/* get (locked) connection */
	c = connection_get( s );

	if( c == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "connection_read: sock %ld no connection\n",
			   (long)s ));
#else
		Debug( LDAP_DEBUG_ANY,
			"connection_read(%ld): no connection!\n",
			(long) s, 0, 0 );
#endif
		slapd_remove(s, 0);

		ldap_pvt_thread_mutex_unlock( &connections_mutex );
		return -1;
	}

	c->c_n_read++;

	if( c->c_conn_state == SLAP_C_CLOSING ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "connection_read: conn %d connection closing, ignoring input\n",
			   c->c_connid));
#else
		Debug( LDAP_DEBUG_TRACE,
			"connection_read(%d): closing, ignoring input for id=%ld\n",
			s, c->c_connid, 0 );
#endif
		connection_return( c );
		ldap_pvt_thread_mutex_unlock( &connections_mutex );
		return 0;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
		   "connection_read: conn %d  checking for input.\n", c->c_connid ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"connection_read(%d): checking for input on id=%ld\n",
		s, c->c_connid, 0 );
#endif

#ifdef HAVE_TLS
	if ( c->c_is_tls && c->c_needs_tls_accept ) {
		rc = ldap_pvt_tls_accept( c->c_sb, NULL );
		if ( rc < 0 ) {
#if 0 /* required by next #if 0 */
			struct timeval tv;
			fd_set rfd;
#endif

#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
				   "connection_read: conn %d  TLS accept error, error %d\n",
				   c->c_connid, rc ));
#else
			Debug( LDAP_DEBUG_TRACE,
				"connection_read(%d): TLS accept error "
				"error=%d id=%ld, closing\n",
				s, rc, c->c_connid );
#endif
			c->c_needs_tls_accept = 0;
			/* connections_mutex and c_mutex are locked */
			connection_closing( c );

#if 0
			/* Drain input before close, to allow SSL error codes
			 * to propagate to client. */
			FD_ZERO(&rfd);
			FD_SET(s, &rfd);
			for (rc=1; rc>0;)
			{
			    tv.tv_sec = 1;
			    tv.tv_usec = 0;
			    rc = select(s+1, &rfd, NULL, NULL, &tv);
			    if (rc == 1)
				ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_DRAIN,
				    NULL);
			}
#endif
			connection_close( c );

		} else if ( rc == 0 ) {
			void *ssl;
			char *authid;

			c->c_needs_tls_accept = 0;

			/* we need to let SASL know */
			ssl = (void *)ldap_pvt_tls_sb_ctx( c->c_sb );

			c->c_tls_ssf = (slap_ssf_t) ldap_pvt_tls_get_strength( ssl );
			if( c->c_tls_ssf > c->c_ssf ) {
				c->c_ssf = c->c_tls_ssf;
			}

			authid = (char *)ldap_pvt_tls_get_peer( ssl );
			slap_sasl_external( c, c->c_tls_ssf, authid );
		}
		connection_return( c );
		ldap_pvt_thread_mutex_unlock( &connections_mutex );
		return 0;
	}
#endif

#ifdef HAVE_CYRUS_SASL
	if ( c->c_sasl_layers ) {
		c->c_sasl_layers = 0;

		rc = ldap_pvt_sasl_install( c->c_sb,  c->c_sasl_context );

		if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
				   "connection_read: conn %d SASL install error %d, closing\n",
				   c->c_connid, rc ));
#else
			Debug( LDAP_DEBUG_TRACE,
				"connection_read(%d): SASL install error "
				"error=%d id=%ld, closing\n",
				s, rc, c->c_connid );
#endif
			/* connections_mutex and c_mutex are locked */
			connection_closing( c );
			connection_close( c );
			connection_return( c );
			ldap_pvt_thread_mutex_unlock( &connections_mutex );
			return 0;
		}
	}
#endif

#define CONNECTION_INPUT_LOOP 1

#ifdef DATA_READY_LOOP
	while( !rc && ber_sockbuf_ctrl( c->c_sb, LBER_SB_DATA_READY, NULL ) )
#elif CONNECTION_INPUT_LOOP
	while(!rc)
#endif
	{
		/* How do we do this without getting into a busy loop ? */
		rc = connection_input( c );
	}

	if( rc < 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "connection_read: conn %d  input error %d, closing.\n",
			   c->c_connid, rc ));
#else
		Debug( LDAP_DEBUG_TRACE,
			"connection_read(%d): input error=%d id=%ld, closing.\n",
			s, rc, c->c_connid );
#endif
		/* connections_mutex and c_mutex are locked */
		connection_closing( c );
		connection_close( c );
		connection_return( c );
		ldap_pvt_thread_mutex_unlock( &connections_mutex );
		return 0;
	}

	if ( ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_NEEDS_READ, NULL ) ) {
		slapd_set_read( s, 1 );
	}

	if ( ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_NEEDS_WRITE, NULL ) ) {
		slapd_set_write( s, 1 );
	}

	connection_return( c );
	ldap_pvt_thread_mutex_unlock( &connections_mutex );
	return 0;
}

static int
connection_input(
    Connection *conn
)
{
	Operation *op;
	ber_tag_t	tag;
	ber_len_t	len;
	ber_int_t	msgid;
	BerElement	*ber;
#ifdef LDAP_CONNECTIONLESS
	Sockaddr	peeraddr;
	char 		*cdn = NULL;
#endif

	if ( conn->c_currentber == NULL && (conn->c_currentber = ber_alloc())
	    == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "connection_input: conn %d  ber_alloc failed.\n", conn->c_connid ));
#else
		Debug( LDAP_DEBUG_ANY, "ber_alloc failed\n", 0, 0, 0 );
#endif
		return -1;
	}

	errno = 0;

#ifdef LDAP_CONNECTIONLESS
	if (conn->c_is_udp)
	{
		char	peername[sizeof("IP=255.255.255.255:65336")];
		len = ber_int_sb_read(conn->c_sb, &peeraddr,
			sizeof(struct sockaddr));
		if (len != sizeof(struct sockaddr))
			return 1;
		sprintf( peername, "IP=%s:%d",
			inet_ntoa( peeraddr.sa_in_addr.sin_addr ),
			(unsigned) ntohs( peeraddr.sa_in_addr.sin_port ) );
		Statslog( LDAP_DEBUG_STATS,
			    "conn=%ld UDP request from %s (%s) accepted.\n",
			    conn->c_connid, peername,
			    conn->c_sock_name, 0, 0 );
	}
#endif
	tag = ber_get_next( conn->c_sb, &len, conn->c_currentber );
	if ( tag != LDAP_TAG_MESSAGE ) {
		int err = errno;
		ber_socket_t	sd;

		ber_sockbuf_ctrl( conn->c_sb, LBER_SB_OPT_GET_FD, &sd );

#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "connection_input: conn %d  ber_get_next failed, errno %d (%s).\n",
			   conn->c_connid, err, sock_errstr(err) ));
#else
		Debug( LDAP_DEBUG_TRACE,
			"ber_get_next on fd %d failed errno=%d (%s)\n",
			sd, err, sock_errstr(err) );
#endif
		if ( err != EWOULDBLOCK && err != EAGAIN ) {
			/* log, close and send error */
			ber_free( conn->c_currentber, 1 );
			conn->c_currentber = NULL;

			return -2;
		}
		return 1;
	}

	ber = conn->c_currentber;
	conn->c_currentber = NULL;

	if ( (tag = ber_get_int( ber, &msgid )) != LDAP_TAG_MSGID ) {
		/* log, close and send error */
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "connection_input: conn %d  ber_get_int returns 0x%lx.\n",
			   conn->c_connid, tag ));
#else
		Debug( LDAP_DEBUG_ANY, "ber_get_int returns 0x%lx\n", tag, 0,
		    0 );
#endif
		ber_free( ber, 1 );
		return -1;
	}

	if ( (tag = ber_peek_tag( ber, &len )) == LBER_ERROR ) {
		/* log, close and send error */
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "connection_input: conn %d  ber_peek_tag returns 0x%lx.\n",
			   conn->c_connid, tag ));
#else
		Debug( LDAP_DEBUG_ANY, "ber_peek_tag returns 0x%lx\n", tag, 0,
		    0 );
#endif
		ber_free( ber, 1 );

		return -1;
	}

#ifdef LDAP_CONNECTIONLESS
	if (conn->c_is_udp) {
		if (tag == LBER_OCTETSTRING) {
			ber_get_stringa( ber, &cdn );
			tag = ber_peek_tag(ber, &len);
		}
		if (tag != LDAP_REQ_ABANDON && tag != LDAP_REQ_SEARCH) {
#ifdef NEW_LOGGING
		    LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			       "connection_input: conn %d  invalid req for UDP 0x%lx.\n",
			       conn->c_connid, tag ));
#else
		    Debug( LDAP_DEBUG_ANY, "invalid req for UDP 0x%lx\n", tag, 0,
			0 );
#endif
		    ber_free( ber, 1 );
		    return 0;
		}
	}
#endif
	if(tag == LDAP_REQ_BIND) {
		/* immediately abandon all exiting operations upon BIND */
		connection_abandon( conn );
	}

	op = slap_op_alloc( ber, msgid, tag, conn->c_n_ops_received++ );

#ifdef LDAP_CONNECTIONLESS
	op->o_peeraddr = peeraddr;
	if (cdn) {
	    op->o_dn = cdn;
	    op->o_protocol = LDAP_VERSION2;
	}
#endif
	if ( conn->c_conn_state == SLAP_C_BINDING
		|| conn->c_conn_state == SLAP_C_CLOSING )
	{
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "connection_input: conn %d  deferring operation\n",
			   conn->c_connid ));
#else
		Debug( LDAP_DEBUG_ANY, "deferring operation\n", 0, 0, 0 );
#endif
		conn->c_n_ops_pending++;
		LDAP_STAILQ_INSERT_TAIL( &conn->c_pending_ops, op, o_next );

	} else {
		conn->c_n_ops_executing++;
		connection_op_activate( conn, op );
	}

#ifdef NO_THREADS
	if ( conn->c_struct_state != SLAP_C_USED ) {
		/* connection must have got closed underneath us */
		return 1;
	}
#endif
	assert( conn->c_struct_state == SLAP_C_USED );

	return 0;
}

static int
connection_resched( Connection *conn )
{
	Operation *op;

	if( conn->c_conn_state == SLAP_C_CLOSING ) {
		int rc;
		ber_socket_t	sd;
		ber_sockbuf_ctrl( conn->c_sb, LBER_SB_OPT_GET_FD, &sd );

		/* us trylock to avoid possible deadlock */
		rc = ldap_pvt_thread_mutex_trylock( &connections_mutex );

		if( rc ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
				   "connection_resched: conn %d  reaquiring locks.\n",
				   conn->c_connid ));
#else
			Debug( LDAP_DEBUG_TRACE,
				"connection_resched: reaquiring locks conn=%ld sd=%d\n",
				conn->c_connid, sd, 0 );
#endif
			/*
			 * reaquire locks in the right order...
			 * this may allow another thread to close this connection,
			 * so recheck state below.
			 */
			ldap_pvt_thread_mutex_unlock( &conn->c_mutex );
			ldap_pvt_thread_mutex_lock( &connections_mutex );
			ldap_pvt_thread_mutex_lock( &conn->c_mutex );
		}

		if( conn->c_conn_state != SLAP_C_CLOSING ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
				   "connection_resched: conn %d  closed by other thread.\n",
				   conn->c_connid ));
#else
			Debug( LDAP_DEBUG_TRACE, "connection_resched: "
				"closed by other thread conn=%ld sd=%d\n",
				conn->c_connid, sd, 0 );
#endif
		} else {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
				   "connection_resched: conn %d  attempting closing.\n",
				   conn->c_connid ));
#else
			Debug( LDAP_DEBUG_TRACE, "connection_resched: "
				"attempting closing conn=%ld sd=%d\n",
				conn->c_connid, sd, 0 );
#endif
			connection_close( conn );
		}

		ldap_pvt_thread_mutex_unlock( &connections_mutex );
		return 0;
	}

	if( conn->c_conn_state != SLAP_C_ACTIVE ) {
		/* other states need different handling */
		return 0;
	}

	while ((op = LDAP_STAILQ_FIRST( &conn->c_pending_ops )) != NULL) {
		LDAP_STAILQ_REMOVE_HEAD( &conn->c_pending_ops, o_next );
		LDAP_STAILQ_NEXT(op, o_next) = NULL;
		/* pending operations should not be marked for abandonment */
		assert(!op->o_abandon);

		conn->c_n_ops_pending--;
		conn->c_n_ops_executing++;

		connection_op_activate( conn, op );

		if ( conn->c_conn_state == SLAP_C_BINDING ) {
			break;
		}
	}
	return 0;
}

static int connection_op_activate( Connection *conn, Operation *op )
{
	struct co_arg *arg;
	int status;
	ber_tag_t tag = op->o_tag;

	if(tag == LDAP_REQ_BIND) {
		conn->c_conn_state = SLAP_C_BINDING;
	}

	arg = (struct co_arg *) ch_malloc( sizeof(struct co_arg) );
	arg->co_conn = conn;
	arg->co_op = op;

	if (!arg->co_op->o_dn.bv_len) {
	    arg->co_op->o_authz = conn->c_authz;
	    arg->co_op->o_dn.bv_val = ch_strdup( conn->c_dn.bv_val ?
	    	conn->c_dn.bv_val : "" );
	    arg->co_op->o_ndn.bv_val = ch_strdup( conn->c_ndn.bv_val ?
	    	conn->c_ndn.bv_val : "" );
	}
	arg->co_op->o_authtype = conn->c_authtype;
	ber_dupbv( &arg->co_op->o_authmech, &conn->c_authmech );
	
	if (!arg->co_op->o_protocol) {
	    arg->co_op->o_protocol = conn->c_protocol
		? conn->c_protocol : LDAP_VERSION3;
	}
	arg->co_op->o_connid = conn->c_connid;

	LDAP_STAILQ_INSERT_TAIL( &conn->c_ops, arg->co_op, o_next );

	status = ldap_pvt_thread_pool_submit( &connection_pool,
		connection_operation, (void *) arg );

	if ( status != 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "connection_op_activate: conn %d	 thread pool submit failed.\n",
			   conn->c_connid ));
#else
		Debug( LDAP_DEBUG_ANY,
		"ldap_pvt_thread_pool_submit failed (%d)\n", status, 0, 0 );
#endif
		/* should move op to pending list */
	}

	return status;
}

int connection_write(ber_socket_t s)
{
	Connection *c;
	assert( connections != NULL );

	ldap_pvt_thread_mutex_lock( &connections_mutex );

	c = connection_get( s );

	slapd_clr_write( s, 0);

	if( c == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "connection_write: sock %ld  no connection!\n",(long)s));
#else
		Debug( LDAP_DEBUG_ANY,
			"connection_write(%ld): no connection!\n",
			(long) s, 0, 0 );
#endif
		slapd_remove(s, 0);
		ldap_pvt_thread_mutex_unlock( &connections_mutex );
		return -1;
	}

	c->c_n_write++;

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
		   "connection_write conn %d  waking output.\n",
		   c->c_connid ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"connection_write(%d): waking output for id=%ld\n",
		s, c->c_connid, 0 );
#endif
	ldap_pvt_thread_cond_signal( &c->c_write_cv );

	if ( ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_NEEDS_READ, NULL ) )
		slapd_set_read( s, 1 );
	if ( ber_sockbuf_ctrl( c->c_sb, LBER_SB_OPT_NEEDS_WRITE, NULL ) )
		slapd_set_write( s, 1 );
	connection_return( c );
	ldap_pvt_thread_mutex_unlock( &connections_mutex );
	return 0;
}

