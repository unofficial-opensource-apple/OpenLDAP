/* $OpenLDAP: pkg/ldap/servers/slapd/daemon.c,v 1.211 2002/01/28 19:26:55 ando Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/ctype.h>
#include <ac/errno.h>
#include <ac/signal.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>
#include <ac/unistd.h>

#include "ldap_pvt.h"
#include "ldap_pvt_thread.h"
#include "lutil.h"
#include "slap.h"

#ifdef HAVE_TCPD
#include <tcpd.h>

int allow_severity = LOG_INFO;
int deny_severity = LOG_NOTICE;
#endif /* TCP Wrappers */

#ifdef LDAP_PF_LOCAL
#include <sys/stat.h>
/* this should go in <ldap.h> as soon as it is accepted */
#define LDAPI_MOD_URLEXT		"x-mod"
#endif /* LDAP_PF_LOCAL */

/* globals */
time_t starttime;
ber_socket_t dtblsize;

Listener **slap_listeners = NULL;

#define SLAPD_LISTEN 10

static ber_socket_t wake_sds[2];

#ifdef NO_THREADS
static int waking;
#define WAKE_LISTENER(w) \
((w && !waking) ? tcp_write( wake_sds[1], "0", 1 ), waking=1 : 0)
#else
#define WAKE_LISTENER(w) \
do { if (w) tcp_write( wake_sds[1], "0", 1 ); } while(0)
#endif

#ifndef HAVE_WINSOCK
static
#endif
volatile sig_atomic_t slapd_shutdown = 0;

static struct slap_daemon {
	ldap_pvt_thread_mutex_t	sd_mutex;

	int sd_nactives;

#ifndef HAVE_WINSOCK
	/* In winsock, accept() returns values higher than dtblsize
		so don't bother with this optimization */
	int sd_nfds;
#endif

	fd_set sd_actives;
	fd_set sd_readers;
	fd_set sd_writers;
} slap_daemon;



#ifdef HAVE_SLP
/*
 * SLP related functions
 */
#include <slp.h>

#define LDAP_SRVTYPE_PREFIX "service:ldap://"
#define LDAPS_SRVTYPE_PREFIX "service:ldaps://"
static char** slapd_srvurls = NULL;
static SLPHandle slapd_hslp = 0;

void slapd_slp_init( const char* urls ) {
	int i;

	slapd_srvurls = str2charray( urls, " " );

	if( slapd_srvurls == NULL ) return;

	/* find and expand INADDR_ANY URLs */
	for( i=0; slapd_srvurls[i] != NULL; i++ ) {
		if( strcmp( slapd_srvurls[i], "ldap:///" ) == 0) {
			char *host = ldap_pvt_get_fqdn( NULL );
			if ( host != NULL ) {
				slapd_srvurls[i] = (char *) realloc( slapd_srvurls[i],
					strlen( host ) +
					sizeof( LDAP_SRVTYPE_PREFIX ) );
				strcpy( slap_strcopy(slapd_srvurls[i],
					LDAP_SRVTYPE_PREFIX ), host );

				ch_free( host );
			}

		} else if ( strcmp( slapd_srvurls[i], "ldaps:///" ) == 0) {
			char *host = ldap_pvt_get_fqdn( NULL );
			if ( host != NULL ) {
				slapd_srvurls[i] = (char *) realloc( slapd_srvurls[i],
					strlen( host ) +
					sizeof( LDAPS_SRVTYPE_PREFIX ) );
				strcpy( slap_strcopy(slapd_srvurls[i],
					LDAPS_SRVTYPE_PREFIX ), host );

				ch_free( host );
			}
		}
	}

	/* open the SLP handle */
	SLPOpen( "en", 0, &slapd_hslp );
}

void slapd_slp_deinit() {
	if( slapd_srvurls == NULL ) return;

	charray_free( slapd_srvurls );
	slapd_srvurls = NULL;

	/* close the SLP handle */
	SLPClose( slapd_hslp );
}

void slapd_slp_regreport(
	SLPHandle hslp,
	SLPError errcode,
	void* cookie )
{
	/* empty report */
}

void slapd_slp_reg() {
	int i;

	for( i=0; slapd_srvurls[i] != NULL; i++ ) {
		if( strncmp( slapd_srvurls[i], LDAP_SRVTYPE_PREFIX,
				sizeof( LDAP_SRVTYPE_PREFIX ) - 1 ) == 0 ||
		    strncmp( slapd_srvurls[i], LDAPS_SRVTYPE_PREFIX,
				sizeof( LDAPS_SRVTYPE_PREFIX ) - 1 ) == 0 )
		{
			SLPReg( slapd_hslp,
				slapd_srvurls[i],
				SLP_LIFETIME_MAXIMUM,
				"ldap",
				"",
				1,
				slapd_slp_regreport,
				NULL );
		}
	}
}

void slapd_slp_dereg() {
	int i;

	for( i=0; slapd_srvurls[i] != NULL; i++ ) {
		SLPDereg( slapd_hslp,
			slapd_srvurls[i],
			slapd_slp_regreport,
			NULL );
	}
}
#endif /* HAVE_SLP */

/*
 * Add a descriptor to daemon control
 */
static void slapd_add(ber_socket_t s) {
	ldap_pvt_thread_mutex_lock( &slap_daemon.sd_mutex );

	assert( !FD_ISSET( s, &slap_daemon.sd_actives ));
	assert( !FD_ISSET( s, &slap_daemon.sd_readers ));
	assert( !FD_ISSET( s, &slap_daemon.sd_writers ));

#ifndef HAVE_WINSOCK
	if (s >= slap_daemon.sd_nfds) {
		slap_daemon.sd_nfds = s + 1;
	}
#endif

	FD_SET( s, &slap_daemon.sd_actives );
	FD_SET( s, &slap_daemon.sd_readers );

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
		   "slapd_add: added %ld%s%s\n",
		   (long)s,
		   FD_ISSET(s, &slap_daemon.sd_readers) ? "r" : "",
		   FD_ISSET(s, &slap_daemon.sd_writers) ? "w" : "" ));
#else
	Debug( LDAP_DEBUG_CONNS, "daemon: added %ld%s%s\n",
		(long) s,
	    FD_ISSET(s, &slap_daemon.sd_readers) ? "r" : "",
		FD_ISSET(s, &slap_daemon.sd_writers) ? "w" : "" );
#endif
	ldap_pvt_thread_mutex_unlock( &slap_daemon.sd_mutex );
}

/*
 * Remove the descriptor from daemon control
 */
void slapd_remove(ber_socket_t s, int wake) {
	ldap_pvt_thread_mutex_lock( &slap_daemon.sd_mutex );

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
		   "slapd_remove: removing %ld%s%s\n",
		   (long) s,
		   FD_ISSET(s, &slap_daemon.sd_readers) ? "r" : "",
		   FD_ISSET(s, &slap_daemon.sd_writers) ? "w" : ""  ));
#else
	Debug( LDAP_DEBUG_CONNS, "daemon: removing %ld%s%s\n",
		(long) s,
	    FD_ISSET(s, &slap_daemon.sd_readers) ? "r" : "",
		FD_ISSET(s, &slap_daemon.sd_writers) ? "w" : "" );
#endif
	FD_CLR( s, &slap_daemon.sd_actives );
	FD_CLR( s, &slap_daemon.sd_readers );
	FD_CLR( s, &slap_daemon.sd_writers );

	ldap_pvt_thread_mutex_unlock( &slap_daemon.sd_mutex );
	WAKE_LISTENER(wake);
}

