#!/bin/bash
workpath=$1

if [ ! -d "${workpath}" ];then
    echo dir ${workpath} not exist!
    exit -1
fi

echo enter ${workpath}
cd ${workpath}

if [ -d "${workpath}/build" ];then
    rm -rf ${workpath}/build
    echo delete build
fi

mkdir build && cd build

cmake .. -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON
if [ $? != 0 ];then
    echo ERROR cmake error
    exit -1
fi

ninja -j$(nproc)
if [ $? != 0 ];then
    echo ERROR ninja error
    exit -1
fi

cd ${workpath}
