/* $OpenLDAP: pkg/ldap/servers/slapd/back-sql/schema-map.c,v 1.19.2.10 2004/09/24 14:09:15 ando Exp $ */
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

#include "slap.h"
#include "lber_pvt.h"
#include "ldap_pvt.h"
#include "proto-sql.h"

#define BACKSQL_DUPLICATE	(-1)

/* NOTE: by default, cannot just compare pointers because
 * objectClass/attributeType order would be machine-dependent
 * (and tests would fail!); however, if you don't want to run
 * tests, or see attributeTypes written in the same order
 * they are defined, define */
/* #undef BACKSQL_USE_PTR_CMP */

/*
 * Uses the pointer to the ObjectClass structure
 */
static int
backsql_cmp_oc( const void *v_m1, const void *v_m2 )
{
	const backsql_oc_map_rec	*m1 = v_m1,
					*m2 = v_m2;

#ifdef BACKSQL_USE_PTR_CMP
	return SLAP_PTRCMP( m1->bom_oc, m2->bom_oc );
#else /* ! BACKSQL_USE_PTR_CMP */
	return ber_bvcmp( &m1->bom_oc->soc_cname, &m2->bom_oc->soc_cname );
#endif /* ! BACKSQL_USE_PTR_CMP */
}

static int
backsql_cmp_oc_id( const void *v_m1, const void *v_m2 )
{
	const backsql_oc_map_rec	*m1 = v_m1,
					*m2 = v_m2;

	return ( m1->bom_id < m2->bom_id ? -1 : ( m1->bom_id > m2->bom_id ? 1 : 0 ) );
}

/*
 * Uses the pointer to the AttributeDescription structure
 */
static int
backsql_cmp_attr( const void *v_m1, const void *v_m2 )
{
	const backsql_at_map_rec	*m1 = v_m1,
					*m2 = v_m2;

#ifdef BACKSQL_USE_PTR_CMP
	return SLAP_PTRCMP( m1->bam_ad, m2->bam_ad );
#else /* ! BACKSQL_USE_PTR_CMP */
	return ber_bvcmp( &m1->bam_ad->ad_cname, &m2->bam_ad->ad_cname );
#endif /* ! BACKSQL_USE_PTR_CMP */
}

int
backsql_dup_attr( void *v_m1, void *v_m2 )
{
	backsql_at_map_rec		*m1 = v_m1,
					*m2 = v_m2;

	assert( m1->bam_ad == m2->bam_ad );

	/* duplicate definitions of attributeTypes are appended;
	 * this allows to define multiple rules for the same 
	 * attributeType.  Use with care! */
	for ( ; m1->bam_next ; m1 = m1->bam_next );
	m1->bam_next = m2;
	m2->bam_next = NULL;

	return BACKSQL_DUPLICATE;
}

static int
backsql_make_attr_query( 
	backsql_oc_map_rec 	*oc_map,
	backsql_at_map_rec 	*at_map )
{
	struct berbuf	bb = BB_NULL;

#ifdef BACKSQL_ALIASING_QUOTE
	backsql_strfcat( &bb, "lblcbclblbcbl", 
			(ber_len_t)STRLENOF( "SELECT " ), "SELECT ", 
			&at_map->bam_sel_expr, 
			(ber_len_t)STRLENOF( " " BACKSQL_ALIASING ), " " BACKSQL_ALIASING, 
			BACKSQL_ALIASING_QUOTE,
			&at_map->bam_ad->ad_cname,
			BACKSQL_ALIASING_QUOTE,
			(ber_len_t)STRLENOF( " FROM " ), " FROM ", 
			&at_map->bam_from_tbls, 
			(ber_len_t)STRLENOF( " WHERE " ), " WHERE ", 
			&oc_map->bom_keytbl,
			'.', 
			&oc_map->bom_keycol,
			(ber_len_t)STRLENOF( "=?" ), "=?" );
#else /* ! BACKSQL_ALIASING_QUOTE */
	backsql_strfcat( &bb, "lblblblbcbl", 
			(ber_len_t)STRLENOF( "SELECT " ), "SELECT ", 
			&at_map->bam_sel_expr, 
			(ber_len_t)STRLENOF( " " BACKSQL_ALIASING ), " " BACKSQL_ALIASING, 
			&at_map->bam_ad->ad_cname,
			(ber_len_t)STRLENOF( " FROM " ), " FROM ", 
			&at_map->bam_from_tbls, 
			(ber_len_t)STRLENOF( " WHERE " ), " WHERE ", 
			&oc_map->bom_keytbl,
			'.', 
			&oc_map->bom_keycol,
			(ber_len_t)STRLENOF( "=?" ), "=?" );
#endif /* ! BACKSQL_ALIASING_QUOTE */

	if ( !BER_BVISNULL( &at_map->bam_join_where ) ) {
		backsql_strfcat( &bb, "lb",
				(ber_len_t)STRLENOF( " AND " ), " AND ", 
				&at_map->bam_join_where );
	}

#ifdef BACKSQL_ALIASING_QUOTE
	backsql_strfcat( &bb, "lcbc", 
			(ber_len_t)STRLENOF( " ORDER BY " ), " ORDER BY ",
			BACKSQL_ALIASING_QUOTE,
			&at_map->bam_sel_expr,
			BACKSQL_ALIASING_QUOTE );
#else /* ! BACKSQL_ALIASING_QUOTE */
	backsql_strfcat( &bb, "lb", 
			(ber_len_t)STRLENOF( " ORDER BY " ), " ORDER BY ",
			&at_map->bam_sel_expr );
#endif /* ! BACKSQL_ALIASING_QUOTE */

	at_map->bam_query = bb.bb_val.bv_val;
	
	return 0;
}