void slapd_clr_write(ber_socket_t s, int wake) {
	ldap_pvt_thread_mutex_lock( &slap_daemon.sd_mutex );

	assert( FD_ISSET( s, &slap_daemon.sd_actives) );
	FD_CLR( s, &slap_daemon.sd_writers );

	ldap_pvt_thread_mutex_unlock( &slap_daemon.sd_mutex );
	WAKE_LISTENER(wake);
}

void slapd_set_write(ber_socket_t s, int wake) {
	ldap_pvt_thread_mutex_lock( &slap_daemon.sd_mutex );

	assert( FD_ISSET( s, &slap_daemon.sd_actives) );
	if (!FD_ISSET(s, &slap_daemon.sd_writers))
	    FD_SET( (unsigned) s, &slap_daemon.sd_writers );

	ldap_pvt_thread_mutex_unlock( &slap_daemon.sd_mutex );
	WAKE_LISTENER(wake);
}

void slapd_clr_read(ber_socket_t s, int wake) {
	ldap_pvt_thread_mutex_lock( &slap_daemon.sd_mutex );

	assert( FD_ISSET( s, &slap_daemon.sd_actives) );
	FD_CLR( s, &slap_daemon.sd_readers );

	ldap_pvt_thread_mutex_unlock( &slap_daemon.sd_mutex );
	WAKE_LISTENER(wake);
}

void slapd_set_read(ber_socket_t s, int wake) {
	ldap_pvt_thread_mutex_lock( &slap_daemon.sd_mutex );

	assert( FD_ISSET( s, &slap_daemon.sd_actives) );
	if (!FD_ISSET(s, &slap_daemon.sd_readers))
	    FD_SET( s, &slap_daemon.sd_readers );

	ldap_pvt_thread_mutex_unlock( &slap_daemon.sd_mutex );
	WAKE_LISTENER(wake);
}

static void slapd_close(ber_socket_t s) {
#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
		   "slapd_close: closing %ld\n", (long)s ));
#else
	Debug( LDAP_DEBUG_CONNS, "daemon: closing %ld\n",
		(long) s, 0, 0 );
#endif
	tcp_close(s);
}

static void slap_free_listener_addresses(struct sockaddr **sal)
{
	struct sockaddr **sap;

	if (sal == NULL) {
		return;
	}

	for (sap = sal; *sap != NULL; sap++) {
		ch_free(*sap);
	}

	ch_free(sal);
}

#ifdef LDAP_PF_LOCAL
static int get_url_perms(
	char 	**exts,
	mode_t	*perms,
	int	*crit )
{
	int	i;

	assert( exts );
	assert( perms );
	assert( crit );

	*crit = 0;
	for ( i = 0; exts[ i ]; i++ ) {
		char	*type = exts[ i ];
		int	c = 0;

		if ( type[ 0 ] == '!' ) {
			c = 1;
			type++;
		}

		if ( strncasecmp( type, LDAPI_MOD_URLEXT "=", sizeof(LDAPI_MOD_URLEXT "=") - 1 ) == 0 ) {
			char 	*value = type + sizeof(LDAPI_MOD_URLEXT "=") - 1;
			mode_t	p = 0;

#if 0
			if ( strlen( value ) != 9 ) {
				return LDAP_OTHER;
			}

			switch ( value[ 0 ] ) {
			case 'r':
				p |= S_IRUSR;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 1 ] ) {
			case 'w':
				p |= S_IWUSR;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 2 ] ) {
			case 'x':
				p |= S_IXUSR;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 3 ] ) {
			case 'r':
				p |= S_IRGRP;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 4 ] ) {
			case 'w':
				p |= S_IWGRP;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 5 ] ) {
			case 'x':
				p |= S_IXGRP;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 6 ] ) {
			case 'r':
				p |= S_IROTH;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 7 ] ) {
			case 'w':
				p |= S_IWOTH;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 8 ] ) {
			case 'x':
				p |= S_IXOTH;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 
#else
			if ( strlen(value) != 3 ) {
				return LDAP_OTHER;
			} 

			switch ( value[ 0 ] ) {
			case 'w':
				p |= S_IRWXU;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 1 ] ) {
			case 'w':
				p |= S_IRWXG;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 

			switch ( value[ 2 ] ) {
			case 'w':
				p |= S_IRWXO;
				break;
			case '-':
				break;
			default:
				return LDAP_OTHER;
			} 
#endif

			*crit = c;
			*perms = p;

			return LDAP_SUCCESS;
		}
	}
}
#endif /* LDAP_PF_LOCAL */

/* port = 0 indicates AF_LOCAL */
static int slap_get_listener_addresses(
	const char *host,
	unsigned short port,
	struct sockaddr ***sal)
{
	struct sockaddr **sap;

#ifdef LDAP_PF_LOCAL
	if ( port == 0 ) {
		*sal = ch_malloc(2 * sizeof(void *));
		if (*sal == NULL) {
			return -1;
		}

		sap = *sal;
		*sap = ch_malloc(sizeof(struct sockaddr_un));
		if (*sap == NULL)
			goto errexit;
		sap[1] = NULL;

		if ( strlen(host) >
		     (sizeof(((struct sockaddr_un *)*sap)->sun_path) - 1) ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
				   "slap_get_listener_addresses: domain socket path (%s) too long in URL\n",
				   host ));
#else
			Debug( LDAP_DEBUG_ANY,
			       "daemon: domain socket path (%s) too long in URL",
			       host, 0, 0);
#endif
			goto errexit;
		}

		(void)memset( (void *)*sap, '\0', sizeof(struct sockaddr_un) );
		(*sap)->sa_family = AF_LOCAL;
		strcpy( ((struct sockaddr_un *)*sap)->sun_path, host );
	} else
#endif
	{
#ifdef HAVE_GETADDRINFO
		struct addrinfo hints, *res, *sai;
		int n, err;
		char serv[7];

		memset( &hints, '\0', sizeof(hints) );
		hints.ai_flags = AI_PASSIVE;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_family = AF_UNSPEC;
		snprintf(serv, sizeof serv, "%d", port);

		if ( (err = getaddrinfo(host, serv, &hints, &res)) ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
				   "slap_get_listener_addresses: getaddrinfo failed: %s\n",
				   AC_GAI_STRERROR(err) ));
#else
			Debug( LDAP_DEBUG_ANY, "daemon: getaddrinfo failed: %s\n",
				AC_GAI_STRERROR(err), 0, 0);
#endif
			return -1;
		}

		sai = res;
		for (n=2; (sai = sai->ai_next) != NULL; n++) {
			/* EMPTY */ ;
		}
		*sal = ch_calloc(n, sizeof(void *));
		if (*sal == NULL) {
			return -1;
		}

		sap = *sal;
		*sap = NULL;

		for ( sai=res; sai; sai=sai->ai_next ) {
			if( sai->ai_addr == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
					"slap_get_listener_addresses: "
					"getaddrinfo ai_addr is NULL?\n" ));
#else
				Debug( LDAP_DEBUG_ANY, "slap_get_listener_addresses: "
					"getaddrinfo ai_addr is NULL?\n", 0, 0, 0 );
