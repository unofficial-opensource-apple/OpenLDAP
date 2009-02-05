/* map.c - ldap backend mapping routines */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-ldap/map.c,v 1.16.2.5 2004/03/17 20:59:58 kurt Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1998-2004 The OpenLDAP Foundation.
 * Portions Copyright 1999-2003 Howard Chu.
 * Portions Copyright 2000-2003 Pierangelo Masarati.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by the Howard Chu for inclusion
 * in OpenLDAP Software and subsequently enhanced by Pierangelo
 * Masarati.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "back-ldap.h"

#undef ldap_debug	/* silence a warning in ldap-int.h */
#include "../../../libraries/libldap/ldap-int.h"

int
mapping_cmp ( const void *c1, const void *c2 )
{
	struct ldapmapping *map1 = (struct ldapmapping *)c1;
	struct ldapmapping *map2 = (struct ldapmapping *)c2;
	int rc = map1->src.bv_len - map2->src.bv_len;
	if (rc) return rc;
	return ( strcasecmp(map1->src.bv_val, map2->src.bv_val) );
}

int
mapping_dup ( void *c1, void *c2 )
{
	struct ldapmapping *map1 = (struct ldapmapping *)c1;
	struct ldapmapping *map2 = (struct ldapmapping *)c2;

	return( ( strcasecmp(map1->src.bv_val, map2->src.bv_val) == 0 ) ? -1 : 0 );
}

void
ldap_back_map_init ( struct ldapmap *lm, struct ldapmapping **m )
{
	struct ldapmapping *mapping;

	assert( m );

	*m = NULL;
	
	mapping = (struct ldapmapping *)ch_calloc( 2, 
			sizeof( struct ldapmapping ) );
	if ( mapping == NULL ) {
		return;
	}

	ber_str2bv( "objectclass", sizeof("objectclass")-1, 1, &mapping->src);
	ber_dupbv( &mapping->dst, &mapping->src );
	mapping[1].src = mapping->src;
	mapping[1].dst = mapping->dst;

	avl_insert( &lm->map, (caddr_t)mapping, 
			mapping_cmp, mapping_dup );
	avl_insert( &lm->remap, (caddr_t)&mapping[1], 
			mapping_cmp, mapping_dup );
	*m = mapping;
}

void
ldap_back_map ( struct ldapmap *map, struct berval *s, struct berval *bv,
	int remap )
{
	Avlnode *tree;
	struct ldapmapping *mapping, fmapping;

	if (remap == BACKLDAP_REMAP)
		tree = map->remap;
	else
		tree = map->map;

	bv->bv_len = 0;
	bv->bv_val = NULL;
	fmapping.src = *s;
	mapping = (struct ldapmapping *)avl_find( tree, (caddr_t)&fmapping, mapping_cmp );
	if (mapping != NULL) {
		if ( mapping->dst.bv_val )
			*bv = mapping->dst;
		return;
	}

	if (!map->drop_missing)
		*bv = *s;

	return;
}

int
ldap_back_map_attrs(
		struct ldapmap *at_map,
		AttributeName *an,
		int remap,
		char ***mapped_attrs
)
{
	int i, j;
	char **na;
	struct berval mapped;

	if (an == NULL) {
		*mapped_attrs = NULL;
		return LDAP_SUCCESS;
	}

	for (i = 0; an[i].an_name.bv_val; i++) {
		/*  */
	}

	na = (char **)ch_calloc( i + 1, sizeof(char *) );
	if (na == NULL) {
		*mapped_attrs = NULL;
		return LDAP_NO_MEMORY;
	}

	for (i = j = 0; an[i].an_name.bv_val; i++) {
		ldap_back_map(at_map, &an[i].an_name, &mapped, remap);
		if (mapped.bv_val != NULL && mapped.bv_val != '\0')
			na[j++] = mapped.bv_val;
	}
	if (j == 0 && i != 0)
		na[j++] = LDAP_NO_ATTRS;
	na[j] = NULL;

	*mapped_attrs = na;
	return LDAP_SUCCESS;
}

