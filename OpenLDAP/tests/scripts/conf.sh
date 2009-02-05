#! /bin/sh
# $OpenLDAP: pkg/ldap/tests/scripts/conf.sh,v 1.12.2.5 2004/01/01 18:16:43 kurt Exp $
## This work is part of OpenLDAP Software <http://www.openldap.org/>.
##
## Copyright 1998-2004 The OpenLDAP Foundation.
## All rights reserved.
##
## Redistribution and use in source and binary forms, with or without
## modification, are permitted only as authorized by the OpenLDAP
## Public License.
##
## A copy of this license is available in the file LICENSE in the
## top-level directory of the distribution or, alternatively, at
## <http://www.OpenLDAP.org/license.html>.
if [ x"$MONITORDB" = x"yes" ] ; then
	MON=monitor
else
	MON=nomonitor
fi
sed -e "s/@BACKEND@/${BACKEND}/"	\
	-e "s/^#${BACKEND}#//"			\
	-e "s/^#${BACKENDTYPE}#//"			\
	-e "s/^#${AC_ldap}#//"			\
	-e "s/^#${AC_pcache}#//"			\
	-e "s/^#${MON}#//"				\
	-e "s/@CACHETTL@/${CACHETTL}/"   \
	-e "s/@ENTRY_LIMIT@/${CACHE_ENTRY_LIMIT}/"   