#endif
				freeaddrinfo(res);
				goto errexit;
			}

			switch (sai->ai_family) {
#  ifdef LDAP_PF_INET6
			case AF_INET6:
				*sap = ch_malloc(sizeof(struct sockaddr_in6));
				if (*sap == NULL) {
					freeaddrinfo(res);
					goto errexit;
				}
				*(struct sockaddr_in6 *)*sap =
					*((struct sockaddr_in6 *)sai->ai_addr);
				break;
#  endif
			case AF_INET:
				*sap = ch_malloc(sizeof(struct sockaddr_in));
				if (*sap == NULL) {
					freeaddrinfo(res);
					goto errexit;
				}
				*(struct sockaddr_in *)*sap =
					*((struct sockaddr_in *)sai->ai_addr);
				break;
			default:
				*sap = NULL;
				break;
			}

			if (*sap != NULL) {
				(*sap)->sa_family = sai->ai_family;
				sap++;
				*sap = NULL;
			}
		}

		freeaddrinfo(res);
#else
		struct in_addr in;

		if ( host == NULL ) {
			in.s_addr = htonl(INADDR_ANY);

		} else if ( !inet_aton( host, &in ) ) {
			struct hostent *he = gethostbyname( host );
			if( he == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
					   "slap_get_listener_addresses: invalid host %s\n",
					   host ));
#else
				Debug( LDAP_DEBUG_ANY,
				       "daemon: invalid host %s", host, 0, 0);
#endif
				return -1;
			}
			AC_MEMCPY( &in, he->h_addr, sizeof( in ) );
		}

		*sal = ch_malloc(2 * sizeof(void *));
		if (*sal == NULL) {
			return -1;
		}

		sap = *sal;
		*sap = ch_malloc(sizeof(struct sockaddr_in));
		if (*sap == NULL) {
			goto errexit;
		}
		sap[1] = NULL;

		(void)memset( (void *)*sap, '\0', sizeof(struct sockaddr_in) );
		(*sap)->sa_family = AF_INET;
		((struct sockaddr_in *)*sap)->sin_port = htons(port);
		((struct sockaddr_in *)*sap)->sin_addr = in;
#endif
	}

	return 0;

errexit:
	slap_free_listener_addresses(*sal);
	return -1;
}

static Listener * slap_open_listener(
	const char* url )
{
	int	tmp, rc;
	Listener l;
	Listener *li;
	LDAPURLDesc *lud;
	unsigned short port;
	int err, addrlen = 0;
	struct sockaddr **sal, **psal;
	int socktype = SOCK_STREAM;	/* default to COTS */
#ifdef LDAP_PF_LOCAL
	mode_t 	perms = S_IRWXU;
	int	crit = 1;
#endif

	rc = ldap_url_parse( url, &lud );

	if( rc != LDAP_URL_SUCCESS ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "slap_open_listener: listen URL \"%s\" parse error %d\n",
			   url, rc ));
#else
		Debug( LDAP_DEBUG_ANY,
			"daemon: listen URL \"%s\" parse error=%d\n",
			url, rc, 0 );
#endif
		return NULL;
	}

#ifndef HAVE_TLS
	if( ldap_pvt_url_scheme2tls( lud->lud_scheme ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "slap_open_listener: TLS is not supported (%s)\n",
			   url ));
#else
		Debug( LDAP_DEBUG_ANY,
			"daemon: TLS not supported (%s)\n",
			url, 0, 0 );
#endif
		ldap_free_urldesc( lud );
		return NULL;
	}

	if(! lud->lud_port ) {
		lud->lud_port = LDAP_PORT;
	}

#else
	l.sl_is_tls = ldap_pvt_url_scheme2tls( lud->lud_scheme );

	if(! lud->lud_port ) {
		lud->lud_port = l.sl_is_tls ? LDAPS_PORT : LDAP_PORT;
	}
#endif

	port = (unsigned short) lud->lud_port;

	tmp = ldap_pvt_url_scheme2proto(lud->lud_scheme);
	if ( tmp == LDAP_PROTO_IPC ) {
#ifdef LDAP_PF_LOCAL
		if ( lud->lud_host == NULL || lud->lud_host[0] == '\0' ) {
			err = slap_get_listener_addresses(LDAPI_SOCK, 0, &sal);
		} else {
			err = slap_get_listener_addresses(lud->lud_host, 0, &sal);
		}

		if ( lud->lud_exts ) {
			err = get_url_perms( lud->lud_exts, &perms, &crit );
		}
#else

#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "slap_open_listener: URL scheme is not supported: %s\n",
			   url ));
#else
		Debug( LDAP_DEBUG_ANY, "daemon: URL scheme not supported: %s",
			url, 0, 0);
#endif
		ldap_free_urldesc( lud );
		return NULL;
#endif
	} else {
#ifdef LDAP_CONNECTIONLESS
		l.sl_is_udp = ( tmp == LDAP_PROTO_UDP );
#endif
		if( lud->lud_host == NULL || lud->lud_host[0] == '\0'
			|| strcmp(lud->lud_host, "*") == 0 )
		{
			err = slap_get_listener_addresses(NULL, port, &sal);
		} else {
			err = slap_get_listener_addresses(lud->lud_host, port, &sal);
		}
	}

	ldap_free_urldesc( lud );
	if ( err ) {
		return NULL;
	}

	psal = sal;
	while ( *sal != NULL ) {
		switch( (*sal)->sa_family ) {
		case AF_INET:
#ifdef LDAP_PF_INET6
		case AF_INET6:
#endif
#ifdef LDAP_PF_LOCAL
		case AF_LOCAL:
#endif
			break;
		default:
			sal++;
			continue;
		}
#ifdef LDAP_CONNECTIONLESS
		if (l.sl_is_udp)
		    socktype = SOCK_DGRAM;
#endif
		l.sl_sd = socket( (*sal)->sa_family, socktype, 0);
		if ( l.sl_sd == AC_SOCKET_INVALID ) {
			int err = sock_errno();
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
				   "slap_open_listener: socket() failed errno=%d (%s)\n",
				   err, sock_errstr(err) ));
#else
			Debug( LDAP_DEBUG_ANY,
				"daemon: socket() failed errno=%d (%s)\n", err,
				sock_errstr(err), 0 );
#endif
			sal++;
			continue;
		}
#ifndef HAVE_WINSOCK
		if ( l.sl_sd >= dtblsize ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
				   "slap_open_listener: listener descriptor %ld is too great %ld\n",
				   (long)l.sl_sd, (long)dtblsize ));
#else
			Debug( LDAP_DEBUG_ANY,
			       "daemon: listener descriptor %ld is too great %ld\n",
			       (long) l.sl_sd, (long) dtblsize, 0 );
#endif
			tcp_close( l.sl_sd );
			sal++;
			continue;
		}
#endif
#ifdef LDAP_PF_LOCAL
		if ( (*sal)->sa_family == AF_LOCAL ) {
			unlink ( ((struct sockaddr_un *)*sal)->sun_path );
		} else
#endif
		{
#ifdef SO_REUSEADDR
			/* enable address reuse */
			tmp = 1;
			rc = setsockopt( l.sl_sd, SOL_SOCKET, SO_REUSEADDR,
					 (char *) &tmp, sizeof(tmp) );
			if ( rc == AC_SOCKET_ERROR ) {
				int err = sock_errno();
#ifdef NEW_LOGGING
				LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
					   "slap_open_listener: setsockopt( %ld, SO_REUSEADDR ) failed errno %d (%s)\n",
					   (long)l.sl_sd, err, sock_errstr(err) ));
