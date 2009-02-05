/* backglue.c - backend glue routines */
/* $OpenLDAP: pkg/ldap/servers/slapd/backglue.c,v 1.38.2.1 2002/02/23 23:24:44 kurt Exp $ */
/*
 * Copyright 2001-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

/*
 * Functions to glue a bunch of other backends into a single tree.
 * All of the glued backends must share a common suffix. E.g., you
 * can glue o=foo and ou=bar,o=foo but you can't glue o=foo and o=bar.
 *
 * This uses the backend structures and routines extensively, but is
 * not an actual backend of its own. To use it you must add a "subordinate"
 * keyword to the configuration of other backends. Subordinates will
 * automatically be connected to their parent backend.
 *
 * The purpose of these functions is to allow you to split a single database
 * into pieces (for load balancing purposes, whatever) but still be able
 * to treat it as a single database after it's been split. As such, each
 * of the glued backends should have identical rootdn and rootpw.
 *
 * If you need more elaborate configuration, you probably should be using
 * back-meta instead.
 *  -- Howard Chu
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>

#define SLAPD_TOOLS
#include "slap.h"

typedef struct gluenode {
	BackendDB *be;
	struct berval pdn;
} gluenode;

typedef struct glueinfo {
	BackendDB *be;
	int nodes;
	gluenode n[1];
} glueinfo;

static int glueMode;
static BackendDB *glueBack;

/* Just like select_backend, but only for our backends */
static BackendDB *
glue_back_select (
	BackendDB *be,
	const char *dn
)
{
	glueinfo *gi = (glueinfo *) be->be_private;
	struct berval bv;
	int i;

	bv.bv_len = strlen(dn);
	bv.bv_val = (char *) dn;

	for (i = 0; i<gi->nodes; i++) {
		if (dnIsSuffix(&bv, gi->n[i].be->be_nsuffix[0])) {
			return gi->n[i].be;
		}
	}
	return NULL;
}

/* This function will only be called in tool mode */
static int
glue_back_open (
	BackendInfo *bi
)
{
	int rc = 0;
	static int glueOpened = 0;

	if (glueOpened) return 0;

	glueOpened = 1;

	/* If we were invoked in tool mode, open all the underlying backends */
	if (slapMode & SLAP_TOOL_MODE) {
		rc = backend_startup (NULL);
	} /* other case is impossible */
	return rc;
}

/* This function will only be called in tool mode */
static int
glue_back_close (
	BackendInfo *bi
)
{
	static int glueClosed = 0;
	int rc;

	if (glueClosed) return 0;

	glueClosed = 1;

	if (slapMode & SLAP_TOOL_MODE) {
		rc = backend_shutdown (NULL);
	}
	return rc;
}

static int
glue_back_db_open (
	BackendDB *be
)
{
	glueinfo *gi = (glueinfo *)be->be_private;
	static int glueOpened = 0;
	int rc = 0;

	if (glueOpened) return 0;

	glueOpened = 1;

	gi->be->be_acl = be->be_acl;

	if (gi->be->bd_info->bi_db_open)
		rc = gi->be->bd_info->bi_db_open(gi->be);

	return rc;
}

static int
glue_back_db_close (
	BackendDB *be
)
{
	glueinfo *gi = (glueinfo *)be->be_private;
	static int glueClosed = 0;

	if (glueClosed) return 0;

	glueClosed = 1;

	/* Close the master */
	if (gi->be->bd_info->bi_db_close)
		gi->be->bd_info->bi_db_close( gi->be );

	return 0;
}

static int
glue_back_db_destroy (
	BackendDB *be
)
{
	glueinfo *gi = (glueinfo *)be->be_private;

	if (gi->be->bd_info->bi_db_destroy)
		gi->be->bd_info->bi_db_destroy( gi->be );
	free (gi->be);
	free (gi);
	return 0;
}

typedef struct glue_state {
	int err;
	int nentries;
	int matchlen;
	char *matched;
	int nrefs;
	BerVarray refs;
	slap_callback *prevcb;
} glue_state;

