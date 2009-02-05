/* config.c - configuration file handling routines */
/* $OpenLDAP: pkg/ldap/servers/slapd/config.c,v 1.165 2002/02/09 04:14:17 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/ctype.h>
#include <ac/socket.h>
#include <ac/errno.h>

#include "lutil.h"
#include "ldap_pvt.h"
#include "slap.h"

#define MAXARGS	500

/*
 * defaults for various global variables
 */
struct slap_limits_set deflimit = {
	SLAPD_DEFAULT_TIMELIMIT,	/* backward compatible limits */
	0,

	SLAPD_DEFAULT_SIZELIMIT,	/* backward compatible limits */
	0,
	-1				/* no limit on unchecked size */
};

AccessControl	*global_acl = NULL;
slap_access_t		global_default_access = ACL_READ;
slap_mask_t		global_restrictops = 0;
slap_mask_t		global_allows = 0;
slap_mask_t		global_disallows = 0;
slap_mask_t		global_requires = 0;
slap_ssf_set_t	global_ssf_set;
char		*replogfile;
int		global_idletimeout = 0;
char	*global_host = NULL;
char	*global_realm = NULL;
char		*ldap_srvtab = "";
char		*default_passwd_hash;
struct berval default_search_base = { 0, NULL };
struct berval default_search_nbase = { 0, NULL };
unsigned		num_subordinates = 0;

ber_len_t sockbuf_max_incoming = SLAP_SB_MAX_INCOMING_DEFAULT;
ber_len_t sockbuf_max_incoming_auth= SLAP_SB_MAX_INCOMING_AUTH;

char   *slapd_pid_file  = NULL;
char   *slapd_args_file = NULL;

int nSaslRegexp = 0;
SaslRegexp_t *SaslRegexp = NULL;
int sasl_external_x509dn_convert;

static char	*fp_getline(FILE *fp, int *lineno);
static void	fp_getline_init(int *lineno);
static int	fp_parse_line(char *line, int *argcp, char **argv);

static char	*strtok_quote(char *line, char *sep);
static int      load_ucdata(char *path);

int
read_config( const char *fname )
{
	FILE	*fp;
	char	*line, *savefname, *saveline;
	int	cargc, savelineno;
	char	*cargv[MAXARGS+1];
	int	lineno, i;
	int rc;
	struct berval vals[2];

	static int lastmod = 1;
	static BackendInfo *bi = NULL;
	static BackendDB	*be = NULL;

	vals[1].bv_val = NULL;

	if ( (fp = fopen( fname, "r" )) == NULL ) {
		ldap_syslog = 1;
#ifdef NEW_LOGGING
		LDAP_LOG(( "config", LDAP_LEVEL_ENTRY, "read_config: "
			"could not open config file \"%s\": %s (%d)\n",
		    fname, strerror(errno), errno ));
#else
		Debug( LDAP_DEBUG_ANY,
		    "could not open config file \"%s\": %s (%d)\n",
		    fname, strerror(errno), errno );
#endif
		return 1;
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "config", LDAP_LEVEL_ENTRY,
		"read_config: reading config file %s\n", fname ));
#else
	Debug( LDAP_DEBUG_CONFIG, "reading config file %s\n", fname, 0, 0 );
#endif


	fp_getline_init( &lineno );

	while ( (line = fp_getline( fp, &lineno )) != NULL ) {
		/* skip comments and blank lines */
		if ( line[0] == '#' || line[0] == '\0' ) {
			continue;
		}

#ifdef NEW_LOGGING
		LDAP_LOG(( "config", LDAP_LEVEL_DETAIL1,
			   "line %d (%s)\n", lineno, line ));
#else
		Debug( LDAP_DEBUG_CONFIG, "line %d (%s)\n", lineno, line, 0 );
#endif


		/* fp_parse_line is destructive, we save a copy */
		saveline = ch_strdup( line );

		if ( fp_parse_line( line, &cargc, cargv ) != 0 ) {
			return( 1 );
		}

		if ( cargc < 1 ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "config", LDAP_LEVEL_INFO,
				   "%s: line %d: bad config line (ignored)\n",
				   fname, lineno ));
#else
			Debug( LDAP_DEBUG_ANY,
			    "%s: line %d: bad config line (ignored)\n",
			    fname, lineno, 0 );
#endif

			continue;
		}

		if ( strcasecmp( cargv[0], "backend" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s : line %d: missing type in \"backend\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
		"%s: line %d: missing type in \"backend <type>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			if( be != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: backend line must appear before any "
					   "database definition.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
"%s: line %d: backend line must appear before any database definition\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			bi = backend_info( cargv[1] );

			if( bi == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "read_config: backend %s initialization failed.\n",
					   cargv[1] ));
#else
				Debug( LDAP_DEBUG_ANY,
					"backend %s initialization failed.\n",
				    cargv[1], 0, 0 );
#endif

				return( 1 );
			}
		} else if ( strcasecmp( cargv[0], "database" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing type in \"database <type>\" line\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
		"%s: line %d: missing type in \"database <type>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			bi = NULL;
			be = backend_db_init( cargv[1] );

			if( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "database %s initialization failed.\n",
					   cargv[1] ));
#else
				Debug( LDAP_DEBUG_ANY,
					"database %s initialization failed.\n",
				    cargv[1], 0, 0 );
#endif

				return( 1 );
			}

		/* set thread concurrency */
		} else if ( strcasecmp( cargv[0], "concurrency" ) == 0 ) {
			int c;
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing level in \"concurrency <level\" line\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing level in \"concurrency <level>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			c = atoi( cargv[1] );

			if( c < 1 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: invalid level (%d) in "
					   "\"concurrency <level>\" line.\n",
					   fname, lineno, c ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: invalid level (%d) in \"concurrency <level>\" line\n",
				    fname, lineno, c );
#endif

				return( 1 );
			}

			ldap_pvt_thread_set_concurrency( c );

		/* set sockbuf max */
		} else if ( strcasecmp( cargv[0], "sockbuf_max_incoming" ) == 0 ) {
			long max;
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing max in \"sockbuf_max_incoming <bytes>\" line\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					   "%s: line %d: missing max in \"sockbuf_max_incoming <bytes>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			max = atol( cargv[1] );

			if( max < 0 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: invalid max value (%ld) in "
					   "\"sockbuf_max_incoming <bytes>\" line.\n",
					   fname, lineno, max ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: invalid max value (%ld) in "
					"\"sockbuf_max_incoming <bytes>\" line.\n",
				    fname, lineno, max );
#endif

				return( 1 );
			}

			sockbuf_max_incoming = max;

		/* set sockbuf max authenticated */
		} else if ( strcasecmp( cargv[0], "sockbuf_max_incoming_auth" ) == 0 ) {
			long max;
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing max in \"sockbuf_max_incoming_auth <bytes>\" line\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					   "%s: line %d: missing max in \"sockbuf_max_incoming_auth <bytes>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			max = atol( cargv[1] );

			if( max < 0 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: invalid max value (%ld) in "
					   "\"sockbuf_max_incoming_auth <bytes>\" line.\n",
					   fname, lineno, max ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: invalid max value (%ld) in "
					"\"sockbuf_max_incoming_auth <bytes>\" line.\n",
				    fname, lineno, max );
#endif

				return( 1 );
			}

			sockbuf_max_incoming_auth = max;

		/* default search base */
		} else if ( strcasecmp( cargv[0], "defaultSearchBase" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: missing dn in \"defaultSearchBase <dn\" "
					"line\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"missing dn in \"defaultSearchBase <dn>\" line\n",
					fname, lineno, 0 );
#endif

				return 1;

			} else if ( cargc > 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					"%s: line %d: extra cruft after <dn> in "
					"\"defaultSearchBase %s\" line (ignored)\n",
					fname, lineno, cargv[1] ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"extra cruft after <dn> in \"defaultSearchBase %s\", "
					"line (ignored)\n",
					fname, lineno, cargv[1] );
