/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
  
 */

/*
 * This program should be run as user www-data 
 *
 * See draft-ietf-netconf-restconf-13.txt [draft]

 * sudo apt-get install libfcgi-dev
 * gcc -o fastcgi fastcgi.c -lfcgi

 * sudo su -c "/www-data/clixon_restconf -D 1 -f /usr/local/etc/example.xml " -s /bin/sh www-data

 * This is the interface:
 * api/data/profile=<name>/metric=<name> PUT data:enable=<flag>
 * api/test
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <pwd.h>
#include <time.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <libgen.h>
#include <sys/stat.h> /* chmod */

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

#include <fcgiapp.h> /* Need to be after clixon_xml.h due to attribute format */

/* restconf */
#include "restconf_lib.h"      /* generic shared with plugins */
#include "restconf_handle.h"
#include "restconf_api.h"      /* generic not shared with plugins */
#include "restconf_err.h"
#include "restconf_root.h"     /* generic not shared with plugins */
#include "restconf_methods.h"  /* fcgi specific */
#include "restconf_methods_get.h"
#include "restconf_methods_post.h"
#include "restconf_stream.h"

/* Command line options to be passed to getopt(3) */
#define RESTCONF_OPTS "hVD:f:E:l:C:p:d:y:a:u:rW:R:o:"

/*! Convert FCGI parameters to clixon runtime data
 *
 * @param[in]  h     Clixon handle
 * @param[in]  envp  Fastcgi request handle parameter array on the format "<param>=<value>"
 * @retval     0    OK
 * @retval    -1    Error
 * @see https://nginx.org/en/docs/http/ngx_http_core_module.html#var_https
 */
static int
fcgi_params_set(clixon_handle h,
                           char        **envp)
{
    int   retval = -1;
    int   i;
    char *param = NULL;
    char *val = NULL;

    clixon_debug(CLIXON_DBG_RESTCONF, "");
    for (i = 0; envp[i] != NULL; i++){ /* on the form <param>=<value> */
        if (clixon_strsplit(envp[i], '=', &param, &val) < 0)
            goto done;
        if (restconf_param_set(h, param, val) < 0)
            goto done;
        if (param){
            free(param);
            param = NULL;
        }
        if (val){
            free(val);
            val = NULL;
        }
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_RESTCONF, "retval:%d", retval);
    return retval;
}

/*! Try to get config: inline, config-file or query backend
 */