static int
backsql_add_sysmaps( backsql_oc_map_rec *oc_map )
{
	backsql_at_map_rec	*at_map;
	char			s[] = "+9223372036854775807L";
	struct berval		sbv;
	struct berbuf		bb;
	
	sbv.bv_val = s;
	snprintf( s, sizeof( s ), "%ld", oc_map->bom_id );
	sbv.bv_len = strlen( s );

	/* extra objectClasses */
	at_map = (backsql_at_map_rec *)ch_calloc(1, 
			sizeof( backsql_at_map_rec ) );
	at_map->bam_ad = slap_schema.si_ad_objectClass;
	ber_str2bv( "ldap_entry_objclasses.oc_name", 0, 1,
			&at_map->bam_sel_expr );
	ber_str2bv( "ldap_entry_objclasses,ldap_entries", 0, 1, 
			&at_map->bam_from_tbls );
	
	bb.bb_len = at_map->bam_from_tbls.bv_len + 1;
	bb.bb_val = at_map->bam_from_tbls;
	backsql_merge_from_clause( &bb, &oc_map->bom_keytbl );
	at_map->bam_from_tbls = bb.bb_val;

	BER_BVZERO( &bb.bb_val );
	bb.bb_len = 0;
	backsql_strfcat( &bb, "lbcblb",
			(ber_len_t)STRLENOF( "ldap_entries.id=ldap_entry_objclasses.entry_id AND ldap_entries.keyval=" ),
				"ldap_entries.id=ldap_entry_objclasses.entry_id AND ldap_entries.keyval=",
			&oc_map->bom_keytbl, 
			'.', 
			&oc_map->bom_keycol,
			(ber_len_t)STRLENOF( " and ldap_entries.oc_map_id=" ), 
				" and ldap_entries.oc_map_id=", 
			&sbv );
	at_map->bam_join_where = bb.bb_val;

	at_map->bam_oc = oc_map->bom_oc;

	at_map->bam_add_proc = NULL;
	{
		char	tmp[] =
			"INSERT INTO ldap_entry_objclasses "
			"(entry_id,oc_name) VALUES "
			"((SELECT id FROM ldap_entries "
			"WHERE oc_map_id="
			"18446744073709551615UL "	/* 64 bit ULONG */
			"AND keyval=?),?)";
		snprintf( tmp, sizeof(tmp), 
			"INSERT INTO ldap_entry_objclasses "
			"(entry_id,oc_name) VALUES "
			"((SELECT id FROM ldap_entries "
			"WHERE oc_map_id=%lu "
			"AND keyval=?),?)", oc_map->bom_id );
		at_map->bam_add_proc = ch_strdup( tmp );
	}

	at_map->bam_delete_proc = NULL;
	{
		char	tmp[] =
			"DELETE FROM ldap_entry_objclasses "
			"WHERE entry_id=(SELECT id FROM ldap_entries "
			"WHERE oc_map_id="
			"18446744073709551615UL "	/* 64 bit ULONG */
			"AND keyval=?) AND oc_name=?";
		snprintf( tmp, sizeof(tmp), 
			"DELETE FROM ldap_entry_objclasses "
			"WHERE entry_id=(SELECT id FROM ldap_entries "
			"WHERE oc_map_id=%lu"
			"AND keyval=?) AND oc_name=?",
			oc_map->bom_id );
		at_map->bam_delete_proc = ch_strdup( tmp );
	}

	at_map->bam_param_order = 0;
	at_map->bam_expect_return = 0;
	at_map->bam_next = NULL;

	backsql_make_attr_query( oc_map, at_map );
	if ( avl_insert( &oc_map->bom_attrs, at_map, backsql_cmp_attr, backsql_dup_attr ) == BACKSQL_DUPLICATE ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_add_sysmaps(): "
				"duplicate attribute \"%s\" in objectClass \"%s\" map\n",
				at_map->bam_ad->ad_cname.bv_val,
				oc_map->bom_oc->soc_cname.bv_val, 0 );
	}

	/* FIXME: we need to correct the objectClass join_where 
	 * after the attribute query is built */
	ch_free( at_map->bam_join_where.bv_val );
	BER_BVZERO( &bb.bb_val );
	bb.bb_len = 0;
	backsql_strfcat( &bb, "lbcblb",
			(ber_len_t)STRLENOF( /* "ldap_entries.id=ldap_entry_objclasses.entry_id AND " */ "ldap_entries.keyval=" ),
				/* "ldap_entries.id=ldap_entry_objclasses.entry_id AND " */ "ldap_entries.keyval=",
			&oc_map->bom_keytbl, 
			'.', 
			&oc_map->bom_keycol,
			(ber_len_t)STRLENOF( " AND ldap_entries.oc_map_id=" ), 
				" AND ldap_entries.oc_map_id=", 
			&sbv );
	at_map->bam_join_where = bb.bb_val;

	/* referral attribute */
	at_map = (backsql_at_map_rec *)ch_calloc( 1, 
			sizeof( backsql_at_map_rec ) );
	at_map->bam_ad = slap_schema.si_ad_ref;
	ber_str2bv( "ldap_referrals.url", 0, 1, &at_map->bam_sel_expr );
	ber_str2bv( "ldap_referrals,ldap_entries", 0, 1, &at_map->bam_from_tbls );

	bb.bb_len = at_map->bam_from_tbls.bv_len + 1;
	bb.bb_val = at_map->bam_from_tbls;
	backsql_merge_from_clause( &bb, &oc_map->bom_keytbl );
	at_map->bam_from_tbls = bb.bb_val;

	BER_BVZERO( &bb.bb_val );
	bb.bb_len = 0;
	backsql_strfcat( &bb, "lbcblb",
			(ber_len_t)STRLENOF( "ldap_entries.id=ldap_referrals.entry_id AND ldap_entries.keyval=" ),
				"ldap_entries.id=ldap_referrals.entry_id AND ldap_entries.keyval=",
			&oc_map->bom_keytbl, 
			'.', 
			&oc_map->bom_keycol,
			(ber_len_t)STRLENOF( " AND ldap_entries.oc_map_id=" ), 
				" AND ldap_entries.oc_map_id=", 
			&sbv );

	at_map->bam_join_where = bb.bb_val;

	at_map->bam_oc = NULL;

	at_map->bam_add_proc = NULL;
	{
		char	tmp[] =
			"INSERT INTO ldap_referrals "
			"(entry_id,url) VALUES "
			"((SELECT id FROM ldap_entries "
			"WHERE oc_map_id="
			"18446744073709551615UL "	/* 64 bit ULONG */
			"AND keyval=?),?)";
		snprintf( tmp, sizeof(tmp), 
			"INSERT INTO ldap_referrals "
			"(entry_id,url) VALUES "
			"((SELECT id FROM ldap_entries "
			"WHERE oc_map_id=%lu "
			"AND keyval=?),?)", oc_map->bom_id );
		at_map->bam_add_proc = ch_strdup( tmp );
	}

	at_map->bam_delete_proc = NULL;
	{
		char	tmp[] =
			"DELETE FROM ldap_referrals "
			"WHERE entry_id=(SELECT id FROM ldap_entries "
			"WHERE oc_map_id="
			"18446744073709551615UL "	/* 64 bit ULONG */
			"AND keyval=?) and url=?";
		snprintf( tmp, sizeof(tmp), 
			"DELETE FROM ldap_referrals "
			"WHERE entry_id=(SELECT id FROM ldap_entries "
			"WHERE oc_map_id=%lu"
			"AND keyval=?) and url=?",
			oc_map->bom_id );
		at_map->bam_delete_proc = ch_strdup( tmp );
	}

	at_map->bam_param_order = 0;
	at_map->bam_expect_return = 0;
	at_map->bam_next = NULL;

	backsql_make_attr_query( oc_map, at_map );
	if ( avl_insert( &oc_map->bom_attrs, at_map, backsql_cmp_attr, backsql_dup_attr ) == BACKSQL_DUPLICATE ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_add_sysmaps(): "
				"duplicate attribute \"%s\" in objectClass \"%s\" map\n",
				at_map->bam_ad->ad_cname.bv_val,
				oc_map->bom_oc->soc_cname.bv_val, 0 );
	}

	return 1;
}

