/* $OpenLDAP: pkg/ldap/servers/slapd/back-sql/modrdn.c,v 1.1.2.3 2004/09/24 14:09:15 ando Exp $ */
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
#include "ldap_pvt.h"
#include "proto-sql.h"

int
backsql_modrdn( Operation *op, SlapReply *rs )
{
	backsql_info		*bi = (backsql_info*)op->o_bd->be_private;
	SQLHDBC			dbh;
	SQLHSTMT		sth;
	RETCODE			rc;
	backsql_entryID		e_id = BACKSQL_ENTRYID_INIT,
				pe_id = BACKSQL_ENTRYID_INIT,
				new_pe_id = BACKSQL_ENTRYID_INIT;
	backsql_oc_map_rec	*oc = NULL;
	struct berval		p_dn = BER_BVNULL, p_ndn = BER_BVNULL,
				*new_pdn = NULL, *new_npdn = NULL,
				new_dn = BER_BVNULL, new_ndn = BER_BVNULL;
	LDAPRDN			new_rdn = NULL;
	LDAPRDN			old_rdn = NULL;
	Entry			e;
	Modifications		*mod = NULL;
	struct berval		*newSuperior = op->oq_modrdn.rs_newSup;
	char			*next;
 
	Debug( LDAP_DEBUG_TRACE, "==>backsql_modrdn() renaming entry \"%s\", "
			"newrdn=\"%s\", newSuperior=\"%s\"\n",
			op->o_req_dn.bv_val, op->oq_modrdn.rs_newrdn.bv_val, 
			newSuperior ? newSuperior->bv_val : "(NULL)" );
	rs->sr_err = backsql_get_db_conn( op, &dbh );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"could not get connection handle - exiting\n", 
			0, 0, 0 );
		rs->sr_text = ( rs->sr_err == LDAP_OTHER )
			?  "SQL-backend error" : NULL;
		send_ldap_result( op, rs );
		return 1;
	}

	rs->sr_err = backsql_dn2id( bi, &e_id, dbh, &op->o_req_ndn );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"could not lookup entry id (%d)\n",
			rs->sr_err, 0, 0 );
		rs->sr_text = ( rs->sr_err == LDAP_OTHER )
			?  "SQL-backend error" : NULL;
		send_ldap_result( op, rs );
		return 1;
	}

#ifdef BACKSQL_ARBITRARY_KEY
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): entry id=%s\n",
		e_id.eid_id.bv_val, 0, 0 );