#endif
			}

			if ( bi != NULL || be != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: defaultSearchBase line must appear "
					"prior to any backend or database definitions\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"defaultSearchBaase line must appear prior to "
					"any backend or database definition\n",
				    fname, lineno, 0 );
#endif

				return 1;
			}

			if ( default_search_nbase.bv_len ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO, "%s: line %d: "
					"default search base \"%s\" already defined "
					"(discarding old)\n", fname, lineno,
					default_search_base->bv_val ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"default search base \"%s\" already defined "
					"(discarding old)\n",
					fname, lineno, default_search_base.bv_val );
#endif

				free( default_search_base.bv_val );
				free( default_search_nbase.bv_val );
			}

			if ( load_ucdata( NULL ) < 0 ) return 1;

			{
				struct berval dn;

				dn.bv_val = cargv[1];
				dn.bv_len = strlen( dn.bv_val );

				rc = dnPrettyNormal( NULL, &dn,
					&default_search_base,
					&default_search_nbase );

				if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						"%s: line %d: defaultSearchBase DN is invalid.\n",
						fname, lineno ));
#else
					Debug( LDAP_DEBUG_ANY,
						"%s: line %d: defaultSearchBase DN is invalid\n",
					   fname, lineno, 0 );
#endif
					return( 1 );
				}
			}

		/* set maximum threads in thread pool */
		} else if ( strcasecmp( cargv[0], "threads" ) == 0 ) {
			int c;
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing count in \"threads <count>\" line\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing count in \"threads <count>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			c = atoi( cargv[1] );

			if( c < 0 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: invalid level (%d) in \"threads <count>\""
					   "line\n",fname, lineno, c ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: invalid level (%d) in \"threads <count>\" line\n",
				    fname, lineno, c );
#endif

				return( 1 );
			}

			ldap_pvt_thread_pool_maxthreads( &connection_pool, c );

			/* save for later use */
			connection_pool_max = c;

		/* get pid file name */
		} else if ( strcasecmp( cargv[0], "pidfile" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d missing file name in \"pidfile <file>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing file name in \"pidfile <file>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			slapd_pid_file = ch_strdup( cargv[1] );

		/* get args file name */
		} else if ( strcasecmp( cargv[0], "argsfile" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: %d: missing file name in "
					   "\"argsfile <file>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing file name in \"argsfile <file>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			slapd_args_file = ch_strdup( cargv[1] );

		/* default password hash */
		} else if ( strcasecmp( cargv[0], "password-hash" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing hash in "
					   "\"password-hash <hash>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing hash in \"password-hash <hash>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( default_passwd_hash != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: already set default password_hash!\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: already set default password_hash!\n",
					fname, lineno, 0 );
#endif

				return 1;

			} else {
				default_passwd_hash = ch_strdup( cargv[1] );
			}

		} else if ( strcasecmp( cargv[0], "password-crypt-salt-format" ) == 0 ) 
		{
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: missing format in "
					"\"password-crypt-salt-format <format>\" line\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: missing format in "
					"\"password-crypt-salt-format <format>\" line\n",
				    fname, lineno, 0 );
#endif

				return 1;
			}

			lutil_salt_format( cargv[1] );

		/* set SASL host */
		} else if ( strcasecmp( cargv[0], "sasl-host" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing host in \"sasl-host <host>\" line\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing host in \"sasl-host <host>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			if ( global_host != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: already set sasl-host!\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: already set sasl-host!\n",
					fname, lineno, 0 );
#endif

				return 1;

			} else {
				global_host = ch_strdup( cargv[1] );
			}

		/* set SASL realm */
		} else if ( strcasecmp( cargv[0], "sasl-realm" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing realm in \"sasl-realm <realm>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing realm in \"sasl-realm <realm>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			if ( global_realm != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: already set sasl-realm!\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: already set sasl-realm!\n",
					fname, lineno, 0 );
#endif

				return 1;

			} else {
				global_realm = ch_strdup( cargv[1] );
			}

		} else if ( !strcasecmp( cargv[0], "sasl-regexp" ) 
			|| !strcasecmp( cargv[0], "saslregexp" ) )
		{
			int rc;
			if ( cargc != 3 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: need 2 args in "
					   "\"saslregexp <match> <replace>\"\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, 
				"%s: line %d: need 2 args in \"saslregexp <match> <replace>\"\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			rc = slap_sasl_regexp_config( cargv[1], cargv[2] );
			if ( rc ) {
				return rc;
			}

		/* SASL security properties */
		} else if ( strcasecmp( cargv[0], "sasl-secprops" ) == 0 ) {
			char *txt;

			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing flags in "
					   "\"sasl-secprops <properties>\" line\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing flags in \"sasl-secprops <properties>\" line\n",
				    fname, lineno, 0 );
#endif

				return 1;
			}

			txt = slap_sasl_secprops( cargv[1] );
			if ( txt != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d sas-secprops: %s\n",
					   fname, lineno, txt ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: sasl-secprops: %s\n",
				    fname, lineno, txt );
#endif

				return 1;
			}

		} else if ( strcasecmp( cargv[0], "sasl-external-x509dn-convert" ) == 0 ) {
			sasl_external_x509dn_convert++;

		/* set UCDATA path */
		} else if ( strcasecmp( cargv[0], "ucdata-path" ) == 0 ) {
			int err;
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing path in "
					   "\"ucdata-path <path>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing path in \"ucdata-path <path>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			err = load_ucdata( cargv[1] );
			if ( err <= 0 ) {
				if ( err == 0 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						   "%s: line %d: ucdata already loaded, ucdata-path "
						   "must be set earlier in the file and/or be "
						   "specified only once!\n",
						   fname, lineno ));
#else
					Debug( LDAP_DEBUG_ANY,
					       "%s: line %d: ucdata already loaded, ucdata-path must be set earlier in the file and/or be specified only once!\n",
					       fname, lineno, 0 );
#endif

				}
				return( 1 );
			}

		/* set size limit */
		} else if ( strcasecmp( cargv[0], "sizelimit" ) == 0 ) {
			int rc = 0, i;
			struct slap_limits_set *lim;
			
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing limit in \"sizelimit <limit>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing limit in \"sizelimit <limit>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			if ( be == NULL ) {
				lim = &deflimit;
			} else {
				lim = &be->be_def_limit;
			}

			for ( i = 1; i < cargc; i++ ) {
				if ( strncasecmp( cargv[i], "size", 4 ) == 0 ) {
					rc = parse_limit( cargv[i], lim );
				} else {
					lim->lms_s_soft = atoi( cargv[i] );
					lim->lms_s_hard = 0;
				}

				if ( rc ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						   "%s: line %d: unable "
						   "to parse value \"%s\" "
						   "in \"sizelimit "
						   "<limit>\" line.\n",
						   fname, lineno, cargv[i] ));
