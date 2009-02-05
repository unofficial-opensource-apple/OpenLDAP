/* backover.c - backend overlay routines */
/* $OpenLDAP: pkg/ldap/servers/slapd/backover.c,v 1.6.2.11 2004/07/19 18:31:41 hyc Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2003-2004 The OpenLDAP Foundation.
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

/* Functions to overlay other modules over a backend. */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#define SLAPD_TOOLS
#include "slap.h"

static slap_overinst *overlays;

enum db_which { db_open = 0, db_close, db_destroy };

static int
over_db_func(
	BackendDB *be,
	enum db_which which
)
{
	slap_overinfo *oi = be->bd_info->bi_private;
	slap_overinst *on = oi->oi_list;
	BackendInfo *bi_orig = be->bd_info;
	BI_db_open **func;
	int rc = 0;

	func = &oi->oi_orig->bi_db_open;
	if ( func[which] ) {
		be->bd_info = oi->oi_orig;
		rc = func[which]( be );
	}

	for (; on && rc == 0; on=on->on_next) {
		be->bd_info = &on->on_bi;
		func = &on->on_bi.bi_db_open;
		if (func[which]) {
			rc = func[which]( be );
		}
	}
	be->bd_info = bi_orig;
	return rc;
}

static int
over_db_config(
	BackendDB *be,
	const char *fname,
	int lineno,
	int argc,
	char **argv
)
{
	slap_overinfo *oi = be->bd_info->bi_private;
	slap_overinst *on = oi->oi_list;
	BackendInfo *bi_orig = be->bd_info;
	int rc = 0;

	if ( oi->oi_orig->bi_db_config ) {
		be->bd_info = oi->oi_orig;
		rc = oi->oi_orig->bi_db_config( be, fname, lineno,
			argc, argv );
		be->bd_info = (BackendInfo *)oi;
		if ( rc != SLAP_CONF_UNKNOWN ) return rc;
	}

	for (; on; on=on->on_next) {
		if (on->on_bi.bi_db_config) {
			be->bd_info = &on->on_bi;
			rc = on->on_bi.bi_db_config( be, fname, lineno,
				argc, argv );
			if ( rc != SLAP_CONF_UNKNOWN ) break;
		}
	}
	be->bd_info = bi_orig;
	return rc;
}

static int
over_db_open(
	BackendDB *be
)
{
	return over_db_func( be, db_open );
}

static int
over_db_close(
	BackendDB *be
)
{
	return over_db_func( be, db_close );
}

static int
over_db_destroy(
	BackendDB *be
)
{
	slap_overinfo *oi = be->bd_info->bi_private;
	slap_overinst *on = oi->oi_list, *next;
	int rc;

	rc = over_db_func( be, db_destroy );

	for (next = on->on_next; on; on=next) {
		next = on->on_next;
		free( on );
	}
	free( oi );
	return rc;
}

static int
over_back_response ( Operation *op, SlapReply *rs )
{
	slap_overinfo *oi = op->o_callback->sc_private;
	slap_overinst *on = oi->oi_list;
	int rc = SLAP_CB_CONTINUE;
	BackendDB *be = op->o_bd, db = *op->o_bd;

	op->o_bd = &db;
	for (; on; on=on->on_next ) {
		if ( on->on_response ) {
			db.bd_info = (BackendInfo *)on;
			rc = on->on_response( op, rs );
			if ( rc != SLAP_CB_CONTINUE ) break;
		}
	}
	op->o_bd = be;
	return rc;
}

enum op_which {
	op_bind = 0,
	op_unbind,
	op_search,
	op_compare,
	op_modify,
	op_modrdn,
	op_add,
	op_delete,
	op_abandon,
	op_cancel,
	op_extended,
	op_aux_chk_referrals,
	op_last
};

/*
 * default return code in case of missing backend function
 * and overlay stack returning SLAP_CB_CONTINUE
 */
static int op_rc[] = {
	LDAP_UNWILLING_TO_PERFORM,	/* bind */
	LDAP_UNWILLING_TO_PERFORM,	/* unbind */
	LDAP_UNWILLING_TO_PERFORM,	/* search */
	LDAP_UNWILLING_TO_PERFORM,	/* compare */
	LDAP_UNWILLING_TO_PERFORM,	/* modify */
	LDAP_UNWILLING_TO_PERFORM,	/* modrdn */
	LDAP_UNWILLING_TO_PERFORM,	/* add */
	LDAP_UNWILLING_TO_PERFORM,	/* delete */
	LDAP_UNWILLING_TO_PERFORM,	/* abandon */
	LDAP_UNWILLING_TO_PERFORM,	/* cancel */
	LDAP_UNWILLING_TO_PERFORM,	/* extended */
	LDAP_SUCCESS			/* aux_chk_referrals */
};

