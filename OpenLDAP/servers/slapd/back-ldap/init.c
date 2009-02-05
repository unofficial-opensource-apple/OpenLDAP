/* init.c - initialize ldap backend */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-ldap/init.c,v 1.43.2.13 2004/11/17 21:53:54 ando Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2003-2004 The OpenLDAP Foundation.
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

#if SLAPD_LDAP == SLAPD_MOD_DYNAMIC

int init_module(int argc, char *argv[]) {
    BackendInfo bi;

    memset( &bi, '\0', sizeof(bi) );
    bi.bi_type = "ldap";
    bi.bi_init = ldap_back_initialize;

    backend_add(&bi);
    return 0;
}

#endif /* SLAPD_LDAP */

int
ldap_back_open(
	BackendInfo *bi
)
{
	bi->bi_controls = slap_known_controls;
	return 0;
}

int
ldap_back_initialize(
    BackendInfo	*bi
)
{
	bi->bi_open = ldap_back_open;
	bi->bi_config = 0;
	bi->bi_close = 0;
	bi->bi_destroy = 0;

	bi->bi_db_init = ldap_back_db_init;
	bi->bi_db_config = ldap_back_db_config;
	bi->bi_db_open = 0;
	bi->bi_db_close = 0;
	bi->bi_db_destroy = ldap_back_db_destroy;

	bi->bi_op_bind = ldap_back_bind;
	bi->bi_op_unbind = 0;
	bi->bi_op_search = ldap_back_search;
	bi->bi_op_compare = ldap_back_compare;
	bi->bi_op_modify = ldap_back_modify;
	bi->bi_op_modrdn = ldap_back_modrdn;
	bi->bi_op_add = ldap_back_add;
	bi->bi_op_delete = ldap_back_delete;
	bi->bi_op_abandon = 0;

	bi->bi_extended = ldap_back_extended;

	bi->bi_chk_referrals = 0;
	bi->bi_entry_get_rw = ldap_back_entry_get;

	bi->bi_connection_init = 0;
	bi->bi_connection_destroy = ldap_back_conn_destroy;

	ldap_chain_setup();

	return 0;
}

int
ldap_back_db_init(
    Backend	*be
)
{
	struct ldapinfo	*li;
	struct ldapmapping *mapping;

	li = (struct ldapinfo *) ch_calloc( 1, sizeof(struct ldapinfo) );
	if ( li == NULL ) {
 		return -1;
 	}

	li->binddn.bv_val = NULL;
	li->binddn.bv_len = 0;
	li->bindpw.bv_val = NULL;
	li->bindpw.bv_len = 0;

#ifdef LDAP_BACK_PROXY_AUTHZ
	li->proxyauthzdn.bv_val = NULL;
	li->proxyauthzdn.bv_len = 0;
	li->proxyauthzpw.bv_val = NULL;
	li->proxyauthzpw.bv_len = 0;
#endif /* LDAP_BACK_PROXY_AUTHZ */

#ifdef ENABLE_REWRITE
 	li->rwmap.rwm_rw = rewrite_info_init( REWRITE_MODE_USE_DEFAULT );
	if ( li->rwmap.rwm_rw == NULL ) {
 		ch_free( li );
 		return -1;
 	}

	{
		char	*rargv[3];

		/*
		 * the filter rewrite as a string must be disabled
		 * by default; it can be re-enabled by adding rules;
		 * this creates an empty rewriteContext
		 */
		rargv[ 0 ] = "rewriteContext";
		rargv[ 1 ] = "searchFilter";
		rargv[ 2 ] = NULL;
		rewrite_parse( li->rwmap.rwm_rw, "<suffix massage>", 
				1, 2, rargv );

		rargv[ 0 ] = "rewriteContext";
		rargv[ 1 ] = "default";
		rargv[ 2 ] = NULL;
		rewrite_parse( li->rwmap.rwm_rw, "<suffix massage>", 
				1, 2, rargv );
	}
#endif /* ENABLE_REWRITE */

	ldap_pvt_thread_mutex_init( &li->conn_mutex );

	ldap_back_map_init( &li->rwmap.rwm_oc, &mapping );
	ldap_back_map_init( &li->rwmap.rwm_at, &mapping );

	be->be_private = li;
	SLAP_DBFLAGS(be) |= SLAP_DBFLAG_NOLASTMOD;

	return 0;
}

void
ldap_back_conn_free( 
	void *v_lc
)
{
	struct ldapconn *lc = v_lc;
	ldap_unbind( lc->ld );
	if ( lc->bound_dn.bv_val ) {
		ch_free( lc->bound_dn.bv_val );
	}
	if ( lc->cred.bv_val ) {
		memset( lc->cred.bv_val, 0, lc->cred.bv_len );
		ch_free( lc->cred.bv_val );
	}
	if ( lc->local_dn.bv_val ) {
		ch_free( lc->local_dn.bv_val );
	}
	ldap_pvt_thread_mutex_destroy( &lc->lc_mutex );
	ch_free( lc );
}

void
mapping_free( void *v_mapping )
{
	struct ldapmapping *mapping = v_mapping;
	ch_free( mapping->src.bv_val );
	ch_free( mapping->dst.bv_val );
	ch_free( mapping );
}

int
ldap_back_db_destroy(
    Backend	*be
)
{
	struct ldapinfo	*li;

	if (be->be_private) {
		li = (struct ldapinfo *)be->be_private;

		ldap_pvt_thread_mutex_lock( &li->conn_mutex );

		if (li->url) {
			ch_free(li->url);
			li->url = NULL;
		}
		if ( li->lud ) {
			ldap_free_urldesc( li->lud );
			li->lud = NULL;
		}
		if (li->binddn.bv_val) {
			ch_free(li->binddn.bv_val);
			li->binddn.bv_val = NULL;
		}
		if (li->bindpw.bv_val) {
			ch_free(li->bindpw.bv_val);
			li->bindpw.bv_val = NULL;
		}
#ifdef LDAP_BACK_PROXY_AUTHZ
		if (li->proxyauthzdn.bv_val) {
			ch_free(li->proxyauthzdn.bv_val);
			li->proxyauthzdn.bv_val = NULL;
		}
		if (li->proxyauthzpw.bv_val) {
			ch_free(li->proxyauthzpw.bv_val);
			li->proxyauthzpw.bv_val = NULL;
		}
#endif /* LDAP_BACK_PROXY_AUTHZ */
                if (li->conntree) {
			avl_free( li->conntree, ldap_back_conn_free );
		}
#ifdef ENABLE_REWRITE
		if (li->rwmap.rwm_rw) {
			rewrite_info_delete( &li->rwmap.rwm_rw );
		}
#else /* !ENABLE_REWRITE */
		if (li->rwmap.rwm_suffix_massage) {
  			ber_bvarray_free( li->rwmap.rwm_suffix_massage );
 		}
#endif /* !ENABLE_REWRITE */

		avl_free( li->rwmap.rwm_oc.remap, NULL );
		avl_free( li->rwmap.rwm_oc.map, mapping_free );
		avl_free( li->rwmap.rwm_at.remap, NULL );
		avl_free( li->rwmap.rwm_at.map, mapping_free );
		
		ldap_pvt_thread_mutex_unlock( &li->conn_mutex );
		ldap_pvt_thread_mutex_destroy( &li->conn_mutex );
	}

	ch_free( be->be_private );
	return 0;
}
