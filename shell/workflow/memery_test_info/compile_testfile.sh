#!/bin/bash

# workpath 应该是项目根路径
workpath=${1}

if [ ! -d ${workpath} ];then
    echo "workpath ${workpath} not exist"
    exit -1
fi

if [ -d ${workpath}/build ];then
    rm -rf build
fi

mkdir build && cd build
# 只生成benchmark测试文件
cmake -DNEED_BENCHMARK=ON -DNEED_TEST=OFF -DPROFILE=OFF -DNEED_EXAMPLE=OFF ..

valgrind --tool=memcheck --leak-check=yes  --read-inline-info=no  --track-origins=yes --log-file=mem_check.log ./mem_check_test 