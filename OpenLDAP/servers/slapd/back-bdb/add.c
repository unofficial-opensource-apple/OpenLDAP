/* add.c - ldap BerkeleyDB back-end add routine */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-bdb/add.c,v 1.55 2002/02/11 20:07:55 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#include "back-bdb.h"
#include "external.h"

int
bdb_add(
	BackendDB	*be,
	Connection	*conn,
	Operation	*op,
	Entry	*e )
{
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	struct berval	pdn;
	Entry		*p = NULL;
	int			rc; 
	const char	*text;
	char textbuf[SLAP_TEXT_BUFLEN];
	size_t textlen = sizeof textbuf;
	AttributeDescription *children = slap_schema.si_ad_children;
	DB_TXN		*ltid = NULL;
	struct bdb_op_info opinfo;
#ifdef BDB_SUBENTRIES
	int subentry;
#endif
#if 0
	u_int32_t	lockid;
	DB_LOCK		lock;
#endif

	Debug(LDAP_DEBUG_ARGS, "==> bdb_add: %s\n", e->e_dn, 0, 0);

	/* check entry's schema */
	rc = entry_schema_check( be, e, NULL, &text, textbuf, textlen );
	if ( rc != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"bdb_add: entry failed schema check: %s (%d)\n",
			text, rc, 0 );
		goto return_results;
	}

#ifdef BDB_SUBENTRIES
	subentry = is_entry_subentry( e );
#endif

	/*
	 * acquire an ID outside of the operation transaction
	 * to avoid serializing adds.
	 */
	rc = bdb_next_id( be, NULL, &e->e_id );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_TRACE,
			"bdb_add: next_id failed (%d)\n",
			rc, 0, 0 );
		rc = LDAP_OTHER;
		text = "internal error";
		goto return_results;
	}

	if( 0 ) {
retry:	/* transaction retry */
		rc = TXN_ABORT( ltid );
		ltid = NULL;
		op->o_private = NULL;
		if( rc != 0 ) {
			rc = LDAP_OTHER;
			text = "internal error";
			goto return_results;
		}
		ldap_pvt_thread_yield();
	}

	/* begin transaction */
	rc = TXN_BEGIN( bdb->bi_dbenv, NULL, &ltid, 
		bdb->bi_db_opflags );
	text = NULL;
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_TRACE,
			"bdb_add: txn_begin failed: %s (%d)\n",
			db_strerror(rc), rc, 0 );
		rc = LDAP_OTHER;
		text = "internal error";
		goto return_results;
	}
#if 0
	lockid = TXN_ID( ltid );