static int
restconf_main_config(clixon_handle h,
                     yang_stmt    *yspec,
                     const char   *inline_config)
{
    int            retval = -1;
    struct passwd *pw;
    cxobj         *xconfig = NULL;
    cxobj         *xrestconf = NULL;
    uint32_t       id = 0;
    cxobj         *xerr = NULL;
    int            configure_done = 0; /* First try local then backend */
    cvec          *nsc = NULL;
    int            ret;

    /* 1. try inline configure option */
    if (inline_config != NULL && strlen(inline_config)){
        clixon_debug(CLIXON_DBG_RESTCONF, "restconf_main_fcgi using restconf inline config");
        if ((ret = clixon_xml_parse_string(inline_config, YB_MODULE, yspec, &xrestconf, &xerr)) < 0)
            goto done;
        if (ret == 0){
            if (clixon_err_netconf(h, OE_NETCONF, 0, xerr, "Inline restconf config") < 0)
                goto done;
            goto done;
        }
        /* Replace parent w first child */
        if (xml_rootchild(xrestconf, 0, &xrestconf) < 0)
            goto done;
    }
    else if (clicon_option_bool(h, "CLICON_BACKEND_RESTCONF_PROCESS") == 0){
        /* 2. If not read from backend, try to get restconf config from local config-file */
        xrestconf = clicon_conf_restconf(h);
    }
    /* 3. If no local config, or it is disabled, try to query backend of config. */
    else {
        /* Loop to wait for backend starting, try again if not done */
        while (1){
            if (clicon_hello_req(h, "cl:restconf", NULL, &id) < 0){
                if (errno == ENOENT){
                    fprintf(stderr, "waiting");
                    sleep(1);
                    continue;
                }
                clixon_err(OE_UNIX, errno, "clicon_session_id_get");
                goto done;
            }
            clicon_session_id_set(h, id);
            break;
        }
        if ((nsc = xml_nsctx_init(NULL, CLIXON_RESTCONF_NS)) == NULL)
            goto done;
        if ((pw = getpwuid(getuid())) == NULL){
            clixon_err(OE_UNIX, errno, "getpwuid");
            goto done;
        }
        if (clicon_rpc_get_config(h, pw->pw_name, "running", "/restconf", nsc, NULL, &xconfig) < 0)
            goto done;
        if ((xerr = xpath_first(xconfig, NULL, "/rpc-error")) != NULL){
            clixon_err_netconf(h, OE_NETCONF, 0, xerr, "Get backend restconf config");
            goto done;
        }
        /* Extract restconf configuration */
        xrestconf = xpath_first(xconfig, nsc, "restconf");
    }
    configure_done = 0;
    if (xrestconf != NULL &&
        (configure_done = restconf_config_init(h, xrestconf)) < 0)
        goto done;
    if (!configure_done){     /* Query backend of config. */
        clixon_err(OE_DAEMON, EFAULT, "Restconf daemon config not found or disabled");
        goto done;
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (inline_config != NULL && strlen(inline_config) && xrestconf)
        xml_free(xrestconf);
    if (xconfig)
        xml_free(xconfig);
    return retval;
}

/* XXX Need global variable to for SIGCHLD signal handler
*/
static clixon_handle _CLIXON_HANDLE = NULL;

/* XXX Need global variable to break FCGI accept loop from signal handler see FCGX_Accept_r(req)
 */
static int _MYSOCK;

/*! Signal terminates process
 */
static void
restconf_sig_term(int arg)
{
    static int i=0;

    clixon_debug(CLIXON_DBG_RESTCONF, "");
    if (i++ == 0)
        clixon_log(NULL, LOG_NOTICE, "%s: %s: pid: %u Signal %d",
                   __PROGRAM__, __FUNCTION__, getpid(), arg);
    else{
        clixon_debug(CLIXON_DBG_RESTCONF, "done");
        exit(-1);
    }

    /* This should ensure no more accepts or incoming packets are processed because next time eventloop
     * is entered, it will terminate.
     * However there may be a case of sockets closing rather abruptly for clients
     */
    clixon_exit_set(1);
    close(_MYSOCK);
}

/*! Reap stream child
 *
 * XXX The -1 should be changed to proper pid, see eg clixon_process_waitpid
 */
static void
restconf_sig_child(int arg)
{
    int status;
    int pid;

    if ((pid = waitpid(-1, &status, 0)) != -1 && WIFEXITED(status))
        stream_child_free(_CLIXON_HANDLE, pid);
}

/*! Usage help routine
 *
 * @param[in]  h      Clixon handle
 * @param[in]  argv0  command line
 */
static void
usage(clixon_handle h,
      char         *argv0)
{
    fprintf(stderr, "usage:%s [options]\n"
            "where options are\n"
            "\t-h \t\t  Help\n"
            "\t-V \t\tPrint version and exit\n"
            "\t-D <level>\tDebug level (see available levels below)\n"
            "\t-f <file>\t  Configuration file (mandatory)\n"
            "\t-E <dir> \t  Extra configuration file directory\n"
            "\t-l <s|e|o|n|f<file>> \tLog on (s)yslog, std(e)rr, std(o)ut, (n)one or (f)ile (syslog is default)\n"
            "\t-C <format>\tDump configuration options on stdout after loading. Format is xml|json|text\n"
            "\t-p <dir>\t  Yang directory path (see CLICON_YANG_DIR)\n"
            "\t-y <file>\t  Load yang spec file (override yang main module)\n"
            "\t-a UNIX|IPv4|IPv6 Internal backend socket family\n"

            "\t-u <path|addr>\t  Internal socket domain path or IP addr (see -a)\n"
            "\t-r \t\t  Do not drop privileges if run as root\n"
            "\t-W <user>\t  Run restconf daemon as this user, drop according to CLICON_RESTCONF_PRIVILEGES\n"
            "\t-R <xml> \t  Restconf configuration in-line overriding config file\n"
            "\t-o \"<option>=<value>\" Give configuration option overriding config file (see clixon-config.yang)\n",
            argv0
            );
    fprintf(stderr, "Debug keys: ");
    clixon_debug_key_dump(stderr);
    fprintf(stderr, "\n");
    exit(0);
}

/*! Main routine for fastcgi restconf */
int
main(int    argc,
     char **argv)
{
    int            retval = -1;
    int            sock;
    char          *argv0 = argv[0];
    FCGX_Request   request;
    FCGX_Request  *req = &request;
    int            c;
    char          *sockpath = NULL;
    char          *path;
    clixon_handle  h;
    char          *dir;
    int            logdst = CLIXON_LOG_SYSLOG;
    yang_stmt     *yspec = NULL;
    char          *query;
    cvec          *qvec;
    int            finish = 0;
    char          *str;
    clixon_plugin_t *cp = NULL;
    cvec          *nsctx_global = NULL; /* Global namespace context */
    size_t         cligen_buflen;
    size_t         cligen_bufthreshold;
    int            dbg = 0;
    cxobj         *xerr = NULL;
    char          *wwwuser;
    char          *inline_config = NULL;
    size_t         sz;
    int           config_dump = 0;
    enum format_enum config_dump_format = FORMAT_XML;
    int              print_version = 0;

    /* Create handle */
    if ((h = restconf_handle_init()) == NULL)
        goto done;
    /* In the startup, logs to stderr & debug flag set later */
    if (clixon_log_init(h, __PROGRAM__, LOG_INFO, logdst) < 0)
        goto done;
    if (clixon_err_init(h) < 0)
        goto done;

    _CLIXON_HANDLE = h; /* for termination handling */

    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
        switch (c) {
        case 'h':
            usage(h, argv[0]);
            break;
        case 'V':
            cligen_output(stdout, "Clixon version: %s\n", CLIXON_VERSION_STRING);
            print_version++; /* plugins may also print versions w ca-version callback */
            break;
        case 'D' : { /* debug */
            int d = 0;
            /* Try first symbolic, then numeric match */
            if ((d = clixon_debug_str2key(optarg)) < 0 &&
                sscanf(optarg, "%d", &d) != 1){
                usage(h, argv[0]);
            }
            dbg |= d;
            break;
        }
        case 'f': /* override config file */
            if (!strlen(optarg))
                usage(h, argv[0]);
            clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
            break;
        case 'E': /* extra config directory */
            if (!strlen(optarg))
                usage(h, argv[0]);
            clicon_option_str_set(h, "CLICON_CONFIGDIR", optarg);
            break;
        case 'l': /* Log destination: s|e|o */
            if ((logdst = clixon_log_opt(optarg[0])) < 0)
                usage(h, argv[0]);
            if (logdst == CLIXON_LOG_FILE &&
                strlen(optarg)>1 &&
                clixon_log_file(optarg+1) < 0)
                goto done;
            break;
        } /* switch getopt */

    /*
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clixon_log_init(h, __PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst);

    clixon_debug_init(h, dbg);
    clixon_log(h, LOG_NOTICE, "%s fcgi: %u Started", __PROGRAM__, getpid());
    if (set_signal(SIGTERM, restconf_sig_term, NULL) < 0){
        clixon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }
    if (set_signal(SIGINT, restconf_sig_term, NULL) < 0){
        clixon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }
    if (set_signal(SIGCHLD, restconf_sig_child, NULL) < 0){
        clixon_err(OE_DAEMON, errno, "Setting signal");
        goto done;
    }

    yang_init(h);
    /* Find and read configfile */
    if (clicon_options_main(h) < 0)
        goto done;

    /* Now rest of options, some overwrite option file */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
        switch (c) {
        case 'h' : /* help */
        case 'V' : /* version */
        case 'D' : /* debug */
        case 'f':  /* config file */
        case 'E':  /* extra config dir */
        case 'l':  /* log  */
            break; /* taken care of in earlier getopt above */
        case 'C': /* Explicitly dump configuration */
            if ((config_dump_format = format_str2int(optarg)) ==  (enum format_enum)-1){
                fprintf(stderr, "Unrecognized dump format: %s(expected: xml|json|text)\n", argv[0]);
                usage(h, argv[0]);
            }
            config_dump++;
            break;
        case 'p' : /* yang dir path */
            if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
                goto done;
            break;
        case 'y' : /* Load yang spec file (override yang main module) */
            clicon_option_str_set(h, "CLICON_YANG_MAIN_FILE", optarg);
            break;
        case 'a': /* internal backend socket address family */
            clicon_option_str_set(h, "CLICON_SOCK_FAMILY", optarg);
            break;
        case 'u': /* internal backend socket unix domain path or ip host */
            if (!strlen(optarg))
                usage(h, argv[0]);
            clicon_option_str_set(h, "CLICON_SOCK", optarg);
            break;
        case 'r':{ /* Do not drop privileges if run as root */
            if (clicon_option_add(h, "CLICON_RESTCONF_PRIVILEGES", "none") < 0)
                goto done;
            break;
        }
        case 'R':  /* Restconf on-line config */
            inline_config = optarg;
            break;
        case 'o':{ /* Configuration option */
            char          *val;
            if ((val = index(optarg, '=')) == NULL)
                usage(h, argv0);
            *val++ = '\0';
            if (clicon_option_add(h, optarg, val) < 0)
                goto done;
            break;
        }
        default:
            usage(h, argv[0]);
            break;
        }
    argc -= optind;
    argv += optind;

    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);

    /* Init restconf auth-type */
    restconf_auth_type_set(h, CLIXON_AUTH_NONE);

    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    if ((sz = clicon_option_int(h, "CLIXON_LOG_STRING_LIMIT")) != 0)
        clixon_log_string_limit_set(sz);

    /* Set default namespace according to CLICON_NAMESPACE_NETCONF_DEFAULT */
    xml_nsctx_namespace_netconf_default(h);

    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
        goto done;

    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
        goto done;
    clicon_dbspec_yang_set(h, yspec);

    /* Initialize plugin module by creating a handle holding plugin and callback lists */
    if (clixon_plugin_module_init(h) < 0)
        goto done;
    /* In case ietf-yang-metadata is loaded by application, handle annotation extension */
    if (yang_metadata_init(h) < 0)
        goto done;
    /* Load restconf plugins before yangs are loaded (eg extension callbacks) */
    if ((dir = clicon_restconf_dir(h)) != NULL)
        if (clixon_plugins_load(h, CLIXON_PLUGIN_INIT, dir, NULL) < 0)
            return -1;
    /* Print version, customized variant must wait for plugins to load */
    if (print_version){
        if (clixon_plugin_version_all(h, stdout) < 0)
            goto done;
        exit(0);
    }
    /* Create a pseudo-plugin to create extension callback to set the ietf-routing
     * yang-data extension for api-root top-level restconf function.
     */
    if (clixon_pseudo_plugin(h, "pseudo restconf", &cp) < 0)
        goto done;
    clixon_plugin_api_get(cp)->ca_extension = restconf_main_extension_cb;

    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
        if (yang_spec_parse_file(h, str, yspec) < 0)
            goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
        if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
                                   yspec) < 0)
            goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
        if (yang_spec_load_dir(h, str, yspec) < 0)
            goto done;
    }

    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
        goto done;
    /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
        goto done;

    /* Load yang restconf module */
    if (yang_spec_parse_module(h, "ietf-restconf", NULL, yspec)< 0)
        goto done;

