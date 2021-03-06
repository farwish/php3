/*
   +----------------------------------------------------------------------+
   | PHP HTML Embedded Scripting Language Version 3.0					  |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-1999 PHP Development Team (See Credits file)	  |
   +----------------------------------------------------------------------+
   | This program is free software; you can redistribute it and/or modify |
   | it under the terms of one of the following licenses:				  |
   |																	  |
   |  A) the GNU General Public License as published by the Free Software |
   |	 Foundation; either version 2 of the License, or (at your option) |
   |	 any later version.												  |
   |																	  |
   |  B) the PHP License as published by the PHP Development Team and	  |
   |	 included in the distribution in the file: LICENSE				  |
   |																	  |
   | This program is distributed in the hope that it will be useful,	  |
   | but WITHOUT ANY WARRANTY; without even the implied warranty of		  |
   | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the		  |
   | GNU General Public License for more details.						  |
   |																	  |
   | You should have received a copy of both licenses referred to here.	  |
   | If you did not, or have any questions about PHP licensing, please	  |
   | contact core@php.net.												  |
   +----------------------------------------------------------------------+
   | Authors: Jouni Ahto <jah@mork.net>								  |
   |		  Andrew Avdeev <andy@simgts.mv.ru>							  |
   +----------------------------------------------------------------------+
 */

/* $Id: interbase.c,v 1.17 1999/05/26 13:08:50 sas Exp $ */

/* TODO: Arrays, roles?
A lot... */

#include "config.h"
#include "php.h"
#include "internal_functions.h"
#include "php3_interbase.h"

#if HAVE_IBASE
#include <ibase.h>
#include <time.h>
#include "php3_list.h"
#include "php3_string.h"
#include "fsock.h"
#include "head.h"

/* {{{ extension definition structures */
function_entry ibase_functions[] = {
	PHP_FE(ibase_connect, NULL)
	PHP_FE(ibase_pconnect, NULL)
	PHP_FE(ibase_close, NULL)
	PHP_FE(ibase_query, NULL)
	PHP_FE(ibase_fetch_row, NULL)
	PHP_FE(ibase_fetch_object, NULL)
	PHP_FE(ibase_free_result, NULL)
	PHP_FE(ibase_prepare, NULL)
	PHP_FE(ibase_execute, NULL)
	PHP_FE(ibase_free_query, NULL)
	PHP_FE(ibase_timefmt, NULL)

	PHP_FE(ibase_num_fields, NULL)
	PHP_FE(ibase_field_info, NULL)

	PHP_FE(ibase_trans, NULL)
	PHP_FE(ibase_commit, NULL)
	PHP_FE(ibase_rollback, NULL)

	PHP_FE(ibase_blob_info, NULL)
	PHP_FE(ibase_blob_create, NULL)
	PHP_FE(ibase_blob_add, NULL)
	PHP_FE(ibase_blob_cancel, NULL)
	PHP_FE(ibase_blob_close, NULL)
	PHP_FE(ibase_blob_open, NULL)
	PHP_FE(ibase_blob_get, NULL)
	PHP_FE(ibase_blob_echo, NULL)
	PHP_FE(ibase_blob_import, NULL)

	PHP_FE(ibase_errmsg, NULL)

	{NULL, NULL, NULL}
};

php3_module_entry ibase_module_entry =
{
	"InterBase",
	ibase_functions,
	php3_minit_ibase,
	php3_mfinish_ibase,
	php3_rinit_ibase,
	php3_rfinish_ibase,
	php3_info_ibase,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */


/* {{{ internal macros and structures */

#define IB_STATUS (IBASE_GLOBAL(php3_ibase_module).status)

/* db_handle and transaction handle keep in one variable
  link = db_handle * IBASE_TRANS_ON_LINK + trans_handle
*/

/* get link and transaction from long argument
 */
#define GET_LINK_TRANS(link_id, ib_link, trans_n) { \
	int type; \
	trans_n = link_id % IBASE_TRANS_ON_LINK; \
	link_id /= IBASE_TRANS_ON_LINK; \
	ib_link = (ibase_db_link *) php3_list_find(link_id, &type); \
	if (type!=IBASE_GLOBAL(php3_ibase_module).le_link && type!=IBASE_GLOBAL(php3_ibase_module).le_plink) { \
		_php3_ibase_module_error("%d is not link or transaction index",link_id); \
		RETURN_FALSE; \
	}}


/* get query  */
#define GET_QUERY(query_id, ib_query) { \
	int type; \
	ib_query = (ibase_query *) php3_list_find(query_id, &type); \
	if (type!=IBASE_GLOBAL(php3_ibase_module).le_query) { \
		_php3_ibase_module_error("%d is not query index",query_id); \
		RETURN_FALSE; \
	}}

/* get result  */
#define GET_RESULT(result_id, ib_result) { \
	int type; \
	ib_result = (ibase_result *) php3_list_find(result_id, &type); \
	if (type!=IBASE_GLOBAL(php3_ibase_module).le_result) { \
		_php3_ibase_module_error("%d is not result index",result_id); \
		RETURN_FALSE; \
	}}


#define RESET_ERRMSG { IBASE_GLOBAL(php3_ibase_module).errmsg[0] = '\0';}

#define TEST_ERRMSG ( IBASE_GLOBAL(php3_ibase_module).errmsg[0] != '\0')

/* sql variables union
used for convert and binding input variables
*/
typedef struct{
	union{
		short sval;
		float fval;
		ISC_QUAD qval;
	}val;
	short sqlind;
}BIND_BUF;


/* get blob identifier from argument
 on empty unset argumnet ib_blob set to NULL
 */
#define GET_BLOB_ID_ARG(blob_arg, ib_blob)\
{\
	if(blob_arg->type == IS_STRING && blob_arg->value.str.len == 0){\
		ib_blob = NULL;\
	}else if(blob_arg->type != IS_STRING\
			 || blob_arg->value.str.len != sizeof(ibase_blob_handle)\
			 || ((ibase_blob_handle *)(blob_arg->value.str.val))->bl_handle != 0){\
		_php3_ibase_module_error("invalid blob id");\
		RETURN_FALSE;\
	}else{\
		ib_blob = (ibase_blob_handle *)blob_arg->value.str.val;\
	}\
}


/* get blob handle from argument
 note: blob already open when handle active
 */
#define GET_BLOB_HANDLE_ARG(blob_arg, blob_ptr) \
{ \
	int type; \
	convert_to_long(blob_arg); \
	blob_ptr = (ibase_blob_handle *) php3_list_find(blob_arg->value.lval, &type); \
	if (type!=IBASE_GLOBAL(php3_ibase_module).le_blob) { \
		_php3_ibase_module_error("%d is not blob handle",blob_arg->value.lval); \
		RETURN_FALSE; \
	} \
}

/* blob information struct */
typedef struct {
	ISC_LONG  max_segment;		/* Length of longest segment */
	ISC_LONG  num_segments;		/* Total number of segments */
	ISC_LONG  total_length;		/* Total length of blob */
	int		  bl_stream;		/* blob is stream ? */
}IBASE_BLOBINFO;

/* }}} */


/* {{{ thread safety stuff */
#if defined(THREAD_SAFE)
typedef ibase_global_struct{
	ibase_module php3_ibase_module;
} ibase_global_struct;

#define IBASE_GLOBAL(a) ibase_globals->a

#define IBASE_TLS_VARS \
		ibase_global_struct *ibase_globals; \
		ibase_globals=TlsGetValue(IBASETls); 

#else
#define IBASE_GLOBAL(a) a
#define IBASE_TLS_VARS
ibase_module php3_ibase_module;
#endif
/* }}} */

/* error handling ---------------------------- */

/* {{{ proto string ibase_errmsg() */
/* Return error message */
void php3_ibase_errmsg(INTERNAL_FUNCTION_PARAMETERS)
{
	char *errmsg = IBASE_GLOBAL(php3_ibase_module).errmsg;
	IBASE_TLS_VARS;
	
	if(errmsg[0]){
		RETURN_STRING(errmsg,1);
	}
	RETURN_FALSE;
}
/* }}} */


/* {{{ _php3_ibase_error() */
/*	print interbase error and save it for ibase_errmsg() */
static void _php3_ibase_error(void)
{
	char *s, *errmsg = IBASE_GLOBAL(php3_ibase_module).errmsg;
	ISC_STATUS *statusp = IB_STATUS;

	s = errmsg;
	while((s - errmsg) < MAX_ERRMSG - (IBASE_MSGSIZE + 2) && isc_interprete(s, &statusp)){
		strcat(errmsg, " ");
		s = errmsg + strlen(errmsg);
	}
	php3_error(E_WARNING, "InterBase: %s",errmsg);
}
/* }}} */


/* {{{ _php3_ibase_module_error() */
/* print php interbase module error	 and save it for ibase_errmsg() */
static void _php3_ibase_module_error(char *msg, ...)
{
	char *errmsg = IBASE_GLOBAL(php3_ibase_module).errmsg;
	va_list ap;
	int len;

	va_start(ap, msg);
	len = vsnprintf(errmsg, MAX_ERRMSG - 1, msg, ap);
	va_end(ap);
	errmsg[len] = '\0';
	
	php3_error(E_WARNING, "InterBase module: %s",errmsg);
}
/* }}} */


/* destructors ---------------------- */


/* {{{ _php3_ibase_free_xsqlda() */
/* not actual destructor ... */
static void _php3_ibase_free_xsqlda(XSQLDA *sqlda)
{
	int i;
	XSQLVAR *var;

	if(sqlda){
		var = sqlda->sqlvar;
		for (i = 0; i < sqlda->sqld; i++, var++) {
			efree(var->sqldata);
			if(var->sqlind)
				efree(var->sqlind);
		}
		efree(sqlda);
	}
}
/* }}} */


/* {{{ _php3_ibase_commit_link() */
static void _php3_ibase_commit_link(ibase_db_link *link)
{
	int i;

	if(link->trans[0] != NULL){ /* commit default */
		if(isc_commit_transaction(IB_STATUS, &link->trans[0])){
			_php3_ibase_error();
		}
		link->trans[0] = NULL;
	}
	for(i = 1; i < IBASE_TRANS_ON_LINK; i++){
		if(link->trans[i] != NULL){
			if(isc_rollback_transaction(IB_STATUS, &link->trans[i])){
				_php3_ibase_error();
			}
			link->trans[i] = NULL;
		}
	}
}
/* }}} */


/* {{{ _php3_ibase_close_link() */
static void _php3_ibase_close_link(ibase_db_link *link)
{
	_php3_ibase_commit_link(link);
	isc_detach_database(IB_STATUS, &link->link);
	IBASE_GLOBAL(php3_ibase_module).num_links--;
	efree(link);
}
/* }}} */


/* {{{ _php3_ibase_close_plink() */
static void _php3_ibase_close_plink(ibase_db_link *link)
{
	_php3_ibase_commit_link(link);
	isc_detach_database(IB_STATUS, &link->link);
	IBASE_GLOBAL(php3_ibase_module).num_persistent--;
	IBASE_GLOBAL(php3_ibase_module).num_links--;
	free(link);
}
/* }}} */


/* {{{ _php3_ibase_free_result() */
static void _php3_ibase_free_result(ibase_result *ib_result)
{
	if (ib_result){
		_php3_ibase_free_xsqlda(ib_result->out_sqlda);
		if(ib_result->drop_stmt && ib_result->stmt){
			if(isc_dsql_free_statement(IB_STATUS, &ib_result->stmt, DSQL_drop)){
				_php3_ibase_error();
			}
		}else{
			if(isc_dsql_free_statement(IB_STATUS, &ib_result->stmt, DSQL_close)){
				_php3_ibase_error();
			}
		}
		if(ib_result->out_array){
			efree(ib_result->out_array);
		}
		efree(ib_result);
	}
}
/* }}} */


