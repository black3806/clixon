/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC

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
 * Client-side functions for clicon_proto protocol
 * Historically this code was part of the clicon_cli application. But
 * it should (is?) be general enough to be used by other applications.
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_yang_module.h"
#include "clixon_plugin.h"
#include "clixon_string.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_proto.h"
#include "clixon_err.h"
#include "clixon_err_string.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_map.h"
#include "clixon_xml_sort.h"
#include "clixon_netconf_lib.h"
#include "clixon_proto_client.h"

/*! Send internal netconf rpc from client to backend
 * @param[in]    h      CLICON handle
 * @param[in]    msg    Encoded message. Deallocate woth free
 * @param[out]   xret0  Return value from backend as xml tree. Free w xml_free
 * @param[inout] sock0  If pointer exists, do not close socket to backend on success 
 *                      and return it here. For keeping a notify socket open
 * @note sock0 is if connection should be persistent, like a notification/subscribe api
 * @note xret is populated with yangspec according to standard handle yangspec
 */
int
clicon_rpc_msg(clicon_handle      h, 
	       struct clicon_msg *msg, 
	       cxobj            **xret0,
	       int               *sock0)
{
    int                retval = -1;
    char              *sock;
    int                port;
    char              *retdata = NULL;
    cxobj             *xret = NULL;

#ifdef RPC_USERNAME_ASSERT
    assert(strstr(msg->op_body, "username")!=NULL); /* XXX */
#endif
    clicon_debug(1, "%s request:%s", __FUNCTION__, msg->op_body);
    if ((sock = clicon_sock(h)) == NULL){
	clicon_err(OE_FATAL, 0, "CLICON_SOCK option not set");
	goto done;
    }
    /* What to do if inet socket? */
    switch (clicon_sock_family(h)){
    case AF_UNIX:
	if (clicon_rpc_connect_unix(h, msg, sock, &retdata, sock0) < 0){
#if 0
	    if (errno == ESHUTDOWN)
		/* Maybe could reconnect on a higher layer, but lets fail
		   loud and proud */
		cligen_exiting_set(cli_cligen(h), 1);
#endif
	    goto done;
	}
	break;
    case AF_INET:
	if ((port = clicon_sock_port(h)) < 0){
	    clicon_err(OE_FATAL, 0, "CLICON_SOCK option not set");
	    goto done;
	}
	if (port < 0){
	    clicon_err(OE_FATAL, 0, "CLICON_SOCK_PORT not set");
	    goto done;
	}
	if (clicon_rpc_connect_inet(h, msg, sock, port, &retdata, sock0) < 0)
	    goto done;
	break;
    }
    clicon_debug(1, "%s retdata:%s", __FUNCTION__, retdata);

    if (retdata){
	/* Cannot populate xret here because need to know RPC name (eg "lock") in order to associate yang
	 * to reply.
	 */
	if (xml_parse_string2(retdata, YB_NONE, NULL, &xret, NULL) < 0)
	    goto done;
    }
    if (xret0){
	*xret0 = xret;
	xret = NULL;
    }
    retval = 0;
 done:
    if (retdata)
	free(retdata);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Check if there is a valid (cached) session-id. If not, send a hello request to backend 
 * Session-ids survive TCP sessions that are created for each message sent to the backend.
 * Clients use two approaches, either:
 * (1) Once at the beginning of the session. Netconf and restconf does this
 * (2) First usage, ie "lazy" evaluation when first needed
 * @param[in]  h           clicon handle
 * @param[out] session_id  Session id
 * @retval     0           OK and session_id set
 * @retval    -1           Error
 */
static int
session_id_check(clicon_handle h,
		 uint32_t     *session_id)
{
    int      retval = -1;
    uint32_t id;
    
    if (clicon_session_id_get(h, &id) < 0){ /* Not set yet */
	if (clicon_hello_req(h, &id) < 0)
	    goto done;
	clicon_session_id_set(h, id); 
    }
    retval = 0;
    *session_id = id;
 done:
    return retval;
}

/*! Generic xml netconf clicon rpc
 * Want to go over to use netconf directly between client and server,...
 * @param[in]  h       clicon handle
 * @param[in]  xmlstr  XML netconf tree as string
 * @param[out] xret    Return XML netconf tree, error or OK (need to be freed)
 * @param[out] sp      Socket pointer for notification, otherwise NULL
 * @code
 *   cxobj *xret = NULL;
 *   if (clicon_rpc_netconf(h, "<rpc></rpc>", &xret, NULL) < 0)
 *	err;
 *   xml_free(xret);
 * @endcode
 * @see clicon_rpc_netconf_xml xml as tree instead of string
 */
int
clicon_rpc_netconf(clicon_handle  h, 
		   char          *xmlstr,
		   cxobj        **xret,
		   int           *sp)
{
    int                retval = -1;
    uint32_t           session_id;
    struct clicon_msg *msg = NULL;

    if (session_id_check(h, &session_id) < 0)
	goto done;
    if ((msg = clicon_msg_encode(session_id, "%s", xmlstr)) < 0)
	goto done;
    if (clicon_rpc_msg(h, msg, xret, sp) < 0)
	goto done;
    retval = 0;
 done:
    if (msg)
	free(msg);
    return retval;
}

/*! Generic xml netconf clicon rpc
 * Want to go over to use netconf directly between client and server,...
 * @param[in]  h       clicon handle
 * @param[in]  xml     XML netconf tree 
 * @param[out] xret    Return XML netconf tree, error or OK
 * @param[out] sp      Socket pointer for notification, otherwise NULL
 * @code
 *   cxobj *xret = NULL;
 *   int    s; 
 *   if (clicon_rpc_netconf_xml(h, x, &xret, &s) < 0)
 *	err;
 *   xml_free(xret);
 * @endcode

 * @see clicon_rpc_netconf xml as string instead of tree
 */
int
clicon_rpc_netconf_xml(clicon_handle  h, 
		       cxobj         *xml,
		       cxobj        **xret,
		       int           *sp)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cxobj     *xname;
    char      *rpcname;
    cxobj     *xreply;
    yang_stmt *yspec;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if ((xname = xml_child_i_type(xml, 0, 0)) == NULL){
	clicon_err(OE_NETCONF, EINVAL, "Missing rpc name");
	goto done;
    }
    rpcname = xml_name(xname); /* Store rpc name and use in yang binding after reply */
    if (clicon_xml2cbuf(cb, xml, 0, 0, -1) < 0)
	goto done;
    if (clicon_rpc_netconf(h, cbuf_get(cb), xret, sp) < 0)
	goto done;
    if ((xreply = xml_find_type(*xret, NULL, "rpc-reply", CX_ELMNT)) != NULL &&
	xml_find_type(xreply, NULL, "rpc-error", CX_ELMNT) == NULL){
	yspec = clicon_dbspec_yang(h);
	/* Here use rpc name to bind to yang */
	if (xml_spec_populate_rpc_reply(xreply, rpcname, yspec, NULL) < 0) 
	    goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Generate clicon error from Netconf error message
 *
 * Get a text error message from netconf error message and generate error on the form:
 *   <msg>: "<arg>": <netconf-error>   or   <msg>: <netconf-error>
 * @param[in]  xerr    Netconf error xml tree on the form: <rpc-error> 
 * @param[in]  format  Format string 
 * @param[in]  arg     String argument to format (optional)
 */
int
clicon_rpc_generate_error(cxobj       *xerr,
			  const char  *msg,
			  const char  *arg)
{
    int    retval = -1;
    cbuf  *cb = NULL;

    if ((cb = cbuf_new()) ==NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (netconf_err2cb(xerr, cb) < 0)
	goto done;
    cprintf(cb, ". %s", msg);
    if (arg)
	cprintf(cb, " \"%s\" ", arg);
    clicon_err(OE_NETCONF, 0, "%s", cbuf_get(cb));
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Get database configuration
 * Same as clicon_proto_change just with a cvec instead of lvec
 * @param[in]  h        CLICON handle
 * @param[in]  username If NULL, use default
 * @param[in]  db       Name of database
 * @param[in]  xpath    XPath (or "")
 * @param[in]  nsc       Namespace context for filter
 * @param[out] xt       XML tree. Free with xml_free. 
 *                      Either <config> or <rpc-error>. 
 * @retval    0         OK
 * @retval   -1         Error, fatal or xml
 * @code
 *   cxobj *xt = NULL;
 *   cvec *nsc = NULL;
 *
 *   if ((nsc = xml_nsctx_init(NULL, "urn:example:hello")) == NULL)
 *       err;
 *   if (clicon_rpc_get_config(h, NULL, "running", "/hello/world", nsc, &xt) < 0)
 *       err;
 *   if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
 *	clicon_rpc_generate_error(xerr, "msg", "/hello/world");
 *      err;
 *   }
 *   if (xt)
 *      xml_free(xt);
 *  if (nsc)
 *     xml_nsctx_free(nsc);
 * @endcode
 * @see clicon_rpc_get
 * @see clicon_rpc_generate_error
 * @note the netconf return message us yang populated, but returned data is not
 */
int
clicon_rpc_get_config(clicon_handle h, 
		      char         *username,
		      char         *db, 
		      char         *xpath,
		      cvec         *nsc,
		      cxobj       **xt)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    cxobj             *xret = NULL;
    cxobj             *xd;
    cg_var            *cv = NULL;
    char              *prefix;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "<rpc");
    if (username == NULL)
	username = clicon_username_get(h);
    if (username != NULL)
	cprintf(cb, " username=\"%s\"", username);
    cprintf(cb, " xmlns:%s=\"%s\"",
	    NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, "><get-config><source><%s/></source>", db);
    if (xpath && strlen(xpath)){
	cprintf(cb, "<%s:filter %s:type=\"xpath\" %s:select=\"%s\"",
		NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX,
		xpath);
	while ((cv = cvec_each(nsc, cv)) != NULL){
	    cprintf(cb, " xmlns");
	    if ((prefix = cv_name_get(cv)))
		cprintf(cb, ":%s", prefix);
	    cprintf(cb, "=\"%s\"", cv_string_get(cv));
	}
	cprintf(cb, "/>");
    }
    cprintf(cb, "</get-config></rpc>");
    if ((msg = clicon_msg_encode(session_id, "%s", cbuf_get(cb))) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    /* Send xml error back: first check error, then ok */
    if ((xd = xpath_first(xret, NULL, "/rpc-reply/rpc-error")) != NULL)
	xd = xml_parent(xd); /* point to rpc-reply */
    else if ((xd = xpath_first(xret, NULL, "/rpc-reply/data")) == NULL)
	if ((xd = xml_new("data", NULL, NULL)) == NULL)
	    goto done;
    if (xt){
	if (xml_rm(xd) < 0)
	    goto done;
	*xt = xd;
    }
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send database entries as XML to backend daemon
 * @param[in] h          CLICON handle
 * @param[in] db         Name of database
 * @param[in] op         Operation on database item: OP_MERGE, OP_REPLACE
 * @param[in] xml        XML string. Ex: <config><a>..</a><b>...</b></config>
 * @retval    0          OK
 * @retval   -1          Error and logged to syslog
 * @note xml arg need to have <config> as top element
 * @code
 * if (clicon_rpc_edit_config(h, "running", OP_MERGE, 
 *                            "<config><a>4</a></config>") < 0)
 *    err;
 * @endcode
 */
int
clicon_rpc_edit_config(clicon_handle       h, 
		       char               *db, 
		       enum operation_type op,
		       char               *xmlstr)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " xmlns:%s=\"%s\"", NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    if ((username = clicon_username_get(h)) != NULL)
	cprintf(cb, " username=\"%s\"", username);
    cprintf(cb, "><edit-config><target><%s/></target>", db);
    cprintf(cb, "<default-operation>%s</default-operation>", 
	    xml_operation2str(op));
    if (xmlstr)
	cprintf(cb, "%s", xmlstr);
    cprintf(cb, "</edit-config></rpc>");
    if ((msg = clicon_msg_encode(session_id, "%s", cbuf_get(cb))) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Editing configuration", NULL);
	goto done;
    }
    retval = 0;
  done:
    if (xret)
	xml_free(xret);
    if (cb)
	cbuf_free(cb);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a request to backend to copy a file from one location to another 
 * Note this assumes the backend can access these files and (usually) assumes
 * clients and servers have the access to the same filesystem.
 * @param[in] h        CLICON handle
 * @param[in] db1      src database, eg "running"
 * @param[in] db2      dst database, eg "startup"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 * @code
 * if (clicon_rpc_copy_config(h, "running", "startup") < 0)
 *    err;
 * @endcode
 */
int
clicon_rpc_copy_config(clicon_handle h, 
		       char         *db1, 
		       char         *db2)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
   "<rpc username=\"%s\"><copy-config><source><%s/></source><target><%s/></target></copy-config></rpc>",
				 username?username:"",
				 db1, db2)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Copying configuration", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a request to backend to delete a config database
 * @param[in] h        CLICON handle
 * @param[in] db       database, eg "running"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 * @code
 * if (clicon_rpc_delete_config(h, "startup") < 0)
 *    err;
 * @endcode
 */
int
clicon_rpc_delete_config(clicon_handle h, 
			 char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc username=\"%s\"><edit-config><target><%s/></target><default-operation>none</default-operation><config operation=\"delete\"/></edit-config></rpc>",
				 username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Deleting configuration", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Lock a database
 * @param[in] h        CLICON handle
 * @param[in] db       database, eg "running"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_lock(clicon_handle h, 
		char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc username=\"%s\"><lock><target><%s/></target></lock></rpc>",
				 username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Locking configuration", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Unlock a database
 * @param[in] h        CLICON handle
 * @param[in] db       database, eg "running"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_unlock(clicon_handle h, 
		  char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc username=\"%s\"><unlock><target><%s/></target></unlock></rpc>", username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Configuration unlock", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Get database configuration and state data
 * @param[in]  h         Clicon handle
 * @param[in]  xpath     XPath in a filter stmt (or NULL/"" for no filter)
 * @param[in]  namespace Namespace associated w xpath
 * @param[in]  nsc       Namespace context for filter
 * @param[in]  content   Clixon extension: all, config, noconfig. -1 means all
 * @param[in]  depth     Nr of XML levels to get, -1 is all, 0 is none
 * @param[out] xt        XML tree. Free with xml_free. 
 *                       Either <config> or <rpc-error>. 
 * @retval    0          OK
 * @retval   -1          Error, fatal or xml
 * @note if xpath is set but namespace is NULL, the default, netconf base 
 *       namespace will be used which is most probably wrong.
 * @code
 *  cxobj *xt = NULL;
 *  cvec *nsc = NULL;
 *
 *  if ((nsc = xml_nsctx_init(NULL, "urn:example:hello")) == NULL)
 *     err;
 *  if (clicon_rpc_get(h, "/hello/world", nsc, CONTENT_ALL, -1, &xt) < 0)
 *     err;
 *  if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
 *     clicon_rpc_generate_error(xerr, "clicon_rpc_get", NULL);
 *     err;
 *  }
 *  if (xt)
 *     xml_free(xt);
 *  if (nsc)
 *     xml_nsctx_free(nsc);
 * @endcode
 * @see clicon_rpc_get_config which is almost the same as with content=config, but you can also select dbname
 * @see clicon_rpc_generate_error
 * @note the netconf return message us yang populated, but returned data is not
 */
int
clicon_rpc_get(clicon_handle   h, 
	       char           *xpath,
	       cvec           *nsc, /* namespace context for filter */
	       netconf_content content,
	       int32_t         depth,
	       cxobj         **xt)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    cxobj             *xret = NULL;
    cxobj             *xd;
    char              *username;
    cg_var            *cv = NULL;
    char              *prefix;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "<rpc");
    if ((username = clicon_username_get(h)) != NULL)
	cprintf(cb, " username=\"%s\"", username);
    cprintf(cb, " xmlns:%s=\"%s\"",
	    NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, "><get");
    /* Clixon extension, content=all,config, or nonconfig */
    if ((int)content != -1)
	cprintf(cb, " content=\"%s\"", netconf_content_int2str(content));
    /* Clixon extension, depth=<level> */
    if (depth != -1)
	cprintf(cb, " depth=\"%d\"", depth);
    cprintf(cb, ">");
    if (xpath && strlen(xpath)) {
	cprintf(cb, "<%s:filter %s:type=\"xpath\" %s:select=\"%s\"",
		NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX,
		xpath);

	while ((cv = cvec_each(nsc, cv)) != NULL){
	    cprintf(cb, " xmlns");
	    if ((prefix = cv_name_get(cv)))
		cprintf(cb, ":%s", prefix);
	    cprintf(cb, "=\"%s\"", cv_string_get(cv));
	}
	cprintf(cb, "/>");
    }
    cprintf(cb, "</get></rpc>");
    if ((msg = clicon_msg_encode(session_id,
				 "%s", cbuf_get(cb))) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    /* Send xml error back: first check error, then ok */
    if ((xd = xpath_first(xret, NULL, "/rpc-reply/rpc-error")) != NULL)
	xd = xml_parent(xd); /* point to rpc-reply */
    else if ((xd = xpath_first(xret, NULL, "/rpc-reply/data")) == NULL)
	if ((xd = xml_new("data", NULL, NULL)) == NULL)
	    goto done;
    if (xt){
	if (xml_rm(xd) < 0)
	    goto done;
	*xt = xd;
    }
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}


/*! Close a (user) session
 * @param[in] h        CLICON handle
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_close_session(clicon_handle h)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc username=\"%s\"><close-session/></rpc>",
				 username?username:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Close session", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Kill other user sessions
 * @param[in] h           CLICON handle
 * @param[in] session_id  Session id of other user session
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_kill_session(clicon_handle h,
			uint32_t      session_id)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           my_session_id; /* Not the one to kill */
    
    if (session_id_check(h, &my_session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(my_session_id,
				 "<rpc username=\"%s\"><kill-session><session-id>%u</session-id></kill-session></rpc>",
				 username?username:"", session_id)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Kill session", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send validate request to backend daemon
 * @param[in] h        CLICON handle
 * @param[in] db       Name of database
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_validate(clicon_handle h, 
		    char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc username=\"%s\"><validate><source><%s/></source></validate></rpc>", username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, CLIXON_ERRSTR_VALIDATE_FAILED, NULL);
	goto done;	
    }
    retval = 0;
 done:
    if (msg)
	free(msg);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Commit changes send a commit request to backend daemon
 * @param[in] h          CLICON handle
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_commit(clicon_handle h)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc username=\"%s\"><commit/></rpc>", username?username:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, CLIXON_ERRSTR_COMMIT_FAILED, NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Discard all changes in candidate / revert to running
 * @param[in] h        CLICON handle
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_discard_changes(clicon_handle h)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc username=\"%s\"><discard-changes/></rpc>", username?username:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Discard changes", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Create a new notification subscription
 * @param[in]   h        Clicon handle
 * @param{in]   stream   name of notificatio/log stream (CLICON is predefined)
 * @param{in]   filter   message filter, eg xpath for xml notifications
 * @param[out]  s0       socket returned where notification mesages will appear
 * @retval      0        OK
 * @retval      -1       Error and logged to syslog

 * @note When using netconf create-subsrciption,status and format is not supported
 */
int
clicon_rpc_create_subscription(clicon_handle    h,
			       char            *stream, 
			       char            *filter, 
			       int             *s0)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc username=\"%s\"><create-subscription xmlns=\"urn:ietf:params:xml:ns:netmod:notification\">"
				 "<stream>%s</stream>"
				 "<filter type=\"xpath\" select=\"%s\" />"
				 "</create-subscription></rpc>", 
				 username?username:"",
				 stream?stream:"", filter?filter:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, s0) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Create subscription", NULL);
	goto done;
    }
    retval = 0;
  done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a debug request to backend server
 * @param[in] h        CLICON handle
 * @param[in] level    Debug level
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_debug(clicon_handle h, 
		int           level)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc username=\"%s\"><debug xmlns=\"http://clicon.org/lib\"><level>%d</level></debug></rpc>", username?username:"", level)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Debug", NULL);
	goto done;
    }
    if (xpath_first(xret, NULL, "//rpc-reply/ok") == NULL){
	clicon_err(OE_XML, 0, "rpc error"); /* XXX extract info from rpc-error */
	goto done;
    }
    retval = 0;
 done:
    if (msg)
	free(msg);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Send a debug request to backend server
 * @param[in] h        CLICON handle
 * @param[in] level    Debug level
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_hello_req(clicon_handle h,
		 uint32_t     *id)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    cxobj             *x;
    char              *username;
    char              *b;
    int                ret;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(0, "<hello username=\"%s\" xmlns=\"%s\"><capabilities><capability>urn:ietf:params:netconf:base:1.0</capability></capabilities></hello>",
				 username?username:"",
				 NETCONF_BASE_NAMESPACE)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(xerr, "Hello", NULL);
	goto done;
    }
    if ((x = xpath_first(xret, NULL, "hello/session-id")) == NULL){
	clicon_err(OE_XML, 0, "hello session-id");
	goto done;
    }
    b = xml_body(x);
    if ((ret = parse_uint32(b, id, NULL)) <= 0){
	clicon_err(OE_XML, errno, "parse_uint32"); 
	goto done;
    }
    retval = 0;
 done:
    if (msg)
	free(msg);
    if (xret)
	xml_free(xret);
    return retval;
}