#else
					Debug( LDAP_DEBUG_ANY,
					    	"%s: line %d: unable "
						"to parse value \"%s\" "
						"in \"sizelimit "
						"<limit>\" line\n",
    						fname, lineno, cargv[i] );
#endif
				}
			}

		/* set time limit */
		} else if ( strcasecmp( cargv[0], "timelimit" ) == 0 ) {
			int rc = 0, i;
			struct slap_limits_set *lim;
			
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d missing limit in \"timelimit <limit>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing limit in \"timelimit <limit>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			
			if ( be == NULL ) {
				lim = &deflimit;
			} else {
				lim = &be->be_def_limit;
			}

			for ( i = 1; i < cargc; i++ ) {
				if ( strncasecmp( cargv[i], "time", 4 ) == 0 ) {
					rc = parse_limit( cargv[i], lim );
				} else {
					lim->lms_t_soft = atoi( cargv[i] );
					lim->lms_t_hard = 0;
				}

				if ( rc ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						   "%s: line %d: unable "
						   "to parse value \"%s\" "
						   "in \"timelimit "
						   "<limit>\" line.\n",
						   fname, lineno, cargv[i] ));
#else
					Debug( LDAP_DEBUG_ANY,
						"%s: line %d: unable "
						"to parse value \"%s\" "
						"in \"timelimit "
						"<limit>\" line\n",
						fname, lineno, cargv[i] );
#endif
				}
			}

		/* set regex-based limits */
		} else if ( strcasecmp( cargv[0], "limits" ) == 0 ) {
			if ( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_WARNING,
					   "%s: line %d \"limits\" allowed only in database environment.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	"%s: line %d \"limits\" allowed only in database environment.\n%s",
					fname, lineno, "" );
#endif
				return( 1 );
			}

			if ( parse_limits( be, fname, lineno, cargc, cargv ) ) {
				return( 1 );
			}

		/* mark this as a subordinate database */
		} else if ( strcasecmp( cargv[0], "subordinate" ) == 0 ) {
			if ( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO, "%s: line %d: "
					"subordinate keyword must appear inside a database "
					"definition (ignored).\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: suffix line "
					"must appear inside a database definition (ignored)\n",
				    fname, lineno, 0 );
#endif
			} else {
				be->be_flags |= SLAP_BFLAG_GLUE_SUBORDINATE;
				num_subordinates++;
			}

		/* set database suffix */
		} else if ( strcasecmp( cargv[0], "suffix" ) == 0 ) {
			Backend *tmp_be;
			struct berval dn;
			struct berval *pdn = NULL;
			struct berval *ndn = NULL;

			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: missing dn in \"suffix <dn>\" line.\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"missing dn in \"suffix <dn>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );

			} else if ( cargc > 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					"%s: line %d: extra cruft after <dn> in \"suffix %s\""
					" line (ignored).\n", fname, lineno, cargv[1] ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: extra cruft "
					"after <dn> in \"suffix %s\" line (ignored)\n",
				    fname, lineno, cargv[1] );
#endif
			}

			if ( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					"%s: line %d: suffix line must appear inside a database "
					"definition.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: suffix line "
					"must appear inside a database definition\n",
				    fname, lineno, 0 );
#endif
				return( 1 );

#if defined(SLAPD_MONITOR_DN)
			/* "cn=Monitor" is reserved for monitoring slap */
			} else if ( strcasecmp( cargv[1], SLAPD_MONITOR_DN ) == 0 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT, "%s: line %d: \""
					SLAPD_MONITOR_DN "\" is reserved for monitoring slapd\n", 
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: \""
					SLAPD_MONITOR_DN "\" is reserved for monitoring slapd\n", 
					fname, lineno, 0 );
#endif
				return( 1 );
#endif /* SLAPD_MONITOR_DN */
			}

			if ( load_ucdata( NULL ) < 0 ) return 1;

			dn.bv_val = cargv[1];
			dn.bv_len = strlen( cargv[1] );
			pdn = ch_malloc( sizeof( struct berval ));
			ndn = ch_malloc( sizeof( struct berval ));

			rc = dnPrettyNormal( NULL, &dn, pdn, ndn );
			if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: suffix DN is invalid.\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: suffix DN is invalid\n",
				   fname, lineno, 0 );
#endif
				return( 1 );
			}

			tmp_be = select_backend( ndn, 0, 0 );
			if ( tmp_be == be ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					"%s: line %d: suffix already served by this backend "
					"(ignored)\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: suffix "
					"already served by this backend (ignored)\n",
				    fname, lineno, 0 );
#endif
				ber_bvfree( pdn );
				ber_bvfree( ndn );

			} else if ( tmp_be  != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					"%s: line %d: suffix already served by a preceding "
					"backend \"%s\"\n", fname, lineno,
					tmp_be->be_suffix[0]->bv_val ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: suffix "
					"already served by a preceeding backend \"%s\"\n",
				    fname, lineno, tmp_be->be_suffix[0]->bv_val );
#endif
				ber_bvfree( pdn );
				ber_bvfree( ndn );
				return( 1 );

			} else if( pdn->bv_len == 0 && default_search_nbase.bv_len ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_INFO,
						"%s: line %d: suffix DN empty and default search "
						"base provided \"%s\" (assuming okay).\n",
						fname, lineno, default_search_base.bv_val ));
#else
					Debug( LDAP_DEBUG_ANY, "%s: line %d: "
						"suffix DN empty and default "
						"search base provided \"%s\" (assuming okay)\n",
			    		fname, lineno, default_search_base.bv_val );
#endif
			}

			ber_bvecadd( &be->be_suffix, pdn );
			ber_bvecadd( &be->be_nsuffix, ndn );

		/* set database suffixAlias */
		} else if ( strcasecmp( cargv[0], "suffixAlias" ) == 0 ) {
			Backend *tmp_be;
			struct berval alias, *palias, nalias;
			struct berval aliased, *paliased, naliased;

			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: missing alias and aliased_dn in "
					"\"suffixAlias <alias> <aliased_dn>\" line.\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: missing alias and aliased_dn in "
					"\"suffixAlias <alias> <aliased_dn>\" line.\n",
					fname, lineno, 0 );
#endif

				return( 1 );
			} else if ( cargc < 3 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: missing aliased_dn in "
					"\"suffixAlias <alias> <aliased_dn>\" line\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: missing aliased_dn in "
					"\"suffixAlias <alias> <aliased_dn>\" line\n",
					fname, lineno, 0 );
#endif

				return( 1 );
			} else if ( cargc > 3 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: extra cruft in suffixAlias line (ignored)\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: extra cruft in suffixAlias line (ignored)\n",
					fname, lineno, 0 );
#endif

			}

			if ( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					"%s: line %d: suffixAlias line must appear inside a "
					"database definition (ignored).\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: suffixAlias line"
					" must appear inside a database definition (ignored)\n",
					fname, lineno, 0 );
#endif
			}

			if ( load_ucdata( NULL ) < 0 ) return 1;
			
			alias.bv_val = cargv[1];
			alias.bv_len = strlen( cargv[1] );
			palias = ch_malloc(sizeof(struct berval));

			rc = dnPrettyNormal( NULL, &alias, palias, &nalias );
			if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: alias DN is invalid.\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: alias DN is invalid\n",
				   fname, lineno, 0 );