static void
glue_back_response (
	Connection *conn,
	Operation *op,
	ber_tag_t tag,
	ber_int_t msgid,
	ber_int_t err,
	const char *matched,
	const char *text,
	BerVarray ref,
	const char *resoid,
	struct berval *resdata,
	struct berval *sasldata,
	LDAPControl **ctrls
)
{
	glue_state *gs = op->o_callback->sc_private;

	if (err == LDAP_SUCCESS || gs->err != LDAP_SUCCESS)
		gs->err = err;
	if (gs->err == LDAP_SUCCESS && gs->matched) {
		free (gs->matched);
		gs->matchlen = 0;
	}
	if (gs->err != LDAP_SUCCESS && matched) {
		int len;
		len = strlen (matched);
		if (len > gs->matchlen) {
			if (gs->matched)
				free (gs->matched);
			gs->matched = ch_strdup (matched);
			gs->matchlen = len;
		}
	}
	if (ref) {
		int i, j, k;
		BerVarray new;

		for (i=0; ref[i].bv_val; i++);

		j = gs->nrefs;
		if (!j) {
			new = ch_malloc ((i+1)*sizeof(struct berval));
		} else {
			new = ch_realloc(gs->refs,
				(j+i+1)*sizeof(struct berval));
		}
		for (k=0; k<i; j++,k++) {
			ber_dupbv( &new[j], &ref[k] );
		}
		new[j].bv_val = NULL;
		gs->nrefs = j;
		gs->refs = new;
	}
}

static void
glue_back_sresult (
	Connection *c,
	Operation *op,
	ber_int_t err,
	const char *matched,
	const char *text,
	BerVarray refs,
	LDAPControl **ctrls,
	int nentries
)
{
	glue_state *gs = op->o_callback->sc_private;

	gs->nentries += nentries;
	glue_back_response (c, op, 0, 0, err, matched, text, refs,
			    NULL, NULL, NULL, ctrls);
}

static int
glue_back_sendentry (
	BackendDB *be,
	Connection *c,
	Operation *op,
	Entry *e,
	AttributeName *an,
	int ao,
	LDAPControl **ctrls
)
{
	slap_callback *tmp = op->o_callback;
	glue_state *gs = tmp->sc_private;
	int rc;

	op->o_callback = gs->prevcb;
	if (op->o_callback && op->o_callback->sc_sendentry) {
		rc = op->o_callback->sc_sendentry(be, c, op, e, an, ao, ctrls);
	} else {
		rc = send_search_entry(be, c, op, e, an, ao, ctrls);
	}
	op->o_callback = tmp;
	return rc;
}

static int
glue_back_search (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	struct berval *dn,
	struct berval *ndn,
	int scope,
	int deref,
	int slimit,
	int tlimit,
	Filter *filter,
	struct berval *filterstr,
	AttributeName *attrs,
	int attrsonly
)
{
	glueinfo *gi = (glueinfo *)b0->be_private;
	BackendDB *be;
	int i, rc, t2limit = 0, s2limit = 0;
	long stoptime = 0;
	struct berval bv;
	glue_state gs = {0};
	slap_callback cb;

	cb.sc_response = glue_back_response;
	cb.sc_sresult = glue_back_sresult;
	cb.sc_sendentry = glue_back_sendentry;
	cb.sc_private = &gs;

	gs.prevcb = op->o_callback;

	if (tlimit) {
		stoptime = slap_get_time () + tlimit;
	}

	switch (scope) {
	case LDAP_SCOPE_BASE:
		be = glue_back_select (b0, ndn->bv_val);

		if (be && be->be_search) {
			rc = be->be_search (be, conn, op, dn, ndn, scope,
				   deref, slimit, tlimit, filter, filterstr,
					    attrs, attrsonly);
		} else {
			rc = LDAP_UNWILLING_TO_PERFORM;
			send_ldap_result (conn, op, rc, NULL,
				      "No search target found", NULL, NULL);
		}
		return rc;

	case LDAP_SCOPE_ONELEVEL:
	case LDAP_SCOPE_SUBTREE:
		op->o_callback = &cb;

		/*
		 * Execute in reverse order, most general first 
		 */
		for (i = gi->nodes-1; i >= 0; i--) {
			if (!gi->n[i].be || !gi->n[i].be->be_search)
				continue;
			if (tlimit) {
				t2limit = stoptime - slap_get_time ();
				if (t2limit <= 0)
					break;
			}
			if (slimit) {
				s2limit = slimit - gs.nentries;
				if (s2limit <= 0)
					break;
			}
			/*
			 * check for abandon 
			 */
			ldap_pvt_thread_mutex_lock (&op->o_abandonmutex);
			rc = op->o_abandon;
			ldap_pvt_thread_mutex_unlock (&op->o_abandonmutex);
			if (rc) {
				rc = 0;
				goto done;
			}
			be = gi->n[i].be;
			if (scope == LDAP_SCOPE_ONELEVEL && 
				dn_match(&gi->n[i].pdn, ndn)) {
				rc = be->be_search (be, conn, op,
					be->be_suffix[0], be->be_nsuffix[0],
					LDAP_SCOPE_BASE, deref,
					s2limit, t2limit, filter, filterstr,
					attrs, attrsonly);

			} else if (scope == LDAP_SCOPE_SUBTREE &&
				dnIsSuffix(be->be_nsuffix[0], ndn)) {
				rc = be->be_search (be, conn, op,
					be->be_suffix[0], be->be_nsuffix[0],
					scope, deref,
					s2limit, t2limit, filter, filterstr,
					attrs, attrsonly);

			} else if (dnIsSuffix(&bv, be->be_nsuffix[0])) {
				rc = be->be_search (be, conn, op, dn, ndn,
					scope, deref,
					s2limit, t2limit, filter, filterstr,
					attrs, attrsonly);
			}
		}
		break;
	}
	op->o_callback = gs.prevcb;

	send_search_result (conn, op, gs.err, gs.matched, NULL,
		gs.refs, NULL, gs.nentries);

done:
	if (gs.matched)
		free (gs.matched);
	if (gs.refs)
		ber_bvarray_free(gs.refs);
	return rc;
}