#else
				Debug( LDAP_DEBUG_ANY,
				       "slapd(%ld): setsockopt(SO_REUSEADDR) failed errno=%d (%s)\n",
				       (long) l.sl_sd, err, sock_errstr(err) );
#endif
			}
#endif
		}

		switch( (*sal)->sa_family ) {
		case AF_INET:
			addrlen = sizeof(struct sockaddr_in);
			break;
#ifdef LDAP_PF_INET6
		case AF_INET6:
			addrlen = sizeof(struct sockaddr_in6);
			break;
#endif
#ifdef LDAP_PF_LOCAL
		case AF_LOCAL:
			addrlen = sizeof(struct sockaddr_un);
			break;
#endif
		}

		if (!bind(l.sl_sd, *sal, addrlen))
			break;
		err = sock_errno();
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "slap_open_listener: bind(%ld) failed errno=%d (%s)\n",
			   (long)l.sl_sd, err, sock_errstr(err) ));
#else
		Debug( LDAP_DEBUG_ANY, "daemon: bind(%ld) failed errno=%d (%s)\n",
		       (long) l.sl_sd, err, sock_errstr(err) );
#endif
		tcp_close( l.sl_sd );
		sal++;
	} /* while ( *sal != NULL ) */

	if ( *sal == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "slap_open_listener: bind(%ld) failed.\n", (long)l.sl_sd ));
#else
		Debug( LDAP_DEBUG_ANY, "daemon: bind(%ld) failed\n",
			(long) l.sl_sd, 0, 0 );
#endif
		slap_free_listener_addresses(psal);
		return NULL;
	}

	switch ( (*sal)->sa_family ) {
#ifdef LDAP_PF_LOCAL
	case AF_LOCAL: {
		char *addr = ((struct sockaddr_un *)*sal)->sun_path;
		if ( chmod( addr, perms ) < 0 && crit ) {
			int err = sock_errno();
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
				   "slap_open_listener: fchmod(%ld) failed errno=%d (%s)\n",
				   (long)l.sl_sd, err, sock_errstr(err) ));
#else
			Debug( LDAP_DEBUG_ANY, "daemon: fchmod(%ld) failed errno=%d (%s)",
			       (long) l.sl_sd, err, sock_errstr(err) );
#endif
			tcp_close( l.sl_sd );
			slap_free_listener_addresses(psal);
			return NULL;
		}
		l.sl_name = ch_malloc( strlen(addr) + sizeof("PATH=") );
		sprintf( l.sl_name, "PATH=%s", addr );
	} break;
#endif /* LDAP_PF_LOCAL */

	case AF_INET: {
		char *s;
#if defined( HAVE_GETADDRINFO ) && defined( HAVE_INET_NTOP )
		char addr[INET_ADDRSTRLEN];
		inet_ntop( AF_INET, &((struct sockaddr_in *)*sal)->sin_addr,
			   addr, sizeof(addr) );
		s = addr;
#else
		s = inet_ntoa( ((struct sockaddr_in *) *sal)->sin_addr );
#endif
		port = ((struct sockaddr_in *)*sal) ->sin_port;
		l.sl_name = ch_malloc( sizeof("IP=255.255.255.255:65535") );
		sprintf( l.sl_name, "IP=%s:%d",
			 s != NULL ? s : "unknown" , port );
	} break;

#ifdef LDAP_PF_INET6
	case AF_INET6: {
		char addr[INET6_ADDRSTRLEN];
		inet_ntop( AF_INET6, &((struct sockaddr_in6 *)*sal)->sin6_addr,
			   addr, sizeof addr);
		port = ((struct sockaddr_in6 *)*sal)->sin6_port;
		l.sl_name = ch_malloc( strlen(addr) + sizeof("IP= 65535") );
		sprintf( l.sl_name, "IP=%s %d", addr, port );
	} break;
#endif /* LDAP_PF_INET6 */

	default:
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "slap_open_listener: unsupported address family (%d)\n",
			   (int)(*sal)->sa_family ));
#else
		Debug( LDAP_DEBUG_ANY, "daemon: unsupported address family (%d)\n",
			(int) (*sal)->sa_family, 0, 0 );
#endif
		break;
	}

	slap_free_listener_addresses(psal);

	l.sl_url = ch_strdup( url );
	li = ch_malloc( sizeof( Listener ) );
	*li = l;

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_RESULTS,
		   "slap_open_listener: daemon initialzed %s\n", l.sl_url ));
#else
	Debug( LDAP_DEBUG_TRACE, "daemon: initialized %s\n",
		l.sl_url, 0, 0 );
#endif
	return li;
}

static int sockinit(void);
static int sockdestroy(void);

int slapd_daemon_init( const char *urls )
{
	int i, rc;
	char **u;

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_ARGS,
		   "slapd_daemon_init: %s\n",
		   urls ? urls : "<null>" ));
#else
	Debug( LDAP_DEBUG_ARGS, "daemon_init: %s\n",
		urls ? urls : "<null>", 0, 0 );
#endif
	if( (rc = sockinit()) != 0 ) {
		return rc;
	}

#ifdef HAVE_SYSCONF
	dtblsize = sysconf( _SC_OPEN_MAX );
#elif HAVE_GETDTABLESIZE
	dtblsize = getdtablesize();
#else
	dtblsize = FD_SETSIZE;
#endif

#ifdef FD_SETSIZE
	if(dtblsize > FD_SETSIZE) {
		dtblsize = FD_SETSIZE;
	}
#endif	/* !FD_SETSIZE */

	/* open a pipe (or something equivalent connected to itself).
	 * we write a byte on this fd whenever we catch a signal. The main
	 * loop will be select'ing on this socket, and will wake up when
	 * this byte arrives.
	 */
	if( (rc = lutil_pair( wake_sds )) < 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "slap_daemon_init: lutil_pair() failed rc=%d\n", rc ));
#else
		Debug( LDAP_DEBUG_ANY,
			"daemon: lutil_pair() failed rc=%d\n", rc, 0, 0 );
#endif
		return rc;
	}

	FD_ZERO( &slap_daemon.sd_readers );
	FD_ZERO( &slap_daemon.sd_writers );

	if( urls == NULL ) {
		urls = "ldap:///";
	}

	u = str2charray( urls, " " );

	if( u == NULL || u[0] == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
			   "slap_daemon_init: no urls (%s) provided.\n", urls ));
#else
		Debug( LDAP_DEBUG_ANY, "daemon_init: no urls (%s) provided.\n",
			urls, 0, 0 );
#endif
		return -1;
	}

	for( i=0; u[i] != NULL; i++ ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
			   "slap_daemon_init: listen on %s\n.", u[i] ));
#else
		Debug( LDAP_DEBUG_TRACE, "daemon_init: listen on %s\n",
			u[i], 0, 0 );
#endif
	}

	if( i == 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
			   "slap_daemon_init: no listeners to open (%s)\n", urls ));
#else
		Debug( LDAP_DEBUG_ANY, "daemon_init: no listeners to open (%s)\n",
			urls, 0, 0 );
#endif
		charray_free( u );
		return -1;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
		   "slap_daemon_init: %d listeners to open...\n", i ));
#else
	Debug( LDAP_DEBUG_TRACE, "daemon_init: %d listeners to open...\n",
		i, 0, 0 );
