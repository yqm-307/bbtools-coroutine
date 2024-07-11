#!/bin/bash

checkexec=$1    # 检测文件路径
outputfile=$2   # 结果输出文件

############
# 
# 使用valgrind生成内存分析的profiler
# 
############
valgrind \
    --tool=memcheck                         `# 使用valgrind中memcheck工具` \
    --leak-check=yes                        `# 泄露检测开启` \
    --read-inline-info=no                   `# 内联函数检测，开启后会有更完善的堆栈信息，以及多到爆的无所谓的警告（警告大多是由第三方库带来的）` \
    --log-file=${outputfile}                `# 检测报告输出文件` \
    ./${checkexec}                          `# 可执行文件` \