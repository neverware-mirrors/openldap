/* init.c - initialize mdb backend */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2000-2011 The OpenLDAP Foundation.
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
#include <ac/unistd.h>
#include <ac/stdlib.h>
#include <ac/errno.h>
#include <sys/stat.h>
#include "back-mdb.h"
#include <lutil.h>
#include <ldap_rq.h>
#include "config.h"

static const struct berval mdmi_databases[] = {
	BER_BVC("ad2i"),
	BER_BVC("dn2i"),
	BER_BVC("id2e"),
	BER_BVNULL
};

static int
mdb_db_init( BackendDB *be, ConfigReply *cr )
{
	struct mdb_info	*mdb;
	int rc;

	Debug( LDAP_DEBUG_TRACE,
		LDAP_XSTRING(mdb_db_init) ": Initializing mdb database\n",
		0, 0, 0 );

	/* allocate backend-database-specific stuff */
	mdb = (struct mdb_info *) ch_calloc( 1, sizeof(struct mdb_info) );

	/* DBEnv parameters */
	mdb->mi_dbenv_home = ch_strdup( SLAPD_DEFAULT_DB_DIR );
	mdb->mi_dbenv_flags = 0;
	mdb->mi_dbenv_mode = SLAPD_DEFAULT_DB_MODE;

	mdb->mi_search_stack_depth = DEFAULT_SEARCH_STACK_DEPTH;
	mdb->mi_search_stack = NULL;

	mdb->mi_mapsize = DEFAULT_MAPSIZE;

	ldap_pvt_thread_mutex_init( &mdb->mi_database_mutex );

	be->be_private = mdb;
	be->be_cf_ocs = be->bd_info->bi_cf_ocs;

#ifndef MDB_MULTIPLE_SUFFIXES
	SLAP_DBFLAGS( be ) |= SLAP_DBFLAG_ONE_SUFFIX;
#endif

	rc = mdb_monitor_db_init( be );

	return rc;
}

static int
mdb_db_close( BackendDB *be, ConfigReply *cr );

static int
mdb_db_open( BackendDB *be, ConfigReply *cr )
{
	int rc, i;
	struct mdb_info *mdb = (struct mdb_info *) be->be_private;
	struct stat stat1, stat2;
	u_int32_t flags;
	char path[MAXPATHLEN];
	char *dbhome;
	Entry *e = NULL;
	MDB_txn *txn;

	if ( be->be_suffix == NULL ) {
		Debug( LDAP_DEBUG_ANY,
			LDAP_XSTRING(mdb_db_open) ": need suffix.\n",
			1, 0, 0 );
		return -1;
	}

	Debug( LDAP_DEBUG_ARGS,
		LDAP_XSTRING(mdb_db_open) ": \"%s\"\n",
		be->be_suffix[0].bv_val, 0, 0 );

	/* Check existence of dbenv_home. Any error means trouble */
	rc = stat( mdb->mi_dbenv_home, &stat1 );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_ANY,
			LDAP_XSTRING(mdb_db_open) ": database \"%s\": "
			"cannot access database directory \"%s\" (%d).\n",
			be->be_suffix[0].bv_val, mdb->mi_dbenv_home, errno );
		return -1;
	}

	/* mdb is always clean */
	be->be_flags |= SLAP_DBFLAG_CLEAN;

	rc = mdb_env_create( &mdb->mi_dbenv );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_ANY,
			LDAP_XSTRING(mdb_db_open) ": database \"%s\": "
			"mdb_env_create failed: %s (%d).\n",
			be->be_suffix[0].bv_val, mdb_strerror(rc), rc );
		goto fail;
	}

	rc = mdb_env_set_mapsize( mdb->mi_dbenv, mdb->mi_mapsize );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_ANY,
			LDAP_XSTRING(mdb_db_open) ": database \"%s\": "
			"mdb_env_set_mapsize failed: %s (%d).\n",
			be->be_suffix[0].bv_val, mdb_strerror(rc), rc );
		goto fail;
	}

	rc = mdb_env_set_maxdbs( mdb->mi_dbenv, MDB_INDICES );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_ANY,
			LDAP_XSTRING(mdb_db_open) ": database \"%s\": "
			"mdb_env_set_maxdbs failed: %s (%d).\n",
			be->be_suffix[0].bv_val, mdb_strerror(rc), rc );
		goto fail;
	}

