/* $OpenLDAP: pkg/ldap/servers/slapd/at.c,v 1.38.2.1 2002/02/18 19:16:54 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/* at.c - routines for dealing with attribute types */

#include "portable.h"

#include <stdio.h>

#include <ac/ctype.h>
#include <ac/errno.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>

#include "ldap_pvt.h"
#include "slap.h"


int is_at_syntax(
	AttributeType *at,
	const char *oid )
{
	for( ; at != NULL; at = at->sat_sup ) {
		if( at->sat_syntax_oid ) {
			return ( strcmp( at->sat_syntax_oid, oid ) == 0 );
		}
	}

	return 0;
}

int is_at_subtype(
	AttributeType *sub,
	AttributeType *sup )
{
	for( ; sub != NULL; sub = sub->sat_sup ) {
		if( sub == sup ) return 1;
	}

	return 0;
}

struct aindexrec {
	struct berval	air_name;
	AttributeType	*air_at;
};

static Avlnode	*attr_index = NULL;
static AttributeType *attr_list = NULL;

static int
attr_index_cmp(
    struct aindexrec	*air1,
    struct aindexrec	*air2
)
{
	int i = air1->air_name.bv_len - air2->air_name.bv_len;
	if (i)
		return i;
	return (strcasecmp( air1->air_name.bv_val, air2->air_name.bv_val ));
}

static int
attr_index_name_cmp(
    struct berval	*type,
    struct aindexrec	*air
)
{
	int i = type->bv_len - air->air_name.bv_len;
	if (i)
		return i;
	return (strncasecmp( type->bv_val, air->air_name.bv_val,
		type->bv_len ));
}

AttributeType *
at_find(
    const char		*name
)
{
	struct berval bv;

	bv.bv_val = (char *)name;
	bv.bv_len = strlen( name );

	return at_bvfind( &bv );
}

AttributeType *
at_bvfind(
    struct berval	*name
)
{
	struct aindexrec *air;

	air = (struct aindexrec *) avl_find( attr_index, name,
            (AVL_CMP) attr_index_name_cmp );

	return air != NULL ? air->air_at : NULL;
}

int
at_append_to_list(
    AttributeType	*sat,
    AttributeType	***listp
)
{
	AttributeType	**list;
	AttributeType	**list1;
	int		size;

	list = *listp;
	if ( !list ) {
		size = 2;
		list = ch_calloc(size, sizeof(AttributeType *));
		if ( !list ) {
			return -1;
		}
	} else {
		size = 0;
		list1 = *listp;
		while ( *list1 ) {
			size++;
			list1++;
		}
		size += 2;
		list1 = ch_realloc(list, size*sizeof(AttributeType *));
		if ( !list1 ) {
			return -1;
		}
		list = list1;
	}
	list[size-2] = sat;
	list[size-1] = NULL;
	*listp = list;
	return 0;
}

int
at_delete_from_list(
    int			pos,
    AttributeType	***listp
)
{
	AttributeType	**list;
	AttributeType	**list1;
	int		i;
	int		j;

	if ( pos < 0 ) {
		return -2;
	}
	list = *listp;
	for ( i=0; list[i]; i++ )
		;
	if ( pos >= i ) {
		return -2;
	}
	for ( i=pos, j=pos+1; list[j]; i++, j++ ) {
		list[i] = list[j];
	}
	list[i] = NULL;
	/* Tell the runtime this can be shrinked */
	list1 = ch_realloc(list, (i+1)*sizeof(AttributeType **));
	if ( !list1 ) {
		return -1;
	}
	*listp = list1;
	return 0;
}

int
at_find_in_list(
    AttributeType	*sat,
    AttributeType	**list
)
{
	int	i;

	if ( !list ) {
		return -1;
	}
	for ( i=0; list[i]; i++ ) {
		if ( sat == list[i] ) {
			return i;
		}
	}
	return -1;
}

void
at_destroy( void )
{
	AttributeType *a, *n;
	avl_free(attr_index, ldap_memfree);

	for (a=attr_list; a; a=n) {
		n = a->sat_next;
		if (a->sat_subtypes) ldap_memfree(a->sat_subtypes);
		ad_destroy(a->sat_ad);
		ldap_pvt_thread_mutex_destroy(&a->sat_ad_mutex);
		ldap_attributetype_free((LDAPAttributeType *)a);
	}
	if ( slap_schema.si_at_undefined )
		ad_destroy(slap_schema.si_at_undefined->sat_ad);
}

