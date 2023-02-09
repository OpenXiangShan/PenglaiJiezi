#!/bin/bash
#
# @Author       : zhuoli
# @Date         : 2022-11-08 20:48:26
# @LastEditTime : 2022-11-08 20:48:26
# @LastEditors  : Please set LastEditors
# @Description  : test script for gem5
# @FilePath     : /GEM5/scripts/test_all.sh

cd /gem5_common/xs-env
source env.sh

cd /data/landun/workspace/
python3 scripts/test_sc2006_simpoint.py --tasks $1 --repo $2 --job_id $3 --repo_output $4 --action $5

if [ $? -ne 0 ]; then
    echo -e "GEM5: exec FAILED!!!"
    exit 2
fi
echo -e "GEM5: SUCCESS"