#endif
				return( 1 );
			}

			tmp_be = select_backend( &nalias, 0, 0 );
			free( nalias.bv_val );
			if ( tmp_be && tmp_be != be ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					"%s: line %d: suffixAlias served by a preceeding "
					"backend \"%s\"\n",
					fname, lineno, tmp_be->be_suffix[0]->bv_val ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: suffixAlias served by"
					"  a preceeding backend \"%s\"\n",
					fname, lineno, tmp_be->be_suffix[0]->bv_val );
#endif
				ber_bvfree( palias );
				return -1;
			}

			aliased.bv_val = cargv[2];
			aliased.bv_len = strlen( cargv[2] );
			paliased = ch_malloc(sizeof(struct berval));

			rc = dnPrettyNormal( NULL, &aliased, paliased, &naliased );
			if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: aliased DN is invalid.\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: aliased DN is invalid\n",
				   fname, lineno, 0 );
#endif
				ber_bvfree( palias );
				return( 1 );
			}

			tmp_be = select_backend( &naliased, 0, 0 );
			free( naliased.bv_val );
			if ( tmp_be && tmp_be != be ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					"%s: line %d: suffixAlias derefs to a different backend "
					"a preceeding backend \"%s\"\n",
					fname, lineno, tmp_be->be_suffix[0]->bv_val ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: suffixAlias derefs to differnet backend"
					"  a preceeding backend \"%s\"\n",
					fname, lineno, tmp_be->be_suffix[0]->bv_val );
#endif
				ber_bvfree( palias );
				ber_bvfree( paliased );
				return -1;
			}

			ber_bvecadd( &be->be_suffixAlias, palias ); 
			ber_bvecadd( &be->be_suffixAlias, paliased );

               /* set max deref depth */
               } else if ( strcasecmp( cargv[0], "maxDerefDepth" ) == 0 ) {
					int i;
                       if ( cargc < 2 ) {
#ifdef NEW_LOGGING
			       LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					  "%s: line %d: missing depth in \"maxDerefDepth <depth>\""
					  " line\n", fname, lineno ));
#else
                               Debug( LDAP_DEBUG_ANY,
                   "%s: line %d: missing depth in \"maxDerefDepth <depth>\" line\n",
                                   fname, lineno, 0 );
#endif

                               return( 1 );
                       }
                       if ( be == NULL ) {
#ifdef NEW_LOGGING
			       LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					  "%s: line %d: depth line must appear inside a database "
					  "definition (ignored)\n", fname, lineno ));
#else
                               Debug( LDAP_DEBUG_ANY,
"%s: line %d: depth line must appear inside a database definition (ignored)\n",
                                   fname, lineno, 0 );
#endif

                       } else if ((i = atoi(cargv[1])) < 0) {
#ifdef NEW_LOGGING
			       LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					  "%s: line %d: depth must be positive (ignored).\n",
					  fname, lineno ));
#else
                               Debug( LDAP_DEBUG_ANY,
"%s: line %d: depth must be positive (ignored)\n",
                                   fname, lineno, 0 );
#endif


                       } else {
                           be->be_max_deref_depth = i;
					   }


		/* set magic "root" dn for this database */
		} else if ( strcasecmp( cargv[0], "rootdn" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: missing dn in \"rootdn <dn>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
		    "%s: line %d: missing dn in \"rootdn <dn>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: rootdn line must appear inside a database "
					   "definition (ignored).\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
"%s: line %d: rootdn line must appear inside a database definition (ignored)\n",
				    fname, lineno, 0 );
#endif

			} else {
				struct berval dn;
				
				if ( load_ucdata( NULL ) < 0 ) return 1;

				dn.bv_val = cargv[1];
				dn.bv_len = strlen( cargv[1] );

				rc = dnPrettyNormal( NULL, &dn,
					&be->be_rootdn,
					&be->be_rootndn );

				if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						"%s: line %d: rootdn DN is invalid.\n",
						fname, lineno ));
#else
					Debug( LDAP_DEBUG_ANY,
						"%s: line %d: rootdn DN is invalid\n",
					   fname, lineno, 0 );
#endif
					return( 1 );
				}
			}

		/* set super-secret magic database password */
		} else if ( strcasecmp( cargv[0], "rootpw" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing passwd in \"rootpw <passwd>\""
					   " line\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing passwd in \"rootpw <passwd>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: rootpw line must appear inside a database "
					   "definition (ignored)\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
"%s: line %d: rootpw line must appear inside a database definition (ignored)\n",
				    fname, lineno, 0 );
#endif

			} else {
				be->be_rootpw.bv_val = ch_strdup( cargv[1] );
				be->be_rootpw.bv_len = strlen( be->be_rootpw.bv_val );
			}

		/* make this database read-only */
		} else if ( strcasecmp( cargv[0], "readonly" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing on|off in \"readonly <on|off>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing on|off in \"readonly <on|off>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( be == NULL ) {
				if ( strcasecmp( cargv[1], "on" ) == 0 ) {
					global_restrictops |= SLAP_RESTRICT_OP_WRITES;
				} else {
					global_restrictops &= ~SLAP_RESTRICT_OP_WRITES;
				}
			} else {
				if ( strcasecmp( cargv[1], "on" ) == 0 ) {
					be->be_restrictops |= SLAP_RESTRICT_OP_WRITES;
				} else {
					be->be_restrictops &= ~SLAP_RESTRICT_OP_WRITES;
				}
			}


		/* allow these features */
		} else if ( strcasecmp( cargv[0], "allows" ) == 0 ||
			strcasecmp( cargv[0], "allow" ) == 0 )
		{
			slap_mask_t	allows;

			if ( be != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: allow line must appear prior to "
					   "database definitions.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
"%s: line %d: allow line must appear prior to database definitions\n",
				    fname, lineno, 0 );
#endif

			}

			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing feature(s) in \"allow <features>\""
					   " line\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing feature(s) in \"allow <features>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			allows = 0;

			for( i=1; i < cargc; i++ ) {
				if( strcasecmp( cargv[i], "bind_v2" ) == 0 ) {
					allows |= SLAP_ALLOW_BIND_V2;

				} else if( strcasecmp( cargv[i], "bind_anon_cred" ) == 0 ) {
					allows |= SLAP_ALLOW_BIND_ANON_CRED;

				} else if( strcasecmp( cargv[i], "bind_anon_dn" ) == 0 ) {
					allows |= SLAP_ALLOW_BIND_ANON_DN;

				} else if( strcasecmp( cargv[i], "none" ) != 0 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						   "%s: line %d: unknown feature %s in "
						   "\"allow <features>\" line.\n",
						   fname, lineno, cargv[1] ));
#else
					Debug( LDAP_DEBUG_ANY,
		    "%s: line %d: unknown feature %s in \"allow <features>\" line\n",
					    fname, lineno, cargv[i] );
