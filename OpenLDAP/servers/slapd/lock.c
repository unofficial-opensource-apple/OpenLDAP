/* lock.c - routines to open and apply an advisory lock to a file */
/* $OpenLDAP: pkg/ldap/servers/slapd/lock.c,v 1.20 2002/01/04 20:17:46 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>
#include <ac/time.h>
#include <ac/unistd.h>

#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif

#include "slap.h"

FILE *
lock_fopen( const char *fname, const char *type, FILE **lfp )
{
	FILE	*fp;
	char	buf[MAXPATHLEN];

	/* open the lock file */
	strcpy(slap_strcopy( buf, fname ), ".lock" );
	if ( (*lfp = fopen( buf, "w" )) == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "lock_fopen: could not open lock file \"%s\".\n", buf ));
#else
		Debug( LDAP_DEBUG_ANY, "could not open \"%s\"\n", buf, 0, 0 );
#endif

		return( NULL );
	}

	/* acquire the lock */
	ldap_lockf( fileno(*lfp) );

	/* open the log file */
	if ( (fp = fopen( fname, type )) == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "lock_fopen: could not open log file \"%s\".\n", buf ));
#else
		Debug( LDAP_DEBUG_ANY, "could not open \"%s\"\n", fname, 0, 0 );
#endif

		ldap_unlockf( fileno(*lfp) );
		fclose( *lfp );
		*lfp = NULL;
		return( NULL );
	}

	return( fp );
}

int
lock_fclose( FILE *fp, FILE *lfp )
{
	/* unlock */
	ldap_unlockf( fileno(lfp) );
	fclose( lfp );

	return( fclose( fp ) );
}