#endif
	slap_listeners = ch_malloc( (i+1)*sizeof(Listener *) );

	for(i = 0; u[i] != NULL; i++ ) {
		slap_listeners[i] = slap_open_listener( u[i] );

		if( slap_listeners[i] == NULL ) {
			charray_free( u );
			return -1;
		}
	}
	slap_listeners[i] = NULL;

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
		   "slap_daemon_init: %d listeners opened\n", i ));
#else
	Debug( LDAP_DEBUG_TRACE, "daemon_init: %d listeners opened\n",
		i, 0, 0 );
#endif

#ifdef HAVE_SLP
	slapd_slp_init( urls );
	slapd_slp_reg();
#endif

	charray_free( u );
	ldap_pvt_thread_mutex_init( &slap_daemon.sd_mutex );
	return !i;
}


int
slapd_daemon_destroy(void)
{
	connections_destroy();
	tcp_close( wake_sds[1] );
	tcp_close( wake_sds[0] );
	sockdestroy();

#ifdef HAVE_SLP
	slapd_slp_dereg();
	slapd_slp_deinit();
#endif

	return 0;
}


static void *
slapd_daemon_task(
	void *ptr
)
{
	int l;
	time_t	last_idle_check = 0;
	time( &starttime );

	if ( global_idletimeout > 0 ) {
		last_idle_check = slap_get_time();
	}
	for ( l = 0; slap_listeners[l] != NULL; l++ ) {
		if ( slap_listeners[l]->sl_sd == AC_SOCKET_INVALID )
			continue;
#ifdef LDAP_CONNECTIONLESS
		/* Since this is connectionless, the data port is the
		 * listening port. The listen() and accept() calls
		 * are unnecessary.
		 */
		if ( slap_listeners[l]->sl_is_udp )
		{
			slapd_add( slap_listeners[l]->sl_sd );
			continue;
		}
#endif

		if ( listen( slap_listeners[l]->sl_sd, SLAPD_LISTEN ) == -1 ) {
			int err = sock_errno();
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
				   "slapd_daemon_task: listen( %s, 5 ) failed errno=%d (%s)\n",
				   slap_listeners[l]->sl_url, err, sock_errstr(err) ));
#else
			Debug( LDAP_DEBUG_ANY,
				"daemon: listen(%s, 5) failed errno=%d (%s)\n",
					slap_listeners[l]->sl_url, err,
					sock_errstr(err) );
#endif
			return( (void*)-1 );
		}

		slapd_add( slap_listeners[l]->sl_sd );
	}

#ifdef HAVE_NT_SERVICE_MANAGER
	if ( started_event != NULL ) {
		ldap_pvt_thread_cond_signal( &started_event );
	}
#endif
	/* initialization complete. Here comes the loop. */

	while ( !slapd_shutdown ) {
		ber_socket_t i;
		int ns;
		int at;
		ber_socket_t nfds;
#define SLAPD_EBADF_LIMIT 16
		int ebadf = 0;
		int emfile = 0;

#define SLAPD_IDLE_CHECK_LIMIT 4
		time_t	now;


		fd_set			readfds;
		fd_set			writefds;
		Sockaddr		from;

#if defined(SLAPD_RLOOKUPS)
		struct hostent		*hp;
#endif
		struct timeval		zero;
		struct timeval		*tvp;

		if( emfile ) {
			now = slap_get_time();
			connections_timeout_idle( now );
		}
		else if ( global_idletimeout > 0 ) {
			now = slap_get_time();
			if ( difftime( last_idle_check+global_idletimeout/SLAPD_IDLE_CHECK_LIMIT, now ) < 0 ) {
				connections_timeout_idle( now );
			}
		}

		FD_ZERO( &writefds );
		FD_ZERO( &readfds );

		zero.tv_sec = 0;
		zero.tv_usec = 0;

		ldap_pvt_thread_mutex_lock( &slap_daemon.sd_mutex );

#ifdef FD_SET_MANUAL_COPY
		for( s = 0; s < nfds; s++ ) {
			if(FD_ISSET( &slap_sd_readers, s )) {
				FD_SET( s, &readfds );
			}
			if(FD_ISSET( &slap_sd_writers, s )) {
				FD_SET( s, &writefds );
			}
		}
#else
		AC_MEMCPY( &readfds, &slap_daemon.sd_readers, sizeof(fd_set) );
		AC_MEMCPY( &writefds, &slap_daemon.sd_writers, sizeof(fd_set) );
#endif
		assert(!FD_ISSET(wake_sds[0], &readfds));
		FD_SET( wake_sds[0], &readfds );

		for ( l = 0; slap_listeners[l] != NULL; l++ ) {
			if ( slap_listeners[l]->sl_sd == AC_SOCKET_INVALID )
				continue;
			if (!FD_ISSET(slap_listeners[l]->sl_sd, &readfds))
			    FD_SET( slap_listeners[l]->sl_sd, &readfds );
		}

#ifndef HAVE_WINSOCK
		nfds = slap_daemon.sd_nfds;
#else
		nfds = dtblsize;
#endif

		ldap_pvt_thread_mutex_unlock( &slap_daemon.sd_mutex );

		at = ldap_pvt_thread_pool_backload(&connection_pool);

#if defined( HAVE_YIELDING_SELECT ) || defined( NO_THREADS )
		tvp = NULL;
#else
		tvp = at ? &zero : NULL;
#endif

		for ( l = 0; slap_listeners[l] != NULL; l++ ) {
			if ( slap_listeners[l]->sl_sd == AC_SOCKET_INVALID )
				continue;

#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
				   "slapd_daemon_task: select: listen=%d active_threads=%d tvp=%s\n",
				   slap_listeners[l]->sl_sd, at, tvp == NULL ? "NULL" : "zero" ));
#else
			Debug( LDAP_DEBUG_CONNS,
				"daemon: select: listen=%d active_threads=%d tvp=%s\n",
					slap_listeners[l]->sl_sd, at,
					tvp == NULL ? "NULL" : "zero" );
#endif
		}

		switch(ns = select( nfds, &readfds,
#ifdef HAVE_WINSOCK
			/* don't pass empty fd_set */
			( writefds.fd_count > 0 ? &writefds : NULL ),
#else
			&writefds,
#endif
			NULL, tvp ))
		{
		case -1: {	/* failure - try again */
				int err = sock_errno();

				if( err == EBADF
#ifdef WSAENOTSOCK
					/* you'd think this would be EBADF */
					|| err == WSAENOTSOCK
#endif
				) {
					if (++ebadf < SLAPD_EBADF_LIMIT)
						continue;
				}

				if( err != EINTR ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
						   "slapd_daemon_task: select failed (%d): %s\n",
						   err, sock_errstr(err) ));
#else
					Debug( LDAP_DEBUG_CONNS,
						"daemon: select failed (%d): %s\n",
						err, sock_errstr(err), 0 );
#endif
					slapd_shutdown = -1;
				}
			}
			continue;

		case 0:		/* timeout - let threads run */
			ebadf = 0;
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL2,
				   "slapd_daemon_task: select timeout - yielding\n" ));
#else
			Debug( LDAP_DEBUG_CONNS, "daemon: select timeout - yielding\n",
			    0, 0, 0 );
#endif
			ldap_pvt_thread_yield();
			continue;

		default:	/* something happened - deal with it */
			if( slapd_shutdown ) continue;

			ebadf = 0;
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL2,
				   "slapd_daemon_task: activity on %d descriptors\n", ns ));
