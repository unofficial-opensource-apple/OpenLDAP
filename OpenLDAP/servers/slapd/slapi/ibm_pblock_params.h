/* $OpenLDAP: pkg/ldap/servers/slapd/slapi/ibm_pblock_params.h,v 1.3.2.3 2004/03/18 01:01:04 kurt Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2002-2004 The OpenLDAP Foundation.
 * Portions Copyright 1997,2002-2003 IBM Corporation.
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
 * This work was initially developed by IBM Corporation for use in
 * IBM products and subsequently ported to OpenLDAP Software by
 * Steve Omrani.
 */

#ifndef _ibm_pblock_params_H
#define _ibm_pblock_params_H

#define FIRST_PARAM -1
#define LAST_IBM_PARAM -48
#define NETSCAPE_RESERVED(p) (p >= 280) && (p <= 299)
#define IBM_RESERVED(p) (p >= LAST_IBM_PARAM) && (p <= FIRST_PARAM)

#define SLAPI_IBM_THREAD_CONTROL					(FIRST_PARAM - 1)
#define SLAPI_IBM_PBLOCK							(FIRST_PARAM - 2)
#define SLAPI_IBM_CONNECTION_PTR					(FIRST_PARAM - 3)
#define SLAPI_IBM_BACKENDS							(FIRST_PARAM - 4)
#define SLAPI_PRE_BIND_ALL							(FIRST_PARAM - 5)
#define SLAPI_POST_BIND_ALL							(FIRST_PARAM - 6)
#define SLAPI_IBM_ADMIN_DN							(FIRST_PARAM - 7)
#define SLAPI_CONFIG_STATE							(FIRST_PARAM - 8)
#define SLAPI_PLUGIN_DB_REGISTER_SERVICE_FN			(FIRST_PARAM - 9)   
#define SLAPI_PLUGIN_DB_INSERT_REPL_ENTRIES_FN		(FIRST_PARAM - 10)
#define SLAPI_PLUGIN_DB_GET_REPL_ENTRIES_FN			(FIRST_PARAM - 11)
#define SLAPI_PLUGIN_DB_REPLICA_DONE_FN				(FIRST_PARAM - 12)
#define SLAPI_PLUGIN_DB_INIT_REPL_LIST_FN			(FIRST_PARAM - 13)
#define SLAPI_PLUGIN_DB_THREAD_INITIALIZE_FN		(FIRST_PARAM - 14)
#define SLAPI_PLUGIN_DB_THREAD_TERMINATE_FN			(FIRST_PARAM - 15)
#define SLAPI_PLUGIN_DB_SCHEMA_MODIFY_ATTRTYPE_FN	(FIRST_PARAM - 16)
#define SLAPI_PLUGIN_DB_SCHEMA_MODIFY_OBJCLASS_FN	(FIRST_PARAM - 17)
#define SLAPI_PLUGIN_DB_INIT_FN						(FIRST_PARAM - 18)
#define SLAPI_IBM_EXTENDED_OPS						(FIRST_PARAM - 19)
#define SLAPI_IBM_CONTROLS							(FIRST_PARAM - 20)
#define SLAPI_IBM_SASLMECHANISMS					(FIRST_PARAM - 21)
#define SLAPI_IBM_BROADCAST_BE						(FIRST_PARAM - 22)
#define SLAPI_IBM_NOTIFY_BIND_FN					(FIRST_PARAM - 23)
#define SLAPI_IBM_SECRET							(FIRST_PARAM - 24)
#define SLAPI_IBM_CL_START_FN						(FIRST_PARAM - 25)
#define SLAPI_IBM_REPLICATE							(FIRST_PARAM - 26)
#define SLAPI_IBM_CL_CLASS							(FIRST_PARAM - 27)
#define SLAPI_IBM_CL_SUFFIX							(FIRST_PARAM - 28)
#define SLAPI_IBM_CL_MAX_ENTRIES					(FIRST_PARAM - 29)
#define SLAPI_IBM_CONNINFO							(FIRST_PARAM - 30)
#define SLAPI_IBM_CL_FIRST_ENTRY					(FIRST_PARAM - 31)
#define SLAPI_IBM_CL_LAST_ENTRY						(FIRST_PARAM - 32)
#define SLAPI_IBM_CONN_DN_ALT						(FIRST_PARAM - 33)
#define SLAPI_IBM_GSSAPI_CONTEXT					(FIRST_PARAM - 34)
#define SLAPI_IBM_ADD_ENTRY							(FIRST_PARAM - 35)
#define SLAPI_IBM_DELETE_ENTRY						(FIRST_PARAM - 36)
#define SLAPI_IBM_MODIFY_ENTRY						(FIRST_PARAM - 37)
#define SLAPI_IBM_MODIFY_MODS						(FIRST_PARAM - 38)
#define SLAPI_IBM_MODRDN_ENTRY						(FIRST_PARAM - 39)
#define SLAPI_IBM_MODRDN_NEWDN						(FIRST_PARAM - 40)
#define SLAPI_IBM_EVENT_ENABLED						(FIRST_PARAM - 41)
#define SLAPI_IBM_EVENT_MAXREG						(FIRST_PARAM - 42)
#define SLAPI_IBM_EVENT_REGPERCONN					(FIRST_PARAM - 43)
#define SLAPI_IBM_EVENT_CURREG						(FIRST_PARAM - 44)
#define SLAPI_IBM_EVENT_SENTREG						(FIRST_PARAM - 45)
#define SLAPI_IBM_CONN_DN_ORIG						(FIRST_PARAM - 46)
#define SLAPI_PLUGIN_DB_DELETE_PROGRESS_FN			(FIRST_PARAM - 47) 
#endif /* _ibm_pblock_params_H */