/* {{{ _php3_ibase_free_query() */
static void _php3_ibase_free_query(ibase_query *ib_query)
{
	
	if(ib_query){
		if (ib_query->in_sqlda) {
			efree(ib_query->in_sqlda);
		}
		if (ib_query->out_sqlda) {
			efree(ib_query->out_sqlda);
		}
		if(ib_query->stmt){
			if(isc_dsql_free_statement(IB_STATUS, &ib_query->stmt, DSQL_drop)){
				_php3_ibase_error();
			}
		}
		if(ib_query->in_array){
			efree(ib_query->in_array);
		}
		if(ib_query->out_array){
			efree(ib_query->out_array);
		}
		efree(ib_query);
	}
}
/* }}} */


/* {{{ _php3_ibase_free_blob()	*/
static void _php3_ibase_free_blob(ibase_blob_handle *ib_blob)
{
	
	if(ib_blob->bl_handle != NULL){ /* blob open*/
		if(isc_cancel_blob(IB_STATUS, &ib_blob->bl_handle)){
			_php3_ibase_error();
		}
	}
	efree(ib_blob);
}
/* }}} */


/* {{{ startup, shutdown and info functions */
int php3_minit_ibase(INIT_FUNC_ARGS)
{
	IBASE_TLS_VARS;

	if (cfg_get_long("ibase.allow_persistent", &IBASE_GLOBAL(php3_ibase_module).allow_persistent) == FAILURE) {
		IBASE_GLOBAL(php3_ibase_module).allow_persistent = 1;
	}
	if (cfg_get_long("ibase.max_persistent", &IBASE_GLOBAL(php3_ibase_module).max_persistent) == FAILURE) {
		IBASE_GLOBAL(php3_ibase_module).max_persistent = -1;
	}
	if (cfg_get_long("ibase.max_links", &IBASE_GLOBAL(php3_ibase_module).max_links) == FAILURE) {
		IBASE_GLOBAL(php3_ibase_module).max_links = -1;
	}
	if (cfg_get_string("ibase.default_user", &IBASE_GLOBAL(php3_ibase_module).default_user) == FAILURE
		|| IBASE_GLOBAL(php3_ibase_module).default_user[0] == 0) {
		IBASE_GLOBAL(php3_ibase_module).default_user = "";
	}
	if (cfg_get_string("ibase.default_password", &IBASE_GLOBAL(php3_ibase_module).default_password) == FAILURE
		|| IBASE_GLOBAL(php3_ibase_module).default_password[0] == 0) {	
		IBASE_GLOBAL(php3_ibase_module).default_password = "";
	}
	if (cfg_get_string("ibase.timeformat", &IBASE_GLOBAL(php3_ibase_module).timeformat) == FAILURE) {
		IBASE_GLOBAL(php3_ibase_module).cfg_timeformat = "%m/%d/%Y %H:%M:%S";
	}
	IBASE_GLOBAL(php3_ibase_module).num_persistent=0;
	IBASE_GLOBAL(php3_ibase_module).le_result = register_list_destructors(_php3_ibase_free_result, NULL);
	IBASE_GLOBAL(php3_ibase_module).le_query = register_list_destructors(_php3_ibase_free_query, NULL);
	IBASE_GLOBAL(php3_ibase_module).le_blob = register_list_destructors(_php3_ibase_free_blob, NULL);
	IBASE_GLOBAL(php3_ibase_module).le_link = register_list_destructors(_php3_ibase_close_link, NULL);
	IBASE_GLOBAL(php3_ibase_module).le_plink = register_list_destructors(_php3_ibase_commit_link, _php3_ibase_close_plink);

	REGISTER_LONG_CONSTANT("IBASE_DEFAULT", PHP3_IBASE_DEFAULT, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("IBASE_TEXT", PHP3_IBASE_TEXT, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("IBASE_TIMESTAMP", PHP3_IBASE_TIMESTAMP, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("IBASE_READ", PHP3_IBASE_READ, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("IBASE_COMMITED", PHP3_IBASE_COMMITED, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("IBASE_CONSISTENCY", PHP3_IBASE_CONSISTENCY, CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("IBASE_NOWAIT", PHP3_IBASE_NOWAIT, CONST_PERSISTENT);
	
	return SUCCESS;
}

int php3_rinit_ibase(INIT_FUNC_ARGS)
{
	IBASE_TLS_VARS;

	IBASE_GLOBAL(php3_ibase_module).default_link= -1;
	IBASE_GLOBAL(php3_ibase_module).num_links = php3_ibase_module.num_persistent;
	IBASE_GLOBAL(php3_ibase_module).timeformat = estrndup(IBASE_GLOBAL(php3_ibase_module).cfg_timeformat, strlen(IBASE_GLOBAL(php3_ibase_module).cfg_timeformat));
	IBASE_GLOBAL(php3_ibase_module).errmsg = emalloc(sizeof(char)*MAX_ERRMSG);
	return SUCCESS;
}


int php3_mfinish_ibase(void)
{
	return SUCCESS;
}


int php3_rfinish_ibase(void)
{
	IBASE_TLS_VARS;

	efree(IBASE_GLOBAL(php3_ibase_module).timeformat);
	efree(IBASE_GLOBAL(php3_ibase_module).errmsg);
	return SUCCESS;
}


void php3_info_ibase(void)
{
	php3_printf("$Revision: 1.17 $");
}
/* }}} */


/* {{{ _php_ibase_attach_db() */
static int _php_ibase_attach_db(char *server, char *uname, char *passwd, char *charset, int buffers, char *role, isc_db_handle *db)
{
	char dpb_buffer[256], *dpb, *p;
	int dpb_length, len;

	dpb = dpb_buffer;

	*dpb++ = isc_dpb_version1;

	if (uname != NULL && (len = strlen(uname))) {
		*dpb++ = isc_dpb_user_name;
		*dpb++ = len;
		for (p = uname; *p;) {
			*dpb++ = *p++;
		}
	}

	if (passwd != NULL && (len = strlen(passwd))) {
		*dpb++ = isc_dpb_password;
		*dpb++ = strlen(passwd);
		for (p = passwd; *p;) {
			*dpb++ = *p++;
		}
	}

	if (charset != NULL && (len = strlen(charset))) {
		*dpb++ = isc_dpb_lc_ctype;
		*dpb++ = strlen(charset);
		for (p = charset; *p;) {
			*dpb++ = *p++;
		}
	}

	if (buffers) {
		*dpb++ = isc_dpb_num_buffers;
		*dpb++ = 1;
		*dpb++ = buffers;
	}

#ifdef isc_dpb_sql_role_name
	if (role != NULL && (len = strlen(role))) {
		*dpb++ = isc_dpb_sql_role_name;
		*dpb++ = strlen(role);
		for (p = role; *p;) {
			*dpb++ = *p++;
		}
	}
#endif

	dpb_length = dpb - dpb_buffer;

	if(isc_attach_database(IB_STATUS, strlen(server), server, db, dpb_length, dpb_buffer)){
		_php3_ibase_error();
		return FAILURE;
	}
	return SUCCESS;
}
/* }}} */


/* {{{ _php3_ibase_connect() */
static void _php3_ibase_connect(INTERNAL_FUNCTION_PARAMETERS, int persistent)
{
	pval **args;
	char *ib_server, *ib_uname, *ib_passwd, *ib_charset = NULL;
	int i, ib_uname_len, ib_passwd_len;
	isc_db_handle db_handle = NULL;
	char *hashed_details;
	int hashed_details_length = 0;
	ibase_db_link *ib_link;
	IBASE_TLS_VARS;
	
	RESET_ERRMSG;
		
	ib_uname = IBASE_GLOBAL(php3_ibase_module).default_user;
	ib_passwd = IBASE_GLOBAL(php3_ibase_module).default_password;
	ib_uname_len = ib_uname ? strlen(ib_uname) : 0;
	ib_passwd_len = ib_passwd ? strlen(ib_passwd) : 0;
	
	if(ARG_COUNT(ht) < 1 || ARG_COUNT(ht) > 4){
		WRONG_PARAM_COUNT;
	}
	
	args = emalloc(sizeof(pval*) * ARG_COUNT(ht));
	if (getParametersArray(ht, ARG_COUNT(ht), args) == FAILURE) {
		efree(args);
		RETURN_FALSE;
	}

	switch(ARG_COUNT(ht)) {
		case 4:
			convert_to_string(args[3]);
			ib_charset = args[3]->value.str.val;
			hashed_details_length += args[3]->value.str.len;
			/* fallout */
		case 3:
			convert_to_string(args[2]);
			ib_passwd = args[2]->value.str.val;
			hashed_details_length += args[2]->value.str.len;
			/* fallout */
		case 2:
			convert_to_string(args[1]);
			ib_uname = args[1]->value.str.val;
			hashed_details_length += args[1]->value.str.len;
			/* fallout */
		case 1:
			convert_to_string(args[0]);
			ib_server = args[0]->value.str.val;
			hashed_details_length += args[0]->value.str.len;
	}/* case */
	
	efree(args);
	
	hashed_details = (char *) emalloc(hashed_details_length+strlen("ibase_%s_%s_%s_%s")+1);
	sprintf(hashed_details, "ibase_%s_%s_%s_%s", ib_server, ib_uname, ib_passwd, ib_charset);

	if (persistent) {
		list_entry *le;
		
		if (_php3_hash_find(plist, hashed_details, hashed_details_length+1, (void **) &le)==FAILURE) {
			list_entry new_le;
			
			if (IBASE_GLOBAL(php3_ibase_module).max_links!=-1 && IBASE_GLOBAL(php3_ibase_module).num_links>=IBASE_GLOBAL(php3_ibase_module).max_links) {
				_php3_ibase_module_error("Too many open links (%d)", IBASE_GLOBAL(php3_ibase_module).num_links);
				efree(hashed_details);
				RETURN_FALSE;
			}
			if (IBASE_GLOBAL(php3_ibase_module).max_persistent!=-1 && IBASE_GLOBAL(php3_ibase_module).num_persistent>=IBASE_GLOBAL(php3_ibase_module).max_persistent) {
				_php3_ibase_module_error("Too many open persistent links (%d)", IBASE_GLOBAL(php3_ibase_module).num_persistent);
				efree(hashed_details);
				RETURN_FALSE;
			}

			/* create the ib_link */

			if (_php_ibase_attach_db(ib_server, ib_uname, ib_passwd, ib_charset, 0, NULL, &db_handle) == FAILURE) {
				efree(hashed_details);
				RETURN_FALSE;
			}

			ib_link = (ibase_db_link *) malloc(sizeof(ibase_db_link));
			ib_link->link = db_handle;
			for(i = 0; i < IBASE_TRANS_ON_LINK; i++)
				ib_link->trans[i] = NULL;
			
			/* hash it up */
			new_le.type = php3_ibase_module.le_plink;
			new_le.ptr = ib_link;
			if (_php3_hash_update(plist, hashed_details, hashed_details_length+1, (void *) &new_le, sizeof(list_entry), NULL)==FAILURE) {
				efree(hashed_details);
				free(ib_link);
				RETURN_FALSE;
			}
			IBASE_GLOBAL(php3_ibase_module).num_links++;
			IBASE_GLOBAL(php3_ibase_module).num_persistent++;
		} else {
			if (le->type != IBASE_GLOBAL(php3_ibase_module).le_plink) {
				RETURN_FALSE;
			}
			/* TODO: ensure that the ib_link did not die */
			ib_link = (ibase_db_link *) le->ptr;
		}
		return_value->value.lval = IBASE_TRANS_ON_LINK *
			php3_list_insert(ib_link, IBASE_GLOBAL(php3_ibase_module).le_plink);
		return_value->type = IS_LONG;
	} else {
		list_entry *index_ptr, new_index_ptr;
		
		/* first we check the hash for the hashed_details key.	if it exists,
		 * it should point us to the right offset where the actual ib_link sits.
		 * if it doesn't, open a new ib_link, add it to the resource list,
		 * and add a pointer to it with hashed_details as the key.
		 */
		if (_php3_hash_find(list,hashed_details,hashed_details_length+1,(void **) &index_ptr)==SUCCESS) {
			int type,xlink;
			void *ptr;

			if (index_ptr->type != le_index_ptr) {
				RETURN_FALSE;
			}
			xlink = (int) index_ptr->ptr;
			ptr = php3_list_find(xlink,&type);	 /* check if the xlink is still there */
			if (ptr && (type==IBASE_GLOBAL(php3_ibase_module).le_link || type==IBASE_GLOBAL(php3_ibase_module).le_plink)) {
				return_value->value.lval = IBASE_GLOBAL(php3_ibase_module).default_link = xlink;
				return_value->type = IS_LONG;
				efree(hashed_details);
				return;
			} else {
				_php3_hash_del(list,hashed_details,hashed_details_length+1);
			}
		}
		if (IBASE_GLOBAL(php3_ibase_module).max_links!=-1 && IBASE_GLOBAL(php3_ibase_module).num_links>=IBASE_GLOBAL(php3_ibase_module).max_links) {
			_php3_ibase_module_error("Too many open links (%d)", IBASE_GLOBAL(php3_ibase_module).num_links);
			efree(hashed_details);
			RETURN_FALSE;
		}
		/* create the ib_link */

		if (_php_ibase_attach_db(ib_server, ib_uname, ib_passwd, ib_charset, 0, NULL, &db_handle) == FAILURE) {
			efree(hashed_details);
			RETURN_FALSE;
		}

		ib_link = (ibase_db_link *) emalloc(sizeof(ibase_db_link));
		ib_link->link = db_handle;
		for(i = 0; i < IBASE_TRANS_ON_LINK; i++)
			ib_link->trans[i] = NULL;

		/* add it to the list */
		return_value->value.lval = IBASE_TRANS_ON_LINK *
			php3_list_insert(ib_link, IBASE_GLOBAL(php3_ibase_module).le_link);
		return_value->type = IS_LONG;

		/* add it to the hash */
		new_index_ptr.ptr = (void *) return_value->value.lval;
		new_index_ptr.type = le_index_ptr;
		if (_php3_hash_update(list,hashed_details,hashed_details_length+1,(void *) &new_index_ptr, sizeof(list_entry), NULL)==FAILURE) {
			efree(hashed_details);
			RETURN_FALSE;
		}
		IBASE_GLOBAL(php3_ibase_module).num_links++;
	}
	efree(hashed_details);
	IBASE_GLOBAL(php3_ibase_module).default_link=return_value->value.lval;
}
/* }}} */


/* {{{ proto int ibase_connect(string database [, string username] [, string password] [, string charset]) */
/* Open a connection to an InterBase database */
PHP_FUNCTION(ibase_connect)
{
	_php3_ibase_connect(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */


/* {{{ proto int ibase_pconnect(string database [, string username] [, string password]	 [, string charset] ) */
/* Open a persistent connection to an InterBase database */
PHP_FUNCTION(ibase_pconnect)
{
	_php3_ibase_connect(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */


/* {{{ proto int ibase_close([int link_identifier]) */
/* Close an InterBase connection */
PHP_FUNCTION(ibase_close)
{
	pval *link_arg;
	ibase_db_link *ib_link;
	int link_id, trans_n;
	IBASE_TLS_VARS;
	
	
	RESET_ERRMSG;
	
	switch (ARG_COUNT(ht)) {
		case 0:
			link_id = IBASE_GLOBAL(php3_ibase_module).default_link;
			break;
		case 1:
			if (getParameters(ht, 1, &link_arg) == FAILURE) {
				RETURN_FALSE;
			}
			convert_to_long(link_arg);
			link_id = link_arg->value.lval;
			break;
		default:
			WRONG_PARAM_COUNT;
			break;
	}
	
	GET_LINK_TRANS(link_id, ib_link, trans_n);
	
	php3_list_delete(link_id);
	RETURN_TRUE;
}
/* }}} */

/* {{{ _php3_ibase_alloc_array() */
static int _php3_ibase_alloc_array(ibase_array **ib_arrayp, int *array_cntp,
								   XSQLDA *sqlda, isc_db_handle link, isc_tr_handle trans)
{
#define IB_ARRAY (*ib_arrayp)
	
	int i, dim, ar_cnt, ar_length;
	XSQLVAR *var;
	

	IB_ARRAY = NULL;
	
	ar_cnt = 0; /* find arrays */
	var = sqlda->sqlvar;
	for(i = 0; i < sqlda->sqld; i++, var++){
		if((var->sqltype & ~1) == SQL_ARRAY)
			ar_cnt++;
	}

	if(ar_cnt){	  /* have  arrays ? */

		*array_cntp = ar_cnt;
		IB_ARRAY = emalloc(sizeof(ibase_array)*ar_cnt);
		ar_cnt = 0;
		var = sqlda->sqlvar;
		for(i = 0; i < sqlda->sqld; i++, var++){
			if((var->sqltype & ~1) == SQL_ARRAY){
				
				ISC_ARRAY_DESC *ar_desc = &IB_ARRAY[ar_cnt].ar_desc;
				
				if(isc_array_lookup_bounds(IB_STATUS, &link, &trans,
										   var->relname, var->sqlname, ar_desc)){
					_php3_ibase_error();
					efree(IB_ARRAY);
					IB_ARRAY = NULL;
					return FAILURE;
				}

				switch (ar_desc->array_desc_dtype){
					case blr_text:
					case blr_text2:
						IB_ARRAY[ar_cnt].el_type = SQL_TEXT;
						IB_ARRAY[ar_cnt].el_size = ar_desc->array_desc_length+1;
						break;
					case blr_short:
						IB_ARRAY[ar_cnt].el_type = SQL_SHORT;
						IB_ARRAY[ar_cnt].el_size = sizeof(short);
						break;
					case blr_long:
						IB_ARRAY[ar_cnt].el_type = SQL_LONG;
						IB_ARRAY[ar_cnt].el_size = sizeof(long);
						break;
					case blr_float:
						IB_ARRAY[ar_cnt].el_type = SQL_FLOAT;
						IB_ARRAY[ar_cnt].el_size = sizeof(float);
						break;
					case blr_double:
						IB_ARRAY[ar_cnt].el_type = SQL_DOUBLE;
						IB_ARRAY[ar_cnt].el_size = sizeof(double);
						break;
					case blr_date:
						IB_ARRAY[ar_cnt].el_type = SQL_DATE;
						IB_ARRAY[ar_cnt].el_size = sizeof(ISC_QUAD);
						break;
					case blr_varying:
					case blr_varying2:	/* changed to SQL_TEXT ? */
						/* sql_type = SQL_VARYING; Why? FIXME: ??? */
						IB_ARRAY[ar_cnt].el_type = SQL_TEXT;
						IB_ARRAY[ar_cnt].el_size = ar_desc->array_desc_length+sizeof(short);
						break;
					default:
						_php3_ibase_module_error("unexpected array type %d in relation '%s' column '%s')",
										   ar_desc->array_desc_dtype,var->relname, var->sqlname);
						efree(IB_ARRAY);
						IB_ARRAY = NULL;
						return FAILURE;
				}/* switch array_desc_type */
				
				ar_length = 0; /* calculate elements count */
				for(dim = 0; dim < ar_desc->array_desc_dimensions; dim++){
					ar_length += 1 + ar_desc->array_desc_bounds[dim].array_bound_upper
						- ar_desc->array_desc_bounds[dim].array_bound_lower;
				}
				IB_ARRAY[ar_cnt].ar_size = IB_ARRAY[ar_cnt].el_size * ar_length;
				
				ar_cnt++;
				
			}/* if SQL_ARRAY */
		}/* for column */
	}/* if array_cnt */
	
	return SUCCESS;
#undef IB_ARRAY
}
/* }}} */


/* {{{ _php3_ibase_alloc_query() */
/* allocate and prepare query */
static int _php3_ibase_alloc_query(ibase_query **ib_queryp, isc_db_handle link, isc_tr_handle trans, char *query)
{
#define IB_QUERY (*ib_queryp)
	
	IB_QUERY = emalloc(sizeof(ibase_query));
	IB_QUERY->link = link;
	IB_QUERY->trans = trans;
	IB_QUERY->stmt = NULL;
	IB_QUERY->out_sqlda = NULL;
	IB_QUERY->in_sqlda = NULL;
	IB_QUERY->in_array = NULL;
	IB_QUERY->in_array_cnt = 0;
	IB_QUERY->out_array = NULL;
	IB_QUERY->out_array_cnt = 0;
	
	if (isc_dsql_allocate_statement(IB_STATUS, &link, &IB_QUERY->stmt)) {
		_php3_ibase_error();
		goto _php3_ibase_alloc_query_error;
	}

	IB_QUERY->out_sqlda = emalloc(XSQLDA_LENGTH(0));
	IB_QUERY->out_sqlda->sqln = 0;
	IB_QUERY->out_sqlda->version = SQLDA_VERSION1;

	if (isc_dsql_prepare(IB_STATUS, &IB_QUERY->trans, &IB_QUERY->stmt, 0, query, SQLDA_VERSION1, IB_QUERY->out_sqlda)) {
		_php3_ibase_error();
		goto _php3_ibase_alloc_query_error;
	}
	
	/* not enough output variables ? */
	if(IB_QUERY->out_sqlda->sqld > IB_QUERY->out_sqlda->sqln){
		IB_QUERY->out_sqlda = erealloc(IB_QUERY->out_sqlda,XSQLDA_LENGTH(IB_QUERY->out_sqlda->sqld));
		IB_QUERY->out_sqlda->sqln = IB_QUERY->out_sqlda->sqld;
		IB_QUERY->out_sqlda->version = SQLDA_VERSION1;
		if (isc_dsql_describe(IB_STATUS, &IB_QUERY->stmt, SQLDA_VERSION1, IB_QUERY->out_sqlda)) {
			_php3_ibase_error();
			goto _php3_ibase_alloc_query_error;
		}
	}

	/* maybe have input placeholders? */
	IB_QUERY->in_sqlda = emalloc(XSQLDA_LENGTH(0));
	IB_QUERY->in_sqlda->sqln = 0;
	IB_QUERY->in_sqlda->version = SQLDA_VERSION1;
	if (isc_dsql_describe_bind(IB_STATUS, &IB_QUERY->stmt, SQLDA_VERSION1, IB_QUERY->in_sqlda)) {
		_php3_ibase_error();
		goto _php3_ibase_alloc_query_error;
	}
	
	/* not enough input variables ? */
	if(IB_QUERY->in_sqlda->sqln < IB_QUERY->in_sqlda->sqld){
		IB_QUERY->in_sqlda = erealloc(IB_QUERY->in_sqlda,XSQLDA_LENGTH(IB_QUERY->in_sqlda->sqld));
		IB_QUERY->in_sqlda->sqln = IB_QUERY->in_sqlda->sqld;
		IB_QUERY->in_sqlda->version = SQLDA_VERSION1;
		if (isc_dsql_describe_bind(IB_STATUS, &IB_QUERY->stmt, SQLDA_VERSION1, IB_QUERY->in_sqlda)) {
			_php3_ibase_error();
			goto _php3_ibase_alloc_query_error;
		}
	}
	
	/* allocate arrays... */
	if (_php3_ibase_alloc_array(&IB_QUERY->in_array, &IB_QUERY->in_array_cnt,
								IB_QUERY->in_sqlda, link, trans) == FAILURE){
		goto _php3_ibase_alloc_query_error; /* error report already done */
	}
	
	if (_php3_ibase_alloc_array(&IB_QUERY->out_array, &IB_QUERY->out_array_cnt,
								IB_QUERY->out_sqlda, link, trans) == FAILURE){
		goto _php3_ibase_alloc_query_error;
	}

	/* no, haven't placeholders at all */
	if(IB_QUERY->in_sqlda->sqld == 0){
		efree(IB_QUERY->in_sqlda);
		IB_QUERY->in_sqlda = NULL;
	}

	if(IB_QUERY->out_sqlda->sqld == 0){
		efree(IB_QUERY->out_sqlda);
		IB_QUERY->out_sqlda = NULL;
	}

	return SUCCESS;
	
_php3_ibase_alloc_query_error:
	
	if(IB_QUERY->out_sqlda)
		efree(IB_QUERY->out_sqlda);
	if(IB_QUERY->in_sqlda)
		efree(IB_QUERY->in_sqlda);
	if(IB_QUERY->out_array)
		efree(IB_QUERY->out_array);
	efree(IB_QUERY);
	IB_QUERY = NULL;
	
	return FAILURE;
#undef IB_QUERY
}
/* }}} */


/* {{{ _php3_ibase_bind() */
/* bind php variables to XSQLDA */
static int _php3_ibase_bind(XSQLDA *sqlda, pval **b_vars, BIND_BUF *buf)
{
	XSQLVAR *var;
	pval *b_var;
	int i;
	

	var = sqlda->sqlvar;
	for(i = 0; i < sqlda->sqld; var++, i++) { /* binded vars */
		
		buf[i].sqlind = 0;
		var->sqlind	 = &buf[i].sqlind;
		b_var = b_vars[i];
		
		switch(var->sqltype & ~1) {
			case SQL_TEXT:			   /* direct to variable */
			case SQL_VARYING:
				convert_to_string(b_var);
				var->sqldata = (void ISC_FAR *)b_var->value.str.val;
				var->sqllen	 = b_var->value.str.len;
				var->sqltype = SQL_TEXT + (var->sqltype & 1);
				break;
			case SQL_SHORT:
				convert_to_long(b_var);
				if(b_var->value.lval > SHRT_MAX || b_var->value.lval < SHRT_MIN){
					_php3_ibase_module_error("field %*s overflow", var->aliasname_length, var->aliasname);
					return FAILURE;
				}
				buf[i].val.sval = (short)b_var->value.lval;
				var->sqldata = (void ISC_FAR *)(&buf[i].val.sval);
				break;
			case SQL_LONG:	/* direct to variable */
				convert_to_long(b_var);
				var->sqldata = (void ISC_FAR *)(&b_var->value.lval);
				break;
			case SQL_FLOAT:
				convert_to_double(b_var);
				buf[i].val.fval = (float)b_var->value.dval;
				var->sqldata = (void ISC_FAR *)(&buf[i].val.fval);
				break;
			case SQL_DOUBLE:  /* direct to variable */
				convert_to_double(b_var);
				var->sqldata = (void ISC_FAR *)(&b_var->value.dval);
				break;
			case SQL_DATE:
				{
					struct tm t;
					
					t.tm_year = t.tm_mon = t.tm_mday = t.tm_hour =
						t.tm_min = t.tm_sec = 0;
					
					convert_to_string(b_var);
					
#if HAVE_STRFTIME /*FIXME: HAVE_STRPTIME ?*/
					
					strptime(b_var->value.str.val, IBASE_GLOBAL(php3_ibase_module).timeformat, &t);
#else
					{
						int n = sscanf(b_var->value.str.val,"%d%*[/]%d%*[/]%d %d%*[:]%d%*[:]%d",
									   &t.tm_mon, &t.tm_mday, &t.tm_year,  &t.tm_hour, &t.tm_min, &t.tm_sec);
						if(n != 3 && n != 6){
							_php3_ibase_module_error("invalid date/time format");
							return FAILURE;
						}
					
						t.tm_year -= 1900;
						t.tm_mon--;
					}
#endif
					isc_encode_date(&t, &buf[i].val.qval);
					var->sqldata = (void ISC_FAR *)(&buf[i].val.qval);
				}
				break;
			case SQL_BLOB:
				{
					ibase_blob_handle *ib_blob_id;
					if(b_var->type != IS_STRING
					   || b_var->value.str.len != sizeof(ibase_blob_handle)
					   || ((ibase_blob_handle *)(b_var->value.str.val))->bl_handle != 0){
						_php3_ibase_module_error("invalid blob id string");
						return FAILURE;
					}
					ib_blob_id = (ibase_blob_handle *)b_var->value.str.val;
					
					var->sqldata = (void ISC_FAR *)&ib_blob_id->bl_qd;
				}
			break;
			case SQL_ARRAY:
				_php3_ibase_module_error("binding arrays not supported yet");
				return FAILURE;
			break;
		}/*switch*/
	}/*for*/
	return SUCCESS;
}
/* }}} */


/* {{{ _php3_ibase_alloc_xsqlda() */
static void _php3_ibase_alloc_xsqlda(XSQLDA *sqlda)
{
	int i;
	XSQLVAR *var = sqlda->sqlvar;
	
	
	for (i = 0; i < sqlda->sqld; i++, var++) {
		switch(var->sqltype & ~1){
			case SQL_TEXT:
				var->sqldata = emalloc(sizeof(char)*(var->sqllen));
				break;
			case SQL_VARYING:
				var->sqldata = emalloc(sizeof(char)*(var->sqllen+sizeof(short)));
				break;
			case SQL_SHORT:
				var->sqldata = emalloc(sizeof(short));
				break;
			case SQL_LONG:
				var->sqldata = emalloc(sizeof(long));
				break;
			case SQL_FLOAT:
				var->sqldata = emalloc(sizeof(float));
					break;
			case SQL_DOUBLE:
				var->sqldata = emalloc(sizeof(double));
				break;
			case SQL_DATE:
			case SQL_BLOB:
			case SQL_ARRAY:
				var->sqldata = emalloc(sizeof(ISC_QUAD));
				break;
		}/*switch*/
		if(var->sqltype & 1) /* sql NULL flag */
			var->sqlind = emalloc(sizeof(short));
		else
			var->sqlind = NULL;
	}/* for*/
}
/* }}} */


/* {{{ _php3_ibase_exec() */
static int _php3_ibase_exec(ibase_result **ib_resultp, ibase_query *ib_query, int argc, pval **args)
{
#define IB_RESULT (*ib_resultp)
	XSQLDA *in_sqlda = NULL, *out_sqlda = NULL;
	BIND_BUF *bind_buf = NULL;
	int rv = FAILURE;
	

	IB_RESULT = NULL;
	
	/* allocate sqlda and output buffers */
	if(ib_query->out_sqlda){ /* output variables in select, select for update */
		IB_RESULT = emalloc(sizeof(ibase_result));
		IB_RESULT->link = ib_query->link;
		IB_RESULT->trans = ib_query->trans;
		IB_RESULT->stmt = ib_query->stmt; 
		IB_RESULT->drop_stmt = 0; /* when free result close but not drop!*/
		out_sqlda = IB_RESULT->out_sqlda = emalloc(XSQLDA_LENGTH(ib_query->out_sqlda->sqld));
		memcpy(out_sqlda,ib_query->out_sqlda,XSQLDA_LENGTH(ib_query->out_sqlda->sqld));
		_php3_ibase_alloc_xsqlda(out_sqlda);

		if(ib_query->out_array){
			IB_RESULT->out_array = emalloc(sizeof(ibase_array)*ib_query->out_array_cnt);
			memcpy(IB_RESULT->out_array, ib_query->out_array, sizeof(ibase_array)*ib_query->out_array_cnt);
		}else{
			IB_RESULT->out_array = NULL;
		}
	}

	if(ib_query->in_sqlda){ /* has placeholders */
		if (ib_query->in_sqlda->sqld != argc){
			_php3_ibase_module_error("placeholders (%d) and variables (%d) mismatch",ib_query->in_sqlda->sqld, argc);
			goto _php3_ibase_exec_error;  /* yes mommy, goto! */
		}
		in_sqlda = emalloc(XSQLDA_LENGTH(ib_query->in_sqlda->sqld));
		memcpy(in_sqlda,ib_query->in_sqlda,XSQLDA_LENGTH(ib_query->in_sqlda->sqld));
		bind_buf = emalloc(sizeof(BIND_BUF) * ib_query->in_sqlda->sqld);
		if(_php3_ibase_bind(in_sqlda, args, bind_buf) == FAILURE){
			goto _php3_ibase_exec_error;
		}
	}

	if (isc_dsql_execute(IB_STATUS, &ib_query->trans, &ib_query->stmt, 1, in_sqlda)) {
		_php3_ibase_error();
		goto _php3_ibase_exec_error;
	}

	rv = SUCCESS;
	
_php3_ibase_exec_error:		 /* I'm a bad boy... */
	
	if(in_sqlda)
		efree(in_sqlda);
	if(bind_buf)
		efree(bind_buf);

	if(rv == FAILURE){
		if(IB_RESULT){
			efree(IB_RESULT);
			IB_RESULT = NULL;
		}
		if(out_sqlda)
			efree(out_sqlda);
	}
	
	return rv;
#undef IB_RESULT
}
/* }}} */


/* {{{ proto int ibase_trans([int trans_args [, int link_identifier]]) */
/* Start transaction */
PHP_FUNCTION(ibase_trans)
{
	pval **args;
	char tpb[20], *tpbp = NULL;
	long trans_argl = 0;
	int tpb_len = 0, argn, link_id, trans_n;
	ibase_db_link *ib_link;
	IBASE_TLS_VARS;
	
	
	RESET_ERRMSG;

	link_id = IBASE_GLOBAL(php3_ibase_module).default_link;

	/* TODO: multi-databases trans */
	argn = ARG_COUNT(ht);
	if(argn < 0 || argn > 2){
		WRONG_PARAM_COUNT;
	}

	if(argn){
		args = emalloc(sizeof(pval*) * argn);
		if (getParametersArray(ht, argn, args) == FAILURE) {
			efree(args);
			RETURN_FALSE;
		}

		argn--;
		convert_to_long(args[argn]); /* last arg - trans_args */
		trans_argl = args[argn]->value.lval;

		/* TODO: multi
		 do{
			 link_id[argn] = args[argn];
			 convert_to_long(args[argn]);
			 GET_LINK_TRANS(args[argn].value.lval, ib_link[argn], trans_n);
		 }while(argn--); */

		if(argn){
			argn--;
			convert_to_long(args[argn]);
			link_id = args[argn]->value.lval;
		}
	
		efree(args);
	}

	GET_LINK_TRANS(link_id, ib_link, trans_n);
	
	if(trans_argl){
		tpb[tpb_len++] = isc_tpb_version3;
		tpbp = tpb;
		/* access mode */
		if(trans_argl & PHP3_IBASE_READ) /* READ ONLY TRANSACTION */
			tpb[tpb_len++] = isc_tpb_read;
		else
			tpb[tpb_len++] = isc_tpb_write;
		/* isolation level */
		if(trans_argl & PHP3_IBASE_COMMITED){
			tpb[tpb_len++] = isc_tpb_read_committed;
		}else if (trans_argl & PHP3_IBASE_CONSISTENCY)
			tpb[tpb_len++] = isc_tpb_consistency;
		else 
			tpb[tpb_len++] = isc_tpb_concurrency;
		/* lock resolution */
		if(trans_argl & PHP3_IBASE_NOWAIT)
			tpb[tpb_len++] = isc_tpb_nowait;
		else 
			tpb[tpb_len++] = isc_tpb_wait;

	}

	/* find empty transaction slot */
	for(trans_n = 0; trans_n < IBASE_TRANS_ON_LINK
		&& ib_link->trans[trans_n]; trans_n++)
		;
	if(trans_n == IBASE_TRANS_ON_LINK){
		_php3_ibase_module_error("too many transactions on link");
		RETURN_FALSE;
	}
	
	if (isc_start_transaction(IB_STATUS, &ib_link->trans[trans_n], 1, &ib_link->link, tpb_len, tpbp)) {
		_php3_ibase_error();
		RETURN_FALSE;
	}

	RETURN_LONG(link_id * IBASE_TRANS_ON_LINK + trans_n);
}
/* }}} */


/* {{{ _php3_ibase_def_trans() */
/* open default transaction */
static int _php3_ibase_def_trans(ibase_db_link * ib_link, int trans_n)
{
	
	if(trans_n == 0 && ib_link->trans[0] == NULL){ 
		if (isc_start_transaction(IB_STATUS, &ib_link->trans[0], 1, &ib_link->link, 0, NULL)) {
			_php3_ibase_error();
			return FAILURE;
		}
	}
	return SUCCESS;
}
/*}}}*/


/* {{{ _php3_ibase_trans_end() */
#define COMMIT 1
#define ROLLBACK 0
static void _php3_ibase_trans_end(INTERNAL_FUNCTION_PARAMETERS, int commit)
{
	pval *link_trans_arg;
	int link_id = 0, trans_n = 0;
	ibase_db_link *ib_link;
	IBASE_TLS_VARS;
	

	RESET_ERRMSG;

	switch (ARG_COUNT(ht)) {
		case 0:
			link_id = IBASE_GLOBAL(php3_ibase_module).default_link;
			break;
		case 1: 
			if (getParameters(ht, 1, &link_trans_arg) == FAILURE) {
				RETURN_FALSE;
			}
			convert_to_long(link_trans_arg);
			link_id = link_trans_arg->value.lval;
			break;
		default:
			WRONG_PARAM_COUNT;
			break;
	}

	GET_LINK_TRANS(link_id, ib_link, trans_n);

	if (commit) {
		if(isc_commit_transaction(IB_STATUS, &ib_link->trans[trans_n])){
			_php3_ibase_error();
			RETURN_FALSE;
		}
	}else{
		if(isc_rollback_transaction(IB_STATUS, &ib_link->trans[trans_n])){
			_php3_ibase_error();
			RETURN_FALSE;
		}
	}
	ib_link->trans[trans_n] = NULL;
	
	RETURN_TRUE;
}
/* }}} */


/* {{{ proto int ibase_commit([int link_identifier, ] int trans_number) */
/* Commit transaction */
PHP_FUNCTION(ibase_commit)
{
	_php3_ibase_trans_end(INTERNAL_FUNCTION_PARAM_PASSTHRU, COMMIT);
}
/* }}} */


/* {{{ proto int ibase_rollback([int link_identifier, ] int trans_number) */
/* Roolback transaction */
PHP_FUNCTION(ibase_rollback)
{
	_php3_ibase_trans_end(INTERNAL_FUNCTION_PARAM_PASSTHRU, ROLLBACK);
}
/* }}} */


/* {{{ proto int ibase_query([int link_identifier, ]string query[,bind args]) */
/* Execute a query. */
PHP_FUNCTION(ibase_query)
{
	pval **args, **bind_args = NULL;
	int i, link_id = 0, trans_n = 0, bind_n = 0;
	char *query;
	ibase_db_link *ib_link;
	ibase_query *ib_query;
	ibase_result *ib_result;
	IBASE_TLS_VARS;
	

	RESET_ERRMSG;

	if(ARG_COUNT(ht) < 1){
		WRONG_PARAM_COUNT;
	}

	args = emalloc(sizeof(pval*) * ARG_COUNT(ht));
	if (getParametersArray(ht, ARG_COUNT(ht), args) == FAILURE) {
		efree(args);
		RETURN_FALSE;
	}

	i = 0;
	if(args[i]->type == IS_LONG){ /* link or transaction argument */
		link_id = args[i]->value.lval;
		i++; /* next arg */
	}else{
		link_id = IBASE_GLOBAL(php3_ibase_module).default_link;
	}

	GET_LINK_TRANS(link_id, ib_link, trans_n);

	if(args[i]->type == IS_STRING){ /* query argument */
		query = args[i]->value.str.val;
		i++; /* next arg */
	}else{
		_php3_ibase_module_error("query argument missed");
		efree(args);
		RETURN_FALSE;
	}

	if(ARG_COUNT(ht) > i){ /* have variables to bind */
		bind_n = ARG_COUNT(ht) - i;
		bind_args = &args[i];
	}
	
	/* open default transaction */
	if(_php3_ibase_def_trans(ib_link, trans_n) == FAILURE){
		efree(args);
		RETURN_FALSE;
	}

	if(_php3_ibase_alloc_query(&ib_query, ib_link->link, ib_link->trans[trans_n],  query) == FAILURE){
		efree(args);
		RETURN_FALSE;
	}

	if(_php3_ibase_exec(&ib_result, ib_query, bind_n, bind_args) == FAILURE){
		_php3_ibase_free_query(ib_query);
		efree(args);
		RETURN_FALSE;
	}
	
	efree(args);
	
	if(ib_result) { /* select statement */
		ib_result->drop_stmt = 1; /* drop stmt when free result */
		ib_query->stmt = NULL; /* keep stmt when free query */
		_php3_ibase_free_query(ib_query);
		RETURN_LONG(php3_list_insert(ib_result, php3_ibase_module.le_result));
	}else{
		_php3_ibase_free_query(ib_query);
		RETURN_TRUE;
	}
}
/* }}} */


/* {{{ _php3_ibase_var_pval() */
static int _php3_ibase_var_pval(pval *val, void *data, int type, int len, int scale, int flag)
{
	char string_data[255];
	

	switch(type & ~1) {
		case SQL_VARYING:
			len = ((IBASE_VCHAR *) data)->var_len;
			data = ((IBASE_VCHAR *) data)->var_str;
			/* fallout */
		case SQL_TEXT:
			val->value.str.val = (char *)emalloc(sizeof(char)*(len+1));
			strncpy(val->value.str.val, data, len);
			if (php3_ini.magic_quotes_runtime) {
				char *tmp = val->value.str.val;
				val->value.str.val = _php3_addslashes(val->value.str.val, len, &len, 0);
				efree(tmp);
			}
			val->type = IS_STRING;
			val->value.str.len = len;
			break;
		case SQL_LONG:
			if (scale) {
				int j, f = 1;
				float n = *(long *)(data);
				
				for (j = 0; j < -scale; j++)
					f *= 10;
				val->type = IS_STRING;
				val->value.str.len = sprintf(string_data, "%.*f", -scale, n / f);
				val->value.str.val = estrdup(string_data);
			}else{
				val->type = IS_LONG;
				val->value.lval = *(long *)(data);
			}
			break;
		case SQL_SHORT:
			val->type = IS_LONG;
			val->value.lval = *(short *)(data);
			break;
		case SQL_FLOAT:
			val->type = IS_DOUBLE;
			val->value.dval = *(float *)(data);
			break;
		case SQL_DOUBLE:
			if (scale) {
				val->type = IS_STRING;
				val->value.str.len = sprintf(string_data, "%.*f", -scale, *(double *)data);
				val->value.str.val = estrdup(string_data);
			}else{
				val->type = IS_DOUBLE;
				val->value.dval = *(double *)data;
			}
			break;
		case SQL_DATE:{
			struct tm t;
			long timestamp = -1;
					 
			isc_decode_date((ISC_QUAD *) data, &t);
			timestamp = mktime(&t);
#if HAVE_TM_ZONE
			t.tm_zone = tzname[0];
#endif
					 
			if(flag & PHP3_IBASE_TIMESTAMP){
				val->type = IS_LONG;
				val->value.lval = timestamp;
			}else{
				val->type = IS_STRING;
#if HAVE_STRFTIME
				val->value.str.len = strftime(string_data, sizeof(string_data), IBASE_GLOBAL(php3_ibase_module).timeformat, &t);
#else
				if(!t.tm_hour && !t.tm_min && !t.tm_sec)
					val->value.str.len = sprintf(string_data, "%02d/%02d/%4d", t.tm_mon+1, t.tm_mday, t.tm_year+1900);
				else
					val->value.str.len = sprintf(string_data, "%02d/%02d/%4d %02d:%02d:%02d",
									 t.tm_mon+1, t.tm_mday, t.tm_year+1900, t.tm_hour, t.tm_min, t.tm_sec);
#endif
				val->value.str.val = estrdup(string_data);
				break;
			}
		}
		default:
			return FAILURE;
	}/* switch (type) */
	return SUCCESS;
}
/* }}}	*/

/* {{{ _php3_ibase_arr_pval() */
/* create multidimension array - resursion function
 (*datap) argument changed */
static int _php3_ibase_arr_pval(pval *ar_pval, char **datap, ibase_array *ib_array, int dim, int flag)
{
	pval tmp;
	int i, dim_len, l_bound, u_bound;
	

	if (dim > 16){ /* InterBase limit */
		_php3_ibase_module_error("php module internal error in %s %d",__FILE__,__LINE__);
		return FAILURE;
	}

	u_bound = ib_array->ar_desc.array_desc_bounds[dim].array_bound_upper;
	l_bound = ib_array->ar_desc.array_desc_bounds[dim].array_bound_lower;
	dim_len = 1 + u_bound - l_bound;
		
	if(dim < ib_array->ar_desc.array_desc_dimensions - 1){ /* array again */
		for(i = 0; i < dim_len; i++){
			/* recursion here */
			if(_php3_ibase_arr_pval(ar_pval, datap, ib_array, dim+1, flag) == FAILURE){
				return FAILURE;
			}
		}
	}else{ /* data at last */
		
		array_init(ar_pval);

		for(i = 0; i < dim_len; i++){
			if(_php3_ibase_var_pval(&tmp, *datap, ib_array->el_type,
								   ib_array->ar_desc.array_desc_length,
								   ib_array->ar_desc.array_desc_scale,
								   flag) == FAILURE){
				return FAILURE;
			}
			_php3_hash_index_update(ar_pval->value.ht,
									l_bound + i,
									(void *) &tmp, sizeof(pval),NULL);
			*datap += ib_array->el_size;
		}
	}
	return SUCCESS;
}


/* {{{ _php3_ibase_fetch_hash() */

#define FETCH_ARRAY 1
#define FETCH_OBJECT 2

static void _php3_ibase_fetch_hash(INTERNAL_FUNCTION_PARAMETERS, int fetch_type)
{
	pval *result_arg, *flag_arg;
	long flag = 0;
	int i, arr_cnt;
	ibase_result *ib_result;
	XSQLVAR *var;
	IBASE_TLS_VARS;
	
	RESET_ERRMSG;
	
	switch(ARG_COUNT(ht)){
		case 1:
			if (ARG_COUNT(ht)==1 && getParameters(ht, 1, &result_arg)==FAILURE) {
				RETURN_FALSE;
			}
			break;
		case 2:
			if(ARG_COUNT(ht)==2 && getParameters(ht, 2, &result_arg, &flag_arg)==FAILURE) {
				RETURN_FALSE;
			}
			convert_to_long(flag_arg);
			flag = flag_arg->value.lval;
			break;
		default:
			WRONG_PARAM_COUNT;
			break;
	}
	
	convert_to_long(result_arg);
	
	GET_RESULT(result_arg->value.lval, ib_result);
	
	if (ib_result->out_sqlda == NULL) {
		_php3_ibase_module_error("trying to fetch results from a non-select query");
		RETURN_FALSE;
	}
	
	if(isc_dsql_fetch(IB_STATUS, &ib_result->stmt, 1, ib_result->out_sqlda) == 100L){
		RETURN_FALSE;  /* end of cursor */
	}
	
	if(IB_STATUS[0] && IB_STATUS[1]){ /* error in fetch */
		_php3_ibase_error();
		RETURN_FALSE;
	}
	
	if(fetch_type == FETCH_ARRAY){
		if (array_init(return_value)==FAILURE) {
			RETURN_FALSE;
		}
	}else if(fetch_type == FETCH_OBJECT){
		if (object_init(return_value)==FAILURE) {
			RETURN_FALSE;
		}
	}
	
	arr_cnt = 0;
	var = ib_result->out_sqlda->sqlvar;
	for (i = 0; i < ib_result->out_sqlda->sqld; i++, var++) {
		pval tmp;
		if (((var->sqltype & 1) == 0) || *var->sqlind != -1){
			switch(var->sqltype & ~1) {
				case SQL_VARYING:
				case SQL_TEXT:
				case SQL_SHORT:
				case SQL_LONG:
				case SQL_FLOAT:
				case SQL_DOUBLE:
				case SQL_DATE:
					_php3_ibase_var_pval(&tmp, var->sqldata, var->sqltype, var->sqllen, var->sqlscale, flag);
					break;
				case SQL_BLOB:
					if(flag & PHP3_IBASE_TEXT){ /* text ? */
						int stat;
						isc_blob_handle bl_handle = NULL;
						ISC_LONG max_len = 0, cur_len = 0;
						char bl_items[1], *bl_data, bl_info[20], *p;
						
						if(isc_open_blob(IB_STATUS, &ib_result->link, &ib_result->trans, &bl_handle, (ISC_QUAD ISC_FAR *) var->sqldata)){
							_php3_ibase_error();
							RETURN_FALSE;
						}
						
						bl_items[0] = isc_info_blob_total_length;
						if(isc_blob_info(IB_STATUS,&bl_handle,sizeof(bl_items),bl_items,sizeof(bl_info),bl_info)){
							_php3_ibase_error();
							RETURN_FALSE;
						}
						
						/* find total length of blob's data */
						for (p = bl_info; *p != isc_info_end && p < bl_info+sizeof(bl_info);){
							int item_len, item = *p++;
							item_len = (short)isc_vax_integer(p, 2);
							p += 2;
							if (item == isc_info_blob_total_length)
								max_len = isc_vax_integer(p, item_len);
							p += item_len;
						}
						
						bl_data = emalloc(max_len+1);
						
						for(cur_len = stat = 0; stat == 0 && cur_len < max_len; ){
							unsigned short seg_len, max_seg = max_len-cur_len > USHRT_MAX ? USHRT_MAX : max_len-cur_len;
							stat = isc_get_segment(IB_STATUS, &bl_handle, &seg_len, max_seg, &bl_data[cur_len]);
							cur_len += seg_len;
							if(cur_len > max_len){ /* never!*/
								efree(bl_data);
								_php3_ibase_module_error("php module internal error in %s %d",__FILE__,__LINE__);
								RETURN_FALSE;
							}
						}
						
						if (IB_STATUS[0] && IB_STATUS[1] && (IB_STATUS[1] != isc_segstr_eof)){
							efree(bl_data);
							_php3_ibase_error();
							RETURN_FALSE;
						}
						bl_data[cur_len] = '\0';
						if(isc_close_blob(IB_STATUS, &bl_handle)){
							efree(bl_data);
							_php3_ibase_error();
							RETURN_FALSE;
						}
						tmp.type = IS_STRING;
						tmp.value.str.len = cur_len;
						tmp.value.str.val = bl_data;
					}else{ /* blob id only */
						ISC_QUAD *bl_qd = (ISC_QUAD ISC_FAR *) var->sqldata;
						ibase_blob_handle *ib_blob_id;
						
						ib_blob_id = (ibase_blob_handle *) emalloc(sizeof(ibase_blob_handle));
						
						ib_blob_id->link = ib_result->link;
						ib_blob_id->trans_handle = ib_result->trans;
						ib_blob_id->bl_qd.gds_quad_high = bl_qd->gds_quad_high;
						ib_blob_id->bl_qd.gds_quad_low = bl_qd->gds_quad_low;
						ib_blob_id->bl_handle = NULL;
						
						tmp.type = IS_STRING;
						tmp.value.str.len = sizeof(ibase_blob_handle);
						tmp.value.str.val = (char *)ib_blob_id;
					}
					break;
				case SQL_ARRAY:{
					ISC_QUAD ar_qd = *(ISC_QUAD ISC_FAR *) var->sqldata;
					ibase_array *ib_array = &ib_result->out_array[arr_cnt];
					void *ar_data;
					char *tmp_ptr;
					
					ar_data = emalloc(ib_array->ar_size);
					
					if (isc_array_get_slice(IB_STATUS, &ib_result->link, &ib_result->trans,
											&ar_qd, &ib_array->ar_desc, ar_data, &ib_array->ar_size)){
						_php3_ibase_error();
						RETURN_FALSE;
					}
					
					tmp_ptr = ar_data; /* avoid changes in _arr_pval */
					if(_php3_ibase_arr_pval(&tmp, &tmp_ptr, ib_array, 0, flag) == FAILURE){
						RETURN_FALSE;
					}
					efree(ar_data);
				}
				break;
				default:
					break;
			}/*switch*/
			if (fetch_type & FETCH_ARRAY) {
				_php3_hash_index_update(return_value->value.ht, i, (void *) &tmp, sizeof(pval),NULL);
			}else{
				_php3_hash_update(return_value->value.ht, var->aliasname, var->aliasname_length+1, (void *) &tmp, sizeof(pval), NULL);
			}
		}/* if not null */
		if((var->sqltype & ~1) == SQL_ARRAY){
			arr_cnt++;
		}
	}/*for field*/
}
/* }}} */


/* {{{ proto array ibase_fetch_row(int result [,blob_flag]) */
/*	 Fetch a row  from the results of a query. */
PHP_FUNCTION(ibase_fetch_row)
{
	_php3_ibase_fetch_hash(INTERNAL_FUNCTION_PARAM_PASSTHRU, FETCH_ARRAY);
}
/* }}} */


/* {{{ proto object ibase_fetch_object(int result [,blob_flag]) */
/* Fetch a object from the results of a query. */
PHP_FUNCTION(ibase_fetch_object)
{
	_php3_ibase_fetch_hash(INTERNAL_FUNCTION_PARAM_PASSTHRU, FETCH_OBJECT);
}
/* }}} */


/* {{{ proto int ibase_free_result(int result) */
/* Free the memory used by a result. */
PHP_FUNCTION(ibase_free_result)
{
	pval *result_arg;
	ibase_result *ib_result;
	IBASE_TLS_VARS;

	RESET_ERRMSG;

	if (ARG_COUNT(ht)!=1 || getParameters(ht, 1, &result_arg)==FAILURE) {
		WRONG_PARAM_COUNT;
	}
	
	convert_to_long(result_arg);
	if (result_arg->value.lval==0) {
		RETURN_FALSE;
	}
	
	GET_RESULT(result_arg->value.lval, ib_result);
	
	php3_list_delete(result_arg->value.lval);
	RETURN_TRUE;
}
/* }}} */


/* {{{ proto int ibase_prepare([int link_identifier, ]string query) */
/* Prepare a query for later execution. */
PHP_FUNCTION(ibase_prepare)
{
	pval *link_arg, *query_arg;
	int link_id, trans_n = 0;
	ibase_db_link *ib_link;
	ibase_query *ib_query;
	char *query;
	IBASE_TLS_VARS;
	

	RESET_ERRMSG;

	switch (ARG_COUNT(ht)) {
		case 1:
			if (getParameters(ht, 1, &query_arg) == FAILURE) {
				RETURN_FALSE;
			}
			link_id = IBASE_GLOBAL(php3_ibase_module).default_link;
			break;
		case 2:
			if (getParameters(ht, 2, &link_arg, &query_arg) == FAILURE) {
				RETURN_FALSE;
			}
			convert_to_long(link_arg);
			link_id = link_arg->value.lval;
			break;
		default:
			WRONG_PARAM_COUNT;
			break;
	}
	
	GET_LINK_TRANS(link_id, ib_link, trans_n);
	
	convert_to_string(query_arg);
	query = query_arg->value.str.val;

	/* open default transaction */
	if(_php3_ibase_def_trans(ib_link, trans_n) == FAILURE){
		RETURN_FALSE;
	}
	
	if(_php3_ibase_alloc_query(&ib_query, ib_link->link, ib_link->trans[trans_n],  query) == FAILURE){
		RETURN_FALSE;
	}

	RETURN_LONG(php3_list_insert(ib_query, IBASE_GLOBAL(php3_ibase_module).le_query));
}
/* }}} */


/* {{{ proto int ibase_execute(int query [,bind_args ... ]) */
/* Execute a previously prepared  query. */
PHP_FUNCTION(ibase_execute)
{
	pval **args, **bind_args = NULL;
	ibase_query *ib_query;
	ibase_result *ib_result;
	IBASE_TLS_VARS;
	

	RESET_ERRMSG;

	if(ARG_COUNT(ht) < 1){
		WRONG_PARAM_COUNT;
	}

	args = emalloc(sizeof(pval*) * ARG_COUNT(ht));
	if (getParametersArray(ht, ARG_COUNT(ht), args) == FAILURE) {
		efree(args);
		RETURN_FALSE;
	}

	GET_QUERY(args[0]->value.lval, ib_query);
	
	if(ARG_COUNT(ht) > 1){ /* have variables to bind */
		bind_args = &args[1];
	}
	
	if( _php3_ibase_exec(&ib_result, ib_query, ARG_COUNT(ht)-1, bind_args) == FAILURE){;
		efree(args);
		RETURN_FALSE;
	}
	
	efree(args);
	
	if (ib_result) { /* select statement */
		RETURN_LONG(php3_list_insert(ib_result, php3_ibase_module.le_result));
	}
	
	RETURN_TRUE;
}
/* }}} */


/* {{{ proto int ibase_free_query(int query) */
/* Free memory used by a query */
PHP_FUNCTION(ibase_free_query)
{
	pval *query_arg;
	ibase_query *ib_query;
	IBASE_TLS_VARS;
	

	RESET_ERRMSG;

	if (ARG_COUNT(ht)!=1 || getParameters(ht, 1, &query_arg) == FAILURE) {
		WRONG_PARAM_COUNT;
	}
	
	convert_to_long(query_arg);

	GET_QUERY(query_arg->value.lval, ib_query);
	
	php3_list_delete(query_arg->value.lval);
	RETURN_TRUE;
}
/* }}} */


/* {{{ proto int ibase_timefmt(string format) */
/* Sets the format of datetime columns returned from queries. Still nonfunctional. */
PHP_FUNCTION(ibase_timefmt)
{
	pval *fmt;
	IBASE_TLS_VARS;
	

	RESET_ERRMSG;

#if HAVE_STRFTIME
	if (ARG_COUNT(ht)!=1 || getParameters(ht, 1, &fmt)==FAILURE) {
		WRONG_PARAM_COUNT;
	}
	convert_to_string(fmt);
	efree(IBASE_GLOBAL(php3_ibase_module).timeformat);
	IBASE_GLOBAL(php3_ibase_module).timeformat = estrdup(fmt->value.str.val);
	RETURN_TRUE;
#else
	_php3_ibase_module_error("ibase_timefmt not supported on this platform");
	RETURN_FALSE;
#endif
}
/* }}} */


/* {{{ proto int ibase_num_fields(int result) */
/* Get the number of fields in result */
PHP_FUNCTION(ibase_num_fields)
{
	pval *result;
	int type;
	ibase_result *ib_result;
	

	RESET_ERRMSG;

	if (ARG_COUNT(ht)!=1 || getParameters(ht, 1, &result)==FAILURE) {
		WRONG_PARAM_COUNT;
	}
	
	convert_to_long(result);
	ib_result = (ibase_result *) php3_list_find(result->value.lval, &type);
	
	if (type!=IBASE_GLOBAL(php3_ibase_module).le_result) {
		_php3_ibase_module_error("%d is not result index", result->value.lval);
		RETURN_FALSE;
	}

	if (ib_result->out_sqlda == NULL) {
		_php3_ibase_module_error("trying to get num fields from a non-select query");
		RETURN_FALSE;
	}

	RETURN_LONG(ib_result->out_sqlda->sqld);
}
/* }}} */


/* {{{ proto array ibase_field_info(int result, int field_number) */
/* Get information about a field */
PHP_FUNCTION(ibase_field_info)
{
	pval *ret_val, *result_arg, *field_arg;
	ibase_result *ib_result;
	char buf[30], *s;
	int len;
	XSQLVAR *var;
	IBASE_TLS_VARS;
	

	RESET_ERRMSG;

	if (ARG_COUNT(ht)!=2 || getParameters(ht, 2, &result_arg, &field_arg)==FAILURE) {
		WRONG_PARAM_COUNT;
	}
	
	convert_to_long(result_arg);

	GET_RESULT(result_arg->value.lval, ib_result);
	
	if (ib_result->out_sqlda == NULL) {
		_php3_ibase_module_error("trying to get field info from a non-select query");
		RETURN_FALSE;
	}

	convert_to_long(field_arg);

	if (field_arg->value.lval<0 || field_arg->value.lval>=ib_result->out_sqlda->sqld)
		RETURN_FALSE;

	if (array_init(return_value)==FAILURE)
		RETURN_FALSE;

	var = ib_result->out_sqlda->sqlvar + field_arg->value.lval;

	add_get_index_stringl(return_value, 0, var->sqlname, var->sqlname_length, (void **) &ret_val, 1);
	_php3_hash_pointer_update(return_value->value.ht, "name", sizeof(char)*5, ret_val);

	add_get_index_stringl(return_value, 1, var->aliasname, var->aliasname_length, (void **) &ret_val, 1);
	_php3_hash_pointer_update(return_value->value.ht, "alias", sizeof(char)*6, ret_val);

	add_get_index_stringl(return_value, 2, var->relname, var->relname_length, (void **) &ret_val, 1);
	_php3_hash_pointer_update(return_value->value.ht, "relation", sizeof(char)*9, ret_val);

	len = sprintf(buf, "%d", var->sqllen);
	add_get_index_stringl(return_value, 3, buf, len, (void **) &ret_val, 1);
	_php3_hash_pointer_update(return_value->value.ht, "length", sizeof(char)*7, ret_val);

	switch (var->sqltype & ~1){
		case SQL_TEXT:	   s = "TEXT"; break;
		case SQL_VARYING:  s = "VARYING"; break;
		case SQL_SHORT:	   s = "SHORT"; break;
		case SQL_LONG:	   s = "LONG"; break;
		case SQL_FLOAT:	   s = "FLOAT"; break;
		case SQL_DOUBLE:   s = "DOUBLE"; break;
		case SQL_D_FLOAT:  s = "D_FLOAT"; break;
		case SQL_DATE:	   s = "DATE"; break;
		case SQL_BLOB:	   s = "BLOB"; break;
		case SQL_ARRAY:	   s = "ARRAY"; break;
		case SQL_QUAD:	   s = "QUAD"; break;
	default:
		sprintf(buf,"unknown (%d)", var->sqltype & ~1);
		s = buf;
		break;
	}
	add_get_index_stringl(return_value, 4, s, strlen(s), (void **) &ret_val, 1);
	_php3_hash_pointer_update(return_value->value.ht, "type", sizeof(char)*5, ret_val);

}
/* }}} */


/* blobs ----------------------------------- */


/* {{{ _php3_ibase_blob_info(isc_blob_handle bl_handle,IBASE_BLOBINFO *bl_info) */
static int _php3_ibase_blob_info(isc_blob_handle bl_handle,IBASE_BLOBINFO *bl_info)
{
	
	static char bl_items[] = {
		isc_info_blob_num_segments,
		isc_info_blob_max_segment,
		isc_info_blob_total_length,
		isc_info_blob_type
	};
	
	char bl_inf[sizeof(long)*8], *p;
	

	bl_info->max_segment = 0;
	bl_info->num_segments = 0;
	bl_info->total_length = 0;
	bl_info->bl_stream = 0;

	if(isc_blob_info(IB_STATUS,&bl_handle,sizeof(bl_items),bl_items,sizeof(bl_inf),bl_inf)){
		_php3_ibase_error();
		return FAILURE;
	}

	for (p = bl_inf; *p != isc_info_end && p < bl_inf+sizeof(bl_inf);){
		int item_len, item = *p++;
		item_len = (short)isc_vax_integer(p, 2);
		p += 2;
		switch(item){
			case isc_info_blob_num_segments:
				bl_info->num_segments = isc_vax_integer(p, item_len);
				break;
			case isc_info_blob_max_segment:
				bl_info->max_segment = isc_vax_integer(p, item_len);
				break;
			case isc_info_blob_total_length:
				bl_info->total_length = isc_vax_integer(p, item_len);
				break;
			case isc_info_blob_type:
				bl_info->bl_stream = isc_vax_integer(p, item_len);
				break;
			case isc_info_end:
				break;
			case isc_info_truncated:
			case isc_info_error:  /* hmm. don't think so...*/
				_php3_ibase_module_error("php module internal error in %s %d",__FILE__,__LINE__);
				return FAILURE;
		}/*switch*/
		p += item_len;
	}/*for*/
	return SUCCESS;
}
/* }}} */


/* {{{ proto int ibase_blob_create([int link_identifier]) */
/* Create blob for adding data */
PHP_FUNCTION(ibase_blob_create)
{
	pval *link_arg;
	int trans_n, link_id = 0;
	ibase_db_link *ib_link;
	ibase_blob_handle *ib_blob;
	IBASE_TLS_VARS;
	

	RESET_ERRMSG;

	switch (ARG_COUNT(ht)) {
		case 0:
			link_id = IBASE_GLOBAL(php3_ibase_module).default_link;
			break;
		case 1:
			if (getParameters(ht, 1, &link_arg) == FAILURE) {
				RETURN_FALSE;
			}
			convert_to_long(link_arg);
			link_id = link_arg->value.lval;
			break;
		default:
			WRONG_PARAM_COUNT;
			break;
	}
	
	GET_LINK_TRANS(link_id, ib_link, trans_n);

	/* open default transaction */
	if(_php3_ibase_def_trans(ib_link, trans_n) == FAILURE){
		RETURN_FALSE;
	}
	
	ib_blob = (ibase_blob_handle *) emalloc(sizeof(ibase_blob_handle));
	ib_blob->trans_handle = ib_link->trans[trans_n];
	ib_blob->link = ib_link->link;
	ib_blob->bl_handle = NULL;
	
	if (isc_create_blob(IB_STATUS, &ib_blob->link, &ib_blob->trans_handle, &ib_blob->bl_handle,&ib_blob->bl_qd)){
		efree(ib_blob);
		_php3_ibase_error();
		RETURN_FALSE;
	}
	
	RETURN_LONG(php3_list_insert(ib_blob, IBASE_GLOBAL(php3_ibase_module).le_blob));
}
/* }}} */


/* {{{ proto int ibase_blob_open(string blob_id) */
/*	 Open blob for retriving data parts	 */
PHP_FUNCTION(ibase_blob_open)
{
	pval *blob_arg;
	ibase_blob_handle *ib_blob, *ib_blob_id;
	IBASE_TLS_VARS;
	

	RESET_ERRMSG;

	if (ARG_COUNT(ht)!=1 || getParameters(ht, 1, &blob_arg)==FAILURE) {
		WRONG_PARAM_COUNT;
	}

	ib_blob = (ibase_blob_handle *) emalloc(sizeof(ibase_blob_handle));

	GET_BLOB_ID_ARG(blob_arg,ib_blob_id);

	if(ib_blob_id == NULL){ /* blob IS NULL or argument unset */
		RETURN_FALSE;
	}
	
	ib_blob->link = ib_blob_id->link;
	ib_blob->trans_handle = ib_blob_id->trans_handle;
	ib_blob->bl_qd.gds_quad_high = ib_blob_id->bl_qd.gds_quad_high;
	ib_blob->bl_qd.gds_quad_low = ib_blob_id->bl_qd.gds_quad_low;
	ib_blob->bl_handle = NULL;
	if (isc_open_blob(IB_STATUS, &ib_blob->link, &ib_blob->trans_handle, &ib_blob->bl_handle, &ib_blob->bl_qd)){
		efree(ib_blob);
		_php3_ibase_error();
		RETURN_FALSE;
	}
	
	RETURN_LONG(php3_list_insert(ib_blob, IBASE_GLOBAL(php3_ibase_module).le_blob));
}
/* }}} */


/* {{{ proto int ibase_blob_add(int blob_id, string data) */
/* Add data into created blob */
PHP_FUNCTION(ibase_blob_add)
{
	pval *blob_arg,*string_arg;
	ibase_blob_handle *ib_blob;
	IBASE_TLS_VARS;


	RESET_ERRMSG;

	if (ARG_COUNT(ht)!=2 || getParameters(ht, 2, &blob_arg, &string_arg)==FAILURE) {
		WRONG_PARAM_COUNT;
	}

	GET_BLOB_HANDLE_ARG(blob_arg, ib_blob);
	
	convert_to_string(string_arg);

	if (isc_put_segment(IB_STATUS, &ib_blob->bl_handle, string_arg->value.str.len, string_arg->value.str.val)){
		_php3_ibase_error();
		RETURN_FALSE;
	}
	RETURN_TRUE;
}
/* }}} */


/* {{{ proto string ibase_blob_get(int blob_id, int len) */
/* Get len bytes data from open blob */
PHP_FUNCTION(ibase_blob_get)
{
	pval *blob_arg, *len_arg;
	int stat;
	char *bl_data;
	unsigned short max_len = 0, cur_len, seg_len;
	ibase_blob_handle *ib_blob;
	IBASE_TLS_VARS;


	RESET_ERRMSG;

	if(ARG_COUNT(ht) != 2 || getParameters(ht, 2, &blob_arg, &len_arg) == FAILURE) {
		WRONG_PARAM_COUNT;
	}

	convert_to_long(len_arg);
	max_len = len_arg->value.lval;

	GET_BLOB_HANDLE_ARG(blob_arg, ib_blob);

	if(ib_blob->bl_qd.gds_quad_high || ib_blob->bl_qd.gds_quad_low){ /*not null ?*/
		
		bl_data = emalloc(max_len+1);

		for(cur_len = stat = 0; stat == 0; ){
			stat = isc_get_segment(IB_STATUS, &ib_blob->bl_handle, &seg_len, max_len-cur_len, &bl_data[cur_len]);
			cur_len += seg_len;
			if(cur_len > max_len){ /* never!*/
				efree(bl_data);
				_php3_ibase_module_error("php module internal error in %s %d",__FILE__,__LINE__);
				RETURN_FALSE;
			}
		}

		bl_data[cur_len] = '\0';
		if (IB_STATUS[0] && (IB_STATUS[1] != isc_segstr_eof && IB_STATUS[1] != isc_segment)){
			efree(bl_data);
			_php3_ibase_error();
			RETURN_FALSE;
		}
		RETURN_STRINGL(bl_data,cur_len,0);
	}else{ /* null blob */
		RETURN_STRING("",1); /* empty string */
	}
}
/* }}} */


#define BLOB_CLOSE 1
#define BLOB_CANCEL 2

/* {{{ _php3_ibase_blob_end() */
/* Close or Cancel created or Close open blob */
static void _php3_ibase_blob_end(INTERNAL_FUNCTION_PARAMETERS, int bl_end)
{
	pval *blob_arg;
	ibase_blob_handle *ib_blob;
	IBASE_TLS_VARS;


	RESET_ERRMSG;
	
	if (ARG_COUNT(ht)!=1 || getParameters(ht, 1, &blob_arg)==FAILURE) {
		WRONG_PARAM_COUNT;
	}

	GET_BLOB_HANDLE_ARG(blob_arg, ib_blob);

	if(bl_end == BLOB_CLOSE){ /* return id here */
		if(ib_blob->bl_qd.gds_quad_high || ib_blob->bl_qd.gds_quad_low){ /*not null ?*/
			if (isc_close_blob(IB_STATUS, &ib_blob->bl_handle)){
				_php3_ibase_error();
				RETURN_FALSE;
			}
		}
		ib_blob->bl_handle = NULL;
		RETVAL_STRINGL((char *)ib_blob, sizeof(ibase_blob_handle), 1);
		php3_list_delete(blob_arg->value.lval);
	}else{ /* discard created blob */
		if (isc_cancel_blob(IB_STATUS, &ib_blob->bl_handle)){
			_php3_ibase_error();
			RETURN_FALSE;
		}
		ib_blob->bl_handle = NULL;
		php3_list_delete(blob_arg->value.lval);
		RETURN_TRUE;
	}
}
/* }}} */


/* {{{ proto int ibase_blob_close(int blob_id) */
/* Close blob */
PHP_FUNCTION(ibase_blob_close)
{
	_php3_ibase_blob_end(INTERNAL_FUNCTION_PARAM_PASSTHRU,BLOB_CLOSE);
}
/* }}} */


/* {{{ proto int ibase_blob_cancel(int blob_id) */
/* Cancel creating blob */
PHP_FUNCTION(ibase_blob_cancel)
{
	_php3_ibase_blob_end(INTERNAL_FUNCTION_PARAM_PASSTHRU,BLOB_CANCEL);
}
/* }}} */


/* {{{ proto object ibase_blob_info(string blob_id_str) */
/* Return blob length and other useful info */
PHP_FUNCTION(ibase_blob_info)
{
	pval *blob_arg, *result_var;
	ibase_blob_handle *ib_blob_id;
	IBASE_BLOBINFO bl_info;
	IBASE_TLS_VARS;


	RESET_ERRMSG;

	if (ARG_COUNT(ht)!=1 || getParameters(ht, 1, &blob_arg)==FAILURE) {
		WRONG_PARAM_COUNT;
	}
	
	GET_BLOB_ID_ARG(blob_arg, ib_blob_id);

	if (array_init(return_value)==FAILURE){
		RETURN_FALSE;
	}
	
	if(ib_blob_id->bl_qd.gds_quad_high || ib_blob_id->bl_qd.gds_quad_low){ /*not null ?*/
		if (isc_open_blob(IB_STATUS, &ib_blob_id->link, &ib_blob_id->trans_handle,
						  &ib_blob_id->bl_handle, &ib_blob_id->bl_qd)){
			_php3_ibase_error();
			RETURN_FALSE;
		}

		if(_php3_ibase_blob_info(ib_blob_id->bl_handle,&bl_info)){
			RETURN_FALSE;
		}
		if(isc_close_blob(IB_STATUS, &ib_blob_id->bl_handle)){
			_php3_ibase_error();
			RETURN_FALSE;
		}
		ib_blob_id->bl_handle = NULL;
	}else{ /* null blob, all values to zero	 */
		bl_info.max_segment = 0;
		bl_info.num_segments = 0;
		bl_info.total_length = 0;
		bl_info.bl_stream = 0;
	}

	add_get_index_long(return_value, 0, bl_info.total_length, (void **)&result_var);
	_php3_hash_pointer_update(return_value->value.ht, "length", sizeof("length"), result_var);

	add_get_index_long(return_value, 1, bl_info.num_segments, (void **)&result_var);
	_php3_hash_pointer_update(return_value->value.ht, "numseg", sizeof("numseg"), result_var);

	add_get_index_long(return_value, 2, bl_info.max_segment, (void **)&result_var);
	_php3_hash_pointer_update(return_value->value.ht, "maxseg", sizeof("maxseg"), result_var);

	add_get_index_long(return_value, 3, bl_info.bl_stream, (void **)&result_var);
	_php3_hash_pointer_update(return_value->value.ht, "stream", sizeof("stream"), result_var);
	
	add_get_index_long(return_value, 4,
					   (!ib_blob_id->bl_qd.gds_quad_high
						&& !ib_blob_id->bl_qd.gds_quad_low),
					   (void **)&result_var);
	_php3_hash_pointer_update(return_value->value.ht, "isnull", sizeof("isnull"), result_var);
}
/* }}} */


/* {{{ proto int ibase_blob_echo(string blob_id_str) */
/* Output blob contents to browser	*/
PHP_FUNCTION(ibase_blob_echo)
{
	pval *blob_arg;
	char bl_data[IBASE_BLOB_SEG];
	unsigned short seg_len;
	ibase_blob_handle *ib_blob_id;
	

	IBASE_TLS_VARS;

	RESET_ERRMSG;

	if (ARG_COUNT(ht)!=1 || getParameters(ht, 1, &blob_arg)==FAILURE) {
		WRONG_PARAM_COUNT;
	}

	GET_BLOB_ID_ARG(blob_arg, ib_blob_id);
	
	if (!php3_header()) {
		RETURN_FALSE;
	}

	if(ib_blob_id){ /*not null ?*/
		
		if (isc_open_blob(IB_STATUS, &ib_blob_id->link, &ib_blob_id->trans_handle,
						  &ib_blob_id->bl_handle,&ib_blob_id->bl_qd)){
			_php3_ibase_error();
			RETURN_FALSE;
		}

		while(!isc_get_segment(IB_STATUS, &ib_blob_id->bl_handle, &seg_len, sizeof(bl_data), bl_data)
			  || IB_STATUS[1] == isc_segment){
			PHPWRITE(bl_data,seg_len);
		}
	
		if (IB_STATUS[0] && (IB_STATUS[1] != isc_segstr_eof)){
			_php3_ibase_error();
			RETURN_FALSE;
		}

		if (isc_close_blob(IB_STATUS, &ib_blob_id->bl_handle)){
			_php3_ibase_error();
			RETURN_FALSE;
		}
		ib_blob_id->bl_handle = NULL;
	}/* not null ? */

	RETURN_TRUE;
}
/* }}} */


/* from file.c. I'm not sure, this variables always initialized? */
extern int le_fp,le_pp;
extern int wsa_fp;

/* {{{ proto string ibase_blob_import([link_identifier, ] file_id) */
/* Create blob, copy file in it, and close it*/
PHP_FUNCTION(ibase_blob_import)
{
	pval *link_arg, *file_arg;
	int trans_n, type,	link_id = 0, file_id, size, b;
	int issock=0, socketd=0, *sock;
	ibase_blob_handle ib_blob;
	ibase_db_link *ib_link;
	char bl_data[IBASE_BLOB_SEG]; /* FIXME? blob_seg_size parameter?	 */
	FILE *fp;
	IBASE_TLS_VARS;


	RESET_ERRMSG;

	switch (ARG_COUNT(ht)) {
		case 1:
			if (getParameters(ht, 1, &file_arg) == FAILURE) {
				RETURN_FALSE;
			}
			link_id = IBASE_GLOBAL(php3_ibase_module).default_link;
			break;
		case 2:
			if (getParameters(ht, 2, &link_arg, &file_arg) == FAILURE) {
				RETURN_FALSE;
			}
			convert_to_long(link_arg);
			link_id = link_arg->value.lval;
			break;
		default:
			WRONG_PARAM_COUNT;
			break;
	}
	
	GET_LINK_TRANS(link_id, ib_link, trans_n);
	
	/* open default transaction */
	if(_php3_ibase_def_trans(ib_link, trans_n) == FAILURE){
		RETURN_FALSE;
	}

	convert_to_long(file_arg);
	file_id = file_arg->value.lval;
	fp = php3_list_find(file_id, &type);
	if (type == GLOBAL(wsa_fp)){
		issock = 1;
		sock = php3_list_find(file_id, &type);
		socketd =* sock;
	}
	if ((!fp || (type!=GLOBAL(le_fp) && type!=GLOBAL(le_pp))) && (!socketd || type!=GLOBAL(wsa_fp))) {
		_php3_ibase_module_error("Unable to find file identifier %d",file_id);
		RETURN_FALSE;
	}

	ib_blob.link = ib_link->link;
	ib_blob.trans_handle = ib_link->trans[trans_n];
	ib_blob.bl_handle = NULL;
	ib_blob.bl_qd.gds_quad_high = 0;
	ib_blob.bl_qd.gds_quad_low = 0;
	
	if (isc_create_blob(IB_STATUS, &ib_blob.link, &ib_blob.trans_handle, &ib_blob.bl_handle,&ib_blob.bl_qd)){
		_php3_ibase_error();
		RETURN_FALSE;
	}

	size = 0;
	while(issock?(b=SOCK_FREAD(bl_data,sizeof(bl_data),socketd)):(b = fread(bl_data, 1, sizeof(bl_data), fp)) > 0) {
		if (isc_put_segment(IB_STATUS, &ib_blob.bl_handle, b, bl_data)){
			_php3_ibase_error();
			RETURN_FALSE;
		}
		size += b ;
	}
	
	if (isc_close_blob(IB_STATUS, &ib_blob.bl_handle)){
		_php3_ibase_error();
		RETURN_FALSE;
	}

	php3_list_delete(file_id);

	ib_blob.bl_handle = NULL;
	RETVAL_STRINGL((char *)&ib_blob, sizeof(ibase_blob_handle), 1);
}
/* }}} */



#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 */