#else /* ! BACKSQL_ARBITRARY_KEY */
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): entry id=%ld\n",
		e_id.eid_id, 0, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */

	if ( backsql_has_children( bi, dbh, &op->o_req_ndn ) == LDAP_COMPARE_TRUE ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"entry \"%s\" has children\n",
			op->o_req_dn.bv_val, 0, 0 );
		rs->sr_err = LDAP_NOT_ALLOWED_ON_NONLEAF;
		rs->sr_text = "subtree rename not supported";
		send_ldap_result( op, rs );
		return 1;
	}

	dnParent( &op->o_req_dn, &p_dn );
	dnParent( &op->o_req_ndn, &p_ndn );

	/*
	 * namingContext "" is not supported
	 */
	if ( p_dn.bv_len == 0 ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"parent is \"\" - aborting\n", 0, 0, 0 );
		rs->sr_err = LDAP_UNWILLING_TO_PERFORM;
		rs->sr_text = "not allowed within namingContext";
		send_ldap_result( op, rs );
		goto modrdn_return;
	}

	/*
	 * Check for children access to parent
	 */
	e.e_attrs = NULL;
	e.e_name = p_dn;
	e.e_nname = p_ndn;
	if ( !access_allowed( op, &e, slap_schema.si_ad_children, 
				NULL, ACL_WRITE, NULL ) ) {
		Debug( LDAP_DEBUG_TRACE, "   no access to parent\n", 0, 0, 0 );
		rs->sr_err = LDAP_INSUFFICIENT_ACCESS;
		goto modrdn_return;
	}

	if ( newSuperior ) {
		/*
		 * namingContext "" is not supported
		 */
		if ( newSuperior->bv_len == 0 ) {
			Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
				"newSuperior is \"\" - aborting\n", 0, 0, 0 );
			rs->sr_err = LDAP_UNWILLING_TO_PERFORM;
			rs->sr_text = "not allowed within namingContext";
			send_ldap_result( op, rs );
			goto modrdn_return;
		}

		new_pdn = newSuperior;
		new_npdn = op->oq_modrdn.rs_nnewSup;

		e.e_name = *new_pdn;
		e.e_nname = *new_npdn;

		/*
		 * Check for children access to new parent
		 */
		if ( !access_allowed( op, &e, slap_schema.si_ad_children, 
					NULL, ACL_WRITE, NULL ) ) {
			Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
					"no access to new parent \"%s\"\n", 
					new_pdn->bv_val, 0, 0 );
			rs->sr_err = LDAP_INSUFFICIENT_ACCESS;
			goto modrdn_return;
		}

	} else {
		new_pdn = &p_dn;
		new_npdn = &p_ndn;
	}

	if ( newSuperior && dn_match( &p_ndn, new_npdn ) ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"newSuperior is equal to old parent - ignored\n",
			0, 0, 0 );
		newSuperior = NULL;
	}

	if ( newSuperior && dn_match( &op->o_req_ndn, new_npdn ) ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"newSuperior is equal to entry being moved "
			"- aborting\n", 0, 0, 0 );
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "newSuperior is equal to old DN";
		send_ldap_result( op, rs );
		goto modrdn_return;
	}

	build_new_dn( &new_dn, new_pdn, &op->oq_modrdn.rs_newrdn,
			op->o_tmpmemctx );
	rs->sr_err = dnNormalize( 0, NULL, NULL, &new_dn, &new_ndn,
			op->o_tmpmemctx );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"new dn is invalid (\"%s\") - aborting\n",
			new_dn.bv_val, 0, 0 );
		rs->sr_text = "unable to build new DN";
		send_ldap_result( op, rs );
		goto modrdn_return;
	}
	
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): new entry dn is \"%s\"\n",
			new_dn.bv_val, 0, 0 );

	rs->sr_err = backsql_dn2id( bi, &pe_id, dbh, &p_ndn );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"could not lookup old parent entry id\n", 0, 0, 0 );
		rs->sr_text = ( rs->sr_err == LDAP_OTHER )
			? "SQL-backend error" : NULL;
		send_ldap_result( op, rs );
		goto modrdn_return;
	}

#ifdef BACKSQL_ARBITRARY_KEY
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"old parent entry id is %s\n", pe_id.eid_id.bv_val, 0, 0 );
#else /* ! BACKSQL_ARBITRARY_KEY */
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"old parent entry id is %ld\n", pe_id.eid_id, 0, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */

	backsql_free_entryID( &pe_id, 0 );

	rs->sr_err = backsql_dn2id( bi, &new_pe_id, dbh, new_npdn );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"could not lookup new parent entry id\n", 0, 0, 0 );
		rs->sr_text = ( rs->sr_err == LDAP_OTHER )
			? "SQL-backend error" : NULL;
		send_ldap_result( op, rs );
		goto modrdn_return;
	}

#ifdef BACKSQL_ARBITRARY_KEY
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"new parent entry id=%s\n", new_pe_id.eid_id.bv_val, 0, 0 );
#else /* ! BACKSQL_ARBITRARY_KEY */
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"new parent entry id=%ld\n", new_pe_id.eid_id, 0, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */

 
	Debug(	LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"executing delentry_query\n", 0, 0, 0 );

	rc = backsql_Prepare( dbh, &sth, bi->delentry_query, 0 );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_modrdn(): "
			"error preparing delentry_query\n", 0, 0, 0 );
		backsql_PrintErrors( bi->db_env, dbh, 
				sth, rc );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		goto done;
	}

#ifdef BACKSQL_ARBITRARY_KEY
	SQLBindParameter( sth, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
			0, 0, e_id.eid_id.bv_val, 0, 0 );
#else /* ! BACKSQL_ARBITRARY_KEY */
	SQLBindParameter( sth, 1, SQL_PARAM_INPUT, SQL_C_ULONG, SQL_INTEGER,
			0, 0, &e_id.eid_id, 0, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */
	rc = SQLExecute( sth );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"failed to delete record from ldap_entries\n",
			0, 0, 0 );
		backsql_PrintErrors( bi->db_env, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "SQL-backend error";
		send_ldap_result( op, rs );
		goto done;
	}

	SQLFreeStmt( sth, SQL_DROP );

	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"executing insentry_query\n", 0, 0, 0 );

	rc = backsql_Prepare( dbh, &sth, bi->insentry_query, 0 );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_modrdn(): "
			"error preparing insentry_query\n", 0, 0, 0 );
		backsql_PrintErrors( bi->db_env, dbh, 
				sth, rc );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		goto done;
	}

	backsql_BindParamStr( sth, 1, new_dn.bv_val, BACKSQL_MAX_DN_LEN );
	SQLBindParameter( sth, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
			0, 0, &e_id.eid_oc_id, 0, 0 );