#else
			Debug( LDAP_DEBUG_CONNS, "daemon: activity on %d descriptors\n",
				ns, 0, 0 );
#endif
			/* FALL THRU */
		}

		if( FD_ISSET( wake_sds[0], &readfds ) ) {
			char c[BUFSIZ];
			tcp_read( wake_sds[0], c, sizeof(c) );
#ifdef NO_THREADS
			waking = 0;
#endif
			continue;
		}

		for ( l = 0; slap_listeners[l] != NULL; l++ ) {
			ber_socket_t s;
			socklen_t len = sizeof(from);
			long id;
			slap_ssf_t ssf = 0;
			char *authid = NULL;

			char	*dnsname = NULL;
			char	*peeraddr;
#ifdef LDAP_PF_LOCAL
			char	peername[MAXPATHLEN + sizeof("PATH=")];
#elif defined(LDAP_PF_INET6)
			char	peername[sizeof("IP=ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff 65535")];
#else
			char	peername[sizeof("IP=255.255.255.255:65336")];
#endif /* LDAP_PF_LOCAL */

			peername[0] = '\0';

			if ( slap_listeners[l]->sl_sd == AC_SOCKET_INVALID )
				continue;

			if ( !FD_ISSET( slap_listeners[l]->sl_sd, &readfds ) )
				continue;

#ifdef LDAP_CONNECTIONLESS
			if ( slap_listeners[l]->sl_is_udp )
			{
			/* The first time we receive a query, we set this
			 * up as a "connection". It remains open for the life
			 * of the slapd.
			 */
				if ( slap_listeners[l]->sl_is_udp < 2 )
				{
				    id = connection_init(
				    	slap_listeners[l]->sl_sd,
					slap_listeners[l]->sl_url, "", "",
					slap_listeners[l]->sl_name,
					2, ssf, authid );
				    slap_listeners[l]->sl_is_udp++;
				}
				continue;
			}
#endif

			s = accept( slap_listeners[l]->sl_sd,
				(struct sockaddr *) &from, &len );
			if ( s == AC_SOCKET_INVALID ) {
				int err = sock_errno();

#ifdef EMFILE
				if( err == EMFILE ) {
					emfile++;
				} else
#endif
#ifdef ENFILE
				if( err == ENFILE ) {
					emfile++;
				} else 
#endif
				{
					emfile=0;
				}

				if( emfile < 3 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
						"slapd_daemon_task: accept(%ld) failed errno=%d (%s)\n",
						(long)slap_listeners[l]->sl_sd, err, sock_errstr(err) ));
#else
					Debug( LDAP_DEBUG_ANY,
					    "daemon: accept(%ld) failed errno=%d (%s)\n",
					    (long) slap_listeners[l]->sl_sd, err,
					    sock_errstr(err) );
#endif
				} else {
					/* prevent busy loop */
#  ifdef HAVE_USLEEP
					if( emfile % 4 == 3 ) usleep( 250 );
#  else
					if( emfile % 8 == 7 ) sleep( 1 );
#  endif
				}

				ldap_pvt_thread_yield();
				continue;
			}
			emfile = 0;

#ifndef HAVE_WINSOCK
			/* make sure descriptor number isn't too great */
			if ( s >= dtblsize ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
				   "slapd_daemon_task: %ld beyond descriptor table size %ld\n",
				   (long)s, (long)dtblsize ));
#else
				Debug( LDAP_DEBUG_ANY,
					"daemon: %ld beyond descriptor table size %ld\n",
					(long) s, (long) dtblsize, 0 );
#endif

				slapd_close(s);
				ldap_pvt_thread_yield();
				continue;
			}
#endif

#ifdef LDAP_DEBUG
			ldap_pvt_thread_mutex_lock( &slap_daemon.sd_mutex );

			/* newly accepted stream should not be in any of the FD SETS */
			assert( !FD_ISSET( s, &slap_daemon.sd_actives) );
			assert( !FD_ISSET( s, &slap_daemon.sd_readers) );
			assert( !FD_ISSET( s, &slap_daemon.sd_writers) );

			ldap_pvt_thread_mutex_unlock( &slap_daemon.sd_mutex );
#endif

#if defined( SO_KEEPALIVE ) || defined( TCP_NODELAY )
#ifdef LDAP_PF_LOCAL
			/* for IPv4 and IPv6 sockets only */
			if ( from.sa_addr.sa_family != AF_LOCAL )
#endif /* LDAP_PF_LOCAL */
			{
				int rc;
				int tmp;
#ifdef SO_KEEPALIVE
				/* enable keep alives */
				tmp = 1;
				rc = setsockopt( s, SOL_SOCKET, SO_KEEPALIVE,
					(char *) &tmp, sizeof(tmp) );
				if ( rc == AC_SOCKET_ERROR ) {
					int err = sock_errno();
#ifdef NEW_LOGGING
					LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
						   "slapd_daemon_task: setsockopt( %ld, SO_KEEPALIVE) failed errno=%d (%s)\n",
						   (long)s, err, sock_errstr(err) ));
#else
					Debug( LDAP_DEBUG_ANY,
						"slapd(%ld): setsockopt(SO_KEEPALIVE) failed "
						"errno=%d (%s)\n", (long) s, err, sock_errstr(err) );
#endif
				}
#endif
#ifdef TCP_NODELAY
				/* enable no delay */
				tmp = 1;
				rc = setsockopt( s, IPPROTO_TCP, TCP_NODELAY,
					(char *)&tmp, sizeof(tmp) );
				if ( rc == AC_SOCKET_ERROR ) {
					int err = sock_errno();
#ifdef NEW_LOGGING
					LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
						   "slapd_daemon_task: setsockopt( %ld, TCP_NODELAY) failed errno=%d (%s)\n",
						   (long)s, err, sock_errstr(err) ));
#else
					Debug( LDAP_DEBUG_ANY,
						"slapd(%ld): setsockopt(TCP_NODELAY) failed "
						"errno=%d (%s)\n", (long) s, err, sock_errstr(err) );
#endif
				}
#endif
			}
#endif

#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL1,
				   "slapd_daemon_task: new connection on %ld\n", (long)s ));
#else
			Debug( LDAP_DEBUG_CONNS, "daemon: new connection on %ld\n",
				(long) s, 0, 0 );
