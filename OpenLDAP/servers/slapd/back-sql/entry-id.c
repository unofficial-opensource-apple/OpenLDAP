/* $OpenLDAP: pkg/ldap/servers/slapd/back-sql/entry-id.c,v 1.20.2.9 2004/09/24 14:09:15 ando Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1999-2004 The OpenLDAP Foundation.
 * Portions Copyright 1999 Dmitry Kovalev.
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
 * This work was initially developed by Dmitry Kovalev for inclusion
 * by OpenLDAP Software.
 */

#include "portable.h"

#ifdef SLAPD_SQL

#include <stdio.h>
#include <sys/types.h>
#include "ac/string.h"

#include "lber_pvt.h"
#include "ldap_pvt.h"
#include "slap.h"
#include "proto-sql.h"

backsql_entryID *
backsql_free_entryID( backsql_entryID *id, int freeit )
{
	backsql_entryID 	*next;

	assert( id );

	next = id->eid_next;

	if ( id->eid_dn.bv_val != NULL ) {
		free( id->eid_dn.bv_val );
	}

#ifdef BACKSQL_ARBITRARY_KEY
	if ( id->eid_id.bv_val ) {
		free( id->eid_id.bv_val );
	}

	if ( id->eid_keyval.bv_val ) {
		free( id->eid_keyval.bv_val );
	}
#endif /* BACKSQL_ARBITRARY_KEY */

	if ( freeit ) {
		free( id );
	}

	return next;
}

int
backsql_dn2id(
	backsql_info		*bi,
	backsql_entryID		*id,
	SQLHDBC			dbh,
	struct berval		*dn )
{
	SQLHSTMT		sth; 
	BACKSQL_ROW_NTS		row;
	RETCODE 		rc;
	int			res;

	/* TimesTen */
	char			upperdn[ BACKSQL_MAX_DN_LEN + 1 ];
	struct berval		toBind;
	int			i, j;

	/*
	 * NOTE: id can be NULL; in this case, the function
	 * simply checks whether the DN can be successfully 
	 * turned into an ID, returning LDAP_SUCCESS for
	 * positive cases, or the most appropriate error
	 */

	Debug( LDAP_DEBUG_TRACE, "==>backsql_dn2id(): dn=\"%s\"%s\n", 
			dn->bv_val, id == NULL ? " (no ID)" : "", 0 );

	if ( dn->bv_len > BACKSQL_MAX_DN_LEN ) {
		Debug( LDAP_DEBUG_TRACE, 
			"backsql_dn2id(): DN \"%s\" (%ld bytes) "
			"exceeds max DN length (%d):\n",
			dn->bv_val, dn->bv_len, BACKSQL_MAX_DN_LEN );
		return LDAP_OTHER;
	}
	
