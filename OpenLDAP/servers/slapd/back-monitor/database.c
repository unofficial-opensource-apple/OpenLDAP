/* database.c - deals with database subsystem */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-monitor/database.c,v 1.30.2.9 2004/07/19 18:31:42 hyc Exp $ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2001-2004 The OpenLDAP Foundation.
 * Portions Copyright 2001-2003 Pierangelo Masarati.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by Pierangelo Masarati for inclusion
 * in OpenLDAP Software.
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#include "slap.h"
#include "back-monitor.h"

#if defined(LDAP_SLAPI)
#include "slapi.h"
static int monitor_back_add_plugin( Backend *be, Entry *e );
#endif /* defined(LDAP_SLAPI) */

#if defined(SLAPD_LDAP) 
#include "../back-ldap/back-ldap.h"
#endif /* defined(SLAPD_LDAP) */

int
monitor_subsys_database_init(
	BackendDB	*be
)
{
	struct monitorinfo	*mi;
	Entry			*e, *e_database, *e_tmp;
	int			i;
	struct monitorentrypriv	*mp;

	assert( be != NULL );

	mi = ( struct monitorinfo * )be->be_private;

	if ( monitor_cache_get( mi, 
				&monitor_subsys[SLAPD_MONITOR_DATABASE].mss_ndn, 
				&e_database ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, CRIT,
			"monitor_subsys_database_init: "
			"unable to get entry '%s'\n",
			monitor_subsys[SLAPD_MONITOR_DATABASE].mss_ndn.bv_val, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY,
			"monitor_subsys_database_init: "
			"unable to get entry '%s'\n%s%s",
			monitor_subsys[SLAPD_MONITOR_DATABASE].mss_ndn.bv_val, 
			"", "" );
#endif
		return( -1 );
	}

	e_tmp = NULL;
	for ( i = nBackendDB; i--; ) {
		char		buf[ BACKMONITOR_BUFSIZE ];
		int		j;
		slap_overinfo	*oi = NULL;
		BackendInfo	*bi;

		be = &backendDB[i];

		bi = be->bd_info;

		if ( strcmp( be->bd_info->bi_type, "over" ) == 0 ) {
			oi = (slap_overinfo *)be->bd_info->bi_private;
			bi = oi->oi_orig;
		}

		/* Subordinates are not exposed as their own naming context */
		if ( SLAP_GLUE_SUBORDINATE( be ) ) {
			continue;
		}

		snprintf( buf, sizeof( buf ),
				"dn: cn=Database %d,%s\n"
				"objectClass: %s\n"
				"structuralObjectClass: %s\n"
				"cn: Database %d\n"
				"description: This object contains the type of the database.\n"
				"%s: %s\n"
				"createTimestamp: %s\n"
				"modifyTimestamp: %s\n",
				i,
				monitor_subsys[SLAPD_MONITOR_DATABASE].mss_dn.bv_val,
				mi->mi_oc_monitoredObject->soc_cname.bv_val,
				mi->mi_oc_monitoredObject->soc_cname.bv_val,
				i,
				mi->mi_ad_monitoredInfo->ad_cname.bv_val,
				bi->bi_type,
				mi->mi_startTime.bv_val,
				mi->mi_startTime.bv_val );
		
		e = str2entry( buf );
		if ( e == NULL ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, CRIT,
				"monitor_subsys_database_init: "
				"unable to create entry 'cn=Database %d,%s'\n",
				i, monitor_subsys[SLAPD_MONITOR_DATABASE].mss_ndn.bv_val, 0 );
#else
			Debug( LDAP_DEBUG_ANY,
				"monitor_subsys_database_init: "
				"unable to create entry 'cn=Database %d,%s'\n%s",
				i, 
				monitor_subsys[SLAPD_MONITOR_DATABASE].mss_ndn.bv_val,
				"" );
#endif
			return( -1 );
		}
		
		if ( SLAP_MONITOR(be) ) {
			attr_merge( e, slap_schema.si_ad_monitorContext,
					be->be_suffix, be->be_nsuffix );
			attr_merge( e_database, slap_schema.si_ad_monitorContext,
					be->be_suffix, be->be_nsuffix );
		} else {
			attr_merge( e, slap_schema.si_ad_namingContexts,
					be->be_suffix, be->be_nsuffix );
			attr_merge( e_database, slap_schema.si_ad_namingContexts,
					be->be_suffix, be->be_nsuffix );
		}

		if ( oi != NULL ) {
			slap_overinst *on = oi->oi_list;

			for ( ; on; on = on->on_next ) {
				struct berval		bv;
				slap_overinst		*on2;
				
				bv.bv_val = on->on_bi.bi_type;
				bv.bv_len = strlen( bv.bv_val );
				attr_merge_normalize_one( e, mi->mi_ad_monitorOverlay,
						&bv, NULL );

				for ( on2 = overlay_next( NULL ), j = 0; on2; on2 = overlay_next( on2 ), j++ ) {
					if ( on2->on_bi.bi_type == on->on_bi.bi_type ) {
						break;
					}
				}
				assert( on2 );

				snprintf( buf, sizeof( buf ), 
					"cn=Overlay %d,%s", 
					j, monitor_subsys[SLAPD_MONITOR_OVERLAY].mss_dn.bv_val );
				bv.bv_val = buf;
				bv.bv_len = strlen( buf );
				attr_merge_normalize_one( e, mi->mi_ad_seeAlso,
						&bv, NULL );
			}
		}

#if defined(SLAPD_LDAP) 
		if ( strcmp( bi->bi_type, "ldap" ) == 0 ) {
			struct ldapinfo		*li = (struct ldapinfo *)be->be_private;
			struct berval		bv;

			bv.bv_val = li->url;
			bv.bv_len = strlen( bv.bv_val );
			attr_merge_normalize_one( e, mi->mi_ad_labeledURI,
					&bv, NULL );
		}
#endif /* defined(SLAPD_LDAP) */

		for ( j = nBackendInfo; j--; ) {
			if ( backendInfo[ j ].bi_type == bi->bi_type ) {
				struct berval 		bv;

				snprintf( buf, sizeof( buf ), 
					"cn=Backend %d,%s", 
					j, monitor_subsys[SLAPD_MONITOR_BACKEND].mss_dn.bv_val );
				bv.bv_val = buf;
				bv.bv_len = strlen( buf );
				attr_merge_normalize_one( e, mi->mi_ad_seeAlso,
						&bv, NULL );
				break;
			}
		}
		/* we must find it! */
		assert( j >= 0 );

		mp = ( struct monitorentrypriv * )ch_calloc( sizeof( struct monitorentrypriv ), 1 );
		e->e_private = ( void * )mp;
		mp->mp_next = e_tmp;
		mp->mp_children = NULL;
		mp->mp_info = &monitor_subsys[SLAPD_MONITOR_DATABASE];
		mp->mp_flags = monitor_subsys[SLAPD_MONITOR_DATABASE].mss_flags
			| MONITOR_F_SUB;

		if ( monitor_cache_add( mi, e ) ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, CRIT,
				"monitor_subsys_database_init: "
				"unable to add entry 'cn=Database %d,%s'\n",
				i, monitor_subsys[SLAPD_MONITOR_DATABASE].mss_ndn.bv_val, 0 );
#else
			Debug( LDAP_DEBUG_ANY,
				"monitor_subsys_database_init: "
				"unable to add entry 'cn=Database %d,%s'\n",
				i, 
				monitor_subsys[SLAPD_MONITOR_DATABASE].mss_ndn.bv_val,
				0 );
#endif
			return( -1 );
		}

