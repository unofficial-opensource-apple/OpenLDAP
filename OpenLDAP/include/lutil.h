/* $OpenLDAP: pkg/ldap/include/lutil.h,v 1.37 2002/01/04 19:40:30 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, Redwood City, California, USA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.  A copy of this license is available at
 * http://www.OpenLDAP.org/license.html or in file LICENSE in the
 * top-level directory of the distribution.
 */

#ifndef _LUTIL_H
#define _LUTIL_H 1

#include <ldap_cdefs.h>
#include <lber_types.h>

/*
 * Include file for LDAP utility routine
 */

LDAP_BEGIN_DECL

/* n octets encode into ceiling(n/3) * 4 bytes */
/* Avoid floating point math by through extra padding */

#define LUTIL_BASE64_ENCODE_LEN(n)	((n)/3 * 4 + 4)
#define LUTIL_BASE64_DECODE_LEN(n)	((n)/4 * 3)

/* ISC Base64 Routines */
/* base64.c */

LDAP_LUTIL_F( int )
lutil_b64_ntop LDAP_P((
	unsigned char const *,
	size_t,
	char *,
	size_t));

LDAP_LUTIL_F( int )
lutil_b64_pton LDAP_P((
	char const *,
	unsigned char *,
	size_t));

/* detach.c */
LDAP_LUTIL_F( void )
lutil_detach LDAP_P((
	int debug,
	int do_close));

/* entropy.c */
LDAP_LUTIL_F( int )
lutil_entropy LDAP_P((
	unsigned char *buf,
	ber_len_t nbytes ));

/* passwd.c */
struct berval; /* avoid pulling in lber.h */

LDAP_LUTIL_F( int )
lutil_authpasswd LDAP_P((
	const struct berval *passwd,	/* stored password */
	const struct berval *cred,	/* user supplied value */
	const char **methods ));

LDAP_LUTIL_F( int )
lutil_authpasswd_hash LDAP_P((
	const struct berval *cred,
	struct berval **passwd,	/* password to store */
	struct berval **salt,	/* salt to store */
	const char *method ));

#if defined( SLAPD_SPASSWD ) && defined( HAVE_CYRUS_SASL )
	/* cheat to avoid pulling in <sasl.h> */
LDAP_LUTIL_V( struct sasl_conn * ) lutil_passwd_sasl_conn;
#endif

LDAP_LUTIL_F( int )
lutil_passwd LDAP_P((
	const struct berval *passwd,	/* stored password */
	const struct berval *cred,	/* user supplied value */
	const char **methods ));

LDAP_LUTIL_F( struct berval * )
lutil_passwd_generate LDAP_P(( ber_len_t ));

LDAP_LUTIL_F( struct berval * )
lutil_passwd_hash LDAP_P((
	const struct berval *passwd,
	const char *method ));

LDAP_LUTIL_F( int )
lutil_passwd_scheme LDAP_P((
	const char *scheme ));

LDAP_LUTIL_F( int )
lutil_salt_format LDAP_P((
	const char *format ));

/* utils.c */
LDAP_LUTIL_F( char* )
lutil_progname LDAP_P((
	const char* name,
	int argc,
	char *argv[] ));

#ifndef HAVE_MKSTEMP
LDAP_LUTIL_F( int )
mkstemp LDAP_P (( char * template ));
#endif

/* sockpair.c */
LDAP_LUTIL_F( int )
lutil_pair( ber_socket_t sd[2] );

/* uuid.c */
LDAP_LUTIL_F( size_t )
lutil_uuidstr( char *buf, size_t len );

/* csn.c */
LDAP_LUTIL_F( size_t )
lutil_csnstr( char *buf, size_t len, unsigned int replica, unsigned int mod );

/*
 * Sometimes not all declarations in a header file are needed.
 * An indicator to this is whether or not the symbol's type has
 * been defined. Thus, we don't need to include a symbol if
 * its type has not been defined through another header file.
 */

#ifdef HAVE_NT_SERVICE_MANAGER
LDAP_LUTIL_V (int) is_NT_Service;

#ifdef _LDAP_PVT_THREAD_H
LDAP_LUTIL_V (ldap_pvt_thread_cond_t) started_event;
#endif /* _LDAP_PVT_THREAD_H */

/* macros are different between Windows and Mingw */
#if defined(_WINSVC_H) || defined(_WINSVC_)
LDAP_LUTIL_V (SERVICE_STATUS) SLAPDServiceStatus;
LDAP_LUTIL_V (SERVICE_STATUS_HANDLE) hSLAPDServiceStatus;
#endif /* _WINSVC_H */

#endif /* HAVE_NT_SERVICE_MANAGER */

LDAP_END_DECL

#endif /* _LUTIL_H */
