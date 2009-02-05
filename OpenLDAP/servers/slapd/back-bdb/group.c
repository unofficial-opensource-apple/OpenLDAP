/* group.c - bdb backend acl group routine */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-bdb/group.c,v 1.9 2002/01/29 03:53:48 kurt Exp $ */
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


/* return 0 IFF op_dn is a value in member attribute
 * of entry with gr_dn AND that entry has an objectClass
 * value of groupOfNames
 */
int
bdb_group(
	Backend	*be,
	Connection *conn,
	Operation *op,
	Entry	*target,
	struct berval	*gr_ndn,
	struct berval	*op_ndn,
	ObjectClass *group_oc,
	AttributeDescription *group_at
)
{
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	struct bdb_op_info *boi = (struct bdb_op_info *) op->o_private;
	DB_TXN *txn;
	Entry *e;
	int	rc = 1;
	Attribute *attr;

	AttributeDescription *ad_objectClass = slap_schema.si_ad_objectClass;
	const char *group_oc_name = NULL;
	const char *group_at_name = group_at->ad_cname.bv_val;

	if( group_oc->soc_names && group_oc->soc_names[0] ) {
		group_oc_name = group_oc->soc_names[0];
	} else {
		group_oc_name = group_oc->soc_oid;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "backend", LDAP_LEVEL_ENTRY,
		"bdb_group: check (%s) member of (%s), oc %s\n",
		op_ndn->bv_val, gr_ndn->bv_val, group_oc_name ));
#else
	Debug( LDAP_DEBUG_ARGS,
		"=> bdb_group: gr dn: \"%s\"\n",
		gr_ndn->bv_val, 0, 0 ); 

	Debug( LDAP_DEBUG_ARGS,
		"=> bdb_group: op dn: \"%s\"\n",
		op_ndn->bv_val, 0, 0 ); 
	Debug( LDAP_DEBUG_ARGS,
		"=> bdb_group: oc: \"%s\" at: \"%s\"\n", 
		group_oc_name, group_at_name, 0 ); 

	Debug( LDAP_DEBUG_ARGS,
		"=> bdb_group: tr dn: \"%s\"\n",
		target->e_ndn, 0, 0 ); 
#endif

	if( boi != NULL && be == boi->boi_bdb ) {
		txn = boi->boi_txn;
	}

	if (dn_match(&target->e_name, gr_ndn)) {
		/* we already have a LOCKED copy of the entry */
		e = target;
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
			"bdb_group: target is group (%s)\n", gr_ndn->bv_val ));
#else
		Debug( LDAP_DEBUG_ARGS,
			"=> bdb_group: target is group: \"%s\"\n",
			gr_ndn->bv_val, 0, 0 );
#endif
	} else {
		/* can we find group entry */
		rc = bdb_dn2entry_r( be, NULL, gr_ndn, &e, NULL, 0 ); 
		if( rc ) {
			if( txn ) {
				boi->boi_err = rc;
			}
			return( 1 );
		}
		if (e == NULL) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
				"bdb_group: cannot find group (%s)\n",
				gr_ndn->bv_val ));
#else
			Debug( LDAP_DEBUG_ACL,
				"=> bdb_group: cannot find group: \"%s\"\n",
					gr_ndn->bv_val, 0, 0 ); 
#endif
			return( 1 );
		}
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
			"bdb_group: found group (%s)\n", gr_ndn->bv_val ));
#else
		Debug( LDAP_DEBUG_ACL,
			"=> bdb_group: found group: \"%s\"\n",
			gr_ndn->bv_val, 0, 0 ); 
#endif
	}

	/* find it's objectClass and member attribute values
	 * make sure this is a group entry
	 * finally test if we can find op_dn in the member attribute value list
	 */
	rc = 1;

#ifdef BDB_ALIASES
	if( is_entry_alias( e ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_INFO,
			"bdb_group: group (%s) is an alias\n", gr_ndn->bv_val ));
#else
		Debug( LDAP_DEBUG_ACL,
			"<= bdb_group: group is an alias\n", 0, 0, 0 );
#endif
		goto return_results;
	}
#endif

	if( is_entry_referral( e ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_INFO,
			"bdb_group: group (%s) is a referral.\n", gr_ndn->bv_val ));
#else
		Debug( LDAP_DEBUG_ACL,
			"<= bdb_group: group is a referral\n", 0, 0, 0 );
#endif
		goto return_results;
	}

	if( !is_entry_objectclass( e, group_oc, 0 ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_ERR,
			"bdb_group: failed to find %s in objectClass.\n",
			group_oc_name ));
#else
		Debug( LDAP_DEBUG_ACL,
			"<= bdb_group: failed to find %s in objectClass\n", 
				group_oc_name, 0, 0 ); 
#endif
		goto return_results;
	}

	if ((attr = attr_find(e->e_attrs, group_at)) == NULL) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_INFO,
			"bdb_group: failed to find %s\n", group_at_name ));
#else
		Debug( LDAP_DEBUG_ACL,
			"<= bdb_group: failed to find %s\n",
			group_at_name, 0, 0 ); 
#endif
		goto return_results;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "backend", LDAP_LEVEL_ENTRY,
		"bdb_group: found objectClass %s and %s\n",
		group_oc_name, group_at_name ));
#else
	Debug( LDAP_DEBUG_ACL,
		"<= bdb_group: found objectClass %s and %s\n",
		group_oc_name, group_at_name, 0 ); 
#endif

	if( value_find( group_at, attr->a_vals, op_ndn ) != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
			"bdb_group: \"%s\" not in \"%s\": %s\n",
			op_ndn->bv_val, gr_ndn->bv_val, group_at_name ));
#else
		Debug( LDAP_DEBUG_ACL,
			"<= bdb_group: \"%s\" not in \"%s\": %s\n", 
			op_ndn->bv_val, gr_ndn->bv_val, group_at_name ); 
#endif
		goto return_results;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
		"bdb_group: %s is in %s: %s\n",
		op_ndn->bv_val, gr_ndn->bv_val, group_at_name ));
#else
	Debug( LDAP_DEBUG_ACL,
		"<= bdb_group: \"%s\" is in \"%s\": %s\n", 
		op_ndn->bv_val, gr_ndn->bv_val, group_at_name ); 
#endif

	rc = 0;

return_results:
	if( target != e ) {
		/* free entry */
		bdb_cache_return_entry_r( &bdb->bi_cache, e );
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "backend", LDAP_LEVEL_ENTRY,
		"bdb_group: rc=%d\n", rc ));
#else
	Debug( LDAP_DEBUG_TRACE, "bdb_group: rc=%d\n", rc, 0, 0 ); 
#endif

	return(rc);
}

