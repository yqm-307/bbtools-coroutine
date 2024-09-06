#!/bin/bash
workpath=`pwd`

echo ${workpath}

if [ ! -d "${workpath}/build" ];then
    mkdir ${workpath}/build
fi

cd ${workpath}/build
cmake -DRELEASE=ON -DNEED_EXAMPLE=ON ..
make

cd ${workpath}/shell && ./install.sh