/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

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
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * 
 * handling netconf plugins
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <assert.h>
#include <dlfcn.h>
#include <dirent.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* clicon netconf*/
#include "clixon_netconf.h"
#include "netconf_lib.h"
#include "netconf_plugin.h"

/* Database dependency description */
struct netconf_reg {
    qelem_t 	 nr_qelem;	/* List header */
    netconf_cb_t nr_callback;	/* Validation/Commit Callback */
    void	*nr_arg;	/* Application specific argument to cb */
    char        *nr_tag;	/* Xml tag when matched, callback called */
};
typedef struct netconf_reg netconf_reg_t;

/*! Unload a plugin
 */
static int
plugin_unload(clicon_handle h, void *handle)
{
    int retval = 0;
    char *error;
    plgexit_t *exitfn;

    /* Call exit function is it exists */
    exitfn = dlsym(handle, PLUGIN_EXIT);
    if (dlerror() == NULL)
	exitfn(h);

    dlerror();    /* Clear any existing error */
    if (dlclose(handle) != 0) {
	error = (char*)dlerror();
	clicon_err(OE_PLUGIN, errno, "dlclose: %s\n", error ? error : "Unknown error");
	/* Just report */
    }
    return retval;
}



/*
 * Load a dynamic plugin object and call it's init-function
 * Note 'file' may be destructively modified
 */
static plghndl_t 
plugin_load (clicon_handle h, char *file, int dlflags)
{
    char      *error;
    void      *handle = NULL;
    plginit_t *initfn;

    dlerror();    /* Clear any existing error */
    if ((handle = dlopen (file, dlflags)) == NULL) {
        error = (char*)dlerror();
	clicon_err(OE_PLUGIN, errno, "dlopen: %s\n", error ? error : "Unknown error");
	goto quit;
    }
    /* call plugin_init() if defined */
    if ((initfn = dlsym(handle, PLUGIN_INIT)) != NULL) {
	if (initfn(h) != 0) {
	    clicon_err(OE_PLUGIN, errno, "Failed to initiate %s\n", strrchr(file,'/')?strchr(file, '/'):file);
	    goto quit;
	}
    }
quit:

    return handle;
}

static int nplugins = 0;
static plghndl_t *plugins = NULL;
static netconf_reg_t *deps = NULL;

/*! Load all plugins you can find in CLICON_NETCONF_DIR
 */
int 
netconf_plugin_load(clicon_handle h)
{
    int            retval = -1;
    char          *dir;
    int            ndp;
    struct dirent *dp = NULL;
    int            i;
    char           filename[MAXPATHLEN];
    plghndl_t     *handle;

    if ((dir = clicon_netconf_dir(h)) == NULL){
	clicon_err(OE_PLUGIN, 0, "clicon_netconf_dir not defined");
	goto quit;
    }

    /* Get plugin objects names from plugin directory */
    if((ndp = clicon_file_dirent(dir, &dp, "(.so)$", S_IFREG))<0)
	goto quit;

    /* Load all plugins */
    for (i = 0; i < ndp; i++) {
	snprintf(filename, MAXPATHLEN-1, "%s/%s", dir, dp[i].d_name);
	clicon_debug(1, "DEBUG: Loading plugin '%.*s' ...", 
		     (int)strlen(filename), filename);
	if ((handle = plugin_load(h, filename, RTLD_NOW)) == NULL)
	    goto quit;
	if ((plugins = realloc(plugins, (nplugins+1) * sizeof (*plugins))) == NULL) {
	    clicon_err(OE_UNIX, errno, "realloc");
	    goto quit;
	}
	plugins[nplugins++] = handle;
    }
    retval = 0;
quit:
    if (dp)
	free(dp);
    return retval;
}

/*! Unload all netconf plugins */
int
netconf_plugin_unload(clicon_handle h)
{
    int i;
    netconf_reg_t *nr;

    while((nr = deps) != NULL) {
	DELQ(nr, deps, netconf_reg_t *);
	if (nr->nr_tag)
	    free(nr->nr_tag);
	free(nr);
    }
    for (i = 0; i < nplugins; i++) 
	plugin_unload(h, plugins[i]);
    if (plugins){
	free(plugins);
	plugins = NULL;
    }
    nplugins = 0;
    return 0;
}

/*! Call plugin_start in all plugins
 */
int
netconf_plugin_start(clicon_handle h, int argc, char **argv)
{
    int i;
    plgstart_t *startfn;

    for (i = 0; i < nplugins; i++) {
	/* Call exit function is it exists */
	if ((startfn = dlsym(plugins[i], PLUGIN_START)) == NULL)
	    break;
	optind = 0;
	if (startfn(h, argc, argv) < 0) {
	    clicon_debug(1, "plugin_start() failed\n");
	    return -1;
	}
    }
    return 0;
}


/*! Register netconf callback
 * Called from plugin to register a callback for a specific netconf XML tag.
 */
int
netconf_register_callback(clicon_handle h,
			  netconf_cb_t cb,      /* Callback called */
			  void *arg,        /* Arg to send to callback */
			  char *tag)        /* Xml tag when callback is made */
{
    netconf_reg_t *nr;

    if ((nr = malloc(sizeof(netconf_reg_t))) == NULL) {
	clicon_err(OE_DB, errno, "malloc: %s", strerror(errno));
	goto catch;
    }
    memset (nr, 0, sizeof (*nr));
    nr->nr_callback = cb;
    nr->nr_arg  = arg;
    nr->nr_tag  = strdup(tag); /* strdup */
    INSQ(nr, deps);
    return 0;
catch:
    if (nr){
	if (nr->nr_tag)
	    free(nr->nr_tag);
	free(nr);
    }
    return -1;
}
    
/*! See if there is any callback registered for this tag
 *
 * @param[in]  h       clicon handle
 * @param[in]  xn      Sub-tree (under xorig) at child of rpc: <rpc><xn></rpc>.
 * @param[out] xret    Return XML, error or OK
 *
 * @retval -1   Error
 * @retval  0   OK, not found handler.
 * @retval  1   OK, handler called
 */
int
netconf_plugin_callbacks(clicon_handle h,
			 cxobj        *xn, 
			 cxobj       **xret)
{
    netconf_reg_t *nreg;
    int            retval;

    if (deps == NULL)
	return 0;
    nreg = deps;
    do {
	if (strcmp(nreg->nr_tag, xml_name(xn)) == 0){
	    if ((retval = nreg->nr_callback(h, xn, xret, nreg->nr_arg)) < 0)
		return -1;
	    else
		return 1; /* handled */
	}
	nreg = NEXTQ(netconf_reg_t *, nreg);
    } while (nreg != deps);
    return 0;
}
    