static int
glue_back_bind (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	struct berval *dn,
	struct berval *ndn,
	int method,
	struct berval *cred,
	struct berval *edn
)
{
	BackendDB *be;
	int rc;
 
	be = glue_back_select (b0, ndn->bv_val);

	if (be && be->be_bind) {
		conn->c_authz_backend = be;
		rc = be->be_bind (be, conn, op, dn, ndn, method, cred, edn);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No bind target found",
				  NULL, NULL);
	}
	return rc;
}

static int
glue_back_compare (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	struct berval *dn,
	struct berval *ndn,
	AttributeAssertion *ava
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn->bv_val);

	if (be && be->be_compare) {
		rc = be->be_compare (be, conn, op, dn, ndn, ava);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No compare target found",
			NULL, NULL);
	}
	return rc;
}

static int
glue_back_modify (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	struct berval *dn,
	struct berval *ndn,
	Modifications *mod
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn->bv_val);

	if (be && be->be_modify) {
		rc = be->be_modify (be, conn, op, dn, ndn, mod);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL,
			"No modify target found", NULL, NULL);
	}
	return rc;
}

static int
glue_back_modrdn (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	struct berval *dn,
	struct berval *ndn,
	struct berval *newrdn,
	struct berval *nnewrdn,
	int del,
	struct berval *newsup,
	struct berval *nnewsup
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn->bv_val);

	if (be && be->be_modrdn) {
		rc = be->be_modrdn (be, conn, op, dn, ndn,
			newrdn, nnewrdn, del, newsup, nnewsup );
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL,
			"No modrdn target found", NULL, NULL);
	}
	return rc;
}

static int
glue_back_add (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	Entry *e
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, e->e_ndn);

	if (be && be->be_add) {
		rc = be->be_add (be, conn, op, e);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No add target found",
				  NULL, NULL);
	}
	return rc;
}

static int
glue_back_delete (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	struct berval *dn,
	struct berval *ndn
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn->bv_val);

	if (be && be->be_delete) {
		rc = be->be_delete (be, conn, op, dn, ndn);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
		send_ldap_result (conn, op, rc, NULL, "No delete target found",
				  NULL, NULL);
	}
	return rc;
}

static int
glue_back_release_rw (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	Entry *e,
	int rw
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, e->e_ndn);

	if (be && be->be_release) {
		rc = be->be_release (be, conn, op, e, rw);
	} else {
		entry_free (e);
		rc = 0;
	}
	return rc;
}

static int
glue_back_group (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	Entry *target,
	struct berval *ndn,
	struct berval *ondn,
	ObjectClass *oc,
	AttributeDescription * ad
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn->bv_val);

	if (be && be->be_group) {
		rc = be->be_group (be, conn, op, target, ndn, ondn, oc, ad);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
	}
	return rc;
}