#endif

					return( 1 );
				}
			}

			global_allows = allows;

		/* disallow these features */
		} else if ( strcasecmp( cargv[0], "disallows" ) == 0 ||
			strcasecmp( cargv[0], "disallow" ) == 0 )
		{
			slap_mask_t	disallows;

			if ( be != NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: disallow line must appear prior to "
					   "database definitions.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
"%s: line %d: disallow line must appear prior to database definitions\n",
				    fname, lineno, 0 );
#endif

			}

			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing feature(s) in \"disallow <features>\""
					   " line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing feature(s) in \"disallow <features>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			disallows = 0;

			for( i=1; i < cargc; i++ ) {
				if( strcasecmp( cargv[i], "bind_anon" ) == 0 ) {
					disallows |= SLAP_DISALLOW_BIND_ANON;

				} else if( strcasecmp( cargv[i], "bind_simple" ) == 0 ) {
					disallows |= SLAP_DISALLOW_BIND_SIMPLE;

				} else if( strcasecmp( cargv[i], "bind_krbv4" ) == 0 ) {
					disallows |= SLAP_DISALLOW_BIND_KRBV4;

				} else if( strcasecmp( cargv[i], "tls_2_anon" ) == 0 ) {
					disallows |= SLAP_DISALLOW_TLS_2_ANON;

				} else if( strcasecmp( cargv[i], "tls_authc" ) == 0 ) {
					disallows |= SLAP_DISALLOW_TLS_AUTHC;

				} else if( strcasecmp( cargv[i], "none" ) != 0 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						"%s: line %d: unknown feature %s in "
						"\"disallow <features>\" line.\n",
						fname, lineno, cargv[i] ));
#else
					Debug( LDAP_DEBUG_ANY,
		    "%s: line %d: unknown feature %s in \"disallow <features>\" line\n",
					    fname, lineno, cargv[i] );
#endif

					return( 1 );
				}
			}

			global_disallows = disallows;

		/* require these features */
		} else if ( strcasecmp( cargv[0], "requires" ) == 0 ||
			strcasecmp( cargv[0], "require" ) == 0 )
		{
			slap_mask_t	requires;

			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing feature(s) in "
					   "\"require <features>\" line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing feature(s) in \"require <features>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			requires = 0;

			for( i=1; i < cargc; i++ ) {
				if( strcasecmp( cargv[i], "bind" ) == 0 ) {
					requires |= SLAP_REQUIRE_BIND;

				} else if( strcasecmp( cargv[i], "LDAPv3" ) == 0 ) {
					requires |= SLAP_REQUIRE_LDAP_V3;

				} else if( strcasecmp( cargv[i], "authc" ) == 0 ) {
					requires |= SLAP_REQUIRE_AUTHC;

				} else if( strcasecmp( cargv[i], "SASL" ) == 0 ) {
					requires |= SLAP_REQUIRE_SASL;

				} else if( strcasecmp( cargv[i], "strong" ) == 0 ) {
					requires |= SLAP_REQUIRE_STRONG;

				} else if( strcasecmp( cargv[i], "none" ) != 0 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						   "%s: line %d: unknown feature %s in "
						   "\"require <features>\" line.\n",
						   fname, lineno ));
#else
					Debug( LDAP_DEBUG_ANY,
		    "%s: line %d: unknown feature %s in \"require <features>\" line\n",
					    fname, lineno, cargv[i] );
#endif

					return( 1 );
				}
			}

			if ( be == NULL ) {
				global_requires = requires;
			} else {
				be->be_requires = requires;
			}

		/* required security factors */
		} else if ( strcasecmp( cargv[0], "security" ) == 0 ) {
			slap_ssf_set_t *set;

			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing factor(s) in \"security <factors>\""
					   " line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing factor(s) in \"security <factors>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			if ( be == NULL ) {
				set = &global_ssf_set;
			} else {
				set = &be->be_ssf_set;
			}

			for( i=1; i < cargc; i++ ) {
				if( strncasecmp( cargv[i], "ssf=",
					sizeof("ssf") ) == 0 )
				{
					set->sss_ssf =
						atoi( &cargv[i][sizeof("ssf")] );

				} else if( strncasecmp( cargv[i], "transport=",
					sizeof("transport") ) == 0 )
				{
					set->sss_transport =
						atoi( &cargv[i][sizeof("transport")] );

				} else if( strncasecmp( cargv[i], "tls=",
					sizeof("tls") ) == 0 )
				{
					set->sss_tls =
						atoi( &cargv[i][sizeof("tls")] );

				} else if( strncasecmp( cargv[i], "sasl=",
					sizeof("sasl") ) == 0 )
				{
					set->sss_sasl =
						atoi( &cargv[i][sizeof("sasl")] );

				} else if( strncasecmp( cargv[i], "update_ssf=",
					sizeof("update_ssf") ) == 0 )
				{
					set->sss_update_ssf =
						atoi( &cargv[i][sizeof("update_ssf")] );

				} else if( strncasecmp( cargv[i], "update_transport=",
					sizeof("update_transport") ) == 0 )
				{
					set->sss_update_transport =
						atoi( &cargv[i][sizeof("update_transport")] );

				} else if( strncasecmp( cargv[i], "update_tls=",
					sizeof("update_tls") ) == 0 )
				{
					set->sss_update_tls =
						atoi( &cargv[i][sizeof("update_tls")] );

				} else if( strncasecmp( cargv[i], "update_sasl=",
					sizeof("update_sasl") ) == 0 )
				{
					set->sss_update_sasl =
						atoi( &cargv[i][sizeof("update_sasl")] );

				} else {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						   "%s: line %d: unknown factor %S in "
						   "\"security <factors>\" line.\n",
						   fname, lineno, cargv[1] ));
#else
					Debug( LDAP_DEBUG_ANY,
		    "%s: line %d: unknown factor %s in \"security <factors>\" line\n",
					    fname, lineno, cargv[i] );
#endif

					return( 1 );
				}
			}
		/* where to send clients when we don't hold it */
		} else if ( strcasecmp( cargv[0], "referral" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing URL in \"referral <URL>\""
					   " line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
		    "%s: line %d: missing URL in \"referral <URL>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			if( validate_global_referral( cargv[1] ) ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT, "%s: line %d: "
					"invalid URL (%s) in \"referral\" line.\n",
					fname, lineno, cargv[1] ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"invalid URL (%s) in \"referral\" line.\n",
				    fname, lineno, cargv[1] );
#endif
				return 1;
			}

			vals[0].bv_val = cargv[1];
			vals[0].bv_len = strlen( vals[0].bv_val );
			value_add( &default_referral, vals );

#ifdef NEW_LOGGING
                } else if ( strcasecmp( cargv[0], "logfile" ) == 0 ) {
                        FILE *logfile;
                        if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: Error in logfile directive, "
					   "\"logfile <filename>\"\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
				       "%s: line %d: Error in logfile directive, \"logfile filename\"\n",
				       fname, lineno, 0 );
#endif

				return( 1 );
                        }
                        logfile = fopen( cargv[1], "w" );
                        if ( logfile != NULL ) lutil_debug_file( logfile );

