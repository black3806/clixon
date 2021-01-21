/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 * Yang cardinality functions according to RFC 7950 
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <regex.h>
#include <dirent.h>
#include <sys/types.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/stat.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_err.h"
#include "clixon_yang.h"
#include "clixon_yang_cardinality.h"

/*
 * Types
 */
/* Encode cardinality according to RFC 7950 
 * Example:
 * 7.20.3.1.  The deviation's Substatements
 *
 *               +--------------+----------+-------------+
 *               | substatement | section  | cardinality |
 *               +--------------+----------+-------------+
 *               | description  | 7.21.3   | 0..1        |
 *               | deviate      | 7.20.3.2 | 1..n        |
 *               | reference    | 7.21.4   | 0..1        |
 *               +--------------+----------+-------------+
 * The cardinalities are (and how many time they occur)
 *   0..1  149 See ycardmap_01
 *   1..n, 1
 *   0..n  176 (no restrictions)
 *   1     10
 */
struct ycard{
    enum rfc_6020 yc_parent;
    enum rfc_6020 yc_child;
    int           yc_min;
    int           yc_max;
};

/*
 * Local variables
 */
/* Yang statements cardinality map
 * The cardinalities are (and how many time they occur)
 *   1..n, 1
 *   1     10
 *   0..1  149
 *   0..n  176 (no restrictions)
 * @note assume array is ordered wrt parent
 * @note yang-version is optional in RFC6020 but mandatory in RFC7950, if not given, it defaults to 1.
 */
