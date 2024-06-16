#!/bin/bash

exec_path=$1

for((i=0; i<100000; ++i))
do
    ./${exec_path}
    if [ $? != 0 ];then
        echo "have bug, please debug"
    fi
done