int
map_attr_value(
		dncookie		*dc,
		AttributeDescription 	*ad,
		struct berval		*mapped_attr,
		struct berval		*value,
		struct berval		*mapped_value,
		int			remap )
{
	struct berval		vtmp;
	int			freeval = 0;

	ldap_back_map( &dc->rwmap->rwm_at, &ad->ad_cname, mapped_attr, remap );
	if ( mapped_attr->bv_val == NULL || mapped_attr->bv_val[0] == '\0') {
		/*
		 * FIXME: are we sure we need to search oc_map if at_map fails?
		 */
		ldap_back_map( &dc->rwmap->rwm_oc, &ad->ad_cname, mapped_attr, remap );
		if ( mapped_attr->bv_val == NULL || mapped_attr->bv_val[0] == '\0' ) {
			*mapped_attr = ad->ad_cname;
		}
	}

	if ( value == NULL ) {
		return LDAP_SUCCESS;
	}

	if ( ad->ad_type->sat_syntax == slap_schema.si_syn_distinguishedName )
	{
		dncookie fdc = *dc;

#ifdef ENABLE_REWRITE
		fdc.ctx = "searchFilterAttrDN";
#endif

		switch ( ldap_back_dn_massage( &fdc, value, &vtmp ) ) {
		case LDAP_SUCCESS:
			if ( vtmp.bv_val != value->bv_val ) {
				freeval = 1;
			}
			break;
		
		case LDAP_UNWILLING_TO_PERFORM:
		case LDAP_OTHER:
			return LDAP_OTHER;
		}

	} else if ( ad == slap_schema.si_ad_objectClass || ad == slap_schema.si_ad_structuralObjectClass ) {
		ldap_back_map( &dc->rwmap->rwm_oc, value, &vtmp, remap );
		if ( vtmp.bv_val == NULL || vtmp.bv_val[0] == '\0' ) {
			vtmp = *value;
		}
		
	} else {
		vtmp = *value;
	}

	filter_escape_value( &vtmp, mapped_value );

	if ( freeval ) {
		ber_memfree( vtmp.bv_val );
	}
	
	return LDAP_SUCCESS;
}

