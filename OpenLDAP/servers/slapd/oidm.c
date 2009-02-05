/* schemaparse.c - routines to parse config file objectclass definitions */
/* $OpenLDAP: pkg/ldap/servers/slapd/oidm.c,v 1.1 2002/01/10 00:46:08 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/ctype.h>
#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"

static OidMacro *om_list = NULL;

/* Replace an OID Macro invocation with its full numeric OID.
 * If the macro is used with "macroname:suffix" append ".suffix"
 * to the expansion.
 */
char *
oidm_find(char *oid)
{
	OidMacro *om;

	/* OID macros must start alpha */
	if ( OID_LEADCHAR( *oid ) )	{
		return oid;
	}

    for (om = om_list; om; om=om->som_next) {
		char **names = om->som_names;

		if( names == NULL ) {
			continue;
		}

		for( ; *names != NULL ; names++ ) {
			int pos = dscompare(*names, oid, ':');

			if( pos ) {
				int suflen = strlen(oid + pos);
				char *tmp = ch_malloc( om->som_oid.bv_len
					+ suflen + 1);
				strcpy(tmp, om->som_oid.bv_val);
				if( suflen ) {
					suflen = om->som_oid.bv_len;
					tmp[suflen++] = '.';
					strcpy(tmp+suflen, oid+pos+1);
				}
				return tmp;
			}
		}
	}
	return NULL;
}

void
oidm_destroy()
{
	OidMacro *om, *n;

	for (om = om_list; om; om = n) {
		n = om->som_next;
		charray_free(om->som_names);
		free(om->som_oid.bv_val);
		free(om);
	}
}

int
parse_oidm(
    const char	*fname,
    int		lineno,
    int		argc,
    char 	**argv
)
{
	char *oid;
	OidMacro *om;

	if (argc != 3) {
		fprintf( stderr, "%s: line %d: too many arguments\n",
			fname, lineno );
usage:	fprintf( stderr, "\tObjectIdentifier <name> <oid>\n");
		return 1;
	}

	oid = oidm_find( argv[1] );
	if( oid != NULL ) {
		fprintf( stderr,
			"%s: line %d: "
			"ObjectIdentifier \"%s\" previously defined \"%s\"",
			fname, lineno, argv[1], oid );
		return 1;
	}

	om = (OidMacro *) ch_malloc( sizeof(OidMacro) );

	om->som_names = NULL;
	charray_add( &om->som_names, argv[1] );
	om->som_oid.bv_val = oidm_find( argv[2] );

	if (!om->som_oid.bv_val) {
		fprintf( stderr, "%s: line %d: OID %s not recognized\n",
			fname, lineno, argv[2] );
		goto usage;
	}

	if (om->som_oid.bv_val == argv[2]) {
		om->som_oid.bv_val = ch_strdup( argv[2] );
	}

	om->som_oid.bv_len = strlen( om->som_oid.bv_val );
	om->som_next = om_list;
	om_list = om;

	return 0;
}
