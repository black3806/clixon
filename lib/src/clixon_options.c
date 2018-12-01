/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2018 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 *
 * CLICON options
 * See clicon_tutorial appendix and clicon.conf.cpp.cpp on documentation of
 * options
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_plugin.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_xml_map.h"

/* Mapping between Clicon startup modes string <--> constants, 
   see clixon-config.yang type startup_mode */
static const map_str2int startup_mode_map[] = {
    {"none",     SM_NONE}, 
    {"running",  SM_RUNNING}, 
    {"startup",  SM_STARTUP}, 
    {"init",     SM_INIT}, 
    {NULL,       -1}
};

/*! Print registry on file. For debugging.
 */
void
clicon_option_dump(clicon_handle h, 
		   int           dbglevel)
{
    clicon_hash_t *hash = clicon_options(h);
    int            i;
    char         **keys;
    void          *val;
    size_t         klen;
    size_t         vlen;
    
    if (hash == NULL)
	return;
    keys = hash_keys(hash, &klen);
    if (keys == NULL)
	return;
	
    for(i = 0; i < klen; i++) {
	val = hash_value(hash, keys[i], &vlen);
	if (vlen){
	    if (((char*)val)[vlen-1]=='\0') /* assume string */
		clicon_debug(dbglevel, "%s =\t \"%s\"", keys[i], (char*)val);
	    else
		clicon_debug(dbglevel, "%s =\t 0x%p , length %zu", keys[i], val, vlen);
	}
	else
	    clicon_debug(dbglevel, "%s = NULL", keys[i]);
    }
    free(keys);

}

/*! Read filename and set values to global options registry. XML variant.
 *
 * @param[out] xconfig Pointer to xml config tree. Should be freed by caller
 * @retval     0       OK
 * @retval    -1       Error
 */
static int
parse_configfile(clicon_handle  h,
		 const char    *filename,
		 yang_spec     *yspec,
		 cxobj        **xconfig)
{
    struct stat st;
    FILE       *f = NULL;
    int         retval = -1;
    int         fd;
    cxobj      *xt = NULL;
    cxobj      *xc = NULL;
    cxobj      *x = NULL;
    char       *name;
    char       *body;
    clicon_hash_t *copt = clicon_options(h);

    if (filename == NULL || !strlen(filename)){
	clicon_err(OE_UNIX, 0, "Not specified");
	goto done;
    }
    if (stat(filename, &st) < 0){
	clicon_err(OE_UNIX, errno, "%s", filename);
	goto done;
    }
    if (!S_ISREG(st.st_mode)){
	clicon_err(OE_UNIX, 0, "%s is not a regular file", filename);
	goto done;
    }
    if ((f = fopen(filename, "r")) == NULL) {
	clicon_err(OE_UNIX, errno, "configure file: %s", filename);
	return -1;
    }
    clicon_debug(2, "%s: Reading config file %s", __FUNCTION__, filename);
    fd = fileno(f);
    if (xml_parse_file(fd, "</clicon>", yspec, &xt) < 0)
	goto done;
    if (xml_child_nr(xt)==1 && xml_child_nr_type(xt, CX_BODY)==1){
	clicon_err(OE_CFG, 0, "Config file %s: Expected XML but is probably old sh style", filename);
	goto done;
    }
    if ((xc = xpath_first(xt, "config")) == NULL) {
	clicon_err(OE_CFG, 0, "Config file %s: Lacks top-level \"config\" element", filename);
	goto done;	
    }
    if (xml_apply0(xc, CX_ELMNT, xml_default, yspec) < 0)
	goto done;	
    if (xml_apply0(xc, CX_ELMNT, xml_yang_validate_add, NULL) < 0)
	goto done;	
    while ((x = xml_child_each(xc, x, CX_ELMNT)) != NULL) {
	name = xml_name(x);
	body = xml_body(x);
	if (name==NULL || body == NULL){
	    clicon_log(LOG_WARNING, "%s option NULL: name:%s body:%s",
		       __FUNCTION__, name, body);
	    continue;
	}
	/* hard-coded exceptions for configure options that are leaf-lists (not leaf)
	 * They must be accessed directly by looping over clicon_conf_xml(h)
	 */
	if (strcmp(name,"CLICON_FEATURE")==0)
	    continue;
	if (strcmp(name,"CLICON_YANG_DIR")==0)
	    continue;
	/* Used as an arg to this fn */
	if (strcmp(name,"CLICON_CONFIGFILE")==0)
	    continue;
	if (hash_add(copt, 
		     name,
		     body,
		     strlen(body)+1) == NULL)
	    goto done;
    }
    retval = 0;
    *xconfig = xt;
    xt = NULL;
  done:
    if (xt)
	xml_free(xt);
    if (f)
	fclose(f);
    return retval;
}

