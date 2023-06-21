#!/bin/bash
#
# @Author       : zhuoli
# @Date         : 2023-06-20 20:48:26
# @LastEditTime : 2022-06-20 20:48:26
# @LastEditors  : Please set LastEditors
# @Description  : report script for gem5 spec2006
# @FilePath     : /GEM5/scripts/run_spec2006_report.sh

#-----> base dir, the directory above scripts(i.e. GEM5/)
export GEM5_BASE_DIR=$(cd "$(dirname ${BASH_SOURCE[0]})/.."; pwd )
source $GEM5_BASE_DIR/scripts/env.sh

function print_help()
{
    echo "
    Usage:
        --gcpt_path: directory of spec 2006 checkpoint
        --gem5: workspace of GEM5 
        --json_path: path of simpoint summary json file
        --dir_profiling: dir of nemu spec 2006 profiling
        --dir_spec_reftime: dir of spec 2006 ref time

    exmaple:

        python3  $GEM5_SCRIPTS_DIR/lib/tencent_spectest_gem5_opensource.py \\
                --gcpt_path /cdata/simpoint/spec06_rv64gcb_o2_20m/take_cpt \\
                --gem5 /cdata/zhuoli/git/GEM5 \\
                --json_path /gem5_common_trace/spec06_rv64gcb_o2_20m/json/simpoint_summary.json \\
                --dir_profiling /gem5_common/sdata/proj/cpu/incoming/simpoint/spec06_rv64gcb_o2_20m/logs/profiling \\
                --dir_spec_reftime /cdata/speccpu2006/benchspec/CPU2006 \\
                --report
        
        or

        bash $GEM5_SCRIPTS_DIR/run_spec2006_report.sh /cdata/simpoint/spec06_rv64gcb_o2_20m/take_cpt  \\
             /cdata/zhuoli/git/GEM5 \\
             /gem5_common_trace/spec06_rv64gcb_o2_20m/json/simpoint_summary.json \\
             /gem5_common/sdata/proj/cpu/incoming/simpoint/spec06_rv64gcb_o2_20m/logs/profiling \\
             /cdata/speccpu2006/benchspec/CPU2006
        "
}

if [ $# -ne 5 ]; then
  echo -e "\nError: Invalid number of arguments."
  print_help
  exit 1
fi

cmd="python3  $GEM5_SCRIPTS_DIR/lib/tencent_spectest_gem5_opensource.py --gcpt_path $1 --gem5 $2 --json_path $3 \
              --dir_profiling $4 --dir_spec_reftime $5 --report"
echo -e $cmd
${cmd}
