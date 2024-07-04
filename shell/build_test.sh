#!/bin/bash

workpath=`pwd`

if [ -d "${workpath}/build" ];then
    rm -rf ${workpath}/build
fi

mkdir build && cd build
cmake ..
make -j2
cd ..