static int
ldap_int_back_filter_map_rewrite(
		dncookie		*dc,
		Filter			*f,
		struct berval		*fstr,
		int			remap )
{
	int		i, rc;
	Filter		*p;
	struct berval	atmp;
	struct berval	vtmp;
	ber_len_t	len;

	if ( f == NULL ) {
		ber_str2bv( "(?=error)", sizeof("(?=error)")-1, 1, fstr );
		return LDAP_OTHER;
	}

	switch ( f->f_choice ) {
	case LDAP_FILTER_EQUALITY:
		rc = map_attr_value( dc, f->f_av_desc, &atmp,
					&f->f_av_value, &vtmp, remap );
		if ( rc ) {
			return rc;
		}

		fstr->bv_len = atmp.bv_len + vtmp.bv_len
			+ ( sizeof("(=)") - 1 );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s=%s)",
			atmp.bv_val, vtmp.bv_val );

		ber_memfree( vtmp.bv_val );
		break;

	case LDAP_FILTER_GE:
		rc = map_attr_value( dc, f->f_av_desc, &atmp,
					&f->f_av_value, &vtmp, remap );
		if ( rc ) {
			return rc;
		}

		fstr->bv_len = atmp.bv_len + vtmp.bv_len
			+ ( sizeof("(>=)") - 1 );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s>=%s)",
			atmp.bv_val, vtmp.bv_val );

		ber_memfree( vtmp.bv_val );
		break;

	case LDAP_FILTER_LE:
		rc = map_attr_value( dc, f->f_av_desc, &atmp,
					&f->f_av_value, &vtmp, remap );
		if ( rc ) {
			return rc;
		}

		fstr->bv_len = atmp.bv_len + vtmp.bv_len
			+ ( sizeof("(<=)") - 1 );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s<=%s)",
			atmp.bv_val, vtmp.bv_val );

		ber_memfree( vtmp.bv_val );
		break;

	case LDAP_FILTER_APPROX:
		rc = map_attr_value( dc, f->f_av_desc, &atmp,
					&f->f_av_value, &vtmp, remap );
		if ( rc ) {
			return rc;
		}

		fstr->bv_len = atmp.bv_len + vtmp.bv_len
			+ ( sizeof("(~=)") - 1 );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s~=%s)",
			atmp.bv_val, vtmp.bv_val );

		ber_memfree( vtmp.bv_val );
		break;

	case LDAP_FILTER_SUBSTRINGS:
		rc = map_attr_value( dc, f->f_sub_desc, &atmp,
					NULL, NULL, remap );
		if ( rc ) {
			return rc;
		}

		/* cannot be a DN ... */

		fstr->bv_len = atmp.bv_len + ( sizeof("(=*)") - 1 );
		fstr->bv_val = malloc( fstr->bv_len + 128 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s=*)",
			atmp.bv_val );

		if ( f->f_sub_initial.bv_val != NULL ) {
			len = fstr->bv_len;

			filter_escape_value( &f->f_sub_initial, &vtmp );

			fstr->bv_len += vtmp.bv_len;
			fstr->bv_val = ch_realloc( fstr->bv_val, fstr->bv_len + 1 );

			snprintf( &fstr->bv_val[len - 2], vtmp.bv_len + 3,
				/* "(attr=" */ "%s*)",
				vtmp.bv_val );

			ber_memfree( vtmp.bv_val );
		}

		if ( f->f_sub_any != NULL ) {
			for ( i = 0; f->f_sub_any[i].bv_val != NULL; i++ ) {
				len = fstr->bv_len;
				filter_escape_value( &f->f_sub_any[i], &vtmp );

				fstr->bv_len += vtmp.bv_len + 1;
				fstr->bv_val = ch_realloc( fstr->bv_val, fstr->bv_len + 1 );

				snprintf( &fstr->bv_val[len - 1], vtmp.bv_len + 3,
					/* "(attr=[init]*[any*]" */ "%s*)",
					vtmp.bv_val );
				ber_memfree( vtmp.bv_val );
			}
		}

		if ( f->f_sub_final.bv_val != NULL ) {
			len = fstr->bv_len;

			filter_escape_value( &f->f_sub_final, &vtmp );

			fstr->bv_len += vtmp.bv_len;
			fstr->bv_val = ch_realloc( fstr->bv_val, fstr->bv_len + 1 );

			snprintf( &fstr->bv_val[len - 1], vtmp.bv_len + 3,
				/* "(attr=[init*][any*]" */ "%s)",
				vtmp.bv_val );

			ber_memfree( vtmp.bv_val );
		}

		break;

	case LDAP_FILTER_PRESENT:
		rc = map_attr_value( dc, f->f_desc, &atmp,
					NULL, NULL, remap );
		if ( rc ) {
			return rc;
		}

		fstr->bv_len = atmp.bv_len + ( sizeof("(=*)") - 1 );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s=*)",
			atmp.bv_val );
		break;

	case LDAP_FILTER_AND:
	case LDAP_FILTER_OR:
	case LDAP_FILTER_NOT:
		fstr->bv_len = sizeof("(%)") - 1;
		fstr->bv_val = malloc( fstr->bv_len + 128 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%c)",
			f->f_choice == LDAP_FILTER_AND ? '&' :
			f->f_choice == LDAP_FILTER_OR ? '|' : '!' );

		for ( p = f->f_list; p != NULL; p = p->f_next ) {
			len = fstr->bv_len;

			rc = ldap_int_back_filter_map_rewrite( dc, p, &vtmp, remap );
			if ( rc != LDAP_SUCCESS ) {
				return rc;
			}
			
			fstr->bv_len += vtmp.bv_len;
			fstr->bv_val = ch_realloc( fstr->bv_val, fstr->bv_len + 1 );

			snprintf( &fstr->bv_val[len-1], vtmp.bv_len + 2, 
				/*"("*/ "%s)", vtmp.bv_val );

			ch_free( vtmp.bv_val );
		}

		break;

	case LDAP_FILTER_EXT: {
		if ( f->f_mr_desc ) {
			rc = map_attr_value( dc, f->f_mr_desc, &atmp,
						&f->f_mr_value, &vtmp, remap );
			if ( rc ) {
				return rc;
			}

		} else {
			atmp.bv_len = 0;
			atmp.bv_val = "";
			
			filter_escape_value( &f->f_mr_value, &vtmp );
		}
			

		fstr->bv_len = atmp.bv_len +
			( f->f_mr_dnattrs ? sizeof(":dn")-1 : 0 ) +
			( f->f_mr_rule_text.bv_len ? f->f_mr_rule_text.bv_len+1 : 0 ) +
			vtmp.bv_len + ( sizeof("(:=)") - 1 );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s%s%s%s:=%s)",
			atmp.bv_val,
			f->f_mr_dnattrs ? ":dn" : "",
			f->f_mr_rule_text.bv_len ? ":" : "",
			f->f_mr_rule_text.bv_len ? f->f_mr_rule_text.bv_val : "",
			vtmp.bv_val );
		ber_memfree( vtmp.bv_val );
		} break;

	case SLAPD_FILTER_COMPUTED:
		ber_str2bv(
			f->f_result == LDAP_COMPARE_FALSE ? "(?=false)" :
			f->f_result == LDAP_COMPARE_TRUE ? "(?=true)" :
			f->f_result == SLAPD_COMPARE_UNDEFINED ? "(?=undefined)" :
			"(?=error)",
			f->f_result == LDAP_COMPARE_FALSE ? sizeof("(?=false)")-1 :
			f->f_result == LDAP_COMPARE_TRUE ? sizeof("(?=true)")-1 :
			f->f_result == SLAPD_COMPARE_UNDEFINED ? sizeof("(?=undefined)")-1 :
			sizeof("(?=error)")-1,
			1, fstr );
		return LDAP_COMPARE_FALSE;

	default:
		ber_str2bv( "(?=unknown)", sizeof("(?=unknown)")-1, 1, fstr );
		return LDAP_COMPARE_FALSE;
	}

	return LDAP_SUCCESS;
}