#endif
		/* start of a new database definition */
		} else if ( strcasecmp( cargv[0], "debug" ) == 0 ) {
                        int level;
			if ( cargc < 3 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: Error in debug directive, "
					   "\"debug <subsys> <level>\"\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: Error in debug directive, \"debug subsys level\"\n",
					fname, lineno, 0 );
#endif

				return( 1 );
			}
                        level = atoi( cargv[2] );
                        if ( level <= 0 ) level = lutil_mnem2level( cargv[2] );
                        lutil_set_debug_level( cargv[1], level );
		/* specify an Object Identifier macro */
		} else if ( strcasecmp( cargv[0], "objectidentifier" ) == 0 ) {
			rc = parse_oidm( fname, lineno, cargc, cargv );
			if( rc ) return rc;

		/* specify an objectclass */
		} else if ( strcasecmp( cargv[0], "objectclass" ) == 0 ) {
			if ( *cargv[1] == '(' ) {
				char * p;
				p = strchr(saveline,'(');
				rc = parse_oc( fname, lineno, p, cargv );
				if( rc ) return rc;

			} else {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: old objectclass format not supported\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
				       "%s: line %d: old objectclass format not supported.\n",
				       fname, lineno, 0 );
#endif

			}

		/* specify an attribute type */
		} else if (( strcasecmp( cargv[0], "attributetype" ) == 0 )
			|| ( strcasecmp( cargv[0], "attribute" ) == 0 ))
		{
			if ( *cargv[1] == '(' ) {
				char * p;
				p = strchr(saveline,'(');
				rc = parse_at( fname, lineno, p, cargv );
				if( rc ) return rc;

			} else {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: old attribute type format not supported.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
    "%s: line %d: old attribute type format not supported.\n",
				    fname, lineno, 0 );
#endif

			}

		/* turn on/off schema checking */
		} else if ( strcasecmp( cargv[0], "schemacheck" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing on|off in "
					   "\"schemacheck <on|off>\" line.\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
    "%s: line %d: missing on|off in \"schemacheck <on|off>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( strcasecmp( cargv[1], "off" ) == 0 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					"%s: line %d: schema checking disabled! your mileage may vary!\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
					"%s: line %d: schema checking disabled! your mileage may vary!\n",
				    fname, lineno, 0 );
#endif
				global_schemacheck = 0;
			} else {
				global_schemacheck = 1;
			}

		/* specify access control info */
		} else if ( strcasecmp( cargv[0], "access" ) == 0 ) {
			parse_acl( be, fname, lineno, cargc, cargv );

		/* debug level to log things to syslog */
		} else if ( strcasecmp( cargv[0], "loglevel" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing level in \"loglevel <level>\""
					   " line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
		    "%s: line %d: missing level in \"loglevel <level>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			ldap_syslog = 0;

			for( i=1; i < cargc; i++ ) {
				ldap_syslog += atoi( cargv[1] );
			}

		/* list of replicas of the data in this backend (master only) */
		} else if ( strcasecmp( cargv[0], "replica" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing host in \"replica "
					   " <host[:port]\" line\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing host in \"replica <host[:port]>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: replica line must appear inside "
					   "a database definition (ignored).\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
"%s: line %d: replica line must appear inside a database definition (ignored)\n",
				    fname, lineno, 0 );
#endif

			} else {
				int nr = -1;

				for ( i = 1; i < cargc; i++ ) {
					if ( strncasecmp( cargv[i], "host=", 5 )
					    == 0 ) {
						nr = add_replica_info( be, 
							cargv[i] + 5 );
						break;
					}
				}
				if ( i == cargc ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_INFO,
						   "%s: line %d: missing host in \"replica\" "
						   "line (ignored)\n", fname, lineno ));
#else
					Debug( LDAP_DEBUG_ANY,
		    "%s: line %d: missing host in \"replica\" line (ignored)\n",
					    fname, lineno, 0 );
#endif

				} else if ( nr == -1 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_INFO,
						   "%s: line %d: unable to add"
				 		   " replica \"%s\""
						   " (ignored)\n",
						   fname, lineno, 
						   cargv[i] + 5 ));
#else
					Debug( LDAP_DEBUG_ANY,
		"%s: line %d: unable to add replica \"%s\" (ignored)\n",
						fname, lineno, cargv[i] + 5 );
#endif
				} else {
					for ( i = 1; i < cargc; i++ ) {
						if ( strncasecmp( cargv[i], "suffix=", 7 ) == 0 ) {

							switch ( add_replica_suffix( be, nr, cargv[i] + 7 ) ) {
							case 1:
#ifdef NEW_LOGGING
								LDAP_LOG(( "config", LDAP_LEVEL_INFO,
										"%s: line %d: suffix \"%s\" in \"replica\" line is not valid for backend (ignored)\n",
										fname, lineno, cargv[i] + 7 ));
#else
								Debug( LDAP_DEBUG_ANY,
										"%s: line %d: suffix \"%s\" in \"replica\" line is not valid for backend (ignored)\n",
										fname, lineno, cargv[i] + 7 );
#endif
								break;

							case 2:
#ifdef NEW_LOGGING
								LDAP_LOG(( "config", LDAP_LEVEL_INFO,
											"%s: line %d: unable to normalize suffix in \"replica\" line (ignored)\n",
											fname, lineno ));
#else
								Debug( LDAP_DEBUG_ANY,
										 "%s: line %d: unable to normalize suffix in \"replica\" line (ignored)\n",
										 fname, lineno, 0 );
#endif
								break;
							}
						} else if ( strncasecmp( cargv[i], "attr=", 5 ) == 0 ) {
							if ( add_replica_attrs( be, nr, cargv[i] + 5 ) ) {
#ifdef NEW_LOGGING
								LDAP_LOG(( "config", LDAP_LEVEL_INFO,
										"%s: line %d: attribute \"%s\" in \"replica\" line is unknown\n",
										fname, lineno, cargv[i] + 5 ));
#else
								Debug( LDAP_DEBUG_ANY,
										"%s: line %d: attribute \"%s\" in \"replica\" line is unknown\n",
										fname, lineno, cargv[i] + 5 );
#endif
								return( 1 );
							}
						}
					}
				}
			}

		/* dn of master entity allowed to write to replica */
		} else if ( strcasecmp( cargv[0], "updatedn" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing dn in \"updatedn <dn>\""
					   " line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
		    "%s: line %d: missing dn in \"updatedn <dn>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: updatedn line must appear inside "
					   "a database definition (ignored)\n",
					   fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
"%s: line %d: updatedn line must appear inside a database definition (ignored)\n",
				    fname, lineno, 0 );
#endif

			} else {
				struct berval dn;

				if ( load_ucdata( NULL ) < 0 ) return 1;

				dn.bv_val = cargv[1];
				dn.bv_len = strlen( cargv[1] );

				rc = dnNormalize2( NULL, &dn, &be->be_update_ndn );
				if( rc != LDAP_SUCCESS ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
						"%s: line %d: updatedn DN is invalid.\n",
						fname, lineno ));
#else
					Debug( LDAP_DEBUG_ANY,
						"%s: line %d: updatedn DN is invalid\n",
					    fname, lineno, 0 );
#endif
					return 1;
				}
			}

		} else if ( strcasecmp( cargv[0], "updateref" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT, "%s: line %d: "
					"missing url in \"updateref <ldapurl>\" line.\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"missing url in \"updateref <ldapurl>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( be == NULL ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO, "%s: line %d: updateref"
					" line must appear inside a database definition\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: updateref"
					" line must appear inside a database definition\n",
					fname, lineno, 0 );
#endif
				return 1;

			} else if ( !be->be_update_ndn.bv_len ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO, "%s: line %d: "
					"updateref line must come after updatedn.\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"updateref line must after updatedn.\n",
				    fname, lineno, 0 );