struct backsql_attr_schema_info {
	backsql_info	*bas_si;
	SQLHDBC		bas_dbh;
	SQLHSTMT	bas_sth;
	unsigned long	*bas_oc_id;
	int		bas_rc;
};

static int
backsql_oc_get_attr_mapping( void *v_oc, void *v_bas )
{
	RETCODE				rc;
	BACKSQL_ROW_NTS			at_row;
	backsql_oc_map_rec		*oc_map = (backsql_oc_map_rec *)v_oc;
	backsql_at_map_rec		*at_map;
	struct backsql_attr_schema_info	*bas = (struct backsql_attr_schema_info *)v_bas;

	/* bas->bas_oc_id has been bound to bas->bas_sth */
	*bas->bas_oc_id = oc_map->bom_id;

	Debug( LDAP_DEBUG_TRACE, "backsql_oc_get_attr_mapping(): "
		"executing at_query\n"
		"    \"%s\"\n"
		"    for objectClass \"%s\"\n"
		"    with param oc_id=\"%lu\"\n",
		bas->bas_si->at_query,
		BACKSQL_OC_NAME( oc_map ),
		*bas->bas_oc_id );

	rc = SQLExecute( bas->bas_sth );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_oc_get_attr_mapping(): "
			"error executing at_query\n"
			"    \"%s\"\n"
			"    for objectClass \"%s\"\n"
			"    with param oc_id=\"%lu\"\n",
			bas->bas_si->at_query,
			BACKSQL_OC_NAME( oc_map ),
			*bas->bas_oc_id );
		backsql_PrintErrors( SQL_NULL_HENV, bas->bas_dbh, bas->bas_sth, rc );
		bas->bas_rc = LDAP_OTHER;
		return BACKSQL_AVL_STOP;
	}

	backsql_BindRowAsStrings( bas->bas_sth, &at_row );
	for ( ; rc = SQLFetch( bas->bas_sth ), BACKSQL_SUCCESS( rc ); ) {
		const char	*text = NULL;
		struct berval	bv;
		struct berbuf	bb = BB_NULL;

		Debug( LDAP_DEBUG_TRACE, 
			"attributeType:\n"
			"\tname=\"%s\"\n"
			"\tsel_expr=\"%s\"\n"
			"\tfrom=\"%s\"\n",
			at_row.cols[ 0 ], at_row.cols[ 1 ],
			at_row.cols[ 2 ] );
		Debug( LDAP_DEBUG_TRACE, 
			"\tjoin_where=\"%s\"\n"
			"\tadd_proc=\"%s\"\n"
			"\tdelete_proc=\"%s\"\n",
			at_row.cols[ 3 ], at_row.cols[ 4 ],
			at_row.cols[ 5 ]);
		/* TimesTen */
		Debug( LDAP_DEBUG_TRACE, "\tsel_expr_u=\"%s\"\n",
				at_row.cols[ 8 ], 0, 0 );
		at_map = (backsql_at_map_rec *)ch_calloc( 1,
				sizeof( backsql_at_map_rec ) );
		rc = slap_str2ad( at_row.cols[ 0 ], 
				&at_map->bam_ad, &text );
		if ( rc != LDAP_SUCCESS ) {
			Debug( LDAP_DEBUG_TRACE, "backsql_oc_get_attr_mapping(): "
				"attribute \"%s\" for objectClass \"%s\" "
				"is not defined in schema: %s\n", 
				at_row.cols[ 0 ],
				BACKSQL_OC_NAME( oc_map ), text );
			bas->bas_rc = LDAP_CONSTRAINT_VIOLATION;
			return BACKSQL_AVL_STOP;
		}

		ber_str2bv( at_row.cols[ 1 ], 0, 1, &at_map->bam_sel_expr );
		if ( at_row.value_len[ 8 ] < 0 ) {
			BER_BVZERO( &at_map->bam_sel_expr_u );

		} else {
			ber_str2bv( at_row.cols[ 8 ], 0, 1, 
					&at_map->bam_sel_expr_u );
		}

		ber_str2bv( at_row.cols[ 2 ], 0, 0, &bv );
		backsql_merge_from_clause( &bb, &bv );
		at_map->bam_from_tbls = bb.bb_val;
		if ( at_row.value_len[ 3 ] < 0 ) {
			BER_BVZERO( &at_map->bam_join_where );

		} else {
			ber_str2bv( at_row.cols[ 3 ], 0, 1, 
					&at_map->bam_join_where );
		}
		at_map->bam_add_proc = NULL;
		if ( at_row.value_len[ 4 ] > 0 ) {
			at_map->bam_add_proc = ch_strdup( at_row.cols[4] );
		}
		at_map->bam_delete_proc = NULL;
		if ( at_row.value_len[ 5 ] > 0 ) {
			at_map->bam_delete_proc = ch_strdup( at_row.cols[ 5 ] );
		}
		at_map->bam_param_order = strtol( at_row.cols[ 6 ], 
				NULL, 0 );
		at_map->bam_expect_return = strtol( at_row.cols[ 7 ],
				NULL, 0 );
		backsql_make_attr_query( oc_map, at_map );
		Debug( LDAP_DEBUG_TRACE, "backsql_oc_get_attr_mapping(): "
			"preconstructed query \"%s\"\n",
			at_map->bam_query, 0, 0 );
		at_map->bam_next = NULL;
		if ( avl_insert( &oc_map->bom_attrs, at_map, backsql_cmp_attr, backsql_dup_attr ) == BACKSQL_DUPLICATE ) {
			Debug( LDAP_DEBUG_TRACE, "backsql_oc_get_attr_mapping(): "
					"duplicate attribute \"%s\" "
					"in objectClass \"%s\" map\n",
					at_map->bam_ad->ad_cname.bv_val,
					oc_map->bom_oc->soc_cname.bv_val, 0 );
		}

		if ( !BER_BVISNULL( &bas->bas_si->upper_func ) &&
				BER_BVISNULL( &at_map->bam_sel_expr_u ) )
		{
			struct berbuf	bb = BB_NULL;

			backsql_strfcat( &bb, "bcbc",
					&bas->bas_si->upper_func,
					'(' /* ) */ ,
					&at_map->bam_sel_expr,
					/* ( */ ')' );
			at_map->bam_sel_expr_u = bb.bb_val;
		}
	}
	backsql_FreeRow( &at_row );
	SQLFreeStmt( bas->bas_sth, SQL_CLOSE );

	Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(\"%s\"): "
		"autoadding 'objectClass' and 'ref' mappings\n",
		BACKSQL_OC_NAME( oc_map ), 0, 0 );

	(void)backsql_add_sysmaps( oc_map );

	return BACKSQL_AVL_CONTINUE;
}