static int
over_op_func(
	Operation *op,
	SlapReply *rs,
	enum op_which which
)
{
	slap_overinfo *oi = op->o_bd->bd_info->bi_private;
	slap_overinst *on = oi->oi_list;
	BI_op_bind **func;
	BackendDB *be = op->o_bd, db = *op->o_bd;
	slap_callback cb = {NULL, over_back_response, NULL, NULL};
	int rc = SLAP_CB_CONTINUE;

	op->o_bd = &db;
	cb.sc_next = op->o_callback;
	cb.sc_private = oi;
	op->o_callback = &cb;

	for (; on; on=on->on_next ) {
		func = &on->on_bi.bi_op_bind;
		if ( func[which] ) {
			db.bd_info = (BackendInfo *)on;
			rc = func[which]( op, rs );
			if ( rc != SLAP_CB_CONTINUE ) break;
		}
	}

	func = &oi->oi_orig->bi_op_bind;
	if ( func[which] && rc == SLAP_CB_CONTINUE ) {
		db.bd_info = oi->oi_orig;
		rc = func[which]( op, rs );
	}
	/* should not fall thru this far without anything happening... */
	if ( rc == SLAP_CB_CONTINUE ) {
		rc = op_rc[ which ];
	}
	op->o_bd = be;
	op->o_callback = cb.sc_next;
	return rc;
}

static int
over_op_bind( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_bind );
}

static int
over_op_unbind( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_unbind );
}

static int
over_op_search( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_search );
}

static int
over_op_compare( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_compare );
}

static int
over_op_modify( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_modify );
}

static int
over_op_modrdn( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_modrdn );
}

static int
over_op_add( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_add );
}

static int
over_op_delete( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_delete );
}

static int
over_op_abandon( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_abandon );
}

static int
over_op_cancel( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_cancel );
}

static int
over_op_extended( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_extended );
}

static int
over_chk_referrals( Operation *op, SlapReply *rs )
{
	return over_op_func( op, rs, op_aux_chk_referrals );
}

int
overlay_register(
	slap_overinst *on
)
{
	on->on_next = overlays;
	overlays = on;
	return 0;
}

slap_overinst *
overlay_next(
	slap_overinst *on
)
{
	if ( on == NULL ) {
		return overlays;
	}

	return on->on_next;
}

static const char overtype[] = "over";

/* add an overlay to a particular backend. */
int
overlay_config( BackendDB *be, const char *ov )
{
	slap_overinst *on = NULL, *on2 = NULL;
	slap_overinfo *oi = NULL;
	BackendInfo *bi = NULL;

	for ( on = overlays; on; on=on->on_next ) {
		if (!strcmp( ov, on->on_bi.bi_type ) )
			break;
	}
	if (!on) {
		Debug( LDAP_DEBUG_ANY, "overlay %s not found\n", ov, 0, 0 );
		return 1;
	}

	/* If this is the first overlay on this backend, set up the
	 * overlay info structure
	 */
	if ( be->bd_info->bi_type != overtype ) {
		oi = ch_malloc( sizeof(slap_overinfo) );
		oi->oi_orig = be->bd_info;
		oi->oi_bi = *be->bd_info;

		/* Save a pointer to ourself in bi_private.
		 * This allows us to keep working in conjunction
		 * with backglue...
		 */
		oi->oi_bi.bi_private = oi;
		oi->oi_list = NULL;
		bi = (BackendInfo *)oi;

		bi->bi_type = (char *)overtype;

		bi->bi_db_config = over_db_config;
		bi->bi_db_open = over_db_open;
		bi->bi_db_close = over_db_close;
		bi->bi_db_destroy = over_db_destroy;

		bi->bi_op_bind = over_op_bind;
		bi->bi_op_unbind = over_op_unbind;
		bi->bi_op_search = over_op_search;
		bi->bi_op_compare = over_op_compare;
		bi->bi_op_modify = over_op_modify;
		bi->bi_op_modrdn = over_op_modrdn;
		bi->bi_op_add = over_op_add;
		bi->bi_op_delete = over_op_delete;
		bi->bi_op_abandon = over_op_abandon;
		bi->bi_op_cancel = over_op_cancel;

		bi->bi_extended = over_op_extended;

		/*
		 * this is fine because it has the same
		 * args of the operations; we need to rework
		 * all the hooks to share the same args
		 * of the operations...
		 */
		bi->bi_chk_referrals = over_chk_referrals;

		be->bd_info = bi;

	} else {
		oi = be->bd_info->bi_private;
	}

	/* Insert new overlay on head of list. Overlays are executed
	 * in reverse of config order...
	 */
	on2 = ch_calloc( 1, sizeof(slap_overinst) );
	*on2 = *on;
	on2->on_info = oi;
	on2->on_next = oi->oi_list;
	oi->oi_list = on2;

	/* Any initialization needed? */
	if ( on->on_bi.bi_db_init ) {
		be->bd_info = (BackendInfo *)on2;
		on2->on_bi.bi_db_init( be );
		be->bd_info = (BackendInfo *)oi;
	}

	return 0;
}