#endif
				return 1;
			}

			if( validate_global_referral( cargv[1] ) ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT, "%s: line %d: "
					"invalid URL (%s) in \"updateref\" line.\n",
					fname, lineno, cargv[1] ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"invalid URL (%s) in \"updateref\" line.\n",
				    fname, lineno, cargv[1] );
#endif
				return 1;
			}

			vals[0].bv_val = cargv[1];
			vals[0].bv_len = strlen( vals[0].bv_val );
			value_add( &be->be_update_refs, vals );

		/* replication log file to which changes are appended */
		} else if ( strcasecmp( cargv[0], "replogfile" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing filename in \"replogfile <filename>\""
					   " line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing filename in \"replogfile <filename>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( be ) {
				be->be_replogfile = ch_strdup( cargv[1] );
			} else {
				replogfile = ch_strdup( cargv[1] );
			}

		/* file from which to read additional rootdse attrs */
		} else if ( strcasecmp( cargv[0], "rootDSE" ) == 0) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT, "%s: line %d: "
					"missing filename in \"rootDSE <filename>\" line.\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"missing filename in \"rootDSE <filename>\" line.\n",
				    fname, lineno, 0 );
#endif
				return 1;
			}

			if( read_root_dse_file( cargv[1] ) ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT, "%s: line %d: "
					"could not read \"rootDSE <filename>\" line.\n",
					fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY, "%s: line %d: "
					"could not read \"rootDSE <filename>\" line\n",
				    fname, lineno, 0 );
#endif
				return 1;
			}

		/* maintain lastmodified{by,time} attributes */
		} else if ( strcasecmp( cargv[0], "lastmod" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing on|off in \"lastmod <on|off>\""
					   " line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing on|off in \"lastmod <on|off>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			if ( strcasecmp( cargv[1], "on" ) == 0 ) {
				if ( be ) {
					be->be_flags &= ~SLAP_BFLAG_NOLASTMOD;
				} else {
					lastmod = 1;
				}
			} else {
				if ( be ) {
					be->be_flags |= SLAP_BFLAG_NOLASTMOD;
				} else {
					lastmod = 0;
				}
			}

		/* set idle timeout value */
		} else if ( strcasecmp( cargv[0], "idletimeout" ) == 0 ) {
			int i;
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing timeout value in "
					   "\"idletimeout <seconds>\" line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing timeout value in \"idletimeout <seconds>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}

			i = atoi( cargv[1] );

			if( i < 0 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: timeout value (%d) invalid "
					   "\"idletimeout <seconds>\" line.\n",
					   fname, lineno, i ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: timeout value (%d) invalid \"idletimeout <seconds>\" line\n",
				    fname, lineno, i );
#endif

				return( 1 );
			}

			global_idletimeout = i;

		/* include another config file */
		} else if ( strcasecmp( cargv[0], "include" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing filename in \"include "
					   "<filename>\" line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
    "%s: line %d: missing filename in \"include <filename>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			savefname = ch_strdup( cargv[1] );
			savelineno = lineno;

			if ( read_config( savefname ) != 0 ) {
				return( 1 );
			}

			free( savefname );
			lineno = savelineno - 1;

		/* location of kerberos srvtab file */
		} else if ( strcasecmp( cargv[0], "srvtab" ) == 0 ) {
			if ( cargc < 2 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
					   "%s: line %d: missing filename in \"srvtab "
					   "<filename>\" line.\n", fname, lineno ));
#else
				Debug( LDAP_DEBUG_ANY,
	    "%s: line %d: missing filename in \"srvtab <filename>\" line\n",
				    fname, lineno, 0 );
#endif

				return( 1 );
			}
			ldap_srvtab = ch_strdup( cargv[1] );

#ifdef SLAPD_MODULES
                } else if (strcasecmp( cargv[0], "moduleload") == 0 ) {
                   if ( cargc < 2 ) {
#ifdef NEW_LOGGING
			   LDAP_LOG(( "config", LDAP_LEVEL_INFO,
				      "%s: line %d: missing filename in \"moduleload "
				      "<filename>\" line.\n", fname, lineno ));
#else
                      Debug( LDAP_DEBUG_ANY,
                             "%s: line %d: missing filename in \"moduleload <filename>\" line\n",
                             fname, lineno, 0 );
#endif

                      exit( EXIT_FAILURE );
                   }
                   if (module_load(cargv[1], cargc - 2, (cargc > 2) ? cargv + 2 : NULL)) {
#ifdef NEW_LOGGING
			   LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
				      "%s: line %d: failed to load or initialize module %s\n"<
				      fname, lineno, cargv[1] ));
#else
                      Debug( LDAP_DEBUG_ANY,
                             "%s: line %d: failed to load or initialize module %s\n",
                             fname, lineno, cargv[1]);