#ifdef HAVE_EBCDIC
	strcpy( path, mdb->mi_dbenv_home );
	__atoe( path );
	dbhome = path;
#else
	dbhome = mdb->mi_dbenv_home;
#endif

	Debug( LDAP_DEBUG_TRACE,
		LDAP_XSTRING(mdb_db_open) ": database \"%s\": "
		"dbenv_open(%s).\n",
		be->be_suffix[0].bv_val, mdb->mi_dbenv_home, 0);

	flags = mdb->mi_dbenv_flags;

	if ( slapMode & SLAP_TOOL_QUICK )
		flags |= MDB_NOSYNC;

	if ( slapMode & SLAP_TOOL_READONLY)
		flags |= MDB_RDONLY;

	rc = mdb_env_open( mdb->mi_dbenv, dbhome,
			flags, mdb->mi_dbenv_mode );

	if ( rc ) {
		Debug( LDAP_DEBUG_ANY,
			LDAP_XSTRING(mdb_db_open) ": database \"%s\" cannot be opened, err %d. "
			"Restore from backup!\n",
			be->be_suffix[0].bv_val, rc, 0 );
		goto fail;
	}

	mdb->mi_databases = (struct mdb_db_info **) ch_malloc(
		MDB_INDICES * sizeof(struct mdb_db_info *) );

	rc = mdb_txn_begin( mdb->mi_dbenv, 0, &txn );
	if ( rc ) {
		Debug( LDAP_DEBUG_ANY,
			LDAP_XSTRING(mdb_db_open) ": database \"%s\" cannot be opened, err %d. "
			"Restore from backup!\n",
			be->be_suffix[0].bv_val, rc, 0 );
		goto fail;
	}

	/* open (and create) main databases */
	for( i = 0; mdmi_databases[i].bv_val; i++ ) {
		struct mdb_db_info *db;

		db = (struct mdb_db_info *) ch_calloc(1, sizeof(struct mdb_db_info));

		flags = MDB_INTEGERKEY;
		if( i == MDB_ID2ENTRY ) {
			if ( !(slapMode & (SLAP_TOOL_READMAIN|SLAP_TOOL_READONLY) ))
				flags |= MDB_CREATE;
		} else {
			if ( i == MDB_DN2ID )
				flags |= MDB_DUPSORT;
			if ( !(slapMode & SLAP_TOOL_READONLY) )
				flags |= MDB_CREATE;
		}

		rc = mdb_open( txn,
			mdmi_databases[i].bv_val,
			flags,
			&db->mdi_dbi );

		if ( rc != 0 ) {
			snprintf( cr->msg, sizeof(cr->msg), "database \"%s\": "
				"mdb_open(%s/%s) failed: %s (%d).", 
				be->be_suffix[0].bv_val, 
				mdb->mi_dbenv_home, mdmi_databases[i].bv_val,
				mdb_strerror(rc), rc );
			Debug( LDAP_DEBUG_ANY,
				LDAP_XSTRING(mdb_db_open) ": %s\n",
				cr->msg, 0, 0 );
			goto fail;
		}

		if ( i == MDB_DN2ID )
			mdb_set_dupsort( txn, db->mdi_dbi, mdb_dup_compare );

		db->mdi_name = mdmi_databases[i];
		mdb->mi_databases[i] = db;
	}

	rc = mdb_txn_commit(txn);
	if ( rc != 0 ) {
		goto fail;
	}

	mdb->mi_databases[i] = NULL;
	mdb->mi_ndatabases = i;

	/* monitor setup */
	rc = mdb_monitor_db_open( be );
	if ( rc != 0 ) {
		goto fail;
	}

	mdb->mi_flags |= MDB_IS_OPEN;

	return 0;

