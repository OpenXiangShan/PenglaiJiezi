#!/bin/bash
#
# @Author       : zhuoli
# @Date         : 2022-11-07 14:19:57
# @LastEditTime : 2022-01-11 14:25:36
# @LastEditors  : Please set LastEditors
# @Description  : environment file, sourced by build.sh
# @FilePath     : /GEM5/scripts/env.sh

#-----> base dir, the directory above scripts(i.e. GEM5/)
export GEM5_BASE_DIR=$(cd "$(dirname ${BASH_SOURCE[0]})/.."; pwd )

#-----> script dir, the directory above scripts(i.e. GEM5/scripts/)
export GEM5_SCRIPTS_DIR=${GEM5_BASE_DIR}/scripts

#funcs
Print_Error() {
    echo -e "\n-----> ERROR: "
    errorInfo=$1
    echo -e "$errorInfo"
    echo ""
}

Print_Info() {
    echo -e "\n-----> INFO: "
    info=$1
    echo -e "$info"
}

Print_Progress() {
    step=$1
    echo -e "\n================================"
    echo "$step"
    echo -e "================================\n"
}