	/* begin TimesTen */
	Debug(LDAP_DEBUG_TRACE, "id_query \"%s\"\n", bi->id_query, 0, 0);
	assert( bi->id_query );
 	rc = backsql_Prepare( dbh, &sth, bi->id_query, 0 );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, 
			"backsql_dn2id(): error preparing SQL:\n%s", 
			bi->id_query, 0, 0);
		backsql_PrintErrors( SQL_NULL_HENV, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		return LDAP_OTHER;
	}

	if ( BACKSQL_HAS_LDAPINFO_DN_RU( bi ) ) {
		/*
		 * Prepare an upper cased, byte reversed version 
		 * that can be searched using indexes
		 */

		for ( i = 0, j = dn->bv_len - 1; dn->bv_val[ i ]; i++, j--) {
			upperdn[ i ] = dn->bv_val[ j ];
		}
		upperdn[ i ] = '\0';
		ldap_pvt_str2upper( upperdn );

		Debug( LDAP_DEBUG_TRACE, "==>backsql_dn2id(): upperdn=\"%s\"\n",
				upperdn, 0, 0 );
		ber_str2bv( upperdn, 0, 0, &toBind );

	} else {
		if ( BACKSQL_USE_REVERSE_DN( bi ) ) {
			AC_MEMCPY( upperdn, dn->bv_val, dn->bv_len + 1 );
			ldap_pvt_str2upper( upperdn );
			Debug( LDAP_DEBUG_TRACE,
				"==>backsql_dn2id(): upperdn=\"%s\"\n",
				upperdn, 0, 0 );
			ber_str2bv( upperdn, 0, 0, &toBind );

		} else {
			toBind = *dn;
		}
	}

	rc = backsql_BindParamStr( sth, 1, toBind.bv_val, BACKSQL_MAX_DN_LEN );
	if ( rc != SQL_SUCCESS) {
		/* end TimesTen */ 
		Debug( LDAP_DEBUG_TRACE, "backsql_dn2id(): "
			"error binding dn=\"%s\" parameter:\n", 
			toBind.bv_val, 0, 0 );
		backsql_PrintErrors( SQL_NULL_HENV, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		return LDAP_OTHER;
	}

	rc = SQLExecute( sth );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_dn2id(): "
			"error executing query (\"%s\", \"%s\"):\n", 
			bi->id_query, toBind.bv_val, 0 );
		backsql_PrintErrors( SQL_NULL_HENV, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		return LDAP_OTHER;
	}

	backsql_BindRowAsStrings( sth, &row );
	rc = SQLFetch( sth );
	if ( BACKSQL_SUCCESS( rc ) ) {
		res = LDAP_SUCCESS;
		Debug( LDAP_DEBUG_TRACE, "<==backsql_dn2id(): id=%s keyval=%s oc_id=%s\n",
				row.cols[ 0 ], row.cols[ 1 ], row.cols[ 2 ] );

		if ( id != NULL ) {
#ifdef BACKSQL_ARBITRARY_KEY
			ber_str2bv( row.cols[ 0 ], 0, 1, &id->eid_id );
			ber_str2bv( row.cols[ 1 ], 0, 1, &id->eid_keyval );
#else /* ! BACKSQL_ARBITRARY_KEY */
			id->eid_id = strtol( row.cols[ 0 ], NULL, 0 );
			id->eid_keyval = strtol( row.cols[ 1 ], NULL, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */
			id->eid_oc_id = strtol( row.cols[ 2 ], NULL, 0 );

			ber_dupbv( &id->eid_dn, dn );
			id->eid_next = NULL;
		}

	} else {
		res = LDAP_NO_SUCH_OBJECT;
		Debug( LDAP_DEBUG_TRACE, "<==backsql_dn2id(): no match\n",
				0, 0, 0 );
	}
	backsql_FreeRow( &row );

	SQLFreeStmt( sth, SQL_DROP );
	return res;
}