fail:
	mdb_db_close( be, NULL );
	return rc;
}

static int
mdb_db_close( BackendDB *be, ConfigReply *cr )
{
	int rc;
	struct mdb_info *mdb = (struct mdb_info *) be->be_private;
	struct mdb_db_info *db;
	MDB_txn *txn;

	/* monitor handling */
	(void)mdb_monitor_db_close( be );

	mdb->mi_flags &= ~MDB_IS_OPEN;

#if 0
	if( mdb->mi_dbenv ) {
		mdb_reader_flush( mdb->mi_dbenv );
	}
#endif

	if ( mdb->mi_dbenv ) {
		rc = mdb_txn_begin( mdb->mi_dbenv, 1, &txn );

		while( mdb->mi_databases && mdb->mi_ndatabases-- ) {
			db = mdb->mi_databases[mdb->mi_ndatabases];
			mdb_close( txn, db->mdi_dbi );
			/* Lower numbered names are not strdup'd */
			if( mdb->mi_ndatabases >= MDB_NDB )
				free( db->mdi_name.bv_val );
			free( db );
		}
		mdb_txn_abort( txn );

		/* force a sync */
		rc = mdb_env_sync( mdb->mi_dbenv, 1 );
		if( rc != 0 ) {
			Debug( LDAP_DEBUG_ANY,
				"mdb_db_close: database \"%s\": "
				"mdb_env_sync failed: %s (%d).\n",
				be->be_suffix[0].bv_val, mdb_strerror(rc), rc );
		}

		mdb_env_close( mdb->mi_dbenv );
		mdb->mi_dbenv = NULL;
	}

	free( mdb->mi_databases );
	mdb->mi_databases = NULL;

	return 0;
}

static int
mdb_db_destroy( BackendDB *be, ConfigReply *cr )
{
	struct mdb_info *mdb = (struct mdb_info *) be->be_private;

	/* stop and remove checkpoint task */
	if ( mdb->mi_txn_cp_task ) {
		struct re_s *re = mdb->mi_txn_cp_task;
		mdb->mi_txn_cp_task = NULL;
		ldap_pvt_thread_mutex_lock( &slapd_rq.rq_mutex );
		if ( ldap_pvt_runqueue_isrunning( &slapd_rq, re ) )
			ldap_pvt_runqueue_stoptask( &slapd_rq, re );
		ldap_pvt_runqueue_remove( &slapd_rq, re );
		ldap_pvt_thread_mutex_unlock( &slapd_rq.rq_mutex );
	}

	/* monitor handling */
	(void)mdb_monitor_db_destroy( be );

	if( mdb->mi_dbenv_home ) ch_free( mdb->mi_dbenv_home );

	mdb_attr_index_destroy( mdb );

	ldap_pvt_thread_mutex_destroy( &mdb->mi_database_mutex );

	ch_free( mdb );
	be->be_private = NULL;

	return 0;
}