static int
at_insert(
    AttributeType	*sat,
    const char		**err
)
{
	AttributeType		**atp;
	struct aindexrec	*air;
	char			**names;

	atp = &attr_list;
	while ( *atp != NULL ) {
		atp = &(*atp)->sat_next;
	}
	*atp = sat;

	if ( sat->sat_oid ) {
		air = (struct aindexrec *)
			ch_calloc( 1, sizeof(struct aindexrec) );
		air->air_name.bv_val = sat->sat_oid;
		air->air_name.bv_len = strlen(sat->sat_oid);
		air->air_at = sat;
		if ( avl_insert( &attr_index, (caddr_t) air,
				 (AVL_CMP) attr_index_cmp,
				 (AVL_DUP) avl_dup_error ) ) {
			*err = sat->sat_oid;
			ldap_memfree(air);
			return SLAP_SCHERR_DUP_ATTR;
		}
		/* FIX: temporal consistency check */
		at_bvfind(&air->air_name);
	}

	if ( (names = sat->sat_names) ) {
		while ( *names ) {
			air = (struct aindexrec *)
				ch_calloc( 1, sizeof(struct aindexrec) );
			air->air_name.bv_val = *names;
			air->air_name.bv_len = strlen(*names);
			air->air_at = sat;
			if ( avl_insert( &attr_index, (caddr_t) air,
					 (AVL_CMP) attr_index_cmp,
					 (AVL_DUP) avl_dup_error ) ) {
				*err = *names;
				ldap_memfree(air);
				return SLAP_SCHERR_DUP_ATTR;
			}
			/* FIX: temporal consistency check */
			at_bvfind(&air->air_name);
			names++;
		}
	}

	return 0;
}