int
backsql_count_children(
	backsql_info		*bi,
	SQLHDBC			dbh,
	struct berval		*dn,
	unsigned long		*nchildren )
{
	SQLHSTMT		sth; 
	BACKSQL_ROW_NTS		row;
	RETCODE 		rc;
	int			res = LDAP_SUCCESS;

	Debug( LDAP_DEBUG_TRACE, "==>backsql_count_children(): dn=\"%s\"\n", 
			dn->bv_val, 0, 0 );

	if ( dn->bv_len > BACKSQL_MAX_DN_LEN ) {
		Debug( LDAP_DEBUG_TRACE, 
			"backsql_count_children(): DN \"%s\" (%ld bytes) "
			"exceeds max DN length (%d):\n",
			dn->bv_val, dn->bv_len, BACKSQL_MAX_DN_LEN );
		return LDAP_OTHER;
	}
	
	/* begin TimesTen */
	Debug(LDAP_DEBUG_TRACE, "children id query \"%s\"\n", 
			bi->has_children_query, 0, 0);
	assert( bi->has_children_query );
 	rc = backsql_Prepare( dbh, &sth, bi->has_children_query, 0 );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, 
			"backsql_count_children(): error preparing SQL:\n%s", 
			bi->has_children_query, 0, 0);
		backsql_PrintErrors( SQL_NULL_HENV, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		return LDAP_OTHER;
	}

	rc = backsql_BindParamStr( sth, 1, dn->bv_val, BACKSQL_MAX_DN_LEN );
	if ( rc != SQL_SUCCESS) {
		/* end TimesTen */ 
		Debug( LDAP_DEBUG_TRACE, "backsql_count_children(): "
			"error binding dn=\"%s\" parameter:\n", 
			dn->bv_val, 0, 0 );
		backsql_PrintErrors( SQL_NULL_HENV, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		return LDAP_OTHER;
	}

	rc = SQLExecute( sth );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_count_children(): "
			"error executing query (\"%s\", \"%s\"):\n", 
			bi->has_children_query, dn->bv_val, 0 );
		backsql_PrintErrors( SQL_NULL_HENV, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		return LDAP_OTHER;
	}

	backsql_BindRowAsStrings( sth, &row );
	
	rc = SQLFetch( sth );
	if ( BACKSQL_SUCCESS( rc ) ) {
		char *end;

		*nchildren = strtol( row.cols[ 0 ], &end, 0 );
		if ( end[ 0 ] != '\0' && end[0] != '.' ) {
			/* FIXME: braindead RDBMSes return
			 * a fractional number from COUNT!
			 */
			res = LDAP_OTHER;
		}

	} else {
		res = LDAP_OTHER;
	}
	backsql_FreeRow( &row );

	SQLFreeStmt( sth, SQL_DROP );

	Debug( LDAP_DEBUG_TRACE, "<==backsql_count_children(): %lu\n",
			*nchildren, 0, 0 );

	return res;
}

int
backsql_has_children(
	backsql_info		*bi,
	SQLHDBC			dbh,
	struct berval		*dn )
{
	unsigned long	nchildren;
	int		rc;

	rc = backsql_count_children( bi, dbh, dn, &nchildren );

	if ( rc == LDAP_SUCCESS ) {
		return nchildren > 0 ? LDAP_COMPARE_TRUE : LDAP_COMPARE_FALSE;
	}

	return rc;
}

static int
backsql_get_attr_vals( void *v_at, void *v_bsi )
{
	backsql_at_map_rec	*at = v_at;
	backsql_srch_info	*bsi = v_bsi;
	backsql_info		*bi = (backsql_info *)bsi->bsi_op->o_bd->be_private;
	RETCODE			rc;
	SQLHSTMT		sth;
	BACKSQL_ROW_NTS		row;
	int			i;

	assert( at );
	assert( bsi );

#ifdef BACKSQL_ARBITRARY_KEY
	Debug( LDAP_DEBUG_TRACE, "==>backsql_get_attr_vals(): "
		"oc=\"%s\" attr=\"%s\" keyval=%s\n",
		BACKSQL_OC_NAME( bsi->bsi_oc ), at->bam_ad->ad_cname.bv_val, 
		bsi->bsi_c_eid->eid_keyval.bv_val );
#else /* ! BACKSQL_ARBITRARY_KEY */
	Debug( LDAP_DEBUG_TRACE, "==>backsql_get_attr_vals(): "
		"oc=\"%s\" attr=\"%s\" keyval=%ld\n",
		BACKSQL_OC_NAME( bsi->bsi_oc ), at->bam_ad->ad_cname.bv_val, 
		bsi->bsi_c_eid->eid_keyval );
#endif /* ! BACKSQL_ARBITRARY_KEY */

	rc = backsql_Prepare( bsi->bsi_dbh, &sth, at->bam_query, 0 );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_get_attr_values(): "
			"error preparing query: %s\n", at->bam_query, 0, 0 );
		backsql_PrintErrors( bi->db_env, bsi->bsi_dbh, sth, rc );
		return 1;
	}

#ifdef BACKSQL_ARBITRARY_KEY
	rc = backsql_BindParamStr( sth, 1, bsi->bsi_c_eid->eid_keyval.bv_val,
		       BACKSQL_MAX_KEY_LEN );
