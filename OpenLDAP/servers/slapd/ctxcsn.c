/* ctxcsn.c -- Context CSN Management Routines */
/* $OpenLDAP: pkg/ldap/servers/slapd/ctxcsn.c,v 1.9.2.10 2004/08/30 17:47:12 kurt Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2003-2004 The OpenLDAP Foundation.
 * Portions Copyright 2003 IBM Corporation.
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

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "ldap_pvt.h"
#include "lutil.h"
#include "slap.h"
#include "lutil_ldap.h"

const struct berval slap_ldapsync_bv = BER_BVC("ldapsync");
const struct berval slap_ldapsync_cn_bv = BER_BVC("cn=ldapsync");

void
slap_get_commit_csn( Operation *op, struct berval *csn )
{
	struct slap_csn_entry *csne, *committed_csne = NULL;

	csn->bv_val = NULL;
	csn->bv_len = 0;

	ldap_pvt_thread_mutex_lock( op->o_bd->be_pcl_mutexp );

	LDAP_TAILQ_FOREACH( csne, op->o_bd->be_pending_csn_list, ce_csn_link ) {
		if ( csne->ce_opid == op->o_opid && csne->ce_connid == op->o_connid ) {
			csne->ce_state = SLAP_CSN_COMMIT;
			break;
		}
	}

	LDAP_TAILQ_FOREACH( csne, op->o_bd->be_pending_csn_list, ce_csn_link ) {
		if ( csne->ce_state == SLAP_CSN_COMMIT ) committed_csne = csne;
		if ( csne->ce_state == SLAP_CSN_PENDING ) break;
	}

	if ( committed_csne ) ber_dupbv_x( csn, committed_csne->ce_csn, op->o_tmpmemctx );
	ldap_pvt_thread_mutex_unlock( op->o_bd->be_pcl_mutexp );
}

void
slap_rewind_commit_csn( Operation *op )
{
	struct slap_csn_entry *csne;

	ldap_pvt_thread_mutex_lock( op->o_bd->be_pcl_mutexp );

	LDAP_TAILQ_FOREACH( csne, op->o_bd->be_pending_csn_list, ce_csn_link ) {
		if ( csne->ce_opid == op->o_opid && csne->ce_connid == op->o_connid ) {
			csne->ce_state = SLAP_CSN_PENDING;
			break;
		}
	}

	ldap_pvt_thread_mutex_unlock( op->o_bd->be_pcl_mutexp );
}

void
slap_graduate_commit_csn( Operation *op )
{
	struct slap_csn_entry *csne;

	if ( op == NULL ) return;
	if ( op->o_bd == NULL ) return;

	ldap_pvt_thread_mutex_lock( op->o_bd->be_pcl_mutexp );

	LDAP_TAILQ_FOREACH( csne, op->o_bd->be_pending_csn_list, ce_csn_link ) {
		if ( csne->ce_opid == op->o_opid && csne->ce_connid == op->o_connid ) {
			LDAP_TAILQ_REMOVE( op->o_bd->be_pending_csn_list,
				csne, ce_csn_link );
			ch_free( csne->ce_csn->bv_val );
			ch_free( csne->ce_csn );
			ch_free( csne );
			break;
		}
	}

	ldap_pvt_thread_mutex_unlock( op->o_bd->be_pcl_mutexp );

	return;
}

static struct berval ocbva[] = {
	BER_BVC("top"),
	BER_BVC("subentry"),
	BER_BVC("syncProviderSubentry"),
	{0,NULL}
};

Entry *
slap_create_context_csn_entry(
	Backend *be,
	struct berval *context_csn )
{
	Entry* e;

	struct berval bv;

	e = (Entry *) ch_calloc( 1, sizeof( Entry ));

	attr_merge( e, slap_schema.si_ad_objectClass,
		ocbva, NULL );
	attr_merge_one( e, slap_schema.si_ad_structuralObjectClass,
		&ocbva[1], NULL );
	attr_merge_one( e, slap_schema.si_ad_cn,
		(struct berval *)&slap_ldapsync_bv, NULL );

	if ( context_csn ) {
		attr_merge_one( e, slap_schema.si_ad_contextCSN,
			context_csn, NULL );
	}

	bv.bv_val = "{}";
	bv.bv_len = sizeof("{}")-1;
	attr_merge_one( e, slap_schema.si_ad_subtreeSpecification, &bv, NULL );

	build_new_dn( &e->e_name, &be->be_nsuffix[0],
		(struct berval *)&slap_ldapsync_cn_bv, NULL );
	ber_dupbv( &e->e_nname, &e->e_name );

	return e;
}

int
slap_get_csn(
	Operation *op,
	char *csnbuf,
	int	len,
	struct berval *csn,
	int manage_ctxcsn )
{
	struct slap_csn_entry *pending;

	if ( csn == NULL ) return LDAP_OTHER;

	csn->bv_len = lutil_csnstr( csnbuf, len, 0, 0 );
	csn->bv_val = csnbuf;

	if ( manage_ctxcsn ) {
		pending = (struct slap_csn_entry *) ch_calloc( 1,
			sizeof( struct slap_csn_entry ));
		ldap_pvt_thread_mutex_lock( op->o_bd->be_pcl_mutexp );
		ber_dupbv( &op->o_sync_csn, csn );
		pending->ce_csn = ber_dupbv( NULL, csn );
		pending->ce_connid = op->o_connid;
		pending->ce_opid = op->o_opid;
		pending->ce_state = SLAP_CSN_PENDING;
		LDAP_TAILQ_INSERT_TAIL( op->o_bd->be_pending_csn_list,
			pending, ce_csn_link );
		ldap_pvt_thread_mutex_unlock( op->o_bd->be_pcl_mutexp );
	}

	return LDAP_SUCCESS;
}