int
backsql_load_schema_map( backsql_info *si, SQLHDBC dbh )
{
	SQLHSTMT 			sth;
	RETCODE				rc;
	BACKSQL_ROW_NTS			oc_row;
	unsigned long			oc_id;
	backsql_oc_map_rec		*oc_map;
	struct backsql_attr_schema_info	bas;

	Debug( LDAP_DEBUG_TRACE, "==>backsql_load_schema_map()\n", 0, 0, 0 );

	/* 
	 * TimesTen : See if the ldap_entries.dn_ru field exists in the schema
	 */
	if ( !BACKSQL_DONTCHECK_LDAPINFO_DN_RU( si ) ) {
		rc = backsql_Prepare( dbh, &sth, 
				backsql_check_dn_ru_query, 0 );
		if ( rc == SQL_SUCCESS ) {
			/* Yes, the field exists */
			si->bsql_flags |= BSQLF_HAS_LDAPINFO_DN_RU;
   			Debug( LDAP_DEBUG_TRACE, "ldapinfo.dn_ru field exists "
				"in the schema\n", 0, 0, 0 );
		} else {
			/* No such field exists */
			si->bsql_flags &= ~BSQLF_HAS_LDAPINFO_DN_RU;
		}

		SQLFreeStmt( sth, SQL_DROP );
	}

	Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): oc_query \"%s\"\n", 
			si->oc_query, 0, 0 );

	rc = backsql_Prepare( dbh, &sth, si->oc_query, 0 );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): "
			"error preparing oc_query: \"%s\"\n", 
			si->oc_query, 0, 0 );
		backsql_PrintErrors( si->db_env, dbh, sth, rc );
		return LDAP_OTHER;
	}

	rc = SQLExecute( sth );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): "
			"error executing oc_query: \n", 0, 0, 0 );
		backsql_PrintErrors( si->db_env, dbh, sth, rc );
		return LDAP_OTHER;
	}

	backsql_BindRowAsStrings( sth, &oc_row );
	rc = SQLFetch( sth );
	for ( ; BACKSQL_SUCCESS( rc ); rc = SQLFetch( sth ) ) {
		int	colnum;

		oc_map = (backsql_oc_map_rec *)ch_calloc( 1,
				sizeof( backsql_oc_map_rec ) );

		oc_map->bom_id = strtol( oc_row.cols[ 0 ], NULL, 0 );

		oc_map->bom_oc = oc_find( oc_row.cols[ 1 ] );
		if ( oc_map->bom_oc == NULL ) {
			Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): "
				"objectClass \"%s\" is not defined in schema\n", 
				oc_row.cols[ 1 ], 0, 0 );
			return LDAP_OTHER;	/* undefined objectClass ? */
		}
		
		ber_str2bv( oc_row.cols[ 2 ], 0, 1, &oc_map->bom_keytbl );
		ber_str2bv( oc_row.cols[ 3 ], 0, 1, &oc_map->bom_keycol );
		oc_map->bom_create_proc = ( oc_row.value_len[ 4 ] < 0 ) ? NULL 
			: ch_strdup( oc_row.cols[ 4 ] );

		colnum = 5;
		if ( BACKSQL_CREATE_NEEDS_SELECT( si ) ) {
			colnum = 6;
			oc_map->bom_create_keyval = ( oc_row.value_len[ 5 ] < 0 ) 
				? NULL : ch_strdup( oc_row.cols[ 5 ] );
		}
		oc_map->bom_delete_proc = ( oc_row.value_len[ colnum ] < 0 ) ? NULL 
			: ch_strdup( oc_row.cols[ colnum ] );
		oc_map->bom_expect_return = strtol( oc_row.cols[ colnum + 1 ], 
				NULL, 0 );

		/*
		 * FIXME: first attempt to check for offending
		 * instructions in {create|delete}_proc
		 */

		oc_map->bom_attrs = NULL;
		if ( avl_insert( &si->oc_by_oc, oc_map, backsql_cmp_oc, avl_dup_error ) == -1 ) {
			Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): "
					"duplicate objectClass \"%s\" in objectClass map\n",
					oc_map->bom_oc->soc_cname.bv_val, 0, 0 );
			return LDAP_OTHER;
		}
		if ( avl_insert( &si->oc_by_id, oc_map, backsql_cmp_oc_id, avl_dup_error ) == -1 ) {
			Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): "
					"duplicate objectClass \"%s\" in objectClass by ID map\n",
					oc_map->bom_oc->soc_cname.bv_val, 0, 0 );
			return LDAP_OTHER;
		}
		oc_id = oc_map->bom_id;
		Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): "
			"objectClass \"%s\": keytbl=\"%s\" keycol=\"%s\"\n",
			BACKSQL_OC_NAME( oc_map ),
			oc_map->bom_keytbl.bv_val, oc_map->bom_keycol.bv_val );
		if ( oc_map->bom_create_proc ) {
			Debug( LDAP_DEBUG_TRACE, "create_proc=\"%s\"\n",
				oc_map->bom_create_proc, 0, 0 );
		}
		if ( oc_map->bom_create_keyval ) {
			Debug( LDAP_DEBUG_TRACE, "create_keyval=\"%s\"\n",
				oc_map->bom_create_keyval, 0, 0 );
		}
		if ( oc_map->bom_delete_proc ) {
			Debug( LDAP_DEBUG_TRACE, "delete_proc=\"%s\"\n", 
				oc_map->bom_delete_proc, 0, 0 );
		}
		Debug( LDAP_DEBUG_TRACE, "expect_return: "
			"add=%d, del=%d; attributes:\n",
			BACKSQL_IS_ADD( oc_map->bom_expect_return ), 
			BACKSQL_IS_DEL( oc_map->bom_expect_return ), 0 );
	}

	backsql_FreeRow( &oc_row );
	SQLFreeStmt( sth, SQL_DROP );

	/* prepare for attribute fetching */
	Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): at_query \"%s\"\n", 
			si->at_query, 0, 0 );

	rc = backsql_Prepare( dbh, &sth, si->at_query, 0 );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): "
			"error preparing at_query: \"%s\"\n", 
			si->at_query, 0, 0 );
		backsql_PrintErrors( si->db_env, dbh, sth, rc );
		return LDAP_OTHER;
	}

	rc = backsql_BindParamID( sth, 1, &oc_id );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_load_schema_map(): "
			"error binding param \"oc_id\" for at_query\n", 0, 0, 0 );
		backsql_PrintErrors( si->db_env, dbh, sth, rc );
		return LDAP_OTHER;
	}

	bas.bas_si = si;
	bas.bas_dbh = dbh;
	bas.bas_sth = sth;
	bas.bas_oc_id = &oc_id;
	bas.bas_rc = LDAP_SUCCESS;

	(void)avl_apply( si->oc_by_oc, backsql_oc_get_attr_mapping,
			&bas, BACKSQL_AVL_STOP, AVL_INORDER );

	SQLFreeStmt( sth, SQL_DROP );

	si->bsql_flags |= BSQLF_SCHEMA_LOADED;

	Debug( LDAP_DEBUG_TRACE, "<==backsql_load_schema_map()\n", 0, 0, 0 );

	return bas.bas_rc;
}