#else /* ! BACKSQL_ARBITRARY_KEY */
	rc = backsql_BindParamID( sth, 1, &bsi->bsi_c_eid->eid_keyval );
#endif /* ! BACKSQL_ARBITRARY_KEY */
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_get_attr_values(): "
			"error binding key value parameter\n", 0, 0, 0 );
		return 1;
	}

#ifdef BACKSQL_TRACE
#ifdef BACKSQL_ARBITRARY_KEY
	Debug( LDAP_DEBUG_TRACE, "backsql_get_attr_values(): "
		"query=\"%s\" keyval=%s\n", at->bam_query,
		bsi->bsi_c_eid->eid_keyval.bv_val, 0 );
#else /* !BACKSQL_ARBITRARY_KEY */
	Debug( LDAP_DEBUG_TRACE, "backsql_get_attr_values(): "
		"query=\"%s\" keyval=%d\n", at->bam_query,
		bsi->bsi_c_eid->eid_keyval, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */
#endif /* BACKSQL_TRACE */

	rc = SQLExecute( sth );
	if ( ! BACKSQL_SUCCESS( rc ) ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_get_attr_values(): "
			"error executing attribute query \"%s\"\n",
			at->bam_query, 0, 0 );
		backsql_PrintErrors( bi->db_env, bsi->bsi_dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		return 1;
	}

	backsql_BindRowAsStrings( sth, &row );

	rc = SQLFetch( sth );
	for ( ; BACKSQL_SUCCESS( rc ); rc = SQLFetch( sth ) ) {
		for ( i = 0; i < row.ncols; i++ ) {
			if ( row.value_len[ i ] > 0 ) {
				struct berval	bv;

				bv.bv_val = row.cols[ i ];
#if 0
				bv.bv_len = row.col_prec[ i ];
#else
				/*
				 * FIXME: what if a binary 
				 * is fetched?
				 */
				bv.bv_len = strlen( row.cols[ i ] );
#endif
       				backsql_entry_addattr( bsi->bsi_e, 
						&row.col_names[ i ], &bv,
						bsi->bsi_op->o_tmpmemctx );

#ifdef BACKSQL_TRACE
				Debug( LDAP_DEBUG_TRACE, "prec=%d\n",
					(int)row.col_prec[ i ], 0, 0 );
			} else {
      				Debug( LDAP_DEBUG_TRACE, "NULL value "
					"in this row for attribute \"%s\"\n",
					row.col_names[ i ].bv_val, 0, 0 );
#endif /* BACKSQL_TRACE */
			}
		}
	}

	backsql_FreeRow( &row );
	SQLFreeStmt( sth, SQL_DROP );
	Debug( LDAP_DEBUG_TRACE, "<==backsql_get_attr_vals()\n", 0, 0, 0 );

	if ( at->bam_next ) {
		return backsql_get_attr_vals( at->bam_next, v_bsi );
	}

	return 1;
}