#endif

	opinfo.boi_bdb = be;
	opinfo.boi_txn = ltid;
	opinfo.boi_err = 0;
	op->o_private = &opinfo;
	
	/*
	 * Get the parent dn and see if the corresponding entry exists.
	 * If the parent does not exist, only allow the "root" user to
	 * add the entry.
	 */
	if ( be_issuffix( be, &e->e_nname ) ) {
		pdn = slap_empty_bv;
	} else {
		dnParent( &e->e_nname, &pdn );
	}

	if( pdn.bv_len != 0 ) {
		Entry *matched = NULL;

#if 0
		if ( ltid ) {
			DBT obj;
			obj.data = pdn.bv_val-1;
			obj.size = pdn.bv_len+1;
			rc = LOCK_GET( bdb->bi_dbenv, lockid, 0, &obj,
				DB_LOCK_WRITE, &lock);
		}
#endif

		/* get parent */
		rc = bdb_dn2entry_r( be, ltid, &pdn, &p, &matched, 0 );

		switch( rc ) {
		case 0:
		case DB_NOTFOUND:
			break;
		case DB_LOCK_DEADLOCK:
		case DB_LOCK_NOTGRANTED:
			goto retry;
		default:
			rc = LDAP_OTHER;
			text = "internal error";
			goto return_results;
		}

		if ( p == NULL ) {
			char *matched_dn = NULL;
			BerVarray refs;

			if ( matched != NULL ) {
				matched_dn = ch_strdup( matched->e_dn );
				refs = is_entry_referral( matched )
					? get_entry_referrals( be, conn, op, matched )
					: NULL;
				bdb_cache_return_entry_r(&bdb->bi_cache, matched);
				matched = NULL;

			} else {
				refs = referral_rewrite( default_referral,
					NULL, &e->e_name, LDAP_SCOPE_DEFAULT );
			}

			Debug( LDAP_DEBUG_TRACE, "bdb_add: parent does not exist\n",
				0, 0, 0 );

			send_ldap_result( conn, op, rc = LDAP_REFERRAL,
				matched_dn, NULL, refs, NULL );

			ber_bvarray_free( refs );
			ch_free( matched_dn );

			goto done;
		}

		rc = access_allowed( be, conn, op, p,
			children, NULL, ACL_WRITE, NULL );

		switch( opinfo.boi_err ) {
		case DB_LOCK_DEADLOCK:
		case DB_LOCK_NOTGRANTED:
			/* free parent and reader lock */
			bdb_cache_return_entry_r( &bdb->bi_cache, p );
			p = NULL;
			goto retry;
		}

		if ( ! rc ) {
			Debug( LDAP_DEBUG_TRACE, "bdb_add: no write access to parent\n",
				0, 0, 0 );
			rc = LDAP_INSUFFICIENT_ACCESS;
			text = "no write access to parent";
			goto return_results;;
		}

#ifdef BDB_SUBENTRIES
		if ( is_entry_subentry( p ) ) {
			/* parent is a subentry, don't allow add */
			Debug( LDAP_DEBUG_TRACE, "bdb_add: parent is subentry\n",
				0, 0, 0 );
			rc = LDAP_OBJECT_CLASS_VIOLATION;
			text = "parent is a subentry";
			goto return_results;;
		}
#endif
#ifdef BDB_ALIASES
		if ( is_entry_alias( p ) ) {
			/* parent is an alias, don't allow add */
			Debug( LDAP_DEBUG_TRACE, "bdb_add: parent is alias\n",
				0, 0, 0 );
			rc = LDAP_ALIAS_PROBLEM;
			text = "parent is an alias";
			goto return_results;;
		}
#endif

		if ( is_entry_referral( p ) ) {
			/* parent is a referral, don't allow add */
			char *matched_dn = p->e_dn;
			BerVarray refs = get_entry_referrals( be, conn, op, p );

			Debug( LDAP_DEBUG_TRACE, "bdb_add: parent is referral\n",
				0, 0, 0 );

			send_ldap_result( conn, op, rc = LDAP_REFERRAL,
				matched_dn, NULL, refs, NULL );

			ber_bvarray_free( refs );
			bdb_cache_return_entry_r( &bdb->bi_cache, p );
			p = NULL;
			goto done;
		}

#ifdef BDB_SUBENTRIES
		if ( subentry ) {
			/* FIXME: */
			/* parent must be an administrative point of the required kind */
		}
#endif

		/* free parent and reader lock */
		bdb_cache_return_entry_r( &bdb->bi_cache, p );
		p = NULL;

	} else {
		/*
		 * no parent!
		 *	must be adding entry at suffix or with parent ""
		 */
		if ( !be_isroot( be, &op->o_ndn )) {
			if ( be_issuffix( be, (struct berval *)&slap_empty_bv )
				|| be_isupdate( be, &op->o_ndn ) ) {
				p = (Entry *)&slap_entry_root;

				/* check parent for "children" acl */
				rc = access_allowed( be, conn, op, p,
					children, NULL, ACL_WRITE, NULL );
				p = NULL;

				switch( opinfo.boi_err ) {
				case DB_LOCK_DEADLOCK:
				case DB_LOCK_NOTGRANTED:
					goto retry;
				}

				if ( ! rc ) {
					Debug( LDAP_DEBUG_TRACE,
						"bdb_add: no write access to parent\n",
						0, 0, 0 );
					rc = LDAP_INSUFFICIENT_ACCESS;
					text = "no write access to parent";
					goto return_results;;
				}

			} else {
				Debug( LDAP_DEBUG_TRACE, "bdb_add: %s denied\n",
					pdn.bv_len == 0 ? "suffix" : "entry at root",
					0, 0 );
				rc = LDAP_INSUFFICIENT_ACCESS;
				goto return_results;
			}
		}

#ifdef BDB_SUBENTRIES
		if( subentry ) {
			Debug( LDAP_DEBUG_TRACE,
				"bdb_add: no parent, cannot add subentry\n",
				0, 0, 0 );
			rc = LDAP_INSUFFICIENT_ACCESS;
			text = "no parent, cannot add subentry";
			goto return_results;;
		}
#endif
#if 0
		if ( ltid ) {
			DBT obj;
			obj.data = ",";
			obj.size = 1;
			rc = LOCK_GET( bdb->bi_dbenv, lockid, 0, &obj,
				DB_LOCK_WRITE, &lock);
		}
#endif
	}

	/* dn2id index */
	rc = bdb_dn2id_add( be, ltid, &pdn, e );
	if ( rc != 0 ) {
		Debug( LDAP_DEBUG_TRACE, "bdb_add: dn2id_add failed: %s (%d)\n",
			db_strerror(rc), rc, 0 );

		switch( rc ) {
		case DB_LOCK_DEADLOCK:
		case DB_LOCK_NOTGRANTED:
			goto retry;
		case DB_KEYEXIST:
			rc = LDAP_ALREADY_EXISTS;
			break;
		default:
			rc = LDAP_OTHER;
		}
		goto return_results;
	}

	/* id2entry index */
	rc = bdb_id2entry_add( be, ltid, e );
	if ( rc != 0 ) {
		Debug( LDAP_DEBUG_TRACE, "bdb_add: id2entry_add failed\n",
			0, 0, 0 );
		switch( rc ) {
		case DB_LOCK_DEADLOCK:
		case DB_LOCK_NOTGRANTED:
			goto retry;
		default:
			rc = LDAP_OTHER;
		}
		text = "entry store failed";
		goto return_results;
	}

	/* attribute indexes */
	rc = bdb_index_entry_add( be, ltid, e, e->e_attrs );
	if ( rc != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "bdb_add: index_entry_add failed\n",
			0, 0, 0 );
		switch( rc ) {
		case DB_LOCK_DEADLOCK:
		case DB_LOCK_NOTGRANTED:
			goto retry;
		default:
			rc = LDAP_OTHER;
		}
		text = "index generation failed";
		goto return_results;
	}

	if( op->o_noop ) {
		if (( rc=TXN_ABORT( ltid )) != 0 ) {
			text = "txn_abort (no-op) failed";
		} else {
			rc = LDAP_SUCCESS;
		}

	} else {
		char gid[DB_XIDDATASIZE];

		snprintf( gid, sizeof( gid ), "%s-%08lx-%08lx",
			bdb_uuid.bv_val, (long) op->o_connid, (long) op->o_opid );

		if (( rc=TXN_PREPARE( ltid, gid )) != 0 ) {
			text = "txn_prepare failed";

		} else {
			if ( bdb_cache_add_entry_rw(&bdb->bi_cache,
				e, CACHE_WRITE_LOCK) != 0 )
			{
				if(( rc=TXN_ABORT( ltid )) != 0 ) {
					text = "cache add & txn_abort failed";
				} else {
					rc = LDAP_OTHER;
					text = "cache add failed";
				}
			} else {
				if(( rc=TXN_COMMIT( ltid, 0 )) != 0 ) {
					text = "txn_commit failed";
				} else {
					rc = LDAP_SUCCESS;
				}
			}
		}
	}

	ltid = NULL;
	op->o_private = NULL;

	if (rc == LDAP_SUCCESS) {
		Debug(LDAP_DEBUG_TRACE, "bdb_add: added%s id=%08lx dn=\"%s\"\n",
			op->o_noop ? " (no-op)" : "", e->e_id, e->e_dn );
		text = NULL;
		bdb_cache_entry_commit( e );
	}
	else {
		Debug( LDAP_DEBUG_TRACE, "bdb_add: %s : %s (%d)\n",
			text, db_strerror(rc), rc );
		rc = LDAP_OTHER;
	}

return_results:
	send_ldap_result( conn, op, rc,
		NULL, text, NULL, NULL );

	if( rc == LDAP_SUCCESS && bdb->bi_txn_cp ) {
		ldap_pvt_thread_yield();
		TXN_CHECKPOINT( bdb->bi_dbenv,
			bdb->bi_txn_cp_kbyte, bdb->bi_txn_cp_min, 0 );
	}

done:

	if( ltid != NULL ) {
		TXN_ABORT( ltid );
		op->o_private = NULL;
	}

	return rc;
}