int
ldap_back_filter_map_rewrite(
		dncookie		*dc,
		Filter			*f,
		struct berval		*fstr,
		int			remap )
{
	int		rc;
	dncookie 	fdc;
	struct berval	ftmp;

	rc = ldap_int_back_filter_map_rewrite( dc, f, fstr, remap );

#ifdef ENABLE_REWRITE
	if ( rc != LDAP_SUCCESS ) {
		return rc;
	}

	fdc = *dc;
	ftmp = *fstr;

	fdc.ctx = "searchFilter";

	switch ( rewrite_session( fdc.rwmap->rwm_rw, fdc.ctx, 
				( ftmp.bv_len ? ftmp.bv_val : "" ), 
				fdc.conn, &fstr->bv_val )) {
	case REWRITE_REGEXEC_OK:
		if ( fstr->bv_val != NULL ) {
			fstr->bv_len = strlen( fstr->bv_val );
			free( ftmp.bv_val );
		} else {
			*fstr = ftmp;
		}

#ifdef NEW_LOGGING
		LDAP_LOG( BACK_LDAP, DETAIL1, 
			"[rw] %s: \"%s\" -> \"%s\"\n",
			dc->ctx, ftmp.bv_val, fstr->bv_val );		
#else /* !NEW_LOGGING */
		Debug( LDAP_DEBUG_ARGS,
			"[rw] %s: \"%s\" -> \"%s\"\n",
			dc->ctx, ftmp.bv_val, fstr->bv_val );		
#endif /* !NEW_LOGGING */
		rc = LDAP_SUCCESS;
		break;
 		
 	case REWRITE_REGEXEC_UNWILLING:
		if ( fdc.rs ) {
			fdc.rs->sr_err = LDAP_UNWILLING_TO_PERFORM;
			fdc.rs->sr_text = "Operation not allowed";
		}
		rc = LDAP_UNWILLING_TO_PERFORM;
		break;
	       	
	case REWRITE_REGEXEC_ERR:
		if ( fdc.rs ) {
			fdc.rs->sr_err = LDAP_OTHER;
			fdc.rs->sr_text = "Rewrite error";
		}
		rc = LDAP_OTHER;
		break;
	}

#endif /* ENABLE_REWRITE */
	return rc;
}

/*
 * I don't like this much, but we need two different
 * functions because different heap managers may be
 * in use in back-ldap/meta to reduce the amount of
 * calls to malloc routines, and some of the free()
 * routines may be macros with args
 */
int
ldap_dnattr_rewrite(
	dncookie		*dc,
	BerVarray		a_vals
)
{
	struct berval	bv;
	int		i, last;

	for ( last = 0; a_vals[last].bv_val != NULL; last++ );
	last--;

	for ( i = 0; a_vals[i].bv_val != NULL; i++ ) {
		switch ( ldap_back_dn_massage( dc, &a_vals[i], &bv ) ) {
		case LDAP_UNWILLING_TO_PERFORM:
			/*
			 * FIXME: need to check if it may be considered 
			 * legal to trim values when adding/modifying;
			 * it should be when searching (e.g. ACLs).
			 */
			ch_free( a_vals[i].bv_val );
			if (last > i ) {
				a_vals[i] = a_vals[last];
			}
			a_vals[last].bv_len = 0;
			a_vals[last].bv_val = NULL;
			last--;
			break;

		default:
			/* leave attr untouched if massage failed */
			if ( bv.bv_val && bv.bv_val != a_vals[i].bv_val ) {
				ch_free( a_vals[i].bv_val );
				a_vals[i] = bv;
			}
			break;
		}
	}
	
	return 0;
}

int
ldap_dnattr_result_rewrite(
	dncookie		*dc,
	BerVarray		a_vals
)
{
	struct berval	bv;
	int		i, last;

	for ( last = 0; a_vals[last].bv_val; last++ );
	last--;

	for ( i = 0; a_vals[i].bv_val; i++ ) {
		switch ( ldap_back_dn_massage( dc, &a_vals[i], &bv ) ) {
		case LDAP_UNWILLING_TO_PERFORM:
			/*
			 * FIXME: need to check if it may be considered 
			 * legal to trim values when adding/modifying;
			 * it should be when searching (e.g. ACLs).
			 */
			LBER_FREE( &a_vals[i].bv_val );
			if ( last > i ) {
				a_vals[i] = a_vals[last];
			}
			a_vals[last].bv_val = NULL;
			a_vals[last].bv_len = 0;
			last--;
			break;

		default:
			/* leave attr untouched if massage failed */
			if ( bv.bv_val && a_vals[i].bv_val != bv.bv_val ) {
				LBER_FREE( a_vals[i].bv_val );
				a_vals[i] = bv;
			}
			break;
		}
	}

	return 0;
}

