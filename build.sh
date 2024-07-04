#!/bin/bash
workpath=`pwd`

echo ${workpath}

if [ ! -d "${workpath}/build" ];then
    mkdir ${workpath}/build
fi

cd ${workpath}/build && cmake ..
make

cd ${workpath}/shell && ./install.sh