int
backsql_id2entry( backsql_srch_info *bsi, backsql_entryID *eid )
{
	int			i;
	int			rc;
	AttributeDescription	*ad_oc = slap_schema.si_ad_objectClass;

	Debug( LDAP_DEBUG_TRACE, "==>backsql_id2entry()\n", 0, 0, 0 );

	assert( bsi->bsi_e );

	memset( bsi->bsi_e, 0, sizeof( Entry ) );

	rc = dnPrettyNormal( NULL, &eid->eid_dn,
			&bsi->bsi_e->e_name, &bsi->bsi_e->e_nname,
			bsi->bsi_op->o_tmpmemctx );
	if ( rc != LDAP_SUCCESS ) {
		return rc;
	}

	bsi->bsi_e->e_attrs = NULL;
	bsi->bsi_e->e_private = NULL;

	bsi->bsi_oc = backsql_id2oc( bsi->bsi_op->o_bd->be_private,
			eid->eid_oc_id );
	bsi->bsi_c_eid = eid;

#ifndef BACKSQL_ARBITRARY_KEY	
	bsi->bsi_e->e_id = eid->eid_id;
#endif /* ! BACKSQL_ARBITRARY_KEY */
 
	rc = attr_merge_normalize_one( bsi->bsi_e, ad_oc,
				&bsi->bsi_oc->bom_oc->soc_cname,
				bsi->bsi_op->o_tmpmemctx );
	if ( rc != LDAP_SUCCESS ) {
		entry_clean( bsi->bsi_e );
		return rc;
	}

	if ( bsi->bsi_attrs != NULL ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_id2entry(): "
			"custom attribute list\n", 0, 0, 0 );
		for ( i = 0; bsi->bsi_attrs[ i ].an_name.bv_val; i++ ) {
			backsql_at_map_rec	**vat;
			AttributeName		*an = &bsi->bsi_attrs[ i ];
			int			j;

			/* if one of the attributes listed here is
			 * a subtype of another, it must be ignored,
			 * because subtypes are already dealt with
			 * by backsql_supad2at()
			 */
			for ( j = 0; bsi->bsi_attrs[ j ].an_name.bv_val; j++ ) {
				/* skip self */
				if ( j == i ) {
					continue;
				}

				/* skip subtypes */
				if ( is_at_subtype( an->an_desc->ad_type,
							bsi->bsi_attrs[ j ].an_desc->ad_type ) )
				{
					goto next;
				}
			}

			rc = backsql_supad2at( bsi->bsi_oc, an->an_desc, &vat );
			if ( rc != 0 || vat == NULL ) {
				Debug( LDAP_DEBUG_TRACE, "backsql_id2entry(): "
						"attribute \"%s\" is not defined "
						"for objectlass \"%s\"\n",
						an->an_name.bv_val, 
						BACKSQL_OC_NAME( bsi->bsi_oc ), 0 );
				continue;
			}

			for ( j = 0; vat[j]; j++ ) {
    				backsql_get_attr_vals( vat[j], bsi );
			}

			ch_free( vat );

next:;
		}

	} else {
		Debug( LDAP_DEBUG_TRACE, "backsql_id2entry(): "
			"retrieving all attributes\n", 0, 0, 0 );
		avl_apply( bsi->bsi_oc->bom_attrs, backsql_get_attr_vals,
				bsi, 0, AVL_INORDER );
	}

	if ( global_schemacheck ) {
		const char	*text = NULL;
		char		textbuf[ 1024 ];
		size_t		textlen = sizeof( textbuf );
		struct berval	bv[ 2 ];
		struct berval	soc;
		int rc;

		bv[ 0 ] = bsi->bsi_oc->bom_oc->soc_cname;
		bv[ 1 ].bv_val = NULL;

		rc = structural_class( bv, &soc, NULL, 
				&text, textbuf, textlen );
		if ( rc != LDAP_SUCCESS ) {
      			Debug( LDAP_DEBUG_TRACE, "backsql_id2entry(%s): "
				"structural_class() failed %d (%s)\n",
				bsi->bsi_e->e_name.bv_val,
				rc, text ? text : "" );
			entry_clean( bsi->bsi_e );
			return rc;
		}

		if ( ( bsi->bsi_flags | BSQL_SF_ALL_OPER )
				|| an_find( bsi->bsi_attrs, &AllOper ) ) {
			rc = attr_merge_normalize_one( bsi->bsi_e,
					slap_schema.si_ad_structuralObjectClass,
					&soc, bsi->bsi_op->o_tmpmemctx );
			if ( rc != LDAP_SUCCESS ) {
				entry_clean( bsi->bsi_e );
				return rc;
			}
		}
	}

	Debug( LDAP_DEBUG_TRACE, "<==backsql_id2entry()\n", 0, 0, 0 );

	return LDAP_SUCCESS;
}

#endif /* SLAPD_SQL */