static int
glue_back_attribute (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	Entry *target,
	struct berval *ndn,
	AttributeDescription *ad,
	BerVarray *vals
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn->bv_val);

	if (be && be->be_attribute) {
		rc = be->be_attribute (be, conn, op, target, ndn, ad, vals);
	} else {
		rc = LDAP_UNWILLING_TO_PERFORM;
	}
	return rc;
}

static int
glue_back_referrals (
	BackendDB *b0,
	Connection *conn,
	Operation *op,
	struct berval *dn,
	struct berval *ndn,
	const char **text
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, ndn->bv_val);

	if (be && be->be_chk_referrals) {
		rc = be->be_chk_referrals (be, conn, op, dn, ndn, text);
	} else {
		rc = LDAP_SUCCESS;;
	}
	return rc;
}

static int
glue_tool_entry_open (
	BackendDB *b0,
	int mode
)
{
	/* We don't know which backend to talk to yet, so just
	 * remember the mode and move on...
	 */

	glueMode = mode;
	glueBack = NULL;

	return 0;
}

static int
glue_tool_entry_close (
	BackendDB *b0
)
{
	int rc = 0;

	if (glueBack) {
		if (!glueBack->be_entry_close)
			return 0;
		rc = glueBack->be_entry_close (glueBack);
	}
	return rc;
}

static ID
glue_tool_entry_first (
	BackendDB *b0
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	int i;

	/* If we're starting from scratch, start at the most general */
	if (!glueBack) {
		for (i = gi->nodes-1; i >= 0; i--) {
			if (gi->n[i].be->be_entry_open &&
			    gi->n[i].be->be_entry_first) {
			    	glueBack = gi->n[i].be;
				break;
			}
		}

	}
	if (!glueBack || glueBack->be_entry_open (glueBack, glueMode) != 0)
		return NOID;

	return glueBack->be_entry_first (glueBack);
}

static ID
glue_tool_entry_next (
	BackendDB *b0
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	int i;
	ID rc;

	if (!glueBack || !glueBack->be_entry_next)
		return NOID;

	rc = glueBack->be_entry_next (glueBack);

	/* If we ran out of entries in one database, move on to the next */
	if (rc == NOID) {
		glueBack->be_entry_close (glueBack);
		for (i=0; i<gi->nodes; i++) {
			if (gi->n[i].be == glueBack)
				break;
		}
		if (i == 0) {
			glueBack = NULL;
			rc = NOID;
		} else {
			glueBack = gi->n[i-1].be;
			rc = glue_tool_entry_first (b0);
		}
	}
	return rc;
}

static Entry *
glue_tool_entry_get (
	BackendDB *b0,
	ID id
)
{
	if (!glueBack || !glueBack->be_entry_get)
		return NULL;

	return glueBack->be_entry_get (glueBack, id);
}

static ID
glue_tool_entry_put (
	BackendDB *b0,
	Entry *e,
	struct berval *text
)
{
	BackendDB *be;
	int rc;

	be = glue_back_select (b0, e->e_ndn);
	if (!be->be_entry_put)
		return NOID;

	if (!glueBack) {
		rc = be->be_entry_open (be, glueMode);
		if (rc != 0)
			return NOID;
	} else if (be != glueBack) {
		/* If this entry belongs in a different branch than the
		 * previous one, close the current database and open the
		 * new one.
		 */
		glueBack->be_entry_close (glueBack);
		rc = be->be_entry_open (be, glueMode);
		if (rc != 0)
			return NOID;
	}
	glueBack = be;
	return be->be_entry_put (be, e, text);
}

static int
glue_tool_entry_reindex (
	BackendDB *b0,
	ID id
)
{
	if (!glueBack || !glueBack->be_entry_reindex)
		return -1;

	return glueBack->be_entry_reindex (glueBack, id);
}

static int
glue_tool_sync (
	BackendDB *b0
)
{
	glueinfo *gi = (glueinfo *) b0->be_private;
	int i;

	/* just sync everyone */
	for (i = 0; i<gi->nodes; i++)
		if (gi->n[i].be->be_sync)
			gi->n[i].be->be_sync (gi->n[i].be);
	return 0;
}

