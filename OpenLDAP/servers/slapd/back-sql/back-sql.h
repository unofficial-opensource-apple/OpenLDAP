/* $OpenLDAP: pkg/ldap/servers/slapd/back-sql/back-sql.h,v 1.9.2.5 2004/09/24 14:09:15 ando Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1999-2004 The OpenLDAP Foundation.
 * Portions Copyright 1999 Dmitry Kovalev.
 * Portions Copyright 2002 Pierangelo Mararati.
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
 * by OpenLDAP Software.  Additional significant contributors include
 *    Pierangelo Mararati
 */

/*
 * The following changes have been addressed:
 *	 
 * Enhancements:
 *   - re-styled code for better readability
 *   - upgraded backend API to reflect recent changes
 *   - LDAP schema is checked when loading SQL/LDAP mapping
 *   - AttributeDescription/ObjectClass pointers used for more efficient
 *     mapping lookup
 *   - bervals used where string length is required often
 *   - atomized write operations by committing at the end of each operation
 *     and defaulting connection closure to rollback
 *   - added LDAP access control to write operations
 *   - fully implemented modrdn (with rdn attrs change, deleteoldrdn,
 *     access check, parent/children check and more)
 *   - added parent access control, children control to delete operation
 *   - added structuralObjectClass operational attribute check and
 *     value return on search
 *   - added hasSubordinate operational attribute on demand
 *   - search limits are appropriately enforced
 *   - function backsql_strcat() has been made more efficient
 *   - concat function has been made configurable by means of a pattern
 *   - added config switches:
 *       - fail_if_no_mapping	write operations fail if there is no mapping
 *       - has_ldapinfo_dn_ru	overrides autodetect
 *       - concat_pattern	a string containing two '?' is used
 * 				(note that "?||?" should be more portable
 * 				than builtin function "CONCAT(?,?)")
 *       - strcast_func		cast of string constants in "SELECT DISTINCT
 *				statements (needed by PostgreSQL)
 *       - upper_needs_cast	cast the argument of upper when required
 * 				(basically when building dn substring queries)
 *   - added noop control
 *   - added values return filter control
 *   - hasSubordinate can be used in search filters (with limitations)
 *   - eliminated oc->name; use oc->oc->soc_cname instead
 * 
 * Todo:
 *   - add security checks for SQL statements that can be injected (?)
 *   - re-test with previously supported RDBMs
 *   - replace dn_ru and so with normalized dn (no need for upper() and so
 *     in dn match)
 *   - implement a backsql_normalize() function to replace the upper()
 *     conversion routines
 *   - note that subtree deletion, subtree renaming and so could be easily
 *     implemented (rollback and consistency checks are available :)
 *   - implement "lastmod" and other operational stuff (ldap_entries table ?)
 *   - check how to allow multiple operations with one statement, to remove
 *     BACKSQL_REALLOC_STMT from modify.c (a more recent unixODBC lib?)
 */

#ifndef __BACKSQL_H__
#define __BACKSQL_H__

#include "external.h"
#include "sql-types.h"

/*
 * PostgreSQL 7.0 doesn't work without :(
 */
#define	BACKSQL_REALLOC_STMT

/*
 * Better use the standard length of 8192 (as of slap.h)?
 */
/* #define BACKSQL_MAX_DN_LEN	SLAP_LDAPDN_MAXLEN */
#define BACKSQL_MAX_DN_LEN	255

/*
 * define to enable very extensive trace logging (debug only)
 */
#undef BACKSQL_TRACE

/*
 * define to enable varchars as unique keys in user tables
 */
#undef BACKSQL_ARBITRARY_KEY

/*
 * define to the appropriate aliasing string
 */
#define BACKSQL_ALIASING	"AS "
/* #define	BACKSQL_ALIASING	"" */

/*
 * define to the appropriate quoting char
 */
/* #define BACKSQL_ALIASING_QUOTE	'"' */
/* #define BACKSQL_ALIASING_QUOTE	'\'' */

/*
 * API
 */
typedef struct backsql_api {
	char			*ba_name;
	int 			(*ba_dn2odbc)( Operation *op, SlapReply *rs, struct berval *dn );
	int 			(*ba_odbc2dn)( Operation *op, SlapReply *rs, struct berval *dn );
	struct backsql_api *ba_next;
} backsql_api;

/*
 * Entry ID structure
 */
