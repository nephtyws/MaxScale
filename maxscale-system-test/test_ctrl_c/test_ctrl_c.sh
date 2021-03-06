#!/bin/bash

sudo systemctl stop maxscale || sudo service maxscale stop

hm=`pwd`
$hm/start_killer.sh &
if [ $? -ne 0 ] ; then
        exit 1
fi

T="$(date +%s)"

sudo maxscale -d -U root
if [ $? -ne 0 ] ; then
	exit 1
fi

T="$(($(date +%s)-T))"
echo "Time in seconds: ${T} (including 5 seconds before kill)"

if [ "$T" -lt 10 ] ; then
        echo "PASSED"
        exit 0
else
        echo "FAILED"
        exit 1
fi

