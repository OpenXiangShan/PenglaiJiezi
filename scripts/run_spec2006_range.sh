#!/bin/bash
#
# @Author       : zhuoli
# @Date         : 2023-06-20 20:48:26
# @LastEditTime : 2022-06-20 20:48:26
# @LastEditors  : Please set LastEditors
# @Description  : test script for gem5 spec2006 range checkpoints
# @FilePath     : /GEM5/scripts/run_spec2006_range.sh

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
        --rerun_start: checkpoint start num
        --rerun_end: checkpoint end num
        --threads: checkpoints run in parallel, default is `nproc` - 1

    exmaple:
        python3  $GEM5_SCRIPTS_DIR/lib/tencent_spectest_gem5_opensource.py \\
                --gcpt_path /cdata/simpoint/spec06_rv64gcb_o2_20m/take_cpt \\
                --gem5 /cdata/zhuoli/git/GEM5 \\
                --json_path /gem5_common_trace/spec06_rv64gcb_o2_20m/json/simpoint_summary.json \\
                --rerun_start 0 \\
                --rerun_end 20 \\
                --threads `nproc`
        or

        bash $GEM5_SCRIPTS_DIR/run_spec2006_range.sh /cdata/simpoint/spec06_rv64gcb_o2_20m/take_cpt \\
             /cdata/zhuoli/git/GEM5 \\
             /gem5_common_trace/spec06_rv64gcb_o2_20m/json/simpoint_summary.json \\
             0 \\
             20 \\
             `nproc`
        "

}

if [ $# -ne 6 ]; then
  echo -e "\nError: Invalid number of arguments."
  print_help
  exit 1
fi

cmd="python3  $GEM5_SCRIPTS_DIR/lib/tencent_spectest_gem5_opensource.py --gcpt_path $1 --gem5 $2 \
              --json_path $3 --rerun_start $4 --rerun_end $5  --threads $6"
echo -e $cmd
${cmd}
