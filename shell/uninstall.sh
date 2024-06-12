#!/bin/bash

installpath="/usr/local/include"
libpath="/usr/local/lib"

sudo rm -rf $installpath/bbt/coroutine
sudo rm -rf $libpath/libbbt_coroutine.so

echo "删除完毕"