backsql_oc_map_rec *
backsql_oc2oc( backsql_info *si, ObjectClass *oc )
{
	backsql_oc_map_rec	tmp, *res;

#ifdef BACKSQL_TRACE
	Debug( LDAP_DEBUG_TRACE, "==>backsql_oc2oc(): "
		"searching for objectclass with name=\"%s\"\n",
		oc->soc_cname.bv_val, 0, 0 );
#endif /* BACKSQL_TRACE */

	tmp.bom_oc = oc;
	res = (backsql_oc_map_rec *)avl_find( si->oc_by_oc, &tmp, backsql_cmp_oc );
#ifdef BACKSQL_TRACE
	if ( res != NULL ) {
		Debug( LDAP_DEBUG_TRACE, "<==backsql_oc2oc(): "
			"found name=\"%s\", id=%d\n", 
			BACKSQL_OC_NAME( res ), res->bom_id, 0 );
	} else {
		Debug( LDAP_DEBUG_TRACE, "<==backsql_oc2oc(): "
			"not found\n", 0, 0, 0 );
	}
#endif /* BACKSQL_TRACE */
 
	return res;
}

backsql_oc_map_rec *
backsql_name2oc( backsql_info *si, struct berval *oc_name )
{
	backsql_oc_map_rec	tmp, *res;

#ifdef BACKSQL_TRACE
	Debug( LDAP_DEBUG_TRACE, "==>oc_with_name(): "
		"searching for objectclass with name=\"%s\"\n",
		oc_name->bv_val, 0, 0 );
#endif /* BACKSQL_TRACE */

	tmp.bom_oc = oc_bvfind( oc_name );
	if ( tmp.bom_oc == NULL ) {
		return NULL;
	}

	res = (backsql_oc_map_rec *)avl_find( si->oc_by_oc, &tmp, backsql_cmp_oc );
#ifdef BACKSQL_TRACE
	if ( res != NULL ) {
		Debug( LDAP_DEBUG_TRACE, "<==oc_with_name(): "
			"found name=\"%s\", id=%d\n", 
			BACKSQL_OC_NAME( res ), res->bom_id, 0 );
	} else {
		Debug( LDAP_DEBUG_TRACE, "<==oc_with_name(): "
			"not found\n", 0, 0, 0 );
	}
#endif /* BACKSQL_TRACE */
 
	return res;
}