#endif
			switch ( from.sa_addr.sa_family ) {
#  ifdef LDAP_PF_LOCAL
			case AF_LOCAL:
				sprintf( peername, "PATH=%s", from.sa_un_addr.sun_path );
				ssf = LDAP_PVT_SASL_LOCAL_SSF;
				dnsname = "local";
				break;
#endif /* LDAP_PF_LOCAL */

#  ifdef LDAP_PF_INET6
			case AF_INET6:
			if ( IN6_IS_ADDR_V4MAPPED(&from.sa_in6_addr.sin6_addr) ) {
				peeraddr = inet_ntoa( *((struct in_addr *)
							&from.sa_in6_addr.sin6_addr.s6_addr[12]) );
				sprintf( peername, "IP=%s:%d",
					 peeraddr != NULL ? peeraddr : "unknown",
					 (unsigned) ntohs( from.sa_in6_addr.sin6_port ) );
			} else {
				char addr[INET6_ADDRSTRLEN];
				sprintf( peername, "IP=%s %d",
					 inet_ntop( AF_INET6,
						    &from.sa_in6_addr.sin6_addr,
						    addr, sizeof addr) ? addr : "unknown",
					 (unsigned) ntohs( from.sa_in6_addr.sin6_port ) );
			}
			break;
#  endif /* LDAP_PF_INET6 */

			case AF_INET:
			peeraddr = inet_ntoa( from.sa_in_addr.sin_addr );
			sprintf( peername, "IP=%s:%d",
				peeraddr != NULL ? peeraddr : "unknown",
				(unsigned) ntohs( from.sa_in_addr.sin_port ) );
				break;

			default:
				slapd_close(s);
				continue;
			}

			if ( ( from.sa_addr.sa_family == AF_INET )
#ifdef LDAP_PF_INET6
				|| ( from.sa_addr.sa_family == AF_INET6 )
#endif
			) {
#ifdef SLAPD_RLOOKUPS
#  ifdef LDAP_PF_INET6
				if ( from.sa_addr.sa_family == AF_INET6 )
					hp = gethostbyaddr(
						(char *)&(from.sa_in6_addr.sin6_addr),
						sizeof(from.sa_in6_addr.sin6_addr),
						AF_INET6 );
				else
#  endif /* LDAP_PF_INET6 */
				hp = gethostbyaddr(
					(char *) &(from.sa_in_addr.sin_addr),
					sizeof(from.sa_in_addr.sin_addr),
					AF_INET );
				dnsname = hp ? ldap_pvt_str2lower( hp->h_name ) : NULL;
#else
				dnsname = NULL;
#endif /* SLAPD_RLOOKUPS */

#ifdef HAVE_TCPD
				if ( !hosts_ctl("slapd",
						dnsname != NULL ? dnsname : STRING_UNKNOWN,
						peeraddr != NULL ? peeraddr : STRING_UNKNOWN,
						STRING_UNKNOWN ))
				{
					/* DENY ACCESS */
					Statslog( LDAP_DEBUG_ANY,
						"fd=%ld host access from %s (%s) denied.\n",
						(long) s,
						dnsname != NULL ? dnsname : "unknown",
						peeraddr != NULL ? peeraddr : "unknown",
						0, 0 );
					slapd_close(s);
					continue;
				}
#endif /* HAVE_TCPD */
			}

			id = connection_init(s,
				slap_listeners[l]->sl_url,
				dnsname != NULL ? dnsname : "unknown",
				peername,
				slap_listeners[l]->sl_name,
#ifdef HAVE_TLS
				slap_listeners[l]->sl_is_tls,
#else
				0,
#endif
				ssf,
				authid );

			if( authid ) ch_free(authid);

			if( id < 0 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "connection", LDAP_LEVEL_INFO,
					   "slapd_daemon_task: connection_init(%ld, %s, %s) failed.\n",
					   (long)s, peername, slap_listeners[l]->sl_name ));
#else
				Debug( LDAP_DEBUG_ANY,
					"daemon: connection_init(%ld, %s, %s) failed.\n",
					(long) s,
					peername,
					slap_listeners[l]->sl_name );
#endif
				slapd_close(s);
				continue;
			}

			Statslog( LDAP_DEBUG_STATS,
				"daemon: conn=%ld fd=%ld connection from %s (%s) accepted.\n",
				id, (long) s,
				peername,
				slap_listeners[l]->sl_name,
				0 );

			slapd_add( s );
			continue;
		}

#ifdef LDAP_DEBUG
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL2,
			   "slapd_daemon_task: activity on " ));
#else
		Debug( LDAP_DEBUG_CONNS, "daemon: activity on:", 0, 0, 0 );
#endif
#ifdef HAVE_WINSOCK
		for ( i = 0; i < readfds.fd_count; i++ ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL2,
				   " %d%s", readfds.fd_array[i], "r", 0 ));
#else
			Debug( LDAP_DEBUG_CONNS, " %d%s",
				readfds.fd_array[i], "r", 0 );
#endif
		}
		for ( i = 0; i < writefds.fd_count; i++ ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL2,
				   " %d%s", writefds.fd_array[i], "w" ));
#else
			Debug( LDAP_DEBUG_CONNS, " %d%s",
				writefds.fd_array[i], "w", 0 );
#endif
		}

#else
		for ( i = 0; i < nfds; i++ ) {
			int	r, w;
			int	is_listener = 0;

			for ( l = 0; slap_listeners[l] != NULL; l++ ) {
				if ( i == slap_listeners[l]->sl_sd ) {
#ifdef LDAP_CONNECTIONLESS
				/* The listener is the data port. Don't
				 * skip it.
				 */
					if (slap_listeners[l]->sl_is_udp)
						continue;
#endif
					is_listener = 1;
					break;
				}
			}
			if ( is_listener ) {
				continue;
			}
			r = FD_ISSET( i, &readfds );
			w = FD_ISSET( i, &writefds );
			if ( r || w ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL2,
					   " %d%s%s", i,
					   r ? "r" : "", w ? "w" : "" ));
#else
				Debug( LDAP_DEBUG_CONNS, " %d%s%s", i,
				    r ? "r" : "", w ? "w" : "" );
#endif
			}
		}
#endif
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL2, "\n" ));
#else
		Debug( LDAP_DEBUG_CONNS, "\n", 0, 0, 0 );
#endif

#endif

		/* loop through the writers */
#ifdef HAVE_WINSOCK
		for ( i = 0; i < writefds.fd_count; i++ )
#else
		for ( i = 0; i < nfds; i++ )
#endif
		{
			ber_socket_t wd;
			int is_listener = 0;
#ifdef HAVE_WINSOCK
			wd = writefds.fd_array[i];
#else
			if( ! FD_ISSET( i, &writefds ) ) {
				continue;
			}
			wd = i;
#endif

			for ( l = 0; slap_listeners[l] != NULL; l++ ) {
				if ( i == slap_listeners[l]->sl_sd ) {
#ifdef LDAP_CONNECTIONLESS
					if (slap_listeners[l]->sl_is_udp)
						continue;
#endif
					is_listener = 1;
					break;
				}
			}
			if ( is_listener ) {
				continue;
			}
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL2,
				   "slapd_daemon_task: write active on %d\n", wd ));
#else
			Debug( LDAP_DEBUG_CONNS,
				"daemon: write active on %d\n",
				wd, 0, 0 );
#endif
			/*
			 * NOTE: it is possible that the connection was closed
			 * and that the stream is now inactive.
			 * connection_write() must valid the stream is still
			 * active.
			 */

			if ( connection_write( wd ) < 0 ) {
				FD_CLR( (unsigned) wd, &readfds );
				slapd_close( wd );
			}
		}

#ifdef HAVE_WINSOCK
		for ( i = 0; i < readfds.fd_count; i++ )
#else
		for ( i = 0; i < nfds; i++ )
#endif
		{
			ber_socket_t rd;
			int is_listener = 0;

#ifdef HAVE_WINSOCK
			rd = readfds.fd_array[i];
#else
			if( ! FD_ISSET( i, &readfds ) ) {
				continue;
			}
			rd = i;
#endif

			for ( l = 0; slap_listeners[l] != NULL; l++ ) {
				if ( rd == slap_listeners[l]->sl_sd ) {
#ifdef LDAP_CONNECTIONLESS
					if (slap_listeners[l]->sl_is_udp)
						continue;
#endif
					is_listener = 1;
					break;
				}
			}
			if ( is_listener ) {
				continue;
			}

#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_DETAIL2,
				   "slapd_daemon_task: read activity on %d\n", rd ));
