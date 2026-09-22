#!/bin/bash
# 卸载本仓安装的产物。只删除 install.sh 中列出的自有路径，
# 不动 bbt/core 下归 bbtools-infra 的其它路径。

installpath="/usr/local/include"
libpath="/usr/local/lib"

sudo rm -rf $installpath/bbt/coroutine
sudo rm -rf $installpath/bbt/pollevent
sudo rm -rf $installpath/bbt/core/clock
sudo rm -rf $installpath/bbt/core/errcode
sudo rm -rf $installpath/bbt/core/thread
sudo rm -f  $installpath/bbt/core/Attribute.hpp
sudo rm -f  $installpath/bbt/core/Define.hpp
sudo rm -f  $installpath/bbt/core/util/Assert.hpp
sudo rm -f  $installpath/bbt/core/util/Result.hpp
sudo rm -f  $installpath/bbt/core/log/DebugPrint.hpp
sudo rm -f  $libpath/libbbt_coroutine.so

echo "删除完毕"