backsql_oc_map_rec *
backsql_id2oc( backsql_info *si, unsigned long id )
{
	backsql_oc_map_rec	tmp, *res;
 
#ifdef BACKSQL_TRACE
	Debug( LDAP_DEBUG_TRACE, "==>oc_with_id(): "
		"searching for objectclass with id='%d'\n", id, 0, 0 );
#endif /* BACKSQL_TRACE */

	tmp.bom_id = id;
	res = (backsql_oc_map_rec *)avl_find( si->oc_by_id, &tmp,
			backsql_cmp_oc_id );

#ifdef BACKSQL_TRACE
	if ( res != NULL ) {
		Debug( LDAP_DEBUG_TRACE, "<==oc_with_name(): "
			"found name=\"%s\", id=%d\n",
			BACKSQL_OC_NAME( res ), res->bom_id, 0 );
	} else {
		Debug( LDAP_DEBUG_TRACE, "<==oc_with_name(): "
			"not found\n", 0, 0, 0 );
	}
#endif /* BACKSQL_TRACE */
	
	return res;
}

backsql_at_map_rec *
backsql_ad2at( backsql_oc_map_rec* objclass, AttributeDescription *ad )
{
	backsql_at_map_rec	tmp, *res;
 
#ifdef BACKSQL_TRACE
	Debug( LDAP_DEBUG_TRACE, "==>backsql_ad2at(): "
		"searching for attribute \"%s\" for objectclass \"%s\"\n",
		ad->ad_cname.bv_val, BACKSQL_OC_NAME( objclass ), 0 );
#endif /* BACKSQL_TRACE */

	tmp.bam_ad = ad;
	res = (backsql_at_map_rec *)avl_find( objclass->bom_attrs, &tmp,
			backsql_cmp_attr );

#ifdef BACKSQL_TRACE
	if ( res != NULL ) {
		Debug( LDAP_DEBUG_TRACE, "<==backsql_ad2at(): "
			"found name=\"%s\", sel_expr=\"%s\"\n",
			res->bam_ad->ad_cname.bv_val,
			res->bam_sel_expr.bv_val, 0 );
	} else {
		Debug( LDAP_DEBUG_TRACE, "<==backsql_ad2at(): "
			"not found\n", 0, 0, 0 );
	}
#endif /* BACKSQL_TRACE */

	return res;
}

