#!/bin/bash

exec_path=$1

for((i=0; i<100000; ++i))
do
    echo "---- turn ${i} ----"
    ./${exec_path}
    if [ $? != 0 ];then
        echo "have bug, please debug"
        exit -1
    else
        echo "succ"
    fi
done