int
mdb_back_initialize(
	BackendInfo	*bi )
{
	int rc;

	static char *controls[] = {
		LDAP_CONTROL_ASSERT,
		LDAP_CONTROL_MANAGEDSAIT,
		LDAP_CONTROL_NOOP,
		LDAP_CONTROL_PAGEDRESULTS,
		LDAP_CONTROL_PRE_READ,
		LDAP_CONTROL_POST_READ,
		LDAP_CONTROL_SUBENTRIES,
		LDAP_CONTROL_X_PERMISSIVE_MODIFY,
#ifdef LDAP_X_TXN
		LDAP_CONTROL_X_TXN_SPEC,
#endif
		NULL
	};

	/* initialize the underlying database system */
	Debug( LDAP_DEBUG_TRACE,
		LDAP_XSTRING(mdb_back_initialize) ": initialize " 
		MDB_UCTYPE " backend\n", 0, 0, 0 );

	bi->bi_flags |=
		SLAP_BFLAG_INCREMENT |
		SLAP_BFLAG_SUBENTRIES |
		SLAP_BFLAG_ALIASES |
		SLAP_BFLAG_REFERRALS;

	bi->bi_controls = controls;

	{	/* version check */
		int major, minor, patch, ver;
		char *version = mdb_version( &major, &minor, &patch );
#ifdef HAVE_EBCDIC
		char v2[1024];

		/* All our stdio does an ASCII to EBCDIC conversion on
		 * the output. Strings from the MDB library are already
		 * in EBCDIC; we have to go back and forth...
		 */
		strcpy( v2, version );
		__etoa( v2 );
		version = v2;
#endif
		ver = (major << 24) | (minor << 16) | patch;
		if( ver != MDB_VERSION_FULL ) {
			/* fail if a versions don't match */
			Debug( LDAP_DEBUG_ANY,
				LDAP_XSTRING(mdb_back_initialize) ": "
				"MDB library version mismatch:"
				" expected " MDB_VERSION_STRING ","
				" got %s\n", version, 0, 0 );
			return -1;
		}

		Debug( LDAP_DEBUG_TRACE, LDAP_XSTRING(mdb_back_initialize)
			": %s\n", version, 0, 0 );
	}

	bi->bi_open = 0;
	bi->bi_close = 0;
	bi->bi_config = 0;
	bi->bi_destroy = 0;

	bi->bi_db_init = mdb_db_init;
	bi->bi_db_config = config_generic_wrapper;
	bi->bi_db_open = mdb_db_open;
	bi->bi_db_close = mdb_db_close;
	bi->bi_db_destroy = mdb_db_destroy;

#if 0
	bi->bi_op_add = mdb_add;
	bi->bi_op_bind = mdb_bind;
	bi->bi_op_compare = mdb_compare;
	bi->bi_op_delete = mdb_delete;
	bi->bi_op_modify = mdb_modify;
	bi->bi_op_modrdn = mdb_modrdn;
	bi->bi_op_search = mdb_search;

	bi->bi_op_unbind = 0;

	bi->bi_extended = mdb_extended;

	bi->bi_chk_referrals = mdb_referrals;
	bi->bi_operational = mdb_operational;

	bi->bi_has_subordinates = mdb_hasSubordinates;
#endif
	bi->bi_entry_release_rw = mdb_entry_release;
	bi->bi_entry_get_rw = mdb_entry_get;

	/*
	 * hooks for slap tools
	 */
	bi->bi_tool_entry_open = mdb_tool_entry_open;
	bi->bi_tool_entry_close = mdb_tool_entry_close;
	bi->bi_tool_entry_first = backend_tool_entry_first;
	bi->bi_tool_entry_first_x = mdb_tool_entry_first_x;
	bi->bi_tool_entry_next = mdb_tool_entry_next;
	bi->bi_tool_entry_get = mdb_tool_entry_get;
	bi->bi_tool_entry_put = mdb_tool_entry_put;
#if 0
	bi->bi_tool_entry_reindex = mdb_tool_entry_reindex;
	bi->bi_tool_sync = 0;
	bi->bi_tool_dn2id_get = mdb_tool_dn2id_get;
#endif
	bi->bi_tool_entry_modify = mdb_tool_entry_modify;

	bi->bi_connection_init = 0;
	bi->bi_connection_destroy = 0;

	rc = mdb_back_init_cf( bi );

	return rc;
}

#if	(SLAPD_MDB == SLAPD_MOD_DYNAMIC)

SLAP_BACKEND_INIT_MODULE( mdb )

#endif /* SLAPD_MDB == SLAPD_MOD_DYNAMIC */