/* attributeType inheritance */
struct supad2at_t {
	backsql_at_map_rec	**ret;
	AttributeDescription	*ad;
	unsigned		n;
};

#define SUPAD2AT_STOP	(-1)

static int
supad2at_f( void *v_at, void *v_arg )
{
	backsql_at_map_rec	*at = (backsql_at_map_rec *)v_at;
	struct supad2at_t	*va = (struct supad2at_t *)v_arg;

	if ( is_at_subtype( at->bam_ad->ad_type, va->ad->ad_type ) ) {
		backsql_at_map_rec	**ret;
		unsigned		i;

		/* if already listed, holler! (should never happen) */
		if ( va->ret ) {
			for ( i = 0; i < va->n; i++ ) {
				if ( va->ret[ i ]->bam_ad == at->bam_ad ) {
					break;
				}
			}

			if ( i < va->n ) {
				return 0;
			}
		}

		ret = ch_realloc( va->ret,
				sizeof( backsql_at_map_rec *) * ( va->n + 2 ) );
		if ( ret == NULL ) {
			ch_free( va->ret );
			return SUPAD2AT_STOP;
		}

		ret[ va->n ] = at;
		va->n++;
		ret[ va->n ] = NULL;
		va->ret = ret;
	}

	return 0;
}

/*
 * stores in *pret a NULL terminated array of pointers
 * to backsql_at_map_rec whose attributeType is supad->ad_type 
 * or derived from it
 */