#define NMAX 1000000 /* Just a large number */
static const struct ycard yclist[] = {
    {Y_ACTION,  Y_DESCRIPTION, 0, 1},
    {Y_ACTION,  Y_GROUPING, 0, NMAX},
    {Y_ACTION,  Y_IF_FEATURE, 0, NMAX},
    {Y_ACTION,  Y_INPUT, 0, 1},
    {Y_ACTION,  Y_OUTPUT, 0, 1},
    {Y_ACTION,  Y_REFERENCE, 0, 1},
    {Y_ACTION,  Y_STATUS, 0, 1},
    {Y_ACTION,  Y_TYPEDEF, 0, NMAX},
    {Y_ANYDATA,  Y_CONFIG, 0, 1},
    {Y_ANYDATA,  Y_DESCRIPTION, 0, 1},
    {Y_ANYDATA,  Y_IF_FEATURE, 0, NMAX},
    {Y_ANYDATA,  Y_MANDATORY, 0, 1},
    {Y_ANYDATA,  Y_MUST, 0, NMAX},
    {Y_ANYDATA,  Y_REFERENCE, 0, 1},
    {Y_ANYDATA,  Y_STATUS, 0, 1},
    {Y_ANYDATA,  Y_WHEN, 0, 1},
    {Y_ANYXML,  Y_CONFIG, 0, 1},
    {Y_ANYXML,  Y_DESCRIPTION, 0, 1},
    {Y_ANYXML,  Y_IF_FEATURE, 0, NMAX},
    {Y_ANYXML,  Y_MANDATORY, 0, 1},
    {Y_ANYXML,  Y_MUST, 0, NMAX},
    {Y_ANYXML,  Y_REFERENCE, 0, 1},
    {Y_ANYXML,  Y_STATUS, 0, 1},
    {Y_ANYXML,  Y_WHEN, 0, 1},
    {Y_ARGUMENT,  Y_YIN_ELEMENT, 0, 1},
    {Y_AUGMENT,  Y_ACTION, 0, NMAX},
    {Y_AUGMENT,  Y_ANYDATA, 0, NMAX},
    {Y_AUGMENT,  Y_ANYXML, 0, NMAX},
    {Y_AUGMENT,  Y_CASE, 0, NMAX},
    {Y_AUGMENT,  Y_CHOICE, 0, NMAX},
    {Y_AUGMENT,  Y_CONTAINER, 0, NMAX},
    {Y_AUGMENT,  Y_DESCRIPTION, 0, 1},
    {Y_AUGMENT,  Y_IF_FEATURE, 0, NMAX},
    {Y_AUGMENT,  Y_LEAF, 0, NMAX},
    {Y_AUGMENT,  Y_LEAF_LIST, 0, NMAX},
    {Y_AUGMENT,  Y_LIST, 0, NMAX},
    {Y_AUGMENT,  Y_NOTIFICATION, 0, NMAX},
    {Y_AUGMENT,  Y_REFERENCE, 0, 1},
    {Y_AUGMENT,  Y_STATUS, 0, 1},
    {Y_AUGMENT,  Y_USES, 0, NMAX},
    {Y_AUGMENT,  Y_WHEN, 0, 1},
    {Y_BELONGS_TO, Y_PREFIX,    1, 1},
    {Y_BIT,  Y_DESCRIPTION, 0, 1},
    {Y_BIT,  Y_IF_FEATURE, 0, NMAX},
    {Y_BIT,  Y_POSITION, 0, 1},
    {Y_BIT,  Y_REFERENCE, 0, 1},
    {Y_BIT,  Y_STATUS, 0, 1},
    {Y_CASE,  Y_ANYDATA, 0, NMAX},
    {Y_CASE,  Y_ANYXML, 0, NMAX},
    {Y_CASE,  Y_CHOICE, 0, NMAX},
    {Y_CASE,  Y_CONTAINER, 0, NMAX},
    {Y_CASE,  Y_DESCRIPTION, 0, 1},
    {Y_CASE,  Y_IF_FEATURE, 0, NMAX},
    {Y_CASE,  Y_LEAF, 0, NMAX},
    {Y_CASE,  Y_LEAF_LIST, 0, NMAX},
    {Y_CASE,  Y_LIST, 0, NMAX},
    {Y_CASE,  Y_REFERENCE, 0, 1},
    {Y_CASE,  Y_STATUS, 0, 1},
    {Y_CASE,  Y_USES, 0, NMAX},
    {Y_CASE,  Y_WHEN, 0, 1},
    {Y_CHOICE,  Y_ANYXML, 0, NMAX},
    {Y_CHOICE,  Y_CASE, 0, NMAX},
    {Y_CHOICE,  Y_CHOICE, 0, NMAX},
    {Y_CHOICE,  Y_CONFIG, 0, 1},
    {Y_CHOICE,  Y_CONTAINER, 0, NMAX},
    {Y_CHOICE,  Y_DEFAULT, 0, 1},
    {Y_CHOICE,  Y_DESCRIPTION, 0, 1},
    {Y_CHOICE,  Y_IF_FEATURE, 0, NMAX},
    {Y_CHOICE,  Y_LEAF, 0, NMAX},
    {Y_CHOICE,  Y_LEAF_LIST, 0, NMAX},
    {Y_CHOICE,  Y_LIST, 0, NMAX},
    {Y_CHOICE,  Y_MANDATORY, 0, 1},
    {Y_CHOICE,  Y_REFERENCE, 0, 1},
    {Y_CHOICE,  Y_STATUS, 0, 1},
    {Y_CHOICE,  Y_WHEN, 0, 1},
    {Y_CHOICE, Y_ANYDATA, 0, NMAX},
    {Y_CONTAINER, Y_ACTION,   0, NMAX},
    {Y_CONTAINER, Y_ANYDATA, 0, NMAX},
    {Y_CONTAINER, Y_ANYXML, 0, NMAX},
    {Y_CONTAINER, Y_CHOICE, 0, NMAX},
    {Y_CONTAINER, Y_CONFIG, 0, 1},
    {Y_CONTAINER, Y_CONTAINER, 0, NMAX},
    {Y_CONTAINER, Y_DESCRIPTION, 0, 1},
    {Y_CONTAINER, Y_GROUPING, 0, NMAX},
    {Y_CONTAINER, Y_IF_FEATURE, 0, NMAX},
    {Y_CONTAINER, Y_LEAF, 0, NMAX},
    {Y_CONTAINER, Y_LEAF_LIST, 0, NMAX},
    {Y_CONTAINER, Y_LIST, 0, NMAX},
    {Y_CONTAINER, Y_MUST, 0, NMAX},
    {Y_CONTAINER, Y_NOTIFICATION, 0, NMAX},
    {Y_CONTAINER, Y_PRESENCE, 0, 1},
    {Y_CONTAINER, Y_REFERENCE, 0, 1},
    {Y_CONTAINER, Y_STATUS, 0, 1},
    {Y_CONTAINER, Y_TYPEDEF, 0, NMAX},
    {Y_CONTAINER, Y_USES, 0, NMAX},
    {Y_CONTAINER, Y_WHEN, 0, 1},
    {Y_DEVIATE,  Y_CONFIG, 0, 1},
    {Y_DEVIATE,  Y_DEFAULT, 0, NMAX},
    {Y_DEVIATE,  Y_MANDATORY, 0, 1},
    {Y_DEVIATE,  Y_MAX_ELEMENTS, 0, 1},
    {Y_DEVIATE,  Y_MIN_ELEMENTS, 0, 1},
    {Y_DEVIATE,  Y_MUST, 0, NMAX},
    {Y_DEVIATE,  Y_TYPE, 0, 1},
    {Y_DEVIATE,  Y_UNIQUE, 0, NMAX},
    {Y_DEVIATE,  Y_UNITS, 0, 1},
    {Y_DEVIATION,  Y_DESCRIPTION, 0, 1},
    {Y_DEVIATION,  Y_DEVIATE, 1, NMAX},
    {Y_DEVIATION,  Y_REFERENCE, 0, 1},
    {Y_ENUM,  Y_DESCRIPTION, 0, 1},
    {Y_ENUM,  Y_IF_FEATURE, 0, NMAX},
    {Y_ENUM,  Y_REFERENCE, 0, 1},
    {Y_ENUM,  Y_STATUS, 0, 1},
    {Y_ENUM,  Y_VALUE, 0, 1},
    {Y_EXTENSION,  Y_ARGUMENT, 0, 1},
    {Y_EXTENSION,  Y_DESCRIPTION, 0, 1},
    {Y_EXTENSION,  Y_REFERENCE, 0, 1},
    {Y_EXTENSION,  Y_STATUS, 0, 1},
    {Y_FEATURE,  Y_DESCRIPTION, 0, 1},
    {Y_FEATURE,  Y_IF_FEATURE, 0, NMAX},
    {Y_FEATURE,  Y_REFERENCE, 0, 1},
    {Y_FEATURE,  Y_STATUS, 0, 1},
    {Y_GROUPING,  Y_ACTION, 0, NMAX},
    {Y_GROUPING,  Y_ANYDATA, 0, NMAX},
    {Y_GROUPING,  Y_ANYXML, 0, NMAX},
    {Y_GROUPING,  Y_CHOICE, 0, NMAX},
    {Y_GROUPING,  Y_CONTAINER, 0, NMAX},
    {Y_GROUPING,  Y_DESCRIPTION, 0, 1},
    {Y_GROUPING,  Y_GROUPING, 0, NMAX},
    {Y_GROUPING,  Y_LEAF, 0, NMAX},
    {Y_GROUPING,  Y_LEAF_LIST, 0, NMAX},
    {Y_GROUPING,  Y_LIST, 0, NMAX},
    {Y_GROUPING,  Y_NOTIFICATION, 0, NMAX},
    {Y_GROUPING,  Y_REFERENCE, 0, 1},
    {Y_GROUPING,  Y_STATUS, 0, 1},
    {Y_GROUPING,  Y_TYPEDEF, 0, NMAX},
    {Y_GROUPING,  Y_USES, 0, NMAX},
    {Y_IDENTITY,  Y_BASE, 0, NMAX},
    {Y_IDENTITY,  Y_DESCRIPTION, 0, 1},
    {Y_IDENTITY,  Y_IF_FEATURE, 0, NMAX},
    {Y_IDENTITY,  Y_REFERENCE, 0, 1},
    {Y_IDENTITY,  Y_STATUS, 0, 1},
    {Y_IMPORT, Y_DESCRIPTION,  0, 1},
    {Y_IMPORT, Y_PREFIX,       1, 1},
    {Y_IMPORT, Y_REFERENCE,    0, 1},
    {Y_IMPORT, Y_REVISION_DATE,0, 1},
    {Y_INCLUDE, Y_DESCRIPTION,  0, 1},
    {Y_INCLUDE, Y_REFERENCE,    0, 1},
    {Y_INCLUDE, Y_REVISION_DATE,0, 1},
    {Y_INPUT,  Y_ANYDATA, 0, NMAX},
    {Y_INPUT,  Y_ANYXML, 0, NMAX},
    {Y_INPUT,  Y_CHOICE, 0, NMAX},
    {Y_INPUT,  Y_CONTAINER, 0, NMAX},
    {Y_INPUT,  Y_GROUPING, 0, NMAX},
    {Y_INPUT,  Y_LEAF, 0, NMAX},
    {Y_INPUT,  Y_LEAF_LIST, 0, NMAX},
    {Y_INPUT,  Y_LIST, 0, NMAX},
    {Y_INPUT,  Y_MUST, 0, NMAX},
    {Y_INPUT,  Y_TYPEDEF, 0, NMAX},
    {Y_INPUT,  Y_USES, 0, NMAX},
    {Y_LEAF, Y_CONFIG, 0, 1},
    {Y_LEAF, Y_DEFAULT, 0, 1},
    {Y_LEAF, Y_DESCRIPTION, 0, 1},
    {Y_LEAF, Y_IF_FEATURE, 0, NMAX},
    {Y_LEAF, Y_MANDATORY, 0, 1},
    {Y_LEAF, Y_MUST, 0, NMAX},
    {Y_LEAF, Y_REFERENCE, 0, 1},
    {Y_LEAF, Y_STATUS, 0, 1},
    {Y_LEAF, Y_TYPE, 1, 1},
    {Y_LEAF, Y_UNITS, 0, 1},
    {Y_LEAF, Y_WHEN, 0, 1},
    {Y_LEAF_LIST, Y_CONFIG, 0, 1},
    {Y_LEAF_LIST, Y_DEFAULT, 0, NMAX},
    {Y_LEAF_LIST, Y_DESCRIPTION, 0, 1},
    {Y_LEAF_LIST, Y_IF_FEATURE, 0, NMAX},
    {Y_LEAF_LIST, Y_MAX_ELEMENTS, 0, 1},
    {Y_LEAF_LIST, Y_MIN_ELEMENTS, 0, 1},
    {Y_LEAF_LIST, Y_MUST, 0, NMAX},
    {Y_LEAF_LIST, Y_ORDERED_BY, 0, 1},
    {Y_LEAF_LIST, Y_REFERENCE, 0, 1},
    {Y_LEAF_LIST, Y_STATUS, 0, 1},
    {Y_LEAF_LIST, Y_TYPE, 1, 1},
    {Y_LEAF_LIST, Y_UNITS, 0, 1},
    {Y_LEAF_LIST, Y_WHEN, 0, 1},
    {Y_LENGTH,  Y_DESCRIPTION, 0, 1},
    {Y_LENGTH,  Y_ERROR_APP_TAG, 0, 1},
    {Y_LENGTH,  Y_ERROR_MESSAGE, 0, 1},
    {Y_LENGTH,  Y_REFERENCE, 0, 1},
    {Y_LIST, Y_ACTION, 0, NMAX},
    {Y_LIST, Y_ANYDATA, 0, NMAX},
    {Y_LIST, Y_ANYXML, 0, NMAX},
    {Y_LIST, Y_CHOICE, 0, NMAX},
    {Y_LIST, Y_CONFIG, 0, 1},
    {Y_LIST, Y_CONTAINER, 0, NMAX},
    {Y_LIST, Y_DESCRIPTION, 0, 1},
    {Y_LIST, Y_GROUPING, 0, NMAX},
    {Y_LIST, Y_IF_FEATURE, 0, NMAX},
    {Y_LIST, Y_KEY, 0, 1},
    {Y_LIST, Y_LEAF, 0, NMAX},
    {Y_LIST, Y_LEAF_LIST, 0, NMAX},
    {Y_LIST, Y_LIST, 0, NMAX},
    {Y_LIST, Y_MAX_ELEMENTS, 0, 1},
    {Y_LIST, Y_MIN_ELEMENTS, 0, 1},
    {Y_LIST, Y_MUST, 0, NMAX},
    {Y_LIST, Y_NOTIFICATION, 0, NMAX},
    {Y_LIST, Y_ORDERED_BY, 0, 1},
    {Y_LIST, Y_REFERENCE, 0, 1},
    {Y_LIST, Y_STATUS, 0, 1},
    {Y_LIST, Y_TYPEDEF, 0, NMAX},
    {Y_LIST, Y_UNIQUE, 0, NMAX},
    {Y_LIST, Y_USES, 0, NMAX},
    {Y_LIST, Y_WHEN, 0,1},
    {Y_MODULE, Y_ANYDATA,      0, NMAX},
    {Y_MODULE, Y_ANYXML,       0, NMAX},
    {Y_MODULE, Y_AUGMENT,      0, NMAX},
    {Y_MODULE, Y_CHOICE,       0, NMAX},
    {Y_MODULE, Y_CONTACT,      0, 1},
    {Y_MODULE, Y_CONTAINER,    0, NMAX},
    {Y_MODULE, Y_DESCRIPTION,  0, 1},
    {Y_MODULE, Y_DEVIATION,    0, NMAX},
    {Y_MODULE, Y_EXTENSION,    0, NMAX},
    {Y_MODULE, Y_FEATURE,      0, NMAX},
    {Y_MODULE, Y_GROUPING,     0, NMAX},
    {Y_MODULE, Y_IDENTITY,     0, NMAX},
    {Y_MODULE, Y_IMPORT,       0, NMAX},
    {Y_MODULE, Y_INCLUDE,      0, NMAX},
    {Y_MODULE, Y_LEAF,         0, NMAX},
    {Y_MODULE, Y_LEAF_LIST,    0, NMAX},
    {Y_MODULE, Y_LIST,         0, NMAX},
    {Y_MODULE, Y_NAMESPACE,    1, 1},
    {Y_MODULE, Y_NOTIFICATION, 0, NMAX},
    {Y_MODULE, Y_ORGANIZATION, 0, 1},
    {Y_MODULE, Y_PREFIX,       1, 1},
    {Y_MODULE, Y_REFERENCE,    0, 1},
    {Y_MODULE, Y_REVISION,     0, NMAX},
    {Y_MODULE, Y_RPC,          0, NMAX},
    {Y_MODULE, Y_TYPEDEF,      0, NMAX},
    {Y_MODULE, Y_USES,         0, NMAX},
    {Y_MODULE, Y_YANG_VERSION, 0, 1},
    {Y_MUST, Y_DESCRIPTION, 0, 1},
    {Y_MUST, Y_ERROR_APP_TAG, 0, 1},
    {Y_MUST, Y_ERROR_MESSAGE, 0, 1},
    {Y_MUST, Y_REFERENCE, 0, 1},
    {Y_NOTIFICATION,  Y_ANYDATA, 0, NMAX},
    {Y_NOTIFICATION,  Y_ANYXML, 0, NMAX},
    {Y_NOTIFICATION,  Y_CHOICE, 0, NMAX},
    {Y_NOTIFICATION,  Y_CONTAINER, 0, NMAX},
    {Y_NOTIFICATION,  Y_DESCRIPTION, 0, 1},
    {Y_NOTIFICATION,  Y_GROUPING, 0, NMAX},
    {Y_NOTIFICATION,  Y_IF_FEATURE, 0, NMAX},
    {Y_NOTIFICATION,  Y_LEAF, 0, NMAX},
    {Y_NOTIFICATION,  Y_LEAF_LIST, 0, NMAX},
    {Y_NOTIFICATION,  Y_LIST, 0, NMAX},
    {Y_NOTIFICATION,  Y_MUST, 0, NMAX},
    {Y_NOTIFICATION,  Y_REFERENCE, 0, 1},
    {Y_NOTIFICATION,  Y_STATUS, 0, 1},
    {Y_NOTIFICATION,  Y_TYPEDEF, 0, NMAX},
    {Y_NOTIFICATION,  Y_USES, 0, NMAX},
    {Y_OUTPUT,  Y_ANYDATA, 0, NMAX},
    {Y_OUTPUT,  Y_ANYXML, 0, NMAX},
    {Y_OUTPUT,  Y_CHOICE, 0, NMAX},
    {Y_OUTPUT,  Y_CONTAINER, 0, NMAX},
    {Y_OUTPUT,  Y_GROUPING, 0, NMAX},
    {Y_OUTPUT,  Y_LEAF, 0, NMAX},
    {Y_OUTPUT,  Y_LEAF_LIST, 0, NMAX},
    {Y_OUTPUT,  Y_LIST, 0, NMAX},
    {Y_OUTPUT,  Y_MUST, 0, NMAX},
    {Y_OUTPUT,  Y_TYPEDEF, 0, NMAX},
    {Y_OUTPUT,  Y_USES, 0, NMAX},
    {Y_PATTERN,  Y_DESCRIPTION, 0, 1},
    {Y_PATTERN,  Y_ERROR_APP_TAG, 0, 1},
    {Y_PATTERN,  Y_ERROR_MESSAGE, 0, 1},
    {Y_PATTERN,  Y_MODIFIER, 0, 1},
    {Y_PATTERN,  Y_REFERENCE, 0, 1},
    {Y_RANGE,  Y_DESCRIPTION, 0, 1},
    {Y_RANGE,  Y_ERROR_APP_TAG, 0, 1},
    {Y_RANGE,  Y_ERROR_MESSAGE, 0, 1},
    {Y_RANGE,  Y_REFERENCE, 0, 1},
    {Y_REVISION, Y_DESCRIPTION, 0, 1},
    {Y_REVISION, Y_REFERENCE,   0, 1},
    {Y_RPC,  Y_DESCRIPTION, 0, 1},
    {Y_RPC,  Y_GROUPING, 0, NMAX},
    {Y_RPC,  Y_IF_FEATURE, 0, NMAX},
    {Y_RPC,  Y_INPUT, 0, 1},
    {Y_RPC,  Y_OUTPUT, 0, 1},
    {Y_RPC,  Y_REFERENCE, 0, 1},
    {Y_RPC,  Y_STATUS, 0, 1},
    {Y_RPC,  Y_TYPEDEF, 0, NMAX},
    {Y_SUBMODULE, Y_ANYDATA,    0, NMAX},
    {Y_SUBMODULE, Y_AUGMENT,    0, NMAX},
    {Y_SUBMODULE, Y_BELONGS_TO, 1, 1},
    {Y_SUBMODULE, Y_CHOICE,     0, NMAX},
    {Y_SUBMODULE, Y_CONTACT,    0, 1},
    {Y_SUBMODULE, Y_CONTAINER,  0, NMAX},
    {Y_SUBMODULE, Y_DESCRIPTION,0, 1},
    {Y_SUBMODULE, Y_DEVIATION,  0, NMAX},
    {Y_SUBMODULE, Y_EXTENSION,  0, NMAX},
    {Y_SUBMODULE, Y_FEATURE,    0, NMAX},
    {Y_SUBMODULE, Y_GROUPING,   0, NMAX},
    {Y_SUBMODULE, Y_IDENTITY,   0, NMAX},
    {Y_SUBMODULE, Y_IMPORT,     0, NMAX},
    {Y_SUBMODULE, Y_INCLUDE,    0, NMAX},
    {Y_SUBMODULE, Y_LEAF,       0, NMAX},
    {Y_SUBMODULE, Y_LEAF_LIST,  0, NMAX},
    {Y_SUBMODULE, Y_LIST,       0, NMAX},
    {Y_SUBMODULE, Y_NOTIFICATION,0, NMAX},
    {Y_SUBMODULE, Y_ORGANIZATION,0, 1},
    {Y_SUBMODULE, Y_REFERENCE,  0, 1},
    {Y_SUBMODULE, Y_REVISION,   0, NMAX},
    {Y_SUBMODULE, Y_RPC,        0, NMAX},
    {Y_SUBMODULE, Y_TYPEDEF,    0, NMAX},
    {Y_SUBMODULE, Y_USES,       0, NMAX},
    {Y_SUBMODULE, Y_YANG_VERSION,0, 1}, /* "yang-version" statement is mandatory in YANG version "1.1". */
    {Y_TYPE, Y_BASE,          0, NMAX},
    {Y_TYPE, Y_BIT,           0, NMAX},
    {Y_TYPE, Y_ENUM,          0, NMAX},
    {Y_TYPE, Y_FRACTION_DIGITS, 0, 1},
    {Y_TYPE, Y_LENGTH,        0, 1},
    {Y_TYPE, Y_PATH,          0, 1},
    {Y_TYPE, Y_PATTERN,       0, NMAX},
    {Y_TYPE, Y_RANGE,         0, 1},
    {Y_TYPE, Y_REQUIRE_INSTANCE, 0, 1},
    {Y_TYPE, Y_TYPE,          0, NMAX},
    {Y_TYPEDEF, Y_DEFAULT,    0, 1},
    {Y_TYPEDEF, Y_DESCRIPTION,0, 1},
    {Y_TYPEDEF, Y_REFERENCE,  0, 1},
    {Y_TYPEDEF, Y_STATUS,     0, 1},
    {Y_TYPEDEF, Y_TYPE,       1, 1},
    {Y_TYPEDEF, Y_UNITS,      0, 1},
    {Y_USES,  Y_AUGMENT, 0, NMAX},
    {Y_USES,  Y_DESCRIPTION, 0, 1},
    {Y_USES,  Y_IF_FEATURE, 0, NMAX},
    {Y_USES,  Y_REFERENCE, 0, 1},
    {Y_USES,  Y_REFINE, 0, NMAX},
    {Y_USES,  Y_STATUS, 0, 1},
    {Y_USES,  Y_WHEN, 0, 1},
    {0,}
};

