/* schema_check.c - routines to enforce schema definitions */
/* $OpenLDAP: pkg/ldap/servers/slapd/schema_check.c,v 1.50.2.1 2002/02/18 19:40:38 kurt Exp $ */
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
#include "ldap_pvt.h"

static char * oc_check_required(
	Entry *e,
	ObjectClass *oc,
	struct berval *ocname );

/*
 * entry_schema_check - check that entry e conforms to the schema required
 * by its object class(es).
 *
 * returns 0 if so, non-zero otherwise.
 */

int
entry_schema_check( 
	Backend *be,
	Entry *e,
	Attribute *oldattrs,
	const char** text,
	char *textbuf, size_t textlen )
{
	Attribute	*a, *asc, *aoc;
	ObjectClass *sc, *oc;
	int	rc, i;
	struct berval nsc;
	AttributeDescription *ad_structuralObjectClass
		= slap_schema.si_ad_structuralObjectClass;
	AttributeDescription *ad_objectClass
		= slap_schema.si_ad_objectClass;
	int extensible = 0;
	int subentry = is_entry_subentry( e );
	int collectiveSubentry = 0;

#if 0
	if( subentry) collectiveSubentry = is_entry_collectiveAttributeSubentry( e );
#endif

	*text = textbuf;

	/* misc attribute checks */
	for ( a = e->e_attrs; a != NULL; a = a->a_next ) {
		const char *type = a->a_desc->ad_cname.bv_val;

		/* there should be at least one value */
		assert( a->a_vals );
		assert( a->a_vals[0].bv_val != NULL ); 

		if( a->a_desc->ad_type->sat_check ) {
			int rc = (a->a_desc->ad_type->sat_check)(
				be, e, a, text, textbuf, textlen );
			if( rc != LDAP_SUCCESS ) {
				return rc;
			}
		}

		if( !collectiveSubentry && is_at_collective( a->a_desc->ad_type ) ) {
			snprintf( textbuf, textlen,
				"'%s' can only appear in collectiveAttributeSubentry",
				type );
			return LDAP_OBJECT_CLASS_VIOLATION;
		}

		/* if single value type, check for multiple values */
		if( is_at_single_value( a->a_desc->ad_type ) &&
			a->a_vals[1].bv_val != NULL )
		{
			snprintf( textbuf, textlen, 
				"attribute '%s' cannot have multiple values",
				type );

#ifdef NEW_LOGGING
			LDAP_LOG(( "schema", LDAP_LEVEL_INFO,
				"entry_schema_check: dn=\"%s\" %s\n",
				e->e_dn, textbuf ));
#else
			Debug( LDAP_DEBUG_ANY,
			    "Entry (%s), %s\n",
			    e->e_dn, textbuf, 0 );
#endif

			return LDAP_CONSTRAINT_VIOLATION;
		}
	}

	/* it's a REALLY bad idea to disable schema checks */
	if( !global_schemacheck ) return LDAP_SUCCESS;

	/* find the object class attribute - could error out here */
	asc = attr_find( e->e_attrs, ad_structuralObjectClass );
	if ( asc == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "schema", LDAP_LEVEL_INFO, "entry_schema_check: "
			"No structuralObjectClass for entry (%s)\n",
			e->e_dn ));
#else
		Debug( LDAP_DEBUG_ANY,
			"No structuralObjectClass for entry (%s)\n",
		    e->e_dn, 0, 0 );
#endif

		*text = "no structuralObjectClass operational attribute";
		return LDAP_OBJECT_CLASS_VIOLATION;
	}

	assert( asc->a_vals != NULL );
	assert( asc->a_vals[0].bv_val != NULL );
	assert( asc->a_vals[1].bv_val == NULL );

	sc = oc_bvfind( &asc->a_vals[0] );
	if( sc == NULL ) {
		snprintf( textbuf, textlen, 
			"unrecognized structuralObjectClass '%s'",
			asc->a_vals[0].bv_val );

#ifdef NEW_LOGGING
		LDAP_LOG(( "schema", LDAP_LEVEL_INFO,
			"entry_schema_check: dn (%s), %s\n",
			e->e_dn, textbuf ));
#else
		Debug( LDAP_DEBUG_ANY,
			"entry_check_schema(%s): %s\n",
			e->e_dn, textbuf, 0 );
#endif

		return LDAP_OBJECT_CLASS_VIOLATION;
	}

	if( sc->soc_kind != LDAP_SCHEMA_STRUCTURAL ) {
		snprintf( textbuf, textlen, 
			"structuralObjectClass '%s' is not STRUCTURAL",
			asc->a_vals[0].bv_val );

#ifdef NEW_LOGGING
		LDAP_LOG(( "schema", LDAP_LEVEL_INFO,
			"entry_schema_check: dn (%s), %s\n",
			e->e_dn, textbuf ));
#else
		Debug( LDAP_DEBUG_ANY,
			"entry_check_schema(%s): %s\n",
			e->e_dn, textbuf, 0 );
#endif

		return LDAP_OBJECT_CLASS_VIOLATION;
	}

	/* find the object class attribute */
	aoc = attr_find( e->e_attrs, ad_objectClass );
	if ( aoc == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "schema", LDAP_LEVEL_INFO,
			"entry_schema_check: No objectClass for entry (%s).\n"
			e->e_dn ));
