#!/usr/bin/env bash
# Restconf direct start/stop using RPC (as alternative to systemd or other)
# Also try ip netns 

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

APPNAME=example

cfg=$dir/conf.xml

# Cant get it to work in the general case, single tests work fine
# More specifically, if mem.sh background netconf, netconf crashes
if [ $valgrindtest -ne 0 ]; then
    echo "...skipped "
    return 0 # skip
fi

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$IETFRFC</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>/usr/local/lib/$APPNAME/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_REGEXP>example_backend.so$</CLICON_BACKEND_REGEXP>
  <CLICON_RESTCONF_DIR>/usr/local/lib/$APPNAME/restconf</CLICON_RESTCONF_DIR>
  <CLICON_RESTCONF_PRETTY>false</CLICON_RESTCONF_PRETTY>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>/usr/local/var/$APPNAME</CLICON_XMLDB_DIR>
  <CLICON_MODULE_LIBRARY_RFC7895>true</CLICON_MODULE_LIBRARY_RFC7895>
  $RESTCONFIG
</clixon-config>
EOF

new "test params: -f $cfg"
if [ $BE -ne 0 ]; then
    new "kill old backend"
    sudo clixon_backend -z -f $cfg
    if [ $? -ne 0 ]; then
	err
    fi
    new "start backend -s init -f $cfg"
    start_backend -s init -f $cfg

    new "waiting"
    wait_backend
fi

new "kill old restconf"
stop_restconf_pre

new "1)check no restconf"
ps=$(ps aux|grep "$WWWDIR/clixon_restconf" | grep -v grep)
if [ -n "$ps" ]; then
    err "No restconf running" "$ps"
fi

new "2)check status off"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>status</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><status xmlns=\"http://clicon.org/lib\">false</status></rpc-reply>]]>]]>"

new "start restconf"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>start</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><ok/></rpc-reply>]]>]]>"

new "3)check restconf on"
ps=$(ps aux|grep "$WWWDIR/clixon_restconf -f $cfg" | grep -v grep)
if [ -z "$ps" ]; then
    err "restconf running"
fi

new "4)check status on"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>status</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><status xmlns=\"http://clicon.org/lib\">true</status></rpc-reply>]]>]]>"

new "stop restconf"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>stop</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><ok/></rpc-reply>]]>]]>"

new "start restconf again"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>start</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><ok/></rpc-reply>]]>]]>"

new "5)check restconf on"
ps=$(ps aux|grep "$WWWDIR/clixon_restconf -f $cfg" | grep -v grep)
if [ -z "$ps" ]; then
    err "A restconf running"
fi

new "kill restconf"
stop_restconf_pre

new "6)check no restconf"
ps=$(ps aux|grep "$WWWDIR/clixon_restconf" | grep -v grep)
if [ -n "$ps" ]; then
    err "No restconf running" "$ps"
fi

new "restart restconf"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>restart</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><ok/></rpc-reply>]]>]]>"

new "7)check status on"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>status</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><status xmlns=\"http://clicon.org/lib\">true</status></rpc-reply>]]>]]>"

pid0=$(pgrep clixon_restconf)

new "restart restconf"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>restart</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><ok/></rpc-reply>]]>]]>"

new "8)check status on"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>status</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><status xmlns=\"http://clicon.org/lib\">true</status></rpc-reply>]]>]]>"

new "9)check new pid"
pid1=$(pgrep clixon_restconf)
if [ $pid0 -eq $pid1 ]; then
    err "Different pids" "pid0:$pid0 = pid1:$pid1"
fi

if [ $BE -ne 0 ]; then
    new "Kill backend"
    # Check if premature kill
    pid=$(pgrep -u root -f clixon_backend)
    if [ -z "$pid" ]; then
	err "backend already dead"
    fi
    # kill backend
    stop_backend -f $cfg

    new "10)check no restconf"
    ps=$(ps aux|grep "$WWWDIR/clixon_restconf" | grep -v grep)
    if [ -n "$ps" ]; then
	err "No restconf running" "$ps"
    fi
fi

if false; then # Work in progress
#-------------------------------
# Now in a separate network namespace
new "restconf rpc in network namespace"
netns=xxx
sudo ip netns delete $netns
#sudo ip netns add $netns

new "test params: -f $cfg"
if [ $BE -ne 0 ]; then
    new "kill old backend"
    sudo clixon_backend -z -f $cfg
    if [ $? -ne 0 ]; then
	err
    fi
    new "start backend -s init -f $cfg -- -n $netns"
    start_backend -s init -f $cfg -- -n $netns

    new "waiting"
    wait_backend
fi

new "kill old restconf"
stop_restconf_pre

new "netconf start restconf"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>start</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><ok/></rpc-reply>]]>]]>"

new "10)check status on"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>status</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><status xmlns=\"http://clicon.org/lib\">true</status></rpc-reply>]]>]]>"

new "stop restconf"
expecteof "$clixon_netconf -qf $cfg" 0 "<rpc $DEFAULTNS><process-control xmlns=\"http://clicon.org/lib\"><name>restconf</name><operation>stop</operation></process-control></rpc>]]>]]>" "<rpc-reply $DEFAULTNS><ok/></rpc-reply>]]>]]>"

if [ $BE -ne 0 ]; then
    new "Kill backend"
    # Check if premature kill
    pid=$(pgrep -u root -f clixon_backend)
    if [ -z "$pid" ]; then
	err "backend already dead"
    fi
    # kill backend
    stop_backend -f $cfg

    new "11)check no restconf"
    ps=$(ps aux|grep "$WWWDIR/clixon_restconf" | grep -v grep)
fi

sudo ip netns delete $netns

fi # namespaces

rm -rf $dir