/*! Parse clixon yang file. Parse XML config file. Initialize option values
 *
 * Set default options, Read config-file, Check that all values are set.
 * Parse clixon yang file and save in yspec.
 * Read clixon system config files
 * @param[in]  h     clicon handle
 * @param[in]  yspec Yang spec of clixon config file
 * @note Due to Bug: Top-level Yang symbol cannot be called "config" in any 
 *       imported yang file, the config module needs to be isolated from all 
 *       other yang modules.
 */
int
clicon_options_main(clicon_handle h,
		    yang_spec    *yspec)
{
    int            retval = -1;
    char          *configfile;
    clicon_hash_t *copt = clicon_options(h);
    char          *suffix;
    char           xml = 0; /* Configfile is xml, otherwise legacy */
    cxobj         *xconfig = NULL;

    /*
     * Set configure file if not set by command-line above
     */
    if (!hash_lookup(copt, "CLICON_CONFIGFILE")){ 
	clicon_option_str_set(h, "CLICON_CONFIGFILE", CLIXON_DEFAULT_CONFIG);
    }
    configfile = hash_value(copt, "CLICON_CONFIGFILE", NULL);
    clicon_debug(1, "CLICON_CONFIGFILE=%s", configfile);
    /* If file ends with .xml, assume it is new format */
    if ((suffix = rindex(configfile, '.')) != NULL){
	suffix++;
	xml = strcmp(suffix, "xml") == 0;
    }
    if (xml == 0){
	clicon_err(OE_CFG, 0, "%s: suffix %s not recognized (Run ./configure --with-config-compat?)", configfile, suffix);
	goto done;
    }
    /* Read configfile first without yangspec, for bootstrapping */
    if (parse_configfile(h, configfile, yspec, &xconfig) < 0)
	goto done;
    if (xml_rootchild(xconfig, 0, &xconfig) < 0)
	goto done;
    /* Set clixon_conf pointer to handle */
    clicon_conf_xml_set(h, xconfig);
    /* Parse clixon yang spec */
    if (yang_parse(h, NULL, "clixon-config", NULL, yspec, NULL) < 0)
	goto done;    
    clicon_conf_xml_set(h, NULL);
    if (xconfig)
	xml_free(xconfig);
    /* Read configfile second time now with check yang spec */
    if (parse_configfile(h, configfile, yspec, &xconfig) < 0)
	goto done;
    if (xml_rootchild(xconfig, 0, &xconfig) < 0)
	goto done;
    /* Set clixon_conf pointer to handle */
    clicon_conf_xml_set(h, xconfig);
    /* Specific option handling */
    if (clicon_option_bool(h, "CLICON_XML_SORT") == 1)
	xml_child_sort = 1;
    else
	xml_child_sort = 0;
    retval = 0;
 done:
    return retval;
}

/*! Check if a clicon option has a value
 * @param[in] h     clicon_handle
 * @param[in] name  option name
 * @retval  !=0     option exists
 * @retval    0     option does not exists
 */
int
clicon_option_exists(clicon_handle h,
		     const char   *name)
{
    clicon_hash_t *copt = clicon_options(h);

    return (hash_lookup(copt, (char*)name) != NULL);
}

/*! Get a single string option string via handle
 *
 * @param[in] h       clicon_handle
 * @param[in] name    option name
 * @retval    NULL    If option not found, or value of option is NULL
 * @retval    string  value of option if found
 * clicon options should be strings.
 * @note To differentiate the two reasons why NULL may be returned, use function 
 * clicon_option_exists() before the call
 */
char *
clicon_option_str(clicon_handle h, 
		  const char   *name)
{
    clicon_hash_t *copt = clicon_options(h);

    if (hash_lookup(copt, (char*)name) == NULL)
	return NULL;
    return hash_value(copt, (char*)name, NULL);
}

