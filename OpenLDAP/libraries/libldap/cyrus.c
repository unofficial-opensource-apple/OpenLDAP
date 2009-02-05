/* $OpenLDAP: pkg/ldap/libraries/libldap/cyrus.c,v 1.45 2002/02/11 17:18:34 kurt Exp $ */
/*
 * Copyright 1999-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>
#include <ac/stdlib.h>
#include <ac/string.h>
#include <ac/time.h>
#include <ac/errno.h>
#include <ac/ctype.h>

#include "ldap-int.h"

#ifdef HAVE_CYRUS_SASL

#ifdef __APPLE__
#include <pthread.h>
static pthread_mutex_t _sasl_lock_data_ = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _sasl_lock_main_ = PTHREAD_MUTEX_INITIALIZER;
static int _sasl_lock_count_ = 0;
static unsigned int _sasl_thread_ = (unsigned int)-1;
#endif

#ifdef LDAP_R_COMPILE
ldap_pvt_thread_mutex_t ldap_int_sasl_mutex;
#endif

#ifdef HAVE_SASL_SASL_H
#include <sasl/sasl.h>
#else
#include <sasl.h>
#endif

#if SASL_VERSION_MAJOR >= 2
#define SASL_CONST const
#else
#define SASL_CONST
#endif

#ifdef __APPLE__
void _lock_sasl(void)
{
	unsigned int t;

	t = (unsigned int)pthread_self();

	pthread_mutex_lock(&_sasl_lock_data_);
	if ((_sasl_lock_count_ > 0) && (_sasl_thread_ == t))
	{
		/* Recursive lock: just increment the locked counter */
		_sasl_lock_count_++;
		pthread_mutex_unlock(&_sasl_lock_data_);
		return;
	}
	pthread_mutex_unlock(&_sasl_lock_data_);

	pthread_mutex_lock(&_sasl_lock_main_);

	pthread_mutex_lock(&_sasl_lock_data_);
	_sasl_lock_count_ = 1;
	_sasl_thread_ = t;
	pthread_mutex_unlock(&_sasl_lock_data_);
}

void _unlock_sasl(void)
{
	int unlock_me;

	unlock_me = 0;

	pthread_mutex_lock(&_sasl_lock_data_);
	if (_sasl_lock_count_ > 0) _sasl_lock_count_--;
	if (_sasl_lock_count_ == 0)
	{
		_sasl_thread_ = (unsigned int)-1;
		unlock_me = 1;
	}
	pthread_mutex_unlock(&_sasl_lock_data_);

	if (unlock_me == 1) pthread_mutex_unlock(&_sasl_lock_main_);
}
#endif

/*
* Various Cyrus SASL related stuff.
*/