#ifdef CLIXON_YANG_PATCH
    /* Load yang restconf patch module */
    if (yang_spec_parse_module(h, "ietf-yang-patch", NULL, yspec)< 0)
       goto done;
#endif

    /* Add netconf yang spec, used as internal protocol */
    if (netconf_module_load(h) < 0)
        goto done;

    /* Add system modules */
    if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC8040") &&
        yang_spec_parse_module(h, "ietf-restconf-monitoring", NULL, yspec)< 0)
        goto done;
    if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC5277") &&
        yang_spec_parse_module(h, "clixon-rfc5277", NULL, yspec)< 0)
        goto done;

    /* Here all modules are loaded 
     * Compute and set canonical namespace context
     */
    if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
        goto done;
    if (clicon_nsctx_global_set(h, nsctx_global) < 0)
        goto done;

    /* Explicit dump of config (also debug dump below). */
    if (config_dump){
        if (clicon_option_dump1(h, stdout, config_dump_format, 1) < 0)
            goto done;
        goto ok;
    }
    /* Dump configuration options on debug */
    clicon_option_dump(h, 1);

    /* Call start function in all plugins before we go interactive */
    if (clixon_plugin_start_all(h) < 0)
        goto done;

    /* Try to get config: inline, config-file or query backend */
    if (restconf_main_config(h, yspec, inline_config) < 0)
        goto done;
    if ((sockpath = restconf_fcgi_socket_get(h)) == NULL){
        clixon_err(OE_CFG, 0, "No restconf fcgi-socket (have you set FEATURE fcgi in config?)");
        goto done;
    }
    if (FCGX_Init() != 0){ /* How to cleanup memory after this? */
        clixon_err(OE_CFG, errno, "FCGX_Init");
        goto done;
    }
    clixon_debug(CLIXON_DBG_RESTCONF, "restconf_main: Opening FCGX socket: %s", sockpath);
    if ((sock = FCGX_OpenSocket(sockpath, 10)) < 0){
        clixon_err(OE_CFG, errno, "FCGX_OpenSocket");
        goto done;
    }
    _MYSOCK = sock;
    /* Change group of fcgi sock fronting reverse proxy to CLICON_RESTCONF_USER, 
     * the effective group is clicon which is backend. */
    gid_t wgid = -1;
    wwwuser = clicon_option_str(h, "CLICON_RESTCONF_USER");
    if (group_name2gid(wwwuser, &wgid) < 0){
        clixon_log(h, LOG_ERR, "'%s' does not seem to be a valid user group.", wwwuser);
        goto done;
    }
    if (chown(sockpath, -1, wgid) < 0){
        clixon_err(OE_CFG, errno, "chown");
        goto done;
    }
    if (clicon_socket_set(h, sock) < 0)
        goto done;
    /* umask settings may interfer: we want group to write: this is 774 */
    if (chmod(sockpath, S_IRWXU|S_IRWXG|S_IROTH) < 0){
        clixon_err(OE_UNIX, errno, "chmod");
        goto done;
    }

    /* Drop privileges if started as root to CLICON_RESTCONF_USER
     * and use drop mode: CLICON_RESTCONF_PRIVILEGES
     */
    if (restconf_drop_privileges(h) < 0)
        goto done;
    /* Set RFC6022 session parameters that will be sent in first hello,
     * @see clicon_hello_req
     */
    clicon_data_set(h, "session-transport", "cl:restconf");

    if (FCGX_InitRequest(req, sock, 0) != 0){
        clixon_err(OE_CFG, errno, "FCGX_InitRequest");
        goto done;
    }
    while (1) {
        finish = 1; /* If zero, dont finish request, initiate new */

        if (FCGX_Accept_r(req) < 0) {
            clixon_err(OE_CFG, errno, "FCGX_Accept_r");
            goto done;
        }
        clixon_debug(CLIXON_DBG_RESTCONF, "------------");

        /* Translate from FCGI parameter form to Clixon runtime data 
         * XXX: potential name collision?
         */
        if (fcgi_params_set(h, req->envp) < 0)
            goto done;
        if ((path = restconf_param_get(h, "REQUEST_URI")) == NULL){
            clixon_debug(CLIXON_DBG_RESTCONF, "NULL URI");
        }
        else {
            /* Matching algorithm:
             * 1. try well-known
             * 2. try /restconf
             * 3. try /stream
             * 4. return error
             */
            query = NULL;
            qvec = NULL;
            if (strcmp(path, RESTCONF_WELL_KNOWN) == 0){
                if (api_well_known(h, req) < 0)
                    goto done;
            }
            else if (api_path_is_restconf(h)){
                query = restconf_param_get(h, "QUERY_STRING");
                if (query != NULL && strlen(query))
                    if (uri_str2cvec(query, '&', '=', 1, &qvec) < 0)
                        goto done;
                if (api_root_restconf(h, req, qvec) < 0)
                    goto done;
            }
            else if (api_path_is_stream(h)){
                query = restconf_param_get(h, "QUERY_STRING");
                if (query != NULL && strlen(query))
                    if (uri_str2cvec(query, '&', '=', 1, &qvec) < 0)
                        goto done;
                /* XXX doing goto done on error causes test errors */
                (void)api_stream(h, req, qvec, &finish);
            }
            else{
                clixon_debug(CLIXON_DBG_RESTCONF, "top-level %s not found", path);
                if (netconf_invalid_value_xml(&xerr, "protocol", "Top-level path not found") < 0)
                    goto done;
                if (api_return_err0(h, req, xerr, 1, YANG_DATA_JSON, 0) < 0)
                    goto done;
                if (xerr){
                    xml_free(xerr);
                    xerr = NULL;
                }
            }
            if (qvec){
                cvec_free(qvec);
                qvec = NULL;
            }
        }
        if (restconf_param_del_all(h) < 0)
            goto done;
        if (finish)
            FCGX_Finish_r(req);
        else if (clixon_exit_get()){
            FCGX_Finish_r(req);
            break;
        }
        else{ /* A handler is forked so we initiate a new request after instead 
                 of finishing the old */
            if (FCGX_InitRequest(req, sock, 0) != 0){
                clixon_err(OE_CFG, errno, "FCGX_InitRequest");
                goto done;
            }
        }
    } /* while */
 ok:
    retval = 0;
 done:
    stream_child_freeall(h);
    restconf_terminate(h);
    return retval;
}