#endif

                      exit( EXIT_FAILURE );
                   }
                } else if (strcasecmp( cargv[0], "modulepath") == 0 ) {
                   if ( cargc != 2 ) {
#ifdef NEW_LOGGING
			   LDAP_LOG(( "config", LDAP_LEVEL_INFO,
				      "%s: line %d: missing path in \"modulepath <path>\""
				      " line\n", fname, lineno ));
#else
                      Debug( LDAP_DEBUG_ANY,
                             "%s: line %d: missing path in \"modulepath <path>\" line\n",
                             fname, lineno, 0 );
#endif

                      exit( EXIT_FAILURE );
                   }
                   if (module_path( cargv[1] )) {
#ifdef NEW_LOGGING
			   LDAP_LOG(( "cofig", LDAP_LEVEL_CRIT,
				      "%s: line %d: failed to set module search path to %s.\n",
				      fname, lineno, cargv[1] ));
#else
			   Debug( LDAP_DEBUG_ANY,
				  "%s: line %d: failed to set module search path to %s\n",
				  fname, lineno, cargv[1]);
#endif

                      exit( EXIT_FAILURE );
                   }
		   
#endif /*SLAPD_MODULES*/

#ifdef HAVE_TLS
		} else if ( !strcasecmp( cargv[0], "TLSRandFile" ) ) {
			rc = ldap_pvt_tls_set_option( NULL,
						      LDAP_OPT_X_TLS_RANDOM_FILE,
						      cargv[1] );
			if ( rc )
				return rc;

		} else if ( !strcasecmp( cargv[0], "TLSCipherSuite" ) ) {
			rc = ldap_pvt_tls_set_option( NULL,
						      LDAP_OPT_X_TLS_CIPHER_SUITE,
						      cargv[1] );
			if ( rc )
				return rc;

		} else if ( !strcasecmp( cargv[0], "TLSCertificateFile" ) ) {
			rc = ldap_pvt_tls_set_option( NULL,
						      LDAP_OPT_X_TLS_CERTFILE,
						      cargv[1] );
			if ( rc )
				return rc;

		} else if ( !strcasecmp( cargv[0], "TLSCertificateKeyFile" ) ) {
			rc = ldap_pvt_tls_set_option( NULL,
						      LDAP_OPT_X_TLS_KEYFILE,
						      cargv[1] );
			if ( rc )
				return rc;

		} else if ( !strcasecmp( cargv[0], "TLSCACertificatePath" ) ) {
			rc = ldap_pvt_tls_set_option( NULL,
						      LDAP_OPT_X_TLS_CACERTDIR,
						      cargv[1] );
			if ( rc )
				return rc;

		} else if ( !strcasecmp( cargv[0], "TLSCACertificateFile" ) ) {
			rc = ldap_pvt_tls_set_option( NULL,
						      LDAP_OPT_X_TLS_CACERTFILE,
						      cargv[1] );
			if ( rc )
				return rc;
		} else if ( !strcasecmp( cargv[0], "TLSVerifyClient" ) ) {
			if ( isdigit( cargv[1][0] ) ) {
				i = atoi(cargv[1]);
				rc = ldap_pvt_tls_set_option( NULL,
						      LDAP_OPT_X_TLS_REQUIRE_CERT,
						      &i );
			} else {
				rc = ldap_int_tls_config( NULL,
						      LDAP_OPT_X_TLS_REQUIRE_CERT,
						      cargv[1] );
			}

			if ( rc )
				return rc;

#endif

		/* pass anything else to the current backend info/db config routine */
		} else {
			if ( bi != NULL ) {
				if ( bi->bi_config == 0 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_INFO,
						   "%s: line %d: unknown directive \"%s\" inside "
						   "backend info definition (ignored).\n",
						   fname, lineno, cargv[0] ));
#else
					Debug( LDAP_DEBUG_ANY,
"%s: line %d: unknown directive \"%s\" inside backend info definition (ignored)\n",
				   		fname, lineno, cargv[0] );
#endif

				} else {
					if ( (*bi->bi_config)( bi, fname, lineno, cargc, cargv )
						!= 0 )
					{
						return( 1 );
					}
				}
			} else if ( be != NULL ) {
				if ( be->be_config == 0 ) {
#ifdef NEW_LOGGING
					LDAP_LOG(( "config", LDAP_LEVEL_INFO,
						   "%s: line %d: uknown directive \"%s\" inside "
						   "backend database definition (ignored).\n",
						   fname, lineno, cargv[0] ));
#else
					Debug( LDAP_DEBUG_ANY,
"%s: line %d: unknown directive \"%s\" inside backend database definition (ignored)\n",
				    	fname, lineno, cargv[0] );
#endif

				} else {
					if ( (*be->be_config)( be, fname, lineno, cargc, cargv )
						!= 0 )
					{
						return( 1 );
					}
				}
			} else {
#ifdef NEW_LOGGING
				LDAP_LOG(( "config", LDAP_LEVEL_INFO,
					   "%s: line %d: unknown directive \"%s\" outside backend "
					   "info and database definitions (ignored).\n",
					   fname, lineno, cargv[0] ));
#else
				Debug( LDAP_DEBUG_ANY,
"%s: line %d: unknown directive \"%s\" outside backend info and database definitions (ignored)\n",
				    fname, lineno, cargv[0] );
#endif

			}
		}
		free( saveline );
	}
	fclose( fp );

	if ( load_ucdata( NULL ) < 0 ) return 1;
	return( 0 );
}

static int
fp_parse_line(
    char	*line,
    int		*argcp,
    char	**argv
)
{
	char *	token;

	*argcp = 0;
	for ( token = strtok_quote( line, " \t" ); token != NULL;
	    token = strtok_quote( NULL, " \t" ) ) {
		if ( *argcp == MAXARGS ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
				   "fp_parse_line: too many tokens (%d max).\n",
				   MAXARGS ));
#else
			Debug( LDAP_DEBUG_ANY, "Too many tokens (max %d)\n",
			    MAXARGS, 0, 0 );
#endif

			return( 1 );
		}
		argv[(*argcp)++] = token;
	}
	argv[*argcp] = NULL;
	return 0;
}

static char *
strtok_quote( char *line, char *sep )
{
	int		inquote;
	char		*tmp;
	static char	*next;

	if ( line != NULL ) {
		next = line;
	}
	while ( *next && strchr( sep, *next ) ) {
		next++;
	}

	if ( *next == '\0' ) {
		next = NULL;
		return( NULL );
	}
	tmp = next;

	for ( inquote = 0; *next; ) {
		switch ( *next ) {
		case '"':
			if ( inquote ) {
				inquote = 0;
			} else {
				inquote = 1;
			}
			AC_MEMCPY( next, next + 1, strlen( next + 1 ) + 1 );
			break;

		case '\\':
			if ( next[1] )
				AC_MEMCPY( next,
					    next + 1, strlen( next + 1 ) + 1 );
			next++;		/* dont parse the escaped character */
			break;

		default:
			if ( ! inquote ) {
				if ( strchr( sep, *next ) != NULL ) {
					*next++ = '\0';
					return( tmp );
				}
			}
			next++;
			break;
		}
	}

	return( tmp );
}

static char	buf[BUFSIZ];
static char	*line;
static int	lmax, lcur;

#define CATLINE( buf )	{ \
	int	len; \
	len = strlen( buf ); \
	while ( lcur + len + 1 > lmax ) { \
		lmax += BUFSIZ; \
		line = (char *) ch_realloc( line, lmax ); \
	} \
	strcpy( line + lcur, buf ); \
	lcur += len; \
}

static char *
fp_getline( FILE *fp, int *lineno )
{
	char		*p;

	lcur = 0;
	CATLINE( buf );
	(*lineno)++;

	/* hack attack - keeps us from having to keep a stack of bufs... */
	if ( strncasecmp( line, "include", 7 ) == 0 ) {
		buf[0] = '\0';
		return( line );
	}

	while ( fgets( buf, sizeof(buf), fp ) != NULL ) {
		/* trim off \r\n or \n */
		if ( (p = strchr( buf, '\n' )) != NULL ) {
			if( p > buf && p[-1] == '\r' ) --p;
			*p = '\0';
		}
		
		/* trim off trailing \ and append the next line */
		if ( line[ 0 ] != '\0' 
				&& (p = line + strlen( line ) - 1)[ 0 ] == '\\'
				&& p[ -1 ] != '\\' ) {
			p[ 0 ] = '\0';
			lcur--;

		} else {
			if ( ! isspace( (unsigned char) buf[0] ) ) {
				return( line );
			}

			/* change leading whitespace to a space */
			buf[0] = ' ';
		}

		CATLINE( buf );
		(*lineno)++;
	}
	buf[0] = '\0';

	return( line[0] ? line : NULL );
}

static void
fp_getline_init( int *lineno )
{
	*lineno = -1;
	buf[0] = '\0';
}

/* Loads ucdata, returns 1 if loading, 0 if already loaded, -1 on error */
static int
load_ucdata( char *path )
{
	static int loaded = 0;
	int err;
	
	if ( loaded ) {
		return( 0 );
	}
	err = ucdata_load( path ? path : SLAPD_DEFAULT_UCDATA, UCDATA_ALL );
	if ( err ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "config", LDAP_LEVEL_CRIT,
			   "load_ucdata: Error %d loading ucdata.\n", err ));
#else
		Debug( LDAP_DEBUG_ANY, "error loading ucdata (error %d)\n",
		       err, 0, 0 );
#endif

		return( -1 );
	}
	loaded = 1;
	return( 1 );
}

void
config_destroy( )
{
	ucdata_unload( UCDATA_ALL );
	free( line );
	if ( slapd_args_file )
		free ( slapd_args_file );
	if ( slapd_pid_file )
		free ( slapd_pid_file );
	acl_destroy( global_acl, NULL );
}