/*! Find yang parent and child combination in yang cardinality table
 * @param[in] parent Parent Yang spec
 * @param[in] child  Child yang spec if 0 first
 * @param[in] yc     Yang cardinality map
 * @param[in] p      If set, quit as soon as parents dont match
 * @retval    NULL   Not found
 * @retval    yp     Found
 * @note if cardinal list above is sorted, this search could do binary
 */
static const struct ycard *
ycard_find(enum rfc_6020       parent,
	   enum rfc_6020       child,
	   const struct ycard *yclist,
	   int                 p)

{
     const struct ycard *yc;
    
     for (yc = &yclist[0]; (int)yc->yc_parent; yc++){
	 if (yc->yc_parent == parent){
	     if (!child || yc->yc_child == child)
		 return yc;
	 }
	 else
	     if (p)
		 return NULL; /* premature quit */
     }
    return NULL;
}

/*! Check cardinality, ie if each yang node has the expected nr of children 
 * @param[in] h        Clicon handle
 * @param[in] yt       Yang statement
 * @param[in] modname  Name of module (for debug message)
 * 1) For all children, if neither in 0..n, 0..1, 1 or 1..n   ->ERROR 
 * 2) For all in 1 and 1..n list, if 0 such children          ->ERROR
 * 3) For all in 0..1 and 1 list, if >1 such children         ->ERROR
 * 4) Recurse
 * @note always accept UNKNOWN (due to extension)
 */
