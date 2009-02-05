/* $OpenLDAP: pkg/ldap/servers/slapd/back-bdb/external.h,v 1.11 2002/01/04 20:17:49 kurt Exp $ */
/*
 * Copyright 2000-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#ifndef _BDB_EXTERNAL_H
#define _BDB_EXTERNAL_H

LDAP_BEGIN_DECL

extern BI_init	bdb_initialize;

extern BI_db_config	bdb_db_config;

extern BI_op_add	bdb_add;

extern BI_op_bind	bdb_bind;

extern BI_op_compare	bdb_compare;

extern BI_op_delete	bdb_delete;

extern BI_op_modify	bdb_modify;

extern BI_op_modrdn	bdb_modrdn;

extern BI_op_search	bdb_search;

extern BI_op_extended	bdb_extended;

extern BI_chk_referrals	bdb_referrals;

/* tools.c */
extern BI_tool_entry_open	bdb_tool_entry_open;
extern BI_tool_entry_close	bdb_tool_entry_close;
extern BI_tool_entry_next	bdb_tool_entry_next;
extern BI_tool_entry_get	bdb_tool_entry_get;
extern BI_tool_entry_put	bdb_tool_entry_put;
extern BI_tool_entry_reindex	bdb_tool_entry_reindex;



LDAP_END_DECL

#endif /* _BDB_EXTERNAL_H */