#if defined(LDAP_SLAPI)
		monitor_back_add_plugin( be, e );
#endif /* defined(LDAP_SLAPI) */

		e_tmp = e;
	}
	
	mp = ( struct monitorentrypriv * )e_database->e_private;
	mp->mp_children = e_tmp;

	monitor_cache_release( mi, e_database );

	return( 0 );
}

#if defined(LDAP_SLAPI)
static int
monitor_back_add_plugin( Backend *be, Entry *e_database )
{
	Slapi_PBlock		*pCurrentPB; 
	int			i, rc = LDAP_SUCCESS;
	struct monitorinfo	*mi = ( struct monitorinfo * )be->be_private;

	if ( slapi_int_pblock_get_first( be, &pCurrentPB ) != LDAP_SUCCESS ) {
		/*
		 * LDAP_OTHER is returned if no plugins are installed
		 */
		rc = LDAP_OTHER;
		goto done;
	}

	i = 0;
	do {
		Slapi_PluginDesc	*srchdesc;
		char			buf[ BACKMONITOR_BUFSIZE ];
		struct berval		bv;

		rc = slapi_pblock_get( pCurrentPB, SLAPI_PLUGIN_DESCRIPTION,
				&srchdesc );
		if ( rc != LDAP_SUCCESS ) {
			goto done;
		}

		snprintf( buf, sizeof(buf),
				"plugin %d name: %s; "
				"vendor: %s; "
				"version: %s; "
				"description: %s", 
				i,
				srchdesc->spd_id,
				srchdesc->spd_vendor,
				srchdesc->spd_version,
				srchdesc->spd_description );

		bv.bv_val = buf;
		bv.bv_len = strlen( buf );
		attr_merge_normalize_one( e_database,
				mi->mi_ad_monitoredInfo, &bv, NULL );

		i++;

	} while ( ( slapi_int_pblock_get_next( &pCurrentPB ) == LDAP_SUCCESS )
			&& ( pCurrentPB != NULL ) );

done:
	return rc;
}
#endif /* defined(LDAP_SLAPI) */