typedef struct backsql_entryID {
	/* #define BACKSQL_ARBITRARY_KEY to allow a non-numeric key.
	 * It is required by some special applications that use
	 * strings as keys for the main table.
	 * In this case, #define BACKSQL_MAX_KEY_LEN consistently
	 * with the key size definition */
#ifdef BACKSQL_ARBITRARY_KEY
	struct berval		eid_id;
	struct berval		eid_keyval;
#define BACKSQL_MAX_KEY_LEN	64
#else /* ! BACKSQL_ARBITRARY_KEY */
	/* The original numeric key is maintained as default. */
	unsigned long		eid_id;
	unsigned long		eid_keyval;
#endif /* ! BACKSQL_ARBITRARY_KEY */

	unsigned long		eid_oc_id;
	struct berval		eid_dn;
	struct backsql_entryID	*eid_next;
} backsql_entryID;

#ifdef BACKSQL_ARBITRARY_KEY
#define BACKSQL_ENTRYID_INIT { BER_BVNULL, BER_BVNULL, 0, BER_BVNULL, NULL }
#else /* ! BACKSQL_ARBITRARY_KEY */
#define BACKSQL_ENTRYID_INIT { 0, 0, 0, BER_BVNULL, NULL }
#endif /* BACKSQL_ARBITRARY_KEY */

/*
 * "structural" objectClass mapping structure
 */
typedef struct backsql_oc_map_rec {
	/*
	 * Structure of corresponding LDAP objectClass definition
	 */
	ObjectClass	*bom_oc;
#define BACKSQL_OC_NAME(ocmap)	((ocmap)->bom_oc->soc_cname.bv_val)
	
	struct berval	bom_keytbl;
	struct berval	bom_keycol;
	/* expected to return keyval of newly created entry */
	char		*bom_create_proc;
	/* in case create_proc does not return the keyval of the newly
	 * created row */
	char		*bom_create_keyval;
	/* supposed to expect keyval as parameter and delete 
	 * all the attributes as well */
	char		*bom_delete_proc;
	/* flags whether delete_proc is a function (whether back-sql 
	 * should bind first parameter as output for return code) */
	int		bom_expect_return;
	unsigned long	bom_id;
	Avlnode		*bom_attrs;
} backsql_oc_map_rec;

/*
 * attributeType mapping structure
 */
typedef struct backsql_at_map_rec {
	/* Description of corresponding LDAP attribute type */
	AttributeDescription	*bam_ad;
	/* ObjectClass if bam_ad is objectClass */
	ObjectClass		*bam_oc;

	struct berval	bam_from_tbls;
	struct berval	bam_join_where;
	struct berval	bam_sel_expr;

	/* TimesTen, or, if a uppercase function is defined,
	 * an uppercased version of bam_sel_expr */
	struct berval	bam_sel_expr_u;

	/* supposed to expect 2 binded values: entry keyval 
	 * and attr. value to add, like "add_name(?,?,?)" */
	char		*bam_add_proc;
	/* supposed to expect 2 binded values: entry keyval 
	 * and attr. value to delete */
	char		*bam_delete_proc;
	/* for optimization purposes attribute load query 
	 * is preconstructed from parts on schemamap load time */
	char		*bam_query;
	/* following flags are bitmasks (first bit used for add_proc, 
	 * second - for delete_proc) */
	/* order of parameters for procedures above; 
	 * 1 means "data then keyval", 0 means "keyval then data" */
	int 		bam_param_order;
	/* flags whether one or more of procedures is a function 
	 * (whether back-sql should bind first parameter as output 
	 * for return code) */
	int 		bam_expect_return;

	/* next mapping for attribute */
	struct backsql_at_map_rec	*bam_next;
} backsql_at_map_rec;

#define BACKSQL_AT_MAP_REC_INIT { NULL, NULL, BER_BVC(""), BER_BVC(""), BER_BVNULL, BER_BVNULL, NULL, NULL, NULL, 0, 0, NULL }

/* define to uppercase filters only if the matching rule requires it
 * (currently broken) */
/* #define	BACKSQL_UPPERCASE_FILTER */

#define	BACKSQL_AT_CANUPPERCASE(at)	((at)->bam_sel_expr_u.bv_val)

/* defines to support bitmasks above */
#define BACKSQL_ADD	0x1
#define BACKSQL_DEL	0x2

#define BACKSQL_IS_ADD(x)	( BACKSQL_ADD & (x) )
#define BACKSQL_IS_DEL(x)	( BACKSQL_DEL & (x) )

#define BACKSQL_NCMP(v1,v2)	ber_bvcmp((v1),(v2))

#define BACKSQL_CONCAT
/*
 * berbuf structure: a berval with a buffer size associated
 */
typedef struct berbuf {
	struct berval	bb_val;
	ber_len_t	bb_len;
} BerBuffer;

