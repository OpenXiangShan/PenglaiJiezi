#!/bin/bash
#
# @Author       : zhuoli
# @Date         : 2022-11-07 16:48:26
# @LastEditTime : 2022-01-11 16:48:26
# @LastEditors  : Please set LastEditors
# @Description  : build script for gem5
# @FilePath     : /GEM5/scripts/build.sh

#cd build.sh directory, users can run this build.sh script from anywhere
CURDIR=$(cd $(dirname ${BASH_SOURCE[0]}); pwd )
cd $CURDIR

source env.sh

function clean_gem5()
{
    rm -rf $GEM5_BASE_DIR/build/RISCV
}

function build_gem5()
{
    mkdir $GEM5_BASE_DIR/build
    echo -e `nproc`
    python3.6 $(which scons-3) build/RISCV/gem5.opt -j`nproc`
}

function git_pull_env_scripts()
{
    rm -rf /gem5_common_trace/env-scripts
    cd /gem5_common_trace
    git clone https://git.woa.com/qianlnzhang/env-scripts.git
    
    if [ $? -ne 0 ]; then
        Print_Error "end: GEM5: FAILED, git clone env-scripts failed!!!"
        exit 2
    fi

    cd -
}

function build_top()
{
    Print_Progress "start"
    
    Print_Progress "step0: git clone env-scripts"
    git_pull_env_scripts
    cp -r /gem5_common_trace/DRAMsim3 /data/landun/workspace/ext/dramsim3/

    Print_Progress "step1: clean GEM5"
    clean_gem5

    Print_Progress "step2: build GEM5"
    build_gem5

    if [ $? -ne 0 ]; then
        Print_Error "end: GEM5: FAILED, BUILD ABORT!!!"
        exit 2
    fi
    Print_Progress "end: GEM5: SUCCESS"
}

cd $GEM5_BASE_DIR
build_top


exit 0
