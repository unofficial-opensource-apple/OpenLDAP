/* attribute.c - bdb backend acl attribute routine */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-bdb/attribute.c,v 1.12 2002/02/09 04:14:18 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>
#include <ac/string.h>

#include "slap.h"
#include "back-bdb.h"
#include "proto-bdb.h"

/* return LDAP_SUCCESS IFF we can retrieve the attributes
 * of entry with e_ndn
 */
int
bdb_attribute(
	Backend	*be,
	Connection *conn,
	Operation *op,
	Entry *target,
	struct berval *entry_ndn,
	AttributeDescription *entry_at,
	BerVarray *vals )
{
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	struct bdb_op_info *boi = (struct bdb_op_info *) op->o_private;
	DB_TXN *txn = NULL;
	Entry *e;
	int	i, j, rc;
	Attribute *attr;
	BerVarray v;
	const char *entry_at_name = entry_at->ad_cname.bv_val;
	AccessControlState acl_state = ACL_STATE_INIT;

#ifdef NEW_LOGGING
	LDAP_LOG(( "backend", LDAP_LEVEL_ARGS,
		"bdb_attribute: gr dn: \"%s\"\n", entry_ndn->bv_val ));
	LDAP_LOG(( "backend", LDAP_LEVEL_ARGS,
		"bdb_attribute: at: \"%s\"\n", entry_at_name));
	LDAP_LOG(( "backend", LDAP_LEVEL_ARGS,
		"bdb_attribute: tr dn: \"%s\"\n",
		target ? target->e_ndn : "" ));
#else
	Debug( LDAP_DEBUG_ARGS,
		"=> bdb_attribute: gr dn: \"%s\"\n",
		entry_ndn->bv_val, 0, 0 ); 
	Debug( LDAP_DEBUG_ARGS,
		"=> bdb_attribute: at: \"%s\"\n", 
		entry_at_name, 0, 0 ); 

	Debug( LDAP_DEBUG_ARGS,
		"=> bdb_attribute: tr dn: \"%s\"\n",
		target ? target->e_ndn : "", 0, 0 ); 
#endif

	if( boi != NULL && be == boi->boi_bdb ) {
		txn = boi->boi_txn;
	}

	if (target != NULL && dn_match(&target->e_nname, entry_ndn)) {
		/* we already have a LOCKED copy of the entry */
		e = target;
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
			"bdb_attribute: target is LOCKED (%s)\n",
			entry_ndn->bv_val ));
#else
		Debug( LDAP_DEBUG_ARGS,
			"=> bdb_attribute: target is entry: \"%s\"\n",
			entry_ndn->bv_val, 0, 0 );
#endif


	} else {
		/* can we find entry */
		rc = bdb_dn2entry_r( be, NULL, entry_ndn, &e, NULL, 0 );
		switch( rc ) {
		case DB_NOTFOUND:
		case 0:
			break;
		default:
			if( txn != NULL ) {
				boi->boi_err = rc;
			}
			return LDAP_OTHER;
		}
		if (e == NULL) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "backend", LDAP_LEVEL_INFO,
				"bdb_attribute: cannot find entry (%s)\n",
				entry_ndn->bv_val ));
#else
			Debug( LDAP_DEBUG_ACL,
				"=> bdb_attribute: cannot find entry: \"%s\"\n",
					entry_ndn->bv_val, 0, 0 ); 
#endif
			return LDAP_NO_SUCH_OBJECT; 
		}
		
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
			"bdb_attribute: found entry (%s)\n",
			entry_ndn->bv_val ));
#else
		Debug( LDAP_DEBUG_ACL,
			"=> bdb_attribute: found entry: \"%s\"\n",
			entry_ndn->bv_val, 0, 0 ); 
#endif
	}

#ifdef BDB_ALIASES
	/* find attribute values */
	if( is_entry_alias( e ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_INFO,
			"bdb_attribute: entry (%s) is an alias\n", e->e_dn ));
#else
		Debug( LDAP_DEBUG_ACL,
			"<= bdb_attribute: entry is an alias\n", 0, 0, 0 );
#endif
		rc = LDAP_ALIAS_PROBLEM;
		goto return_results;
	}
#endif

	if( is_entry_referral( e ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_INFO,
			"bdb_attribute: entry (%s) is a referral.\n", e->e_dn ));
#else
		Debug( LDAP_DEBUG_ACL,
			"<= bdb_attribute: entry is a referral\n", 0, 0, 0 );
#endif
		rc = LDAP_REFERRAL;
		goto return_results;
	}

	if (conn != NULL && op != NULL
		&& access_allowed( be, conn, op, e, slap_schema.si_ad_entry,
			NULL, ACL_READ, &acl_state ) == 0 )
	{
		rc = LDAP_INSUFFICIENT_ACCESS;
		goto return_results;
	}

	if ((attr = attr_find(e->e_attrs, entry_at)) == NULL) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_INFO,
			"bdb_attribute: failed to find %s.\n", entry_at_name ));
#else
		Debug( LDAP_DEBUG_ACL,
			"<= bdb_attribute: failed to find %s\n",
			entry_at_name, 0, 0 ); 
#endif
		rc = LDAP_NO_SUCH_ATTRIBUTE;
		goto return_results;
	}

	if (conn != NULL && op != NULL
		&& access_allowed( be, conn, op, e, entry_at, NULL, ACL_READ, 
			   &acl_state ) == 0 )
	{
		rc = LDAP_INSUFFICIENT_ACCESS;
		goto return_results;
	}

	for ( i = 0; attr->a_vals[i].bv_val != NULL; i++ ) {
		/* count them */
	}

	v = (BerVarray) ch_malloc( sizeof(struct berval) * (i+1) );

	for ( i=0, j=0; attr->a_vals[i].bv_val != NULL; i++ ) {
		if( conn != NULL
			&& op != NULL
			&& access_allowed(be, conn, op, e, entry_at,
				&attr->a_vals[i], ACL_READ, &acl_state ) == 0)
		{
			continue;
		}
		ber_dupbv( &v[j], &attr->a_vals[i] );

		if( v[j].bv_val != NULL ) j++;
	}

	if( j == 0 ) {
		ch_free( v );
		*vals = NULL;
		rc = LDAP_INSUFFICIENT_ACCESS;
	} else {
		v[j].bv_val = NULL;
		v[j].bv_len = 0;
		*vals = v;
		rc = LDAP_SUCCESS;
	}

return_results:
	if( target != e ) {
		/* free entry */
		bdb_cache_return_entry_r(&bdb->bi_cache, e);
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "backend", LDAP_LEVEL_ENTRY,
		"bdb_attribute: rc=%d nvals=%d.\n",
		rc, j ));
#else
	Debug( LDAP_DEBUG_TRACE,
		"bdb_attribute: rc=%d nvals=%d\n",
		rc, j, 0 ); 
#endif
	return(rc);
}
