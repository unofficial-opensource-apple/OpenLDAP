#! /bin/sh
# $OpenLDAP: pkg/ldap/tests/scripts/acfilter.sh,v 1.6.4.2 2004/01/01 18:16:43 kurt Exp $
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
#
# Strip comments
#
egrep -iv '^#'
