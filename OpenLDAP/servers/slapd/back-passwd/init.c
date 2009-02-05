/* init.c - initialize passwd backend */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-passwd/init.c,v 1.19 2001/12/26 07:32:08 hyc Exp $ */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>

#include "slap.h"
#include "external.h"

#ifdef SLAPD_PASSWD_DYNAMIC

int back_passwd_LTX_init_module(int argc, char *argv[]) {
    BackendInfo bi;

    memset( &bi, '\0', sizeof(bi) );
    bi.bi_type = "passwd";
    bi.bi_init = passwd_back_initialize;

    backend_add(&bi);
    return 0;
}

#endif /* SLAPD_PASSWD_DYNAMIC */

int
passwd_back_initialize(
    BackendInfo	*bi
)
{
	bi->bi_open = 0;
	bi->bi_config = 0;
	bi->bi_close = 0;
	bi->bi_destroy = 0;

	bi->bi_db_init = 0;
	bi->bi_db_config = passwd_back_db_config;
	bi->bi_db_open = 0;
	bi->bi_db_close = 0;
	bi->bi_db_destroy = 0;

	bi->bi_op_bind = 0;
	bi->bi_op_unbind = 0;
	bi->bi_op_search = passwd_back_search;
	bi->bi_op_compare = 0;
	bi->bi_op_modify = 0;
	bi->bi_op_modrdn = 0;
	bi->bi_op_add = 0;
	bi->bi_op_delete = 0;
	bi->bi_op_abandon = 0;

	bi->bi_extended = 0;

	bi->bi_acl_group = 0;
	bi->bi_acl_attribute = 0;
	bi->bi_chk_referrals = 0;

	bi->bi_connection_init = 0;
	bi->bi_connection_destroy = 0;

	return 0;
}