#ifdef BACKSQL_ARBITRARY_KEY
	SQLBindParameter( sth, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
			0, 0, new_pe_id.eid_id.bv_val, 0, 0 );
	SQLBindParameter( sth, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
			0, 0, e_id.eid_keyval.bv_val, 0, 0 );
#else /* ! BACKSQL_ARBITRARY_KEY */
	SQLBindParameter( sth, 3, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
			0, 0, &new_pe_id.eid_id, 0, 0 );
	SQLBindParameter( sth, 4, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
			0, 0, &e_id.eid_keyval, 0, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */
	rc = SQLExecute( sth );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"could not insert ldap_entries record\n", 0, 0, 0 );
		backsql_PrintErrors( bi->db_env, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "SQL-backend error";
		send_ldap_result( op, rs );
		goto done;
	}
	SQLFreeStmt( sth, SQL_DROP );

	/*
	 * Get attribute type and attribute value of our new rdn,
	 * we will need to add that to our new entry
	 */
	if ( ldap_bv2rdn( &op->oq_modrdn.rs_newrdn, &new_rdn, &next, 
				LDAP_DN_FORMAT_LDAP ) )
	{
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_modrdn: can't figure out "
			"type(s)/values(s) of newrdn\n", 
			0, 0, 0 );
		rs->sr_err = LDAP_INVALID_DN_SYNTAX;
		goto done;
	}

	Debug( LDAP_DEBUG_TRACE,
		"   backsql_modrdn: new_rdn_type=\"%s\", "
		"new_rdn_val=\"%s\"\n",
		new_rdn[ 0 ]->la_attr.bv_val,
		new_rdn[ 0 ]->la_value.bv_val, 0 );

	if ( op->oq_modrdn.rs_deleteoldrdn ) {
		if ( ldap_bv2rdn( &op->o_req_dn, &old_rdn, &next,
					LDAP_DN_FORMAT_LDAP ) )
		{
			Debug( LDAP_DEBUG_TRACE,
				"   backsql_modrdn: can't figure out "
				"the old_rdn type(s)/value(s)\n", 
				0, 0, 0 );
			rs->sr_err = LDAP_OTHER;
			goto done;		
		}
	}

	e.e_name = new_dn;
	e.e_nname = new_ndn;
	rs->sr_err = slap_modrdn2mods( op, rs, &e, old_rdn, new_rdn, &mod );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		goto modrdn_return;
	}

	if ( !acl_check_modlist( op, &e, mod )) {
		rs->sr_err = LDAP_INSUFFICIENT_ACCESS;
		goto modrdn_return;
	}

	oc = backsql_id2oc( bi, e_id.eid_oc_id );
	rs->sr_err = backsql_modify_internal( op, rs, dbh, oc, &e_id, mod );

done:;
	/*
	 * Commit only if all operations succeed
	 */
	if ( rs->sr_err == LDAP_SUCCESS && !op->o_noop ) {
		SQLTransact( SQL_NULL_HENV, dbh, SQL_COMMIT );

	} else {
		SQLTransact( SQL_NULL_HENV, dbh, SQL_ROLLBACK );
	}

modrdn_return:;
	if ( !BER_BVISNULL( &new_dn ) ) {
		slap_sl_free( new_dn.bv_val, op->o_tmpmemctx );
	}
	
	if ( !BER_BVISNULL( &new_ndn ) ) {
		slap_sl_free( new_ndn.bv_val, op->o_tmpmemctx );
	}
	
	/* LDAP v2 supporting correct attribute handling. */
	if ( new_rdn != NULL ) {
		ldap_rdnfree( new_rdn );
	}
	if ( old_rdn != NULL ) {
		ldap_rdnfree( old_rdn );
	}
	if ( mod != NULL ) {
		Modifications *tmp;
		for (; mod; mod = tmp ) {
			tmp = mod->sml_next;
			free( mod );
		}
	}

	if ( new_pe_id.eid_dn.bv_val ) {
		backsql_free_entryID( &new_pe_id, 0 );
	}

	send_ldap_result( op, rs );

	Debug( LDAP_DEBUG_TRACE, "<==backsql_modrdn()\n", 0, 0, 0 );
	return op->o_noop;
}

#endif /* SLAPD_SQL */