int
glue_sub_init( )
{
	int i, j;
	int cont = num_subordinates;
	BackendDB *b1, *be;
	BackendInfo *bi;
	glueinfo *gi;

	/* While there are subordinate backends, search backwards through the
	 * backends and connect them to their superior.
	 */
	for (i = nBackendDB - 1, b1=&backendDB[i]; cont && i>=0; b1--,i--) {
		if (b1->be_flags & SLAP_BFLAG_GLUE_SUBORDINATE) {
			/* The last database cannot be a subordinate of noone */
			if (i == nBackendDB - 1) {
				b1->be_flags ^= SLAP_BFLAG_GLUE_SUBORDINATE;
			}
			continue;
		}
		gi = NULL;
		for (j = i-1, be=&backendDB[j]; j>=0; be--,j--) {
			if (!(be->be_flags & SLAP_BFLAG_GLUE_SUBORDINATE)) {
				continue;
			}
			/* We will only link it once */
			if (be->be_flags & SLAP_BFLAG_GLUE_LINKED) {
				continue;
			}
			if (!dnIsSuffix(be->be_nsuffix[0], b1->be_nsuffix[0])) {
				continue;
			}
			cont--;
			be->be_flags |= SLAP_BFLAG_GLUE_LINKED;
			if (gi == NULL) {
				/* We create a copy of the superior's be
				 * structure, pointing to all of its original
				 * information. Then we replace elements of
				 * the superior's info with our own. The copy
				 * is used whenever we have operations to pass
				 * down to the real database.
				 */
				b1->be_flags |= SLAP_BFLAG_GLUE_INSTANCE;
				gi = (glueinfo *)ch_malloc(sizeof(glueinfo));
				gi->be = (BackendDB *)ch_malloc(
					sizeof(BackendDB) + sizeof(BackendInfo));
				bi = (BackendInfo *)(gi->be+1);
				*gi->be = *b1;
				gi->nodes = 0;
				*bi = *b1->bd_info;
				bi->bi_open = glue_back_open;
				bi->bi_close = glue_back_close;
				bi->bi_db_open = glue_back_db_open;
				bi->bi_db_close = glue_back_db_close;
				bi->bi_db_destroy = glue_back_db_destroy;

				bi->bi_op_bind = glue_back_bind;
				bi->bi_op_search = glue_back_search;
				bi->bi_op_compare = glue_back_compare;
				bi->bi_op_modify = glue_back_modify;
				bi->bi_op_modrdn = glue_back_modrdn;
				bi->bi_op_add = glue_back_add;
				bi->bi_op_delete = glue_back_delete;

				bi->bi_entry_release_rw = glue_back_release_rw;
				bi->bi_acl_group = glue_back_group;
				bi->bi_acl_attribute = glue_back_attribute;
				bi->bi_chk_referrals = glue_back_referrals;

				/*
				 * hooks for slap tools
				 */
				bi->bi_tool_entry_open = glue_tool_entry_open;
				bi->bi_tool_entry_close = glue_tool_entry_close;
				bi->bi_tool_entry_first = glue_tool_entry_first;
				bi->bi_tool_entry_next = glue_tool_entry_next;
				bi->bi_tool_entry_get = glue_tool_entry_get;
				bi->bi_tool_entry_put = glue_tool_entry_put;
				bi->bi_tool_entry_reindex = glue_tool_entry_reindex;
				bi->bi_tool_sync = glue_tool_sync;
			} else {
				gi = (glueinfo *)ch_realloc(gi,
					sizeof(glueinfo) +
					gi->nodes * sizeof(gluenode));
			}
			gi->n[gi->nodes].be = be;
			dnParent( be->be_nsuffix[0], &gi->n[gi->nodes].pdn ); 
			gi->nodes++;
		}
		if (gi) {
			/* One more node for the master */
			gi = (glueinfo *)ch_realloc(gi,
				sizeof(glueinfo) + gi->nodes * sizeof(gluenode));
			gi->n[gi->nodes].be = gi->be;
			dnParent( b1->be_nsuffix[0], &gi->n[gi->nodes].pdn );
			gi->nodes++;
			b1->be_private = gi;
			b1->bd_info = bi;
		}
	}
	/* If there are any unresolved subordinates left, something is wrong */
	return cont;
}