int
at_add(
    LDAPAttributeType	*at,
    const char		**err
)
{
	AttributeType	*sat;
	MatchingRule	*mr;
	Syntax		*syn;
	int		code;
	char	*cname;
	char	*oid;

	if ( !OID_LEADCHAR( at->at_oid[0] )) {
		/* Expand OID macros */
		oid = oidm_find( at->at_oid );
		if ( !oid ) {
			*err = at->at_oid;
			return SLAP_SCHERR_OIDM;
		}
		if ( oid != at->at_oid ) {
			ldap_memfree( at->at_oid );
			at->at_oid = oid;
		}
	}

	if ( at->at_syntax_oid && !OID_LEADCHAR( at->at_syntax_oid[0] )) {
		/* Expand OID macros */
		oid = oidm_find( at->at_syntax_oid );
		if ( !oid ) {
			*err = at->at_syntax_oid;
			return SLAP_SCHERR_OIDM;
		}
		if ( oid != at->at_syntax_oid ) {
			ldap_memfree( at->at_syntax_oid );
			at->at_syntax_oid = oid;
		}

	}

	if ( at->at_names && at->at_names[0] ) {
		int i;

		for( i=0; at->at_names[i]; i++ ) {
			if( !slap_valid_descr( at->at_names[i] ) ) {
				*err = at->at_names[i];
				return SLAP_SCHERR_BAD_DESCR;
			}
		}

		cname = at->at_names[0];

	} else if ( at->at_oid ) {
		cname = at->at_oid;

	} else {
		*err = "";
		return SLAP_SCHERR_ATTR_INCOMPLETE;
	}

	*err = cname;

	if ( !at->at_usage && at->at_no_user_mod ) {
		/* user attribute must be modifable */
		return SLAP_SCHERR_ATTR_BAD_USAGE;
	}

	if ( at->at_collective ) {
		if( at->at_usage ) {
			/* collective attributes cannot be operational */
			return SLAP_SCHERR_ATTR_BAD_USAGE;
		}

		if( at->at_single_value ) {
			/* collective attributes cannot be single-valued */
			return SLAP_SCHERR_ATTR_BAD_USAGE;
		}

		/* collective attributes not supported */
		return SLAP_SCHERR_NOT_SUPPORTED;
	}

	sat = (AttributeType *) ch_calloc( 1, sizeof(AttributeType) );
	AC_MEMCPY( &sat->sat_atype, at, sizeof(LDAPAttributeType));

	sat->sat_cname.bv_val = cname;
	sat->sat_cname.bv_len = strlen( cname );
	ldap_pvt_thread_mutex_init(&sat->sat_ad_mutex);

	if ( at->at_sup_oid ) {
		AttributeType *supsat = at_find(at->at_sup_oid);

		if ( (supsat == NULL ) ) {
			*err = at->at_sup_oid;
			return SLAP_SCHERR_ATTR_NOT_FOUND;
		}

		sat->sat_sup = supsat;

		if ( at_append_to_list(sat, &supsat->sat_subtypes) ) {
			return SLAP_SCHERR_OUTOFMEM;
		}

		if ( sat->sat_usage != supsat->sat_usage ) {
			/* subtypes must have same usage as their SUP */
			return SLAP_SCHERR_ATTR_BAD_USAGE;
		}
	}

	/*
	 * Inherit definitions from superiors.  We only check the
	 * direct superior since that one has already inherited from
	 * its own superiorss
	 */
	if ( sat->sat_sup ) {
		sat->sat_syntax = sat->sat_sup->sat_syntax;
		sat->sat_equality = sat->sat_sup->sat_equality;
		sat->sat_approx = sat->sat_sup->sat_approx;
		sat->sat_ordering = sat->sat_sup->sat_ordering;
		sat->sat_substr = sat->sat_sup->sat_substr;
	}

	if ( at->at_syntax_oid ) {
		if ( (syn = syn_find(sat->sat_syntax_oid)) ) {
			sat->sat_syntax = syn;
		} else {
			*err = sat->sat_syntax_oid;
			return SLAP_SCHERR_SYN_NOT_FOUND;
		}


	} else if ( sat->sat_syntax == NULL ) {
		return SLAP_SCHERR_ATTR_INCOMPLETE;
	}

	if ( sat->sat_equality_oid ) {
		if ( (mr = mr_find(sat->sat_equality_oid)) ) {
			sat->sat_equality = mr;
			sat->sat_approx = mr->smr_associated;
		} else {
			*err = sat->sat_equality_oid;
			return SLAP_SCHERR_MR_NOT_FOUND;
		}

	}

	if ( sat->sat_ordering_oid ) {
		if ( (mr = mr_find(sat->sat_ordering_oid)) ) {
			sat->sat_ordering = mr;
		} else {
			*err = sat->sat_ordering_oid;
			return SLAP_SCHERR_MR_NOT_FOUND;
		}
	}

	if ( sat->sat_substr_oid ) {
		if ( (mr = mr_find(sat->sat_substr_oid)) ) {
			sat->sat_substr = mr;
		} else {
			*err = sat->sat_substr_oid;
			return SLAP_SCHERR_MR_NOT_FOUND;
		}
	}

	code = at_insert(sat,err);
	return code;
}

#ifdef LDAP_DEBUG
static int
at_index_printnode( struct aindexrec *air )
{

	printf("%s = %s\n",
		air->air_name.bv_val,
		ldap_attributetype2str(&air->air_at->sat_atype) );
	return( 0 );
}

static void
at_index_print( void )
{
	printf("Printing attribute type index:\n");
	(void) avl_apply( attr_index, (AVL_APPLY) at_index_printnode,
		0, -1, AVL_INORDER );
}
#endif

#if defined( SLAPD_SCHEMA_DN )
int
at_schema_info( Entry *e )
{
	struct berval	vals[2];
	AttributeType	*at;

	AttributeDescription *ad_attributeTypes = slap_schema.si_ad_attributeTypes;

	vals[1].bv_val = NULL;

	for ( at = attr_list; at; at = at->sat_next ) {
		if ( ldap_attributetype2bv( &at->sat_atype, vals ) == NULL ) {
			return -1;
		}
#if 0
		Debug( LDAP_DEBUG_TRACE, "Merging at [%ld] %s\n",
		       (long) vals[0].bv_len, vals[0].bv_val, 0 );
#endif
		attr_merge( e, ad_attributeTypes, vals );
		ldap_memfree( vals[0].bv_val );
	}
	return 0;
}
#endif