#else
			Debug ( LDAP_DEBUG_CONNS,
				"daemon: read activity on %d\n", rd, 0, 0 );
#endif
			/*
			 * NOTE: it is possible that the connection was closed
			 * and that the stream is now inactive.
			 * connection_read() must valid the stream is still
			 * active.
			 */

			if ( connection_read( rd ) < 0 ) {
				slapd_close( rd );
			}
		}
		ldap_pvt_thread_yield();
	}

	if( slapd_shutdown > 0 ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_CRIT,
			   "slapd_daemon_task: shutdown requested and initiated.\n"));
#else
		Debug( LDAP_DEBUG_TRACE,
			"daemon: shutdown requested and initiated.\n",
			0, 0, 0 );
#endif

	} else if ( slapd_shutdown < 0 ) {
#ifdef HAVE_NT_SERVICE_MANAGER
		if (slapd_shutdown == -1)
		{
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_CRIT,
				   "slapd_daemon_task: shutdown initiated by Service Manager.\n"));
#else
			Debug( LDAP_DEBUG_TRACE,
			       "daemon: shutdown initiated by Service Manager.\n",
			       0, 0, 0);
#endif
		}
		else
#endif
		{
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_CRIT,
				   "slapd_daemon_task: abnormal condition, shutdown initiated.\n" ));
#else
			Debug( LDAP_DEBUG_TRACE,
			       "daemon: abnormal condition, shutdown initiated.\n",
			       0, 0, 0 );
#endif
		}
	} else {
#ifdef NEW_LOGGING
		LDAP_LOG(( "connection", LDAP_LEVEL_CRIT,
			   "slapd_daemon_task: no active streams, shutdown initiated.\n" ));
#else
		Debug( LDAP_DEBUG_TRACE,
		       "daemon: no active streams, shutdown initiated.\n",
		       0, 0, 0 );
#endif
	}

	for ( l = 0; slap_listeners[l] != NULL; l++ ) {
		if ( slap_listeners[l]->sl_sd != AC_SOCKET_INVALID ) {
#ifdef LDAP_PF_LOCAL
			if ( slap_listeners[l]->sl_sa.sa_addr.sa_family == AF_LOCAL ) {
				unlink( slap_listeners[l]->sl_sa.sa_un_addr.sun_path );
			}
#endif /* LDAP_PF_LOCAL */
			slapd_close( slap_listeners[l]->sl_sd );
		}
		if ( slap_listeners[l]->sl_url )
			free ( slap_listeners[l]->sl_url );
		if ( slap_listeners[l]->sl_name )
			free ( slap_listeners[l]->sl_name );
		free ( slap_listeners[l] );
	}
	free ( slap_listeners );
	slap_listeners = NULL;

#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_CRIT,
		   "slapd_daemon_task: shutdown waiting for %d threads to terminate.\n",
		   ldap_pvt_thread_pool_backload(&connection_pool) ));
#else
	Debug( LDAP_DEBUG_ANY,
	    "slapd shutdown: waiting for %d threads to terminate\n",
	    ldap_pvt_thread_pool_backload(&connection_pool), 0, 0 );
#endif
	ldap_pvt_thread_pool_destroy(&connection_pool, 1);

	return NULL;
}


int slapd_daemon( void )
{
	int rc;

	connections_init();

#define SLAPD_LISTENER_THREAD 1
#if defined( SLAPD_LISTENER_THREAD )
	{
		ldap_pvt_thread_t	listener_tid;

		/* listener as a separate THREAD */
		rc = ldap_pvt_thread_create( &listener_tid,
			0, slapd_daemon_task, NULL );

		if ( rc != 0 ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "connection", LDAP_LEVEL_ERR,
				   "slapd_daemon: listener ldap_pvt_thread_create failed (%d).\n", rc ));
#else
			Debug( LDAP_DEBUG_ANY,
			"listener ldap_pvt_thread_create failed (%d)\n", rc, 0, 0 );
#endif
			return rc;
		}
 
  		/* wait for the listener thread to complete */
  		ldap_pvt_thread_join( listener_tid, (void *) NULL );
	}
#else
	/* experimental code */
	slapd_daemon_task( NULL );
#endif

	return 0;

}

int sockinit(void)
{
#if defined( HAVE_WINSOCK2 )
    WORD wVersionRequested;
	WSADATA wsaData;
	int err;

	wVersionRequested = MAKEWORD( 2, 0 );

	err = WSAStartup( wVersionRequested, &wsaData );
	if ( err != 0 ) {
		/* Tell the user that we couldn't find a usable */
		/* WinSock DLL.					 */
		return -1;
	}

	/* Confirm that the WinSock DLL supports 2.0.*/
	/* Note that if the DLL supports versions greater    */
	/* than 2.0 in addition to 2.0, it will still return */
	/* 2.0 in wVersion since that is the version we	     */
	/* requested.					     */

	if ( LOBYTE( wsaData.wVersion ) != 2 ||
		HIBYTE( wsaData.wVersion ) != 0 )
	{
	    /* Tell the user that we couldn't find a usable */
	    /* WinSock DLL.				     */
	    WSACleanup();
	    return -1;
	}

	/* The WinSock DLL is acceptable. Proceed. */
#elif defined( HAVE_WINSOCK )
	WSADATA wsaData;
	if ( WSAStartup( 0x0101, &wsaData ) != 0 ) {
	    return -1;
	}
#endif
	return 0;
}

int sockdestroy(void)
{
#if defined( HAVE_WINSOCK2 ) || defined( HAVE_WINSOCK )
	WSACleanup();
#endif
	return 0;
}

RETSIGTYPE
slap_sig_shutdown( int sig )
{
#ifdef NEW_LOGGING
	LDAP_LOG(( "connection", LDAP_LEVEL_CRIT,
		   "slap_sig_shutdown: signal %d\n", sig ));
#else
	Debug(LDAP_DEBUG_TRACE, "slap_sig_shutdown: signal %d\n", sig, 0, 0);
#endif

	/*
	 * If the NT Service Manager is controlling the server, we don't
	 * want SIGBREAK to kill the server. For some strange reason,
	 * SIGBREAK is generated when a user logs out.
	 */

#if HAVE_NT_SERVICE_MANAGER && SIGBREAK
	if (is_NT_Service && sig == SIGBREAK)
#ifdef NEW_LOGGING
	    LDAP_LOG(( "connection", LDAP_LEVEL_CRIT,
		       "slap_sig_shutdown: SIGBREAK ignored.\n" ));
#else
	    Debug(LDAP_DEBUG_TRACE, "slap_sig_shutdown: SIGBREAK ignored.\n",
		  0, 0, 0);
#endif
	else
#endif
	slapd_shutdown = sig;

	WAKE_LISTENER(1);

	/* reinstall self */
	(void) SIGNAL_REINSTALL( sig, slap_sig_shutdown );
}

RETSIGTYPE
slap_sig_wake( int sig )
{
	WAKE_LISTENER(1);

	/* reinstall self */
	(void) SIGNAL_REINSTALL( sig, slap_sig_wake );
}


void slapd_add_internal(ber_socket_t s) {
	slapd_add(s);
}

Listener ** slapd_get_listeners(void) {
	return slap_listeners;
}