#else
		Debug( LDAP_DEBUG_ANY, "No objectClass for entry (%s)\n",
		    e->e_dn, 0, 0 );
#endif

		*text = "no objectClass attribute";
		return LDAP_OBJECT_CLASS_VIOLATION;
	}

	assert( aoc->a_vals != NULL );
	assert( aoc->a_vals[0].bv_val != NULL );

	rc = structural_class( aoc->a_vals, &nsc, &oc, text, textbuf, textlen );
	if( rc != LDAP_SUCCESS ) {
		return rc;
	} else if ( nsc.bv_len == 0 ) {
		return LDAP_OBJECT_CLASS_VIOLATION;
	}

	*text = textbuf;

	if ( oc == NULL ) {
		snprintf( textbuf, textlen, 
			"unrecognized objectClass '%s'",
			aoc->a_vals[0].bv_val );
		return LDAP_OBJECT_CLASS_VIOLATION;

	} else if ( sc != oc ) {
		snprintf( textbuf, textlen, 
			"structuralObjectClass modification from '%s' to '%s' not allowed",
			asc->a_vals[0].bv_val, nsc.bv_val );
		return LDAP_NO_OBJECT_CLASS_MODS;
	}

	/* check that the entry has required attrs for each oc */
	for ( i = 0; aoc->a_vals[i].bv_val != NULL; i++ ) {
		if ( (oc = oc_bvfind( &aoc->a_vals[i] )) == NULL ) {
			snprintf( textbuf, textlen, 
				"unrecognized objectClass '%s'",
				aoc->a_vals[i].bv_val );

#ifdef NEW_LOGGING
			LDAP_LOG(( "schema", LDAP_LEVEL_INFO,
				"entry_schema_check: dn (%s), %s\n",
				e->e_dn, textbuf ));
#else
			Debug( LDAP_DEBUG_ANY,
				"entry_check_schema(%s): %s\n",
				e->e_dn, textbuf, 0 );
#endif

			return LDAP_OBJECT_CLASS_VIOLATION;
		}

		if ( oc->soc_check ) {
			int rc = (oc->soc_check)( be, e, oc,
				text, textbuf, textlen );
			if( rc != LDAP_SUCCESS ) {
				return rc;
			}
		}

		if ( oc->soc_kind == LDAP_SCHEMA_ABSTRACT ) {
			/* object class is abstract */
			if ( oc != slap_schema.si_oc_top &&
				!is_object_subclass( oc, sc ))
			{
				int j;
				ObjectClass *xc = NULL;
				for( j=0; aoc->a_vals[j].bv_val; j++ ) {
					if( i != j ) {
						xc = oc_bvfind( &aoc->a_vals[i] );
						if( xc == NULL ) {
							snprintf( textbuf, textlen, 
								"unrecognized objectClass '%s'",
								aoc->a_vals[i].bv_val );

#ifdef NEW_LOGGING
							LDAP_LOG(( "schema", LDAP_LEVEL_INFO,
								"entry_schema_check: dn (%s), %s\n",
								e->e_dn, textbuf ));
#else
							Debug( LDAP_DEBUG_ANY,
								"entry_check_schema(%s): %s\n",
								e->e_dn, textbuf, 0 );
#endif

							return LDAP_OBJECT_CLASS_VIOLATION;
						}

						/* since we previous check against the
						 * structural object of this entry, the
						 * abstract class must be a (direct or indirect)
						 * superclass of one of the auxiliary classes of
						 * the entry.
						 */
						if ( xc->soc_kind == LDAP_SCHEMA_AUXILIARY &&
							is_object_subclass( oc, xc ) )
						{
							break;;
						}

						xc = NULL;
					}
				}

				if( xc == NULL ) {
					snprintf( textbuf, textlen, "instanstantiation of "
						"abstract objectClass '%s' not allowed",
						aoc->a_vals[i].bv_val );

#ifdef NEW_LOGGING
					LDAP_LOG(( "schema", LDAP_LEVEL_INFO,
						"entry_schema_check: dn (%s), %s\n",
						e->e_dn, textbuf ));
#else
					Debug( LDAP_DEBUG_ANY,
						"entry_check_schema(%s): %s\n",
						e->e_dn, textbuf, 0 );
#endif

					return LDAP_OBJECT_CLASS_VIOLATION;
				}
			}

		} else if ( oc->soc_kind != LDAP_SCHEMA_STRUCTURAL || oc == sc ) {
			char *s = oc_check_required( e, oc, &aoc->a_vals[i] );

			if (s != NULL) {
				snprintf( textbuf, textlen, 
					"object class '%s' requires attribute '%s'",
					aoc->a_vals[i].bv_val, s );

#ifdef NEW_LOGGING
				LDAP_LOG(( "schema", LDAP_LEVEL_INFO,
					"entry_schema_check: dn=\"%s\" %s",
					e->e_dn, textbuf ));
#else
				Debug( LDAP_DEBUG_ANY,
					"Entry (%s): %s\n",
					e->e_dn, textbuf, 0 );
#endif

				return LDAP_OBJECT_CLASS_VIOLATION;
			}

			if( oc == slap_schema.si_oc_extensibleObject ) {
				extensible=1;
			}
		}
	}

	if( extensible ) {
		return LDAP_SUCCESS;
	}

	/* check that each attr in the entry is allowed by some oc */
	for ( a = e->e_attrs; a != NULL; a = a->a_next ) {
		int ret = oc_check_allowed( a->a_desc->ad_type, aoc->a_vals, sc );
		if ( ret != LDAP_SUCCESS ) {
			char *type = a->a_desc->ad_cname.bv_val;

			snprintf( textbuf, textlen, 
				"attribute '%s' not allowed",
				type );

#ifdef NEW_LOGGING
			LDAP_LOG(( "schema", LDAP_LEVEL_INFO,
				"entry_schema_check: dn=\"%s\" %s\n",
				e->e_dn, textbuf ));
#else
			Debug( LDAP_DEBUG_ANY,
			    "Entry (%s), %s\n",
			    e->e_dn, textbuf, 0 );
#endif

			return ret;
		}
	}

	return LDAP_SUCCESS;
}

