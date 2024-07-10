#!/bin/bash

workpath=${1}

if [ ! -d ${workpath} ];then
    echo "不存在的路径"
    exit -1
fi

echo "存在的路径"