int ldap_int_sasl_init( void )
{
	static int sasl_initialized = 0;

	static sasl_callback_t client_callbacks[] = {
#ifdef SASL_CB_GETREALM
		{ SASL_CB_GETREALM, NULL, NULL },
#endif
		{ SASL_CB_USER, NULL, NULL },
		{ SASL_CB_AUTHNAME, NULL, NULL },
		{ SASL_CB_PASS, NULL, NULL },
		{ SASL_CB_ECHOPROMPT, NULL, NULL },
		{ SASL_CB_NOECHOPROMPT, NULL, NULL },
		{ SASL_CB_LIST_END, NULL, NULL }
	};

#ifdef __APPLE__
	_lock_sasl();
#endif

	if ( sasl_initialized ) {
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return 0;
	}

#ifndef CSRIMALLOC
	sasl_set_alloc(
		ber_memalloc,
		ber_memcalloc,
		ber_memrealloc,
		ber_memfree );
#endif /* CSRIMALLOC */

#ifdef LDAP_R_COMPILE
	sasl_set_mutex(
		ldap_pvt_sasl_mutex_new,
		ldap_pvt_sasl_mutex_lock,
		ldap_pvt_sasl_mutex_unlock,    
		ldap_pvt_sasl_mutex_dispose );    

	ldap_pvt_thread_mutex_init( &ldap_int_sasl_mutex );
#endif

	if ( sasl_client_init( client_callbacks ) == SASL_OK ) {
		sasl_initialized = 1;
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return 0;
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return -1;
}

/*
 * SASL encryption support for LBER Sockbufs
 */

struct sb_sasl_data {
	sasl_conn_t		*sasl_context;
	Sockbuf_Buf		sec_buf_in;
	Sockbuf_Buf		buf_in;
	Sockbuf_Buf		buf_out;
};

static int
sb_sasl_setup( Sockbuf_IO_Desc *sbiod, void *arg )
{
	struct sb_sasl_data	*p;

#ifdef __APPLE__
	_lock_sasl();
#endif

	assert( sbiod != NULL );

	p = LBER_MALLOC( sizeof( *p ) );
	if ( p == NULL )
	{
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return -1;
	}

	p->sasl_context = (sasl_conn_t *)arg;
	ber_pvt_sb_buf_init( &p->sec_buf_in );
	ber_pvt_sb_buf_init( &p->buf_in );
	ber_pvt_sb_buf_init( &p->buf_out );
	if ( ber_pvt_sb_grow_buffer( &p->sec_buf_in, SASL_MIN_BUFF_SIZE ) < 0 ) {
		errno = ENOMEM;
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return -1;
	}

	sbiod->sbiod_pvt = p;

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return 0;
}

static int
sb_sasl_remove( Sockbuf_IO_Desc *sbiod )
{
	struct sb_sasl_data	*p;

#ifdef __APPLE__
	_lock_sasl();
#endif

	assert( sbiod != NULL );
	
	p = (struct sb_sasl_data *)sbiod->sbiod_pvt;
#if SASL_VERSION_MAJOR >= 2
	/*
	 * SASLv2 encode/decode buffers are managed by
	 * libsasl2. Ensure they are not freed by liblber.
	 */
	p->buf_in.buf_base = NULL;
	p->buf_out.buf_base = NULL;
#endif
	ber_pvt_sb_buf_destroy( &p->sec_buf_in );
	ber_pvt_sb_buf_destroy( &p->buf_in );
	ber_pvt_sb_buf_destroy( &p->buf_out );
	LBER_FREE( p );
	sbiod->sbiod_pvt = NULL;

#ifdef __APPLE__
	_unlock_sasl();
#endif

	return 0;
}

static ber_len_t
sb_sasl_pkt_length( const unsigned char *buf, int debuglevel )
{
	ber_len_t		size;

#ifdef __APPLE__
	_lock_sasl();
#endif

	assert( buf != NULL );

	size = buf[0] << 24
		| buf[1] << 16
		| buf[2] << 8
		| buf[3];
   
	/* we really should check against actual buffer size set
	 * in the secopts.
	 */
	if ( size > SASL_MAX_BUFF_SIZE ) {
		/* somebody is trying to mess me up. */
		ber_log_printf( LDAP_DEBUG_ANY, debuglevel,
			"sb_sasl_pkt_length: received illegal packet length "
			"of %lu bytes\n", (unsigned long)size );      
		size = 16; /* this should lead to an error. */
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif

	return size + 4; /* include the size !!! */
}

/* Drop a processed packet from the input buffer */
static void
sb_sasl_drop_packet ( Sockbuf_Buf *sec_buf_in, int debuglevel )
{
	ber_slen_t			len;

#ifdef __APPLE__
	_lock_sasl();
#endif

	len = sec_buf_in->buf_ptr - sec_buf_in->buf_end;
	if ( len > 0 )
		AC_MEMCPY( sec_buf_in->buf_base, sec_buf_in->buf_base +
			sec_buf_in->buf_end, len );
   
	if ( len >= 4 ) {
		sec_buf_in->buf_end = sb_sasl_pkt_length( sec_buf_in->buf_base,
			debuglevel);
	}
	else {
		sec_buf_in->buf_end = 0;
	}
	sec_buf_in->buf_ptr = len;

#ifdef __APPLE__
	_unlock_sasl();
#endif
}

static ber_slen_t
sb_sasl_read( Sockbuf_IO_Desc *sbiod, void *buf, ber_len_t len)
{
	struct sb_sasl_data	*p;
	ber_slen_t		ret, bufptr;
   
#ifdef __APPLE__
	_lock_sasl();
#endif

	assert( sbiod != NULL );
	assert( SOCKBUF_VALID( sbiod->sbiod_sb ) );

	p = (struct sb_sasl_data *)sbiod->sbiod_pvt;

	/* Are there anything left in the buffer? */
	ret = ber_pvt_sb_copy_out( &p->buf_in, buf, len );
	bufptr = ret;
	len -= ret;

	if ( len == 0 )
	{
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return bufptr;
	}

#if SASL_VERSION_MAJOR >= 2
	ber_pvt_sb_buf_init( &p->buf_in );
#else
	ber_pvt_sb_buf_destroy( &p->buf_in );
#endif

	/* Read the length of the packet */
	while ( p->sec_buf_in.buf_ptr < 4 ) {
		ret = LBER_SBIOD_READ_NEXT( sbiod, p->sec_buf_in.buf_base,
			4 - p->sec_buf_in.buf_ptr );
#ifdef EINTR
		if ( ( ret < 0 ) && ( errno == EINTR ) )
			continue;
#endif
		if ( ret <= 0 )
		{
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return ret;
		}

		p->sec_buf_in.buf_ptr += ret;
	}

	/* The new packet always starts at p->sec_buf_in.buf_base */
	ret = sb_sasl_pkt_length( p->sec_buf_in.buf_base,
		sbiod->sbiod_sb->sb_debug );

	/* Grow the packet buffer if neccessary */
	if ( ( p->sec_buf_in.buf_size < (ber_len_t) ret ) && 
		ber_pvt_sb_grow_buffer( &p->sec_buf_in, ret ) < 0 )
	{
		errno = ENOMEM;
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return -1;
	}
	p->sec_buf_in.buf_end = ret;

	/* Did we read the whole encrypted packet? */
	while ( p->sec_buf_in.buf_ptr < p->sec_buf_in.buf_end ) {
		/* No, we have got only a part of it */
		ret = p->sec_buf_in.buf_end - p->sec_buf_in.buf_ptr;

		ret = LBER_SBIOD_READ_NEXT( sbiod, p->sec_buf_in.buf_base +
			p->sec_buf_in.buf_ptr, ret );
#ifdef EINTR
		if ( ( ret < 0 ) && ( errno == EINTR ) )
			continue;
#endif
		if ( ret <= 0 )
		{
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return ret;
		}
		p->sec_buf_in.buf_ptr += ret;
   	}

	/* Decode the packet */
	ret = sasl_decode( p->sasl_context, p->sec_buf_in.buf_base,
		p->sec_buf_in.buf_end,
		(SASL_CONST char **)&p->buf_in.buf_base,
		(unsigned *)&p->buf_in.buf_end );
	if ( ret != SASL_OK ) {
		ber_log_printf( LDAP_DEBUG_ANY, sbiod->sbiod_sb->sb_debug,
			"sb_sasl_read: failed to decode packet: %s\n",
			sasl_errstring( ret, NULL, NULL ) );
		sb_sasl_drop_packet( &p->sec_buf_in,
			sbiod->sbiod_sb->sb_debug );
		errno = EIO;
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return -1;
	}
	
	/* Drop the packet from the input buffer */
	sb_sasl_drop_packet( &p->sec_buf_in, sbiod->sbiod_sb->sb_debug );

	p->buf_in.buf_size = p->buf_in.buf_end;

	bufptr += ber_pvt_sb_copy_out( &p->buf_in, (char*) buf + bufptr, len );

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return bufptr;
}

static ber_slen_t
sb_sasl_write( Sockbuf_IO_Desc *sbiod, void *buf, ber_len_t len)
{
	struct sb_sasl_data	*p;
	int			ret;

#ifdef __APPLE__
	_lock_sasl();
#endif

	assert( sbiod != NULL );
	assert( SOCKBUF_VALID( sbiod->sbiod_sb ) );

	p = (struct sb_sasl_data *)sbiod->sbiod_pvt;

	/* Are there anything left in the buffer? */
	if ( p->buf_out.buf_ptr != p->buf_out.buf_end ) {
		ret = ber_pvt_sb_do_write( sbiod, &p->buf_out );
		if ( ret <= 0 )
		{
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return ret;
		}
	}

	/* now encode the next packet. */
#if SASL_VERSION_MAJOR >= 2
	ber_pvt_sb_buf_init( &p->buf_out );
#else
	ber_pvt_sb_buf_destroy( &p->buf_out );
#endif
	ret = sasl_encode( p->sasl_context, buf, len,
		(SASL_CONST char **)&p->buf_out.buf_base,
		(unsigned *)&p->buf_out.buf_size );
	if ( ret != SASL_OK ) {
		ber_log_printf( LDAP_DEBUG_ANY, sbiod->sbiod_sb->sb_debug,
			"sb_sasl_write: failed to encode packet: %s\n",
			sasl_errstring( ret, NULL, NULL ) );
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return -1;
	}
	p->buf_out.buf_end = p->buf_out.buf_size;

	ret = ber_pvt_sb_do_write( sbiod, &p->buf_out );
	if ( ret <= 0 )
	{
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return ret;
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return len;
}

static int
sb_sasl_ctrl( Sockbuf_IO_Desc *sbiod, int opt, void *arg )
{
	struct sb_sasl_data	*p;
	int rc;

#ifdef __APPLE__
	_lock_sasl();
#endif
	p = (struct sb_sasl_data *)sbiod->sbiod_pvt;

	if ( opt == LBER_SB_OPT_DATA_READY ) {
		if ( p->buf_in.buf_ptr != p->buf_in.buf_end )
		{
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return 1;
		}
	}

	rc = LBER_SBIOD_CTRL_NEXT( sbiod, opt, arg );

#ifdef __APPLE__
	_unlock_sasl();
#endif

	return rc;
}

Sockbuf_IO ldap_pvt_sockbuf_io_sasl = {
	sb_sasl_setup,		/* sbi_setup */
	sb_sasl_remove,		/* sbi_remove */
	sb_sasl_ctrl,		/* sbi_ctrl */
	sb_sasl_read,		/* sbi_read */
	sb_sasl_write,		/* sbi_write */
	NULL			/* sbi_close */
};

int ldap_pvt_sasl_install( Sockbuf *sb, void *ctx_arg )
{
#ifdef __APPLE__
	_lock_sasl();
#endif

	Debug( LDAP_DEBUG_TRACE, "ldap_pvt_sasl_install\n",
		0, 0, 0 );

	/* don't install the stuff unless security has been negotiated */

	if ( !ber_sockbuf_ctrl( sb, LBER_SB_OPT_HAS_IO,
			&ldap_pvt_sockbuf_io_sasl ) )
	{
#ifdef LDAP_DEBUG
		ber_sockbuf_add_io( sb, &ber_sockbuf_io_debug,
			LBER_SBIOD_LEVEL_APPLICATION, (void *)"sasl_" );
#endif
		ber_sockbuf_add_io( sb, &ldap_pvt_sockbuf_io_sasl,
			LBER_SBIOD_LEVEL_APPLICATION, ctx_arg );
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif

	return LDAP_SUCCESS;
}

static int
sasl_err2ldap( int saslerr )
{
	int rc;

#ifdef __APPLE__
	_lock_sasl();
#endif

	switch (saslerr) {
		case SASL_CONTINUE:
			rc = LDAP_MORE_RESULTS_TO_RETURN;
			break;
		case SASL_INTERACT:
			rc = LDAP_LOCAL_ERROR;
			break;
		case SASL_OK:
			rc = LDAP_SUCCESS;
			break;
		case SASL_FAIL:
			rc = LDAP_LOCAL_ERROR;
			break;
		case SASL_NOMEM:
			rc = LDAP_NO_MEMORY;
			break;
		case SASL_NOMECH:
			rc = LDAP_AUTH_UNKNOWN;
			break;
		case SASL_BADAUTH:
			rc = LDAP_AUTH_UNKNOWN;
			break;
		case SASL_NOAUTHZ:
			rc = LDAP_PARAM_ERROR;
			break;
		case SASL_TOOWEAK:
		case SASL_ENCRYPT:
			rc = LDAP_AUTH_UNKNOWN;
			break;
		default:
			rc = LDAP_LOCAL_ERROR;
			break;
	}

	assert( rc == LDAP_SUCCESS || LDAP_API_ERROR( rc ) );

#ifdef __APPLE__
	_unlock_sasl();
#endif

	return rc;
}

int
ldap_int_sasl_open(
	LDAP *ld, 
	LDAPConn *lc,
	const char * host,
	ber_len_t ssf )
{
	int rc;
	sasl_conn_t *ctx;
	sasl_callback_t *session_callbacks;

#ifdef __APPLE__
	_lock_sasl();
#endif

	session_callbacks = LDAP_CALLOC( 2, sizeof( sasl_callback_t ) );

	if( session_callbacks == NULL ) 
	{
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return LDAP_NO_MEMORY;
	}

	session_callbacks[0].id = SASL_CB_USER;
	session_callbacks[0].proc = NULL;
	session_callbacks[0].context = ld;

	session_callbacks[1].id = SASL_CB_LIST_END;
	session_callbacks[1].proc = NULL;
	session_callbacks[1].context = NULL;

	assert( lc->lconn_sasl_ctx == NULL );

	if ( host == NULL ) {
		ld->ld_errno = LDAP_LOCAL_ERROR;
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return ld->ld_errno;
	}

#if SASL_VERSION_MAJOR >= 2
	rc = sasl_client_new( "ldap", host, NULL, NULL,
		session_callbacks, 0, &ctx );
#else
	rc = sasl_client_new( "ldap", host, session_callbacks,
		SASL_SECURITY_LAYER, &ctx );
#endif
	LDAP_FREE( session_callbacks );

	if ( rc != SASL_OK ) {
		ld->ld_errno = sasl_err2ldap( rc );
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return ld->ld_errno;
	}

	Debug( LDAP_DEBUG_TRACE, "ldap_int_sasl_open: host=%s\n",
		host, 0, 0 );

	lc->lconn_sasl_ctx = ctx;

	if( ssf ) {
#if SASL_VERSION_MAJOR >= 2
		(void) sasl_setprop( ctx, SASL_SSF_EXTERNAL,
			(void *) &ssf );
#else
		sasl_external_properties_t extprops;
		memset(&extprops, 0L, sizeof(extprops));
		extprops.ssf = ssf;

		(void) sasl_setprop( ctx, SASL_SSF_EXTERNAL,
			(void *) &extprops );
#endif
		Debug( LDAP_DEBUG_TRACE, "ldap_int_sasl_open: ssf=%ld\n",
			(long) ssf, 0, 0 );
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return LDAP_SUCCESS;
}

int ldap_int_sasl_close( LDAP *ld, LDAPConn *lc )
{
	sasl_conn_t *ctx = lc->lconn_sasl_ctx;

#ifdef __APPLE__
	_lock_sasl();
#endif

	if( ctx != NULL ) {
		sasl_dispose( &ctx );
		lc->lconn_sasl_ctx = NULL;
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return LDAP_SUCCESS;
}

int
ldap_int_sasl_bind(
	LDAP			*ld,
	const char		*dn,
	const char		*mechs,
	LDAPControl		**sctrls,
	LDAPControl		**cctrls,
	unsigned		flags,
	LDAP_SASL_INTERACT_PROC *interact,
	void * defaults )
{
	char *data;
	const char *mech = NULL;
	const char *pmech = NULL;
	int			saslrc, rc;
	sasl_ssf_t		*ssf = NULL;
	sasl_conn_t	*ctx;
	sasl_interact_t *prompts = NULL;
	unsigned credlen;
	struct berval ccred;
	ber_socket_t		sd;

#ifdef __APPLE__
	_lock_sasl();
#endif

	Debug( LDAP_DEBUG_TRACE, "ldap_int_sasl_bind: %s\n",
		mechs ? mechs : "<null>", 0, 0 );

	/* do a quick !LDAPv3 check... ldap_sasl_bind will do the rest. */
	if (ld->ld_version < LDAP_VERSION3) {
		ld->ld_errno = LDAP_NOT_SUPPORTED;
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return ld->ld_errno;
	}

	ber_sockbuf_ctrl( ld->ld_sb, LBER_SB_OPT_GET_FD, &sd );

	if ( sd == AC_SOCKET_INVALID ) {
 		/* not connected yet */
 		int rc;

		rc = ldap_open_defconn( ld );
		if( rc < 0 )
		{
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return ld->ld_errno;
		}

		ber_sockbuf_ctrl( ld->ld_sb, LBER_SB_OPT_GET_FD, &sd );

		if( sd == AC_SOCKET_INVALID ) {
			ld->ld_errno = LDAP_LOCAL_ERROR;
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return ld->ld_errno;
		}
	}   

	ctx = ld->ld_defconn->lconn_sasl_ctx;

	if( ctx == NULL ) {
		ld->ld_errno = LDAP_LOCAL_ERROR;
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return ld->ld_errno;
	}

	/* (re)set security properties */
	sasl_setprop( ctx, SASL_SEC_PROPS,
		&ld->ld_options.ldo_sasl_secprops );

	ccred.bv_val = NULL;
	ccred.bv_len = 0;

	do {
		saslrc = sasl_client_start( ctx,
			mechs,
#if SASL_VERSION_MAJOR < 2
			NULL,
#endif
			&prompts,
			(SASL_CONST char **)&ccred.bv_val,
			&credlen,
			&mech );

		if( pmech == NULL && mech != NULL ) {
			pmech = mech;

			if( flags != LDAP_SASL_QUIET ) {
				fprintf(stderr,
					"SASL/%s authentication started\n",
					pmech );
			}
		}

#if SASL_VERSION_MAJOR >= 2
		/* XXX the application should free interact results. */
		if ( prompts != NULL && prompts->result != NULL ) {
			LDAP_FREE( (void *)prompts->result );
			prompts->result = NULL;
		}
#endif

		if( saslrc == SASL_INTERACT ) {
			int res;
			if( !interact ) break;
			res = (interact)( ld, flags, defaults, prompts );
			if( res != LDAP_SUCCESS ) {
				break;
			}
		}
	} while ( saslrc == SASL_INTERACT );

	ccred.bv_len = credlen;

	if ( (saslrc != SASL_OK) && (saslrc != SASL_CONTINUE) ) {
		ld->ld_errno = sasl_err2ldap( saslrc );
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return ld->ld_errno;
	}

	do {
		struct berval *scred;
		unsigned credlen;

		scred = NULL;

		rc = ldap_sasl_bind_s( ld, dn, mech, &ccred, sctrls, cctrls, &scred );

		if ( ccred.bv_val != NULL ) {
#if SASL_VERSION_MAJOR < 2
			LDAP_FREE( ccred.bv_val );
#endif
			ccred.bv_val = NULL;
		}

		if ( rc != LDAP_SUCCESS && rc != LDAP_SASL_BIND_IN_PROGRESS ) {
			if( scred && scred->bv_len ) {
				/* and server provided us with data? */
				Debug( LDAP_DEBUG_TRACE,
					"ldap_int_sasl_bind: rc=%d sasl=%d len=%ld\n",
					rc, saslrc, scred->bv_len );
				ber_bvfree( scred );
			}
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return ld->ld_errno;
		}

		if( rc == LDAP_SUCCESS && saslrc == SASL_OK ) {
			/* we're done, no need to step */
			if( scred && scred->bv_len ) {
				/* but server provided us with data! */
				Debug( LDAP_DEBUG_TRACE,
					"ldap_int_sasl_bind: rc=%d sasl=%d len=%ld\n",
					rc, saslrc, scred->bv_len );
				ber_bvfree( scred );
				ld->ld_errno = LDAP_LOCAL_ERROR;
#ifdef __APPLE__
				_unlock_sasl();
#endif
				return ld->ld_errno;
			}
			break;
		}

		do {
			saslrc = sasl_client_step( ctx,
				(scred == NULL) ? NULL : scred->bv_val,
				(scred == NULL) ? 0 : scred->bv_len,
				&prompts,
				(SASL_CONST char **)&ccred.bv_val,
				&credlen );

			Debug( LDAP_DEBUG_TRACE, "sasl_client_step: %d\n",
				saslrc, 0, 0 );

#if SASL_VERSION_MAJOR >= 2
			/* XXX the application should free interact results. */
			if ( prompts != NULL && prompts->result != NULL ) {
				LDAP_FREE( (void *)prompts->result );
				prompts->result = NULL;
			}
#endif

			if( saslrc == SASL_INTERACT ) {
				int res;
				if( !interact ) break;
				res = (interact)( ld, flags, defaults, prompts );
				if( res != LDAP_SUCCESS ) {
					break;
				}
			}
		} while ( saslrc == SASL_INTERACT );

		ccred.bv_len = credlen;
		ber_bvfree( scred );

		if ( (saslrc != SASL_OK) && (saslrc != SASL_CONTINUE) ) {
			ld->ld_errno = sasl_err2ldap( saslrc );
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return ld->ld_errno;
		}
	} while ( rc == LDAP_SASL_BIND_IN_PROGRESS );

	if ( rc != LDAP_SUCCESS ) {
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return rc;
	}

	if ( saslrc != SASL_OK ) {
		ld->ld_errno = sasl_err2ldap( saslrc );
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return ld->ld_errno;
	}

	if( flags != LDAP_SASL_QUIET ) {
		saslrc = sasl_getprop( ctx, SASL_USERNAME, (SASL_CONST void **) &data );
		if( saslrc == SASL_OK && data && *data ) {
			fprintf( stderr, "SASL username: %s\n", data );
		}

#if SASL_VERSION_MAJOR >= 2
		saslrc = sasl_getprop( ctx, SASL_DEFUSERREALM, (SASL_CONST void **) &data );
#else
		saslrc = sasl_getprop( ctx, SASL_REALM, (SASL_CONST void **) &data );
#endif
		if( saslrc == SASL_OK && data && *data ) {
			fprintf( stderr, "SASL realm: %s\n", data );
		}
	}

	saslrc = sasl_getprop( ctx, SASL_SSF, (SASL_CONST void **) &ssf );
	if( saslrc == SASL_OK ) {
		if( flags != LDAP_SASL_QUIET ) {
			fprintf( stderr, "SASL SSF: %lu\n",
				(unsigned long) *ssf );
		}

		if( ssf && *ssf ) {
			if( flags != LDAP_SASL_QUIET ) {
				fprintf( stderr, "SASL installing layers\n" );
			}
			ldap_pvt_sasl_install( ld->ld_conns->lconn_sb, ctx );
		}
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return rc;
}

int
ldap_int_sasl_external(
	LDAP *ld,
	LDAPConn *conn,
	const char * authid,
	ber_len_t ssf )
{
	int sc;
	sasl_conn_t *ctx;
#if SASL_VERSION_MAJOR < 2
	sasl_external_properties_t extprops;
#endif

#ifdef __APPLE__
	_lock_sasl();
#endif

	ctx = conn->lconn_sasl_ctx;

	if ( ctx == NULL ) {
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return LDAP_LOCAL_ERROR;
	}
   
#if SASL_VERSION_MAJOR >= 2
	sc = sasl_setprop( ctx, SASL_SSF_EXTERNAL, &ssf );
#else
	memset( &extprops, '\0', sizeof(extprops) );
	extprops.ssf = ssf;
	extprops.auth_id = (char *) authid;

	sc = sasl_setprop( ctx, SASL_SSF_EXTERNAL,
		(void *) &extprops );
#endif

	if ( sc != SASL_OK ) {
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return LDAP_LOCAL_ERROR;
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return LDAP_SUCCESS;
}


int ldap_pvt_sasl_secprops(
	const char *in,
	sasl_security_properties_t *secprops )
{
	int i;
	char **props = ldap_str2charray( in, "," );
	unsigned sflags = 0;
	int got_sflags = 0;
	sasl_ssf_t max_ssf = 0;
	int got_max_ssf = 0;
	sasl_ssf_t min_ssf = 0;
	int got_min_ssf = 0;
	unsigned maxbufsize = 0;
	int got_maxbufsize = 0;

#ifdef __APPLE__
	_lock_sasl();
#endif

	if( props == NULL || secprops == NULL ) {
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return LDAP_PARAM_ERROR;
	}

	for( i=0; props[i]; i++ ) {
		if( !strcasecmp(props[i], "none") ) {
			got_sflags++;

		} else if( !strcasecmp(props[i], "noplain") ) {
			got_sflags++;
			sflags |= SASL_SEC_NOPLAINTEXT;

		} else if( !strcasecmp(props[i], "noactive") ) {
			got_sflags++;
			sflags |= SASL_SEC_NOACTIVE;

		} else if( !strcasecmp(props[i], "nodict") ) {
			got_sflags++;
			sflags |= SASL_SEC_NODICTIONARY;

		} else if( !strcasecmp(props[i], "forwardsec") ) {
			got_sflags++;
			sflags |= SASL_SEC_FORWARD_SECRECY;

		} else if( !strcasecmp(props[i], "noanonymous")) {
			got_sflags++;
			sflags |= SASL_SEC_NOANONYMOUS;

		} else if( !strcasecmp(props[i], "passcred") ) {
			got_sflags++;
			sflags |= SASL_SEC_PASS_CREDENTIALS;

		} else if( !strncasecmp(props[i],
			"minssf=", sizeof("minssf")) )
		{
			if( isdigit( props[i][sizeof("minssf")] ) ) {
				got_min_ssf++;
				min_ssf = atoi( &props[i][sizeof("minssf")] );
			} else {
#ifdef __APPLE__
				_unlock_sasl();
#endif
				return LDAP_NOT_SUPPORTED;
			}

		} else if( !strncasecmp(props[i],
			"maxssf=", sizeof("maxssf")) )
		{
			if( isdigit( props[i][sizeof("maxssf")] ) ) {
				got_max_ssf++;
				max_ssf = atoi( &props[i][sizeof("maxssf")] );
			} else {
#ifdef __APPLE__
				_unlock_sasl();
#endif
				return LDAP_NOT_SUPPORTED;
			}

		} else if( !strncasecmp(props[i],
			"maxbufsize=", sizeof("maxbufsize")) )
		{
			if( isdigit( props[i][sizeof("maxbufsize")] ) ) {
				got_maxbufsize++;
				maxbufsize = atoi( &props[i][sizeof("maxbufsize")] );
			} else {
#ifdef __APPLE__
				_unlock_sasl();
#endif
				return LDAP_NOT_SUPPORTED;
			}

			if( maxbufsize && (( maxbufsize < SASL_MIN_BUFF_SIZE )
				|| (maxbufsize > SASL_MAX_BUFF_SIZE )))
			{
				/* bad maxbufsize */
#ifdef __APPLE__
				_unlock_sasl();
#endif
				return LDAP_PARAM_ERROR;
			}

		} else {
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return LDAP_NOT_SUPPORTED;
		}
	}

	if(got_sflags) {
		secprops->security_flags = sflags;
	}
	if(got_min_ssf) {
		secprops->min_ssf = min_ssf;
	}
	if(got_max_ssf) {
		secprops->max_ssf = max_ssf;
	}
	if(got_maxbufsize) {
		secprops->maxbufsize = maxbufsize;
	}

	ldap_charray_free( props );
#ifdef __APPLE__
	_unlock_sasl();
#endif
	return LDAP_SUCCESS;
}

int
ldap_int_sasl_config( struct ldapoptions *lo, int option, const char *arg )
{
	int rc;

#ifdef __APPLE__
	_lock_sasl();
#endif

	switch( option ) {
	case LDAP_OPT_X_SASL_SECPROPS:
		rc = ldap_pvt_sasl_secprops( arg, &lo->ldo_sasl_secprops );
		if( rc == LDAP_SUCCESS )
		{
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return 0;
		}
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return -1;
}

int
ldap_int_sasl_get_option( LDAP *ld, int option, void *arg )
{
#ifdef __APPLE__
	_lock_sasl();
#endif

	if ( ld == NULL )
	{
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return -1;
	}

	switch ( option ) {
		case LDAP_OPT_X_SASL_MECH: {
			*(char **)arg = ld->ld_options.ldo_def_sasl_mech
				? LDAP_STRDUP( ld->ld_options.ldo_def_sasl_mech ) : NULL;
		} break;
		case LDAP_OPT_X_SASL_REALM: {
			*(char **)arg = ld->ld_options.ldo_def_sasl_realm
				? LDAP_STRDUP( ld->ld_options.ldo_def_sasl_realm ) : NULL;
		} break;
		case LDAP_OPT_X_SASL_AUTHCID: {
			*(char **)arg = ld->ld_options.ldo_def_sasl_authcid
				? LDAP_STRDUP( ld->ld_options.ldo_def_sasl_authcid ) : NULL;
		} break;
		case LDAP_OPT_X_SASL_AUTHZID: {
			*(char **)arg = ld->ld_options.ldo_def_sasl_authzid
				? LDAP_STRDUP( ld->ld_options.ldo_def_sasl_authzid ) : NULL;
		} break;

		case LDAP_OPT_X_SASL_SSF: {
			int sc;
			sasl_ssf_t	*ssf;
			sasl_conn_t *ctx;

			if( ld->ld_defconn == NULL ) {
#ifdef __APPLE__
				_unlock_sasl();
#endif
				return -1;
			}

			ctx = ld->ld_defconn->lconn_sasl_ctx;

			if ( ctx == NULL ) {
#ifdef __APPLE__
				_unlock_sasl();
#endif
				return -1;
			}

			sc = sasl_getprop( ctx, SASL_SSF,
				(SASL_CONST void **) &ssf );

			if ( sc != SASL_OK ) {
#ifdef __APPLE__
				_unlock_sasl();
#endif
				return -1;
			}

			*(ber_len_t *)arg = *ssf;
		} break;

		case LDAP_OPT_X_SASL_SSF_EXTERNAL:
			/* this option is write only */
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return -1;

		case LDAP_OPT_X_SASL_SSF_MIN:
			*(ber_len_t *)arg = ld->ld_options.ldo_sasl_secprops.min_ssf;
			break;
		case LDAP_OPT_X_SASL_SSF_MAX:
			*(ber_len_t *)arg = ld->ld_options.ldo_sasl_secprops.max_ssf;
			break;
		case LDAP_OPT_X_SASL_MAXBUFSIZE:
			*(ber_len_t *)arg = ld->ld_options.ldo_sasl_secprops.maxbufsize;
			break;

		case LDAP_OPT_X_SASL_SECPROPS:
			/* this option is write only */
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return -1;

		default:
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return -1;
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return 0;
}

int
ldap_int_sasl_set_option( LDAP *ld, int option, void *arg )
{
#ifdef __APPLE__
	_lock_sasl();
#endif

	if ( ld == NULL )
	{
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return -1;
	}

	switch ( option ) {
	case LDAP_OPT_X_SASL_SSF:
		/* This option is read-only */
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return -1;

	case LDAP_OPT_X_SASL_SSF_EXTERNAL: {
		int sc;
#if SASL_VERSION_MAJOR < 2
		sasl_external_properties_t extprops;
#endif
		sasl_conn_t *ctx;

		if( ld->ld_defconn == NULL ) {
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return -1;
		}

		ctx = ld->ld_defconn->lconn_sasl_ctx;

		if ( ctx == NULL ) {
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return -1;
		}

#if SASL_VERSION_MAJOR >= 2
		sc = sasl_setprop( ctx, SASL_SSF_EXTERNAL, arg);
#else
		memset(&extprops, 0L, sizeof(extprops));

		extprops.ssf = * (ber_len_t *) arg;

		sc = sasl_setprop( ctx, SASL_SSF_EXTERNAL,
			(void *) &extprops );
#endif

		if ( sc != SASL_OK ) {
#ifdef __APPLE__
			_unlock_sasl();
#endif
			return -1;
		}
		} break;

	case LDAP_OPT_X_SASL_SSF_MIN:
		ld->ld_options.ldo_sasl_secprops.min_ssf = *(ber_len_t *)arg;
		break;
	case LDAP_OPT_X_SASL_SSF_MAX:
		ld->ld_options.ldo_sasl_secprops.max_ssf = *(ber_len_t *)arg;
		break;
	case LDAP_OPT_X_SASL_MAXBUFSIZE:
		ld->ld_options.ldo_sasl_secprops.maxbufsize = *(ber_len_t *)arg;
		break;

	case LDAP_OPT_X_SASL_SECPROPS: {
		int sc;
		sc = ldap_pvt_sasl_secprops( (char *) arg,
			&ld->ld_options.ldo_sasl_secprops );

#ifdef __APPLE__
		_unlock_sasl();
#endif
		return sc == LDAP_SUCCESS ? 0 : -1;
		}

	default:
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return -1;
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return 0;
}

#ifdef LDAP_R_COMPILE
void *ldap_pvt_sasl_mutex_new(void)
{
	ldap_pvt_thread_mutex_t *mutex;

#ifdef __APPLE__
	_lock_sasl();
#endif

	mutex = (ldap_pvt_thread_mutex_t *) LDAP_MALLOC(
		sizeof(ldap_pvt_thread_mutex_t) );

	if ( ldap_pvt_thread_mutex_init( mutex ) == 0 ) {
#ifdef __APPLE__
		_unlock_sasl();
#endif
		return mutex;
	}

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return NULL;
}

int ldap_pvt_sasl_mutex_lock(void *mutex)
{
	int rc;

#ifdef __APPLE__
	_lock_sasl();
#endif

	rc = ldap_pvt_thread_mutex_lock( (ldap_pvt_thread_mutex_t *)mutex )
		? SASL_FAIL : SASL_OK;

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return rc;
}

int ldap_pvt_sasl_mutex_unlock(void *mutex)
{
	int rc;

#ifdef __APPLE__
	_lock_sasl();
#endif

	rc = ldap_pvt_thread_mutex_unlock( (ldap_pvt_thread_mutex_t *)mutex )
		? SASL_FAIL : SASL_OK;

#ifdef __APPLE__
	_unlock_sasl();
#endif
	return rc;
}

void ldap_pvt_sasl_mutex_dispose(void *mutex)
{
#ifdef __APPLE__
	_lock_sasl();
#endif

#ifdef NOTDEF
	(void) ldap_pvt_thread_mutex_destroy( (ldap_pvt_thread_mutex_t *)mutex );
	LDAP_FREE( mutex );
#endif

#ifdef __APPLE__
	_unlock_sasl();
#endif
}
#endif

#else
int ldap_int_sasl_init( void )
{ return LDAP_SUCCESS; }

int ldap_int_sasl_close( LDAP *ld, LDAPConn *lc )
{ return LDAP_SUCCESS; }

int
ldap_int_sasl_bind(
	LDAP			*ld,
	const char		*dn,
	const char		*mechs,
	LDAPControl		**sctrls,
	LDAPControl		**cctrls,
	unsigned		flags,
	LDAP_SASL_INTERACT_PROC *interact,
	void * defaults )
{ return LDAP_NOT_SUPPORTED; }

int
ldap_int_sasl_external(
	LDAP *ld,
	LDAPConn *conn,
	const char * authid,
	ber_len_t ssf )
{ return LDAP_SUCCESS; }

#endif /* HAVE_CYRUS_SASL */