static char *
oc_check_required(
	Entry *e,
	ObjectClass *oc,
	struct berval *ocname )
{
	AttributeType	*at;
	int		i;
	Attribute	*a;

#ifdef NEW_LOGGING
	LDAP_LOG(( "schema", LDAP_LEVEL_ENTRY,
		"oc_check_required: dn (%s), objectClass \"%s\"\n",
	e->e_dn, ocname->bv_val ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"oc_check_required entry (%s), objectClass \"%s\"\n",
		e->e_dn, ocname->bv_val, 0 );
#endif


	/* check for empty oc_required */
	if(oc->soc_required == NULL) {
		return NULL;
	}

	/* for each required attribute */
	for ( i = 0; oc->soc_required[i] != NULL; i++ ) {
		at = oc->soc_required[i];
		/* see if it's in the entry */
		for ( a = e->e_attrs; a != NULL; a = a->a_next ) {
			if( a->a_desc->ad_type == at ) {
				break;
			}
		}
		/* not there => schema violation */
		if ( a == NULL ) {
			return at->sat_cname.bv_val;
		}
	}

	return( NULL );
}

int oc_check_allowed(
	AttributeType *at,
	BerVarray ocl,
	ObjectClass *sc )
{
	int		i, j;

#ifdef NEW_LOGGING
	LDAP_LOG(( "schema", LDAP_LEVEL_ENTRY,
		"oc_check_allowed: type \"%s\"\n", at->sat_cname.bv_val ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"oc_check_allowed type \"%s\"\n",
		at->sat_cname.bv_val, 0, 0 );
#endif

	/* always allow objectClass attribute */
	if ( strcasecmp( at->sat_cname.bv_val, "objectClass" ) == 0 ) {
		return LDAP_SUCCESS;
	}

	/*
	 * All operational attributions are allowed by schema rules.
	 */
	if( is_at_operational(at) ) {
		return LDAP_SUCCESS;
	}

	/* check to see if its allowed by the structuralObjectClass */
	if( sc ) {
		/* does it require the type? */
		for ( j = 0; sc->soc_required != NULL && 
			sc->soc_required[j] != NULL; j++ )
		{
			if( at == sc->soc_required[j] ) {
				return LDAP_SUCCESS;
			}
		}

		/* does it allow the type? */
		for ( j = 0; sc->soc_allowed != NULL && 
			sc->soc_allowed[j] != NULL; j++ )
		{
			if( at == sc->soc_allowed[j] ) {
				return LDAP_SUCCESS;
			}
		}
	}

	/* check that the type appears as req or opt in at least one oc */
	for ( i = 0; ocl[i].bv_val != NULL; i++ ) {
		/* if we know about the oc */
		ObjectClass	*oc = oc_bvfind( &ocl[i] );
		if ( oc != NULL && oc->soc_kind != LDAP_SCHEMA_ABSTRACT &&
			( sc == NULL || oc->soc_kind == LDAP_SCHEMA_AUXILIARY ))
		{
			/* does it require the type? */
			for ( j = 0; oc->soc_required != NULL && 
				oc->soc_required[j] != NULL; j++ )
			{
				if( at == oc->soc_required[j] ) {
					return LDAP_SUCCESS;
				}
			}
			/* does it allow the type? */
			for ( j = 0; oc->soc_allowed != NULL && 
				oc->soc_allowed[j] != NULL; j++ )
			{
				if( at == oc->soc_allowed[j] ) {
					return LDAP_SUCCESS;
				}
			}
		}
	}

	/* not allowed by any oc */
	return LDAP_OBJECT_CLASS_VIOLATION;
}

/*
 * Determine the structural object class from a set of OIDs
 */
int structural_class(
	BerVarray ocs,
	struct berval *scbv,
	ObjectClass **scp,
	const char **text,
	char *textbuf, size_t textlen )
{
	int i;
	ObjectClass *oc;
	ObjectClass *sc = NULL;
	int scn = -1;

	*text = "structural_class: internal error";
	scbv->bv_len = 0;

	for( i=0; ocs[i].bv_val; i++ ) {
		oc = oc_bvfind( &ocs[i] );

		if( oc == NULL ) {
			snprintf( textbuf, textlen,
				"unrecognized objectClass '%s'",
				ocs[i].bv_val );
			*text = textbuf;
			return LDAP_OBJECT_CLASS_VIOLATION;
		}

		if( oc->soc_kind == LDAP_SCHEMA_STRUCTURAL ) {
			if( sc == NULL || is_object_subclass( sc, oc ) ) {
				sc = oc;
				scn = i;

			} else if ( !is_object_subclass( oc, sc ) ) {
				int j;
				ObjectClass *xc = NULL;

				/* find common superior */
				for( j=i+1; ocs[j].bv_val; j++ ) {
					xc = oc_bvfind( &ocs[j] );

					if( xc == NULL ) {
						snprintf( textbuf, textlen,
							"unrecognized objectClass '%s'",
							ocs[i].bv_val );
						*text = textbuf;
						return LDAP_OBJECT_CLASS_VIOLATION;
					}

					if( xc->soc_kind != LDAP_SCHEMA_STRUCTURAL ) {
						xc = NULL;
						continue;
					}

					if( is_object_subclass( sc, xc ) &&
						is_object_subclass( oc, xc ) )
					{
						/* found common subclass */
						break;
					}

					xc = NULL;
				}

				if( xc == NULL ) {
					/* no common subclass */
					snprintf( textbuf, textlen,
						"invalid structural object class chain (%s/%s)",
						ocs[scn].bv_val, ocs[i].bv_val );
					*text = textbuf;
					return LDAP_OBJECT_CLASS_VIOLATION;
				}
			}
		}
	}

	if( scp )
		*scp = sc;

	if( sc == NULL ) {
		*text = "no structural object classes provided";
		return LDAP_OBJECT_CLASS_VIOLATION;
	}

	*scbv = ocs[scn];
	return LDAP_SUCCESS;
}

/*
 * Return structural object class from list of modifications
 */
int mods_structural_class(
	Modifications *mods,
	struct berval *sc,
	const char **text,
	char *textbuf, size_t textlen )
{
	Modifications *ocmod = NULL;

	for( ; mods != NULL; mods = mods->sml_next ) {
		if( mods->sml_desc == slap_schema.si_ad_objectClass ) {
			if( ocmod != NULL ) {
				*text = "entry has multiple objectClass attributes";
				return LDAP_OBJECT_CLASS_VIOLATION;
			}
			ocmod = mods;
		}
	}

	if( ocmod == NULL ) {
		*text = "entry has no objectClass attribute";
		return LDAP_OBJECT_CLASS_VIOLATION;
	}

	if( ocmod->sml_bvalues == NULL || ocmod->sml_bvalues[0].bv_val == NULL ) {
		*text = "objectClass attribute has no values";
		return LDAP_OBJECT_CLASS_VIOLATION;
	}

	return structural_class( ocmod->sml_bvalues, sc, NULL,
		text, textbuf, textlen );
}