#define BB_NULL		{ { 0, NULL }, 0 }

typedef struct backsql_srch_info {
	Operation		*bsi_op;
	SlapReply		*bsi_rs;

	int			bsi_flags;
#define	BSQL_SF_ALL_OPER		0x0001
#define BSQL_SF_FILTER_HASSUBORDINATE	0x0002

	struct berval		*bsi_base_dn;
	int			bsi_scope;
#define BACKSQL_SCOPE_BASE_LIKE		( LDAP_SCOPE_BASE | 0x1000 )
	Filter			*bsi_filter;
	int			bsi_slimit,
				bsi_tlimit;
	time_t			bsi_stoptime;

	backsql_entryID		*bsi_id_list,
				**bsi_id_listtail,
				*bsi_c_eid;
	int			bsi_n_candidates;
	int			bsi_abandon;
	int			bsi_status;

	backsql_oc_map_rec	*bsi_oc;
	struct berbuf		bsi_sel,
				bsi_from,
				bsi_join_where,
				bsi_flt_where;
	ObjectClass		*bsi_filter_oc;
	SQLHDBC			bsi_dbh;
	AttributeName		*bsi_attrs;

	Entry			*bsi_e;
} backsql_srch_info;

/*
 * Backend private data structure
 */
typedef struct {
	char		*dbhost;
	int		dbport;
	char		*dbuser;
	char		*dbpasswd;
	char		*dbname;
 	/*
	 * SQL condition for subtree searches differs in syntax:
	 * "LIKE CONCAT('%',?)" or "LIKE '%'+?" or "LIKE '%'||?"
	 * or smth else 
	 */
	struct berval	subtree_cond;
	struct berval	children_cond;
	char		*oc_query, *at_query;
	char		*insentry_query,
			*delentry_query,
			*delobjclasses_query,
			*delreferrals_query;
	char		*id_query;
	char		*has_children_query;

	MatchingRule	*bi_caseIgnoreMatch;
	MatchingRule	*bi_telephoneNumberMatch;

	struct berval	upper_func;
	struct berval	upper_func_open;
	struct berval	upper_func_close;
	BerVarray	concat_func;

	unsigned int	bsql_flags;
#define	BSQLF_SCHEMA_LOADED		0x0001
#define	BSQLF_UPPER_NEEDS_CAST		0x0002
#define	BSQLF_CREATE_NEEDS_SELECT	0x0004
#define	BSQLF_FAIL_IF_NO_MAPPING	0x0008
#define BSQLF_HAS_LDAPINFO_DN_RU	0x0010
#define BSQLF_DONTCHECK_LDAPINFO_DN_RU	0x0020
#define BSQLF_USE_REVERSE_DN		0x0040

#define	BACKSQL_SCHEMA_LOADED(si) \
	((si)->bsql_flags & BSQLF_SCHEMA_LOADED)
#define BACKSQL_UPPER_NEEDS_CAST(si) \
	((si)->bsql_flags & BSQLF_UPPER_NEEDS_CAST)
#define BACKSQL_CREATE_NEEDS_SELECT(si) \
	((si)->bsql_flags & BSQLF_CREATE_NEEDS_SELECT)
#define BACKSQL_FAIL_IF_NO_MAPPING(si) \
	((si)->bsql_flags & BSQLF_FAIL_IF_NO_MAPPING)
#define BACKSQL_HAS_LDAPINFO_DN_RU(si) \
	((si)->bsql_flags & BSQLF_HAS_LDAPINFO_DN_RU)
#define BACKSQL_DONTCHECK_LDAPINFO_DN_RU(si) \
	((si)->bsql_flags & BSQLF_DONTCHECK_LDAPINFO_DN_RU)
#define BACKSQL_USE_REVERSE_DN(si) \
	((si)->bsql_flags & BSQLF_USE_REVERSE_DN)
#define BACKSQL_CANUPPERCASE(si) \
	((si)->upper_func.bv_val)
	
	struct berval	strcast_func;
	Avlnode		*db_conns;
	Avlnode		*oc_by_oc;
	Avlnode		*oc_by_id;
	ldap_pvt_thread_mutex_t		dbconn_mutex;
	ldap_pvt_thread_mutex_t		schema_mutex;
 	SQLHENV		db_env;

	backsql_api	*si_api;
} backsql_info;

#define BACKSQL_SUCCESS( rc ) \
	( (rc) == SQL_SUCCESS || (rc) == SQL_SUCCESS_WITH_INFO )

#define BACKSQL_AVL_STOP		0
#define BACKSQL_AVL_CONTINUE		1

#endif /* __BACKSQL_H__ */