/*! Set a single string option via handle 
 * @param[in] h       clicon_handle
 * @param[in] name    option name
 * @param[in] val     option value, must be null-terminated string
 */
int
clicon_option_str_set(clicon_handle h, 
		      const char   *name, 
		      char         *val)
{
    clicon_hash_t *copt = clicon_options(h);

    return hash_add(copt, (char*)name, val, strlen(val)+1)==NULL?-1:0;
}

/*! Get options as integer but stored as string
 *
 * @param[in] h    clicon handle
 * @param[in] name name of option
 * @retval    int  An integer as aresult of atoi
 * @retval    -1   If option does not exist
 * @code
 *  if (clicon_option_exists(h, "X")
 *	return clicon_option_int(h, "X");
 *  else
 *      return 0;
 * @endcode
 * Note that -1 can be both error and value.
 * This means that it should be used together with clicon_option_exists() and
 * supply a defualt value as shown in the example.
 */
int
clicon_option_int(clicon_handle h,
		  const char   *name)
{
    char *s;

    if ((s = clicon_option_str(h, name)) == NULL)
	return -1;
    return atoi(s);
}

/*! Set option given as int.
 */
int
clicon_option_int_set(clicon_handle h,
		      const char   *name,
		      int           val)
{
    char s[64];
    
    if (snprintf(s, sizeof(s)-1, "%u", val) < 0)
	return -1;
    return clicon_option_str_set(h, name, s);
}

/*! Get options as bool but stored as string
 *
 * @param[in] h    clicon handle
 * @param[in] name name of option
 * @retval    0    false, or does not exist, or does not have a boolean value
 * @retval    1    true
 * @code
 *  if (clicon_option_exists(h, "X")
 *	return clicon_option_bool(h, "X");
 *  else
 *      return 0; # default false? 
 * @endcode
 * Note that 0 can be both error and false.
 * This means that it should be used together with clicon_option_exists() and
 * supply a default value as shown in the example.
 */
int
clicon_option_bool(clicon_handle h,
		  const char   *name)
{
    char *s;

    if ((s = clicon_option_str(h, name)) == NULL)
	return 0;
    if (strcmp(s,"true")==0)
	return 1;
    return 0; /* Hopefully false, but anything else than "true" */
}

/*! Set option given as bool
 */
int
clicon_option_bool_set(clicon_handle h,
		      const char   *name,
		      int           val)
{
    char s[64];
    
    if (snprintf(s, sizeof(s)-1, "%u", val) < 0)
	return -1;
    return clicon_option_str_set(h, name, s);
}

/*! Delete option 
 */
int
clicon_option_del(clicon_handle h,
		  const char   *name)
{
    clicon_hash_t *copt = clicon_options(h);

    return hash_del(copt, (char*)name);
}

/*-----------------------------------------------------------------
 * Specific option access functions for YANG configuration variables.
 * Sometimes overridden by command-line options, 
 * such as -f for CLICON_CONFIGFILE
 * @see yang/clixon-config@<date>.yang
 * You can always use the basic access functions, such as
 * clicon_option_str[_set]
 * But sometimes there are type conversions, etc which makes it more
 * convenient to make wrapper functions. Or not?
 *-----------------------------------------------------------------*/
/*! Whether to generate CLIgen syntax from datamodel or not (0 or 1)
 * Must be used with a previous clicon_option_exists().
 * @see clixon-config@<date>.yang CLICON_CLI_GENMODEL
 */