int
yang_cardinality(clicon_handle h,
		 yang_stmt    *yt,
		 char         *modname)
{
    int        retval = -1;
    yang_stmt *ys = NULL;
    int        pk;
    int        ck;
    int        i;
    int        nr;
    const struct ycard *ycplist; /* ycard parent table*/
    const struct ycard *yc;
    
    pk = yang_keyword_get(yt);
    /* 0) Find parent sub-parts of cardinality vector */
    if ((ycplist = ycard_find(pk, 0, yclist, 0)) == NULL)
	goto ok; /* skip */
    /* 1) For all children, if neither in 0..n, 0..1, 1 or 1..n   ->ERROR  */
    ys = NULL;
    while ((ys = yn_each(yt, ys)) != NULL) {
	ck = yang_keyword_get(ys);
	if (ck == Y_UNKNOWN) /* special case */
	    continue;
	/* Find entry in yang cardinality table from parent/child keyword pair */
	if ((yc = ycard_find(pk, ck, ycplist, 1)) == NULL){
	    clicon_err(OE_YANG, 0, "%s: \"%s\"(%s) is child of \"%s\"(%s), but should not be",
		       modname,
		       yang_key2str(ck),
		       yang_argument_get(ys),
		       yang_key2str(pk),
		       yang_argument_get(yt));
	    goto done;
	}
    }
    /* 2) For all in 1 and 1..n list, if 0 such children          ->ERROR */
    for (yc = &ycplist[0]; (int)yc->yc_parent == pk; yc++){
	if (yc->yc_min  &&
	    yang_find(yt, yc->yc_child, NULL) == NULL){
	    clicon_err(OE_YANG, 0, "%s: \"%s\" is missing but is mandatory child of \"%s\"",
		       modname, yang_key2str(yc->yc_child), yang_key2str(pk));
	    goto done;
	}
    }
    /* 3) For all in 0..1 and 1 list, if >1 such children         ->ERROR */
    for (yc = &ycplist[0]; (int)yc->yc_parent == pk; yc++){
	if (yc->yc_max<NMAX &&
	    (nr = yang_match(yt, yc->yc_child, NULL)) > yc->yc_max){
	    clicon_err(OE_YANG, 0, "%s: \"%s\" has %d children of type \"%s\", but only %d allowed",
		       modname,
		       yang_key2str(pk),
		       nr,
		       yang_key2str(yc->yc_child),
		       yc->yc_max);
	    goto done;
	}
    }
    
    /* 4) Recurse */
    i = 0;
    while (i< yang_len_get(yt)){ /* Note, children may be removed cant use yn_each */
	ys = yang_child_i(yt, i++);
	if (yang_cardinality(h, ys, modname) < 0)
	    goto done;
    }
 ok:
    retval = 0;
 done:
    return retval;
}

