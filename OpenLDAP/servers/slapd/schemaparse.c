/* schemaparse.c - routines to parse config file objectclass definitions */
/* $OpenLDAP: pkg/ldap/servers/slapd/schemaparse.c,v 1.53 2002/01/19 03:50:26 hyc Exp $ */
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
#include "ldap_schema.h"

int	global_schemacheck = 1; /* schemacheck ON is default */

static void		oc_usage(void); 
static void		at_usage(void);

static char *const err2text[SLAP_SCHERR_LAST+1] = {
	"Success",
	"Out of memory",
	"ObjectClass not found",
	"ObjectClass inappropriate SUPerior",
	"AttributeType not found",
	"AttributeType inappropriate USAGE",
	"Duplicate objectClass",
	"Duplicate attributeType",
	"Duplicate ldapSyntax",
	"Duplicate matchingRule",
	"OID or name required",
	"SYNTAX or SUPerior required",
	"MatchingRule not found",
	"Syntax not found",
	"Syntax required",
	"Qualifier not supported",
	"Invalid NAME",
	"OID could not be expanded"
};

char *
scherr2str(int code)
{
	if ( code < 0 || code >= (sizeof(err2text)/sizeof(char *)) ) {
		return "Unknown error";
	} else {
		return err2text[code];
	}
}

/* check schema descr validity */
int slap_valid_descr( const char *descr )
{
	int i=0;

	if( !DESC_LEADCHAR( descr[i] ) ) {
		return 0;
	}

	while( descr[++i] ) {
		if( !DESC_CHAR( descr[i] ) ) {
			return 0;
		}
	}

	return 1;
}


/* OID Macros */

/* String compare with delimiter check. Return 0 if not
 * matched, otherwise return length matched.
 */
int
dscompare(const char *s1, const char *s2, char delim)
{
	const char *orig = s1;
	while (*s1++ == *s2++)
		if (!s1[-1]) break;
	--s1;
	--s2;
	if (!*s1 && (!*s2 || *s2 == delim))
		return s1 - orig;
	return 0;
}


int
parse_oc(
    const char	*fname,
    int		lineno,
    char	*line,
    char	**argv
)
{
	LDAPObjectClass *oc;
	int		code;
	const char	*err;

	oc = ldap_str2objectclass(line, &code, &err, LDAP_SCHEMA_ALLOW_ALL );
	if ( !oc ) {
		fprintf( stderr, "%s: line %d: %s before %s\n",
			 fname, lineno, ldap_scherr2str(code), err );
		oc_usage();
		return 1;
	}

	if ( oc->oc_oid == NULL ) {
		fprintf( stderr,
			"%s: line %d: objectclass has no OID\n",
			fname, lineno );
		oc_usage();
		return 1;
	}

	code = oc_add(oc,&err);
	if ( code ) {
		fprintf( stderr, "%s: line %d: %s: \"%s\"\n",
			 fname, lineno, scherr2str(code), err);
		return 1;
	}

	ldap_memfree(oc);
	return 0;
}

static void
oc_usage( void )
{
	fprintf( stderr,
		"ObjectClassDescription = \"(\" whsp\n"
		"  numericoid whsp                 ; ObjectClass identifier\n"
		"  [ \"NAME\" qdescrs ]\n"
		"  [ \"DESC\" qdstring ]\n"
		"  [ \"OBSOLETE\" whsp ]\n"
		"  [ \"SUP\" oids ]                ; Superior ObjectClasses\n"
		"  [ ( \"ABSTRACT\" / \"STRUCTURAL\" / \"AUXILIARY\" ) whsp ]\n"
		"                                  ; default structural\n"
		"  [ \"MUST\" oids ]               ; AttributeTypes\n"
		"  [ \"MAY\" oids ]                ; AttributeTypes\n"
		"  whsp \")\"\n" );
}


static void
at_usage( void )
{
	fprintf( stderr,
		"AttributeTypeDescription = \"(\" whsp\n"
		"  numericoid whsp      ; AttributeType identifier\n"
		"  [ \"NAME\" qdescrs ]             ; name used in AttributeType\n"
		"  [ \"DESC\" qdstring ]            ; description\n"
		"  [ \"OBSOLETE\" whsp ]\n"
		"  [ \"SUP\" woid ]                 ; derived from this other\n"
		"                                   ; AttributeType\n"
		"  [ \"EQUALITY\" woid ]            ; Matching Rule name\n"
		"  [ \"ORDERING\" woid ]            ; Matching Rule name\n"
		"  [ \"SUBSTR\" woid ]              ; Matching Rule name\n"
		"  [ \"SYNTAX\" whsp noidlen whsp ] ; see section 4.3\n"
		"  [ \"SINGLE-VALUE\" whsp ]        ; default multi-valued\n"
		"  [ \"COLLECTIVE\" whsp ]          ; default not collective\n"
		"  [ \"NO-USER-MODIFICATION\" whsp ]; default user modifiable\n"
		"  [ \"USAGE\" whsp AttributeUsage ]; default userApplications\n"
		"                                   ; userApplications\n"
		"                                   ; directoryOperation\n"
		"                                   ; distributedOperation\n"
		"                                   ; dSAOperation\n"
		"  whsp \")\"\n");
}

int
parse_at(
    const char	*fname,
    int		lineno,
    char	*line,
    char	**argv
)
{
	LDAPAttributeType *at;
	int		code;
	const char	*err;

	at = ldap_str2attributetype( line, &code, &err, LDAP_SCHEMA_ALLOW_ALL );
	if ( !at ) {
		fprintf( stderr, "%s: line %d: %s before %s\n",
			 fname, lineno, ldap_scherr2str(code), err );
		at_usage();
		return 1;
	}

	if ( at->at_oid == NULL ) {
		fprintf( stderr,
			"%s: line %d: attributeType has no OID\n",
			fname, lineno );
		at_usage();
		return 1;
	}

	/* operational attributes should be defined internally */
	if ( at->at_usage ) {
		fprintf( stderr, "%s: line %d: attribute type \"%s\" is operational\n",
			 fname, lineno, at->at_oid );
		return 1;
	}

	code = at_add(at,&err);
	if ( code ) {
		fprintf( stderr, "%s: line %d: %s: \"%s\"\n",
			 fname, lineno, scherr2str(code), err);
		return 1;
	}
	ldap_memfree(at);
	return 0;
}