int
clicon_cli_genmodel(clicon_handle h)
{
    char const *opt = "CLICON_CLI_GENMODEL";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/*! Generate code for CLI completion of existing db symbols
 * @see clixon-config@<date>.yang CLICON_CLI_GENMODEL_COMPLETION
 */
int
clicon_cli_genmodel_completion(clicon_handle h)
{
    char const *opt = "CLICON_CLI_GENMODEL_COMPLETION";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/*! How to generate and show CLI syntax: VARS|ALL 
 * @see clixon-config@<date>.yang CLICON_CLI_GENMODEL_TYPE
 */
enum genmodel_type
clicon_cli_genmodel_type(clicon_handle h)
{
    char *s;
    enum genmodel_type gt = GT_ERR;

    if (!clicon_option_exists(h, "CLICON_CLI_GENMODEL_TYPE")){
	gt = GT_VARS;
	goto done;
    }
    s = clicon_option_str(h, "CLICON_CLI_GENMODEL_TYPE");
    if (strcmp(s, "NONE")==0)
	gt = GT_NONE;
    else
    if (strcmp(s, "VARS")==0)
	gt = GT_VARS;
    else
    if (strcmp(s, "ALL")==0)
	gt = GT_ALL;
  done:
    return gt;
}

/*! Get Dont include keys in cvec in cli vars callbacks
 * @see clixon-config@<date>.yang CLICON_CLI_VARONLY
 */
int
clicon_cli_varonly(clicon_handle h)
{
    char const *opt = "CLICON_CLI_VARONLY";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/*! Get family of backend socket: AF_UNIX, AF_INET or AF_INET6 
 * @see clixon-config@<date>.yang CLICON_SOCK_FAMILY
 */
int
clicon_sock_family(clicon_handle h)
{
    char *s;

    if ((s = clicon_option_str(h, "CLICON_SOCK_FAMILY")) == NULL)
	return AF_UNIX;
    else  if (strcmp(s, "IPv4")==0)
	return AF_INET;
    else  if (strcmp(s, "IPv6")==0)
	return AF_INET6;
    else
	return AF_UNIX; /* default */
}

/*! Get port for backend socket in case of AF_INET or AF_INET6 
 * @see clixon-config@<date>.yang CLICON_SOCK_PORT
 */
int
clicon_sock_port(clicon_handle h)
{
    char *s;

    if ((s = clicon_option_str(h, "CLICON_SOCK_PORT")) == NULL)
	return -1;
    return atoi(s);
}

/*! Set if all configuration changes are committed automatically 
 */
int
clicon_autocommit(clicon_handle h)
{
    char const *opt = "CLICON_AUTOCOMMIT";

    if (clicon_option_exists(h, opt))
	return clicon_option_int(h, opt);
    else
	return 0;
}

/*! Which method to boot/start clicon backen
 */
int
clicon_startup_mode(clicon_handle h)
{
    char *mode;
    if ((mode = clicon_option_str(h, "CLICON_STARTUP_MODE")) == NULL)
	return -1;
    return clicon_str2int(startup_mode_map, mode);
}

/*---------------------------------------------------------------------
 * Specific option access functions for non-yang options
 * Typically dynamic values and more complex datatypes,
 * Such as handles to plugins, API:s and parsed structures
 *--------------------------------------------------------------------*/

/* eg -q option, dont print notifications on stdout */
int
clicon_quiet_mode(clicon_handle h)
{
    char *s;
    if ((s = clicon_option_str(h, "CLICON_QUIET")) == NULL)
	return 0; /* default */
    return atoi(s);
}
int
clicon_quiet_mode_set(clicon_handle h, int val)
{
    return clicon_option_int_set(h, "CLICON_QUIET", val);
}

/*! Get YANG specification for application
 * Must use hash functions directly since they are not strings.
 */
yang_spec *
clicon_dbspec_yang(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *p;

    if ((p = hash_value(cdat, "dbspec_yang", &len)) != NULL)
	return *(yang_spec **)p;
    return NULL;
}

/*! Set yang specification for application
 * ys must be a malloced pointer
 */
int
clicon_dbspec_yang_set(clicon_handle     h, 
		       struct yang_spec *ys)
{
    clicon_hash_t  *cdat = clicon_data(h);

    /* It is the pointer to ys that should be copied by hash,
       so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "dbspec_yang", &ys, sizeof(ys)) == NULL)
	return -1;
    return 0;
}

#if 1 /* Temporary function until "Top-level Yang symbol cannot be called "config"" is fixed */
/*! Get YANG specification for clixon config
 * Must use hash functions directly since they are not strings.
 */
yang_spec *
clicon_config_yang(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *p;

    if ((p = hash_value(cdat, "control_yang", &len)) != NULL)
	return *(yang_spec **)p;
    return NULL;
}

/*! Set yang specification for control
 * ys must be a malloced pointer
 */
int
clicon_config_yang_set(clicon_handle     h, 
		       struct yang_spec *ys)
{
    clicon_hash_t  *cdat = clicon_data(h);

    /* It is the pointer to ys that should be copied by hash,
       so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "control_yang", &ys, sizeof(ys)) == NULL)
	return -1;
    return 0;
}
#endif

/*! Get YANG specification for Clixon system options and features
 * Must use hash functions directly since they are not strings.
 * Example: features are typically accessed directly in the config tree.
 */
cxobj *
clicon_conf_xml(clicon_handle h)
{
    clicon_hash_t *cdat = clicon_data(h);
    size_t         len;
    void          *p;

    if ((p = hash_value(cdat, "clixon_conf", &len)) != NULL)
	return *(cxobj **)p;
    return NULL;
}

 /*! Set YANG specification for Clixon system options and features
 * ys must be a malloced pointer
 */
int
clicon_conf_xml_set(clicon_handle h, 
		    cxobj        *x)
{
    clicon_hash_t  *cdat = clicon_data(h);

    /* It is the pointer to x that should be copied by hash,
     * so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "clixon_conf", &x, sizeof(x)) == NULL)
	return -1;
    return 0;
}

/*! Get xmldb datastore plugin handle, as used by dlopen/dlsym/dlclose */
plghndl_t
clicon_xmldb_plugin_get(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *p;

    if ((p = hash_value(cdat, "xmldb_plugin", &len)) != NULL)
	return *(plghndl_t*)p;
    return NULL;
}

/*! Set xmldb datastore plugin handle, as used by dlopen/dlsym/dlclose */
int
clicon_xmldb_plugin_set(clicon_handle h, 
			plghndl_t     handle)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (hash_add(cdat, "xmldb_plugin", &handle, sizeof(void*)) == NULL)
	return -1;
    return 0;
}

/*! Get XMLDB API struct pointer
 * @param[in]  h   Clicon handle
 * @retval     xa  XMLDB API struct
 * @note xa is really of type struct xmldb_api*
 */
void *
clicon_xmldb_api_get(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *xa;

    if ((xa = hash_value(cdat, "xmldb_api", &len)) != NULL)
	return *(void**)xa;
    return NULL;
}

/*! Set or reset XMLDB API struct pointer
 * @param[in]  h   Clicon handle
 * @param[in]  xa  XMLDB API struct
 * @note xa is really of type struct xmldb_api*
 */
int
clicon_xmldb_api_set(clicon_handle     h, 
		     void             *xa)
{
    clicon_hash_t  *cdat = clicon_data(h);

    /* It is the pointer to xa_api that should be copied by hash,
       so we send a ptr to the ptr to indicate what to copy.
     */
    if (hash_add(cdat, "xmldb_api", &xa, sizeof(void*)) == NULL)
	return -1;
    return 0;
}

/*! Get XMLDB storage handle
 * @param[in]  h   Clicon handle
 * @retval     xh  XMLDB storage handle. If not connected return NULL
 */
void *
clicon_xmldb_handle_get(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);
    size_t          len;
    void           *xh;

    if ((xh = hash_value(cdat, "xmldb_handle", &len)) != NULL)
	return *(void**)xh;
    return NULL;
}

/*! Set or reset XMLDB storage handle
 * @param[in]  h   Clicon handle
 * @param[in]  xh  XMLDB storage handle. If NULL reset it
 * @note Just keep note of it, dont allocate it or so.
 */
int
clicon_xmldb_handle_set(clicon_handle h, 
			void         *xh)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (hash_add(cdat, "xmldb_handle", &xh, sizeof(void*)) == NULL)
	return -1;
    return 0;
}

/*! Get authorized user name
 * @param[in]  h   Clicon handle
 * @retval     xh  XMLDB storage handle. If not connected return NULL
 */
char *
clicon_username_get(clicon_handle h)
{
    clicon_hash_t  *cdat = clicon_data(h);

    return (char*)hash_value(cdat, "username", NULL);
}

/*! Set authorized user name
 * @param[in]  h   Clicon handle
 * @param[in]  xh  XMLDB storage handle. If NULL reset it
 * @note Just keep note of it, dont allocate it or so.
 */
int
clicon_username_set(clicon_handle h, 
		    void         *username)
{
    clicon_hash_t  *cdat = clicon_data(h);

    if (username == NULL)
	return hash_del(cdat, "username");
    return hash_add(cdat, "username", username, strlen(username)+1)==NULL?-1:0;
}


