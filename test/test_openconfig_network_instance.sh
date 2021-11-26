#!/usr/bin/env bash
# Tests for openconfig network-instances
# See eg https://github.com/clicon/clixon/issues/287

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

APPNAME=example

cfg=$dir/conf_yang.xml
fyang=$dir/clixon-example.yang

new "openconfig"
if [ ! -d "$OPENCONFIG" ]; then
#    err "Hmm Openconfig dir does not seem to exist, try git clone https://github.com/openconfig/public?"
    echo "...skipped: OPENCONFIG not set"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

OCDIR=$OPENCONFIG/release/models

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$OCDIR</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_FILE>$fyang</CLICON_YANG_MAIN_FILE>	
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_CLI_AUTOCLI_EXCLUDE>clixon-restconf ietf-interfaces</CLICON_CLI_AUTOCLI_EXCLUDE>
  <CLICON_CLI_GENMODEL_TYPE>VARS</CLICON_CLI_GENMODEL_TYPE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_MODULE_LIBRARY_RFC7895>true</CLICON_MODULE_LIBRARY_RFC7895>
</clixon-config>
EOF

# First using ietf-interfaces (not openconfig-interfaces)
# Example yang
cat <<EOF > $fyang
module clixon-example{
  yang-version 1.1;
  namespace "urn:example:example";
  prefix ex;

  import openconfig-network-instance {
    prefix oc-netinst;
  }
}
EOF

# Example system
cat <<EOF > $dir/startup_db
<config>
  <bgp xmlns="http://openconfig.net/yang/bgp">
    <global>
      <config>
        <as>0</as> /* XXX */
      </config>
    </global>
  </bgp>
  <network-instances xmlns="http://openconfig.net/yang/network-instance">
    <network-instance>
      <name>default</name>
      <fdb>
         <config>
            <flood-unknown-unicast-supression>false</flood-unknown-unicast-supression>
         </config>
      </fdb>
      <config>
         <type>oc-ni-types:DEFAULT_INSTANCE</type>
         <enabled>true</enabled>
         <router-id>1.2.3.4</router-id>
      </config>
    </network-instance>
  </network-instances>
</config>
EOF

if [ $BE -ne 0 ]; then
    new "kill old backend"
    sudo clixon_backend -zf $cfg
    if [ $? -ne 0 ]; then
	err
    fi
    sudo pkill -f clixon_backend # to be sure
    
    new "start backend -s startup -f $cfg"
    start_backend -s startup -f $cfg
fi

new "wait backend"
wait_backend

new "$clixon_cli -D $DBG -1f $cfg show version"
expectpart "$($clixon_cli -D $DBG -1f $cfg show version)" 0 "${CLIXON_VERSION}"

new "$clixon_cli -D $DBG -1f $cfg save config as cli"
expectpart "$($clixon_cli -D $DBG -1f $cfg save $dir/config.cli cli)" 0 "^$"

new "Check saved config"
expectpart "$(cat $dir/config.cli)" 0 "set network-instances network-instance default config type oc-ni-types:DEFAULT_INSTANCE" "set network-instances network-instance default config router-id 1.2.3.4"

new "load saved cli config"
expectpart "$(cat $dir/config.cli | $clixon_cli -D $DBG -f $cfg 2>&1 > /dev/null)" 0 "^$"
#time cat $dir/config.cli | $clixon_cli -D $DBG -f $cfg

if [ $BE -ne 0 ]; then
    new "Kill backend"
    # Check if premature kill
    pid=$(pgrep -u root -f clixon_backend)
    if [ -z "$pid" ]; then
	err "backend already dead"
    fi
    # kill backend
    stop_backend -f $cfg
fi

rm -rf $dir

new "endtest"
endtest