int
backsql_supad2at( backsql_oc_map_rec *objclass, AttributeDescription *supad,
		backsql_at_map_rec ***pret )
{
	struct supad2at_t	va;
	int			rc;

	assert( objclass );
	assert( supad );
	assert( pret );

	*pret = NULL;

	va.ret = NULL;
	va.ad = supad;
	va.n = 0;

	rc = avl_apply( objclass->bom_attrs, supad2at_f, &va,
			SUPAD2AT_STOP, AVL_INORDER );
	if ( rc == SUPAD2AT_STOP ) {
		return -1;
	}

	*pret = va.ret;

	return 0;
}

static void
backsql_free_attr( void *v_at )
{
	backsql_at_map_rec	*at = v_at;
	
	Debug( LDAP_DEBUG_TRACE, "==>free_attr(): \"%s\"\n", 
			at->bam_ad->ad_cname.bv_val, 0, 0 );
	ch_free( at->bam_sel_expr.bv_val );
	if ( !BER_BVISNULL( &at->bam_from_tbls ) ) {
		ch_free( at->bam_from_tbls.bv_val );
	}
	if ( !BER_BVISNULL( &at->bam_join_where ) ) {
		ch_free( at->bam_join_where.bv_val );
	}
	if ( at->bam_add_proc != NULL ) {
		ch_free( at->bam_add_proc );
	}
	if ( at->bam_delete_proc != NULL ) {
		ch_free( at->bam_delete_proc );
	}
	if ( at->bam_query ) {
		ch_free( at->bam_query );
	}

	/* TimesTen */
	if ( !BER_BVISNULL( &at->bam_sel_expr_u ) ) {
		ch_free( at->bam_sel_expr_u.bv_val );
	}

	if ( at->bam_next ) {
		backsql_free_attr( at->bam_next );
	}
	
	ch_free( at );

	Debug( LDAP_DEBUG_TRACE, "<==free_attr()\n", 0, 0, 0 );
}

static void
backsql_free_oc( void *v_oc )
{
	backsql_oc_map_rec	*oc = v_oc;
	
	Debug( LDAP_DEBUG_TRACE, "==>free_oc(): \"%s\"\n", 
			BACKSQL_OC_NAME( oc ), 0, 0 );
	avl_free( oc->bom_attrs, backsql_free_attr );
	ch_free( oc->bom_keytbl.bv_val );
	ch_free( oc->bom_keycol.bv_val );
	if ( oc->bom_create_proc != NULL ) {
		ch_free( oc->bom_create_proc );
	}
	if ( oc->bom_create_keyval != NULL ) {
		ch_free( oc->bom_create_keyval );
	}
	if ( oc->bom_delete_proc != NULL ) {
		ch_free( oc->bom_delete_proc );
	}
	ch_free( oc );

	Debug( LDAP_DEBUG_TRACE, "<==free_oc()\n", 0, 0, 0 );
}

int
backsql_destroy_schema_map( backsql_info *si )
{
	Debug( LDAP_DEBUG_TRACE, "==>destroy_schema_map()\n", 0, 0, 0 );
	avl_free( si->oc_by_oc, 0 );
	avl_free( si->oc_by_id, backsql_free_oc );
	Debug( LDAP_DEBUG_TRACE, "<==destroy_schema_map()\n", 0, 0, 0 );
	return 0;
}

#endif /* SLAPD_SQL */

