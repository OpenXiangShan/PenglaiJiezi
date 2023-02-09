# -*- coding:utf8 -*-
"""
    author: zhuoli@tencent.com
    time  : 2022/11/10 11:07
    desc  : test SpecCpu2006 simpoint
"""
import os
import sys
import argparse
import time
import subprocess
from subprocess import STDOUT
from multiprocessing import Process
import json
import math

DEBUG = 0

if 1:
    I_MAX_TASK_NUM = 1062
    I_MIN_TASK_NUM = 0
    S_DIR_COMMON = '/gem5_common_trace'
    S_FILE_TASK_INFO = S_DIR_COMMON + '/task_info_spec06_rv64gcb_o2_20m.txt'
    S_FILE_SCRIPT = S_DIR_COMMON + \
                    '/env-scripts/perf/tencent_spectest_gem5_new.py'
    S_TASK_CPT = './simpoint/spec06_rv64gcb_o2_20m/take_cpt'
    S_TRACE = '/trace_spec06_rv64gcb_o2_20m/'
else:
    I_MAX_TASK_NUM = 996
    I_MIN_TASK_NUM = 0
    S_DIR_COMMON = '/gem5_common'
    S_FILE_TASK_INFO = S_DIR_COMMON + '/task_info.txt'
    S_FILE_SCRIPT = S_DIR_COMMON + \
                    '/env-scripts/perf/tencent_spectest_gem5_new.py'
    S_TASK_CPT = './nemu_take_simpoint_cpt_06'

S_OUTPUT_DIR = './simpoint_cpt_06_output'
S_URL_PREFIX = 'https://mirrors.tencent.com/repository/generic'
S_REPO_OUTPUT = 'gem5_test_output'
S_COLLECT_DIR = '/trace_spec06_rv64gcb_o2_20m'
S_COLLECT_REPO_PREFIX = 'gem5_input_part'


def shell_system(scmd):
    r"""
        func:
            shell exec cmd
        Parameters:
            -- scmd: shell cmd, like "ls /"
        Returns:
            -- s_cmd_out: log
            -- i_retcode: return code
        Note:
            N/A
    """

    if DEBUG:
        print(scmd)

    p = subprocess.Popen(scmd, shell=True,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    s_cmd_out = p.stdout.read()
    p.stdout.close()
    i_retcode = p.poll()

    if DEBUG:
        print("log:", s_cmd_out)

    return s_cmd_out.decode('utf-8'), i_retcode


def exec_download(s_cmd_download, s_cmd_unzip):

    i_ret_code = os.system(s_cmd_download + ' >/dev/null 2>&1')
    if i_ret_code:
        print('err: download failed %d' % i_ret_code)

    i_ret_code = os.system(s_cmd_unzip + ' >/dev/null 2>&1')
    if i_ret_code:
        print('err: unzip failed %d' % i_ret_code)


def get_file_dir(s_directory):
    r"""
        func:
            ls directory
        Parameters:
            -- s_directory: directory
        Returns:
            -- d_name: dict of input files
        Note:
            {
              "0a3d1dcdb9": "0a3d1dcdb9.mp4_720x1280.hevc"
            }
    """

    l_file_names = os.listdir(s_directory)

    d_name = {}
    for s_file in l_file_names:
        d_name[s_file.split('.')[0]] = s_file

    return d_name


def parse_args():
    r"""
        func:
            parse args
        Parameters:
            -- void
        Returns:
            -- o_parser:
        Note:
            N/A
    """
    s_example = '''[example] decode :
                    python3 test_sc2006_simpoint.py
                   --tasks 1,35-54,99-900
                   --repo gem5_input_part1
                   --repo_output /qianlnzhang/GEM5/decoupled-tage-dev/0b/
                   --job_id 1
                   --s_action normal '''

    o_parser = argparse.ArgumentParser(formatter_class=
                                       argparse.ArgumentDefaultsHelpFormatter,
                                       epilog=s_example)

    if len(sys.argv) <= 1:
        o_parser.print_help()
        sys.exit(1)

    o_parser.add_argument('-V', '--version', action='version',
                          version='tools version: 1.0')

    o_parser.add_argument('--tasks', dest='s_tasks',
                          help="for examples: notice: 9,100:125,44:46",
                          type=str,
                          default='[1]')

    o_parser.add_argument('--repo', dest='s_repo',
                          help="available repo: gem5_input_part1 to 9",
                          type=str,
                          default='gem5_input_part1')

    o_parser.add_argument('--repo_output', dest='s_repo_output',
                          help="$BK_CI_GIT_REPO_NAME/$BK_CI_GIT_REPO_BRANCH/"
                               "$BK_CI_GIT_REPO_HEAD_COMMIT_ID/",
                          type=str,
                          default='')

    o_parser.add_argument('--job_id', dest='s_job_id',
                          help="job id of landun", type=str, default='')

    o_parser.add_argument('--action', dest='s_action',
                          help="[normal, collect, replay]",
                          type=str, default='normal')

    o_parser.add_argument('--debug', dest='i_debug',
                          help="--debug [0] or [1]", type=int, default=0)

    return o_parser.parse_args()


def check_and_get_args(o_args):
    r"""
        func:
            check and get args
        Parameters:
            -- o_args
        Returns:
            -- i_ret_code: return code  0, -1
            -- i_ret_msg: return msg
            -- d_tasks_params: task info: {'type': single or range,
                                           'start':,
                                           'end': }
        Note:
            N/A
    """

    d_tasks_params = {}

    if o_args.i_debug:
        DEBUG = 1

    # repo
    if o_args.s_repo not in ('gem5_input_part1', 'gem5_input_part2',
                             'gem5_input_part3', 'gem5_input_part4',
                             'gem5_input_part5', 'gem5_input_part6',
                             'gem5_input_part7', 'gem5_input_part8',
                             'gem5_input_part9'):
        return -1, 'err: invalid --repo % ' % o_args.s_repo, d_tasks_params
    print('##### --repo: %s' % o_args.s_repo)

    # repo_output
    if not o_args.s_repo_output:
        return -1, 'err: --repo_output is null '
    print('##### --repo_output: %s' % o_args.s_repo_output)

    # job_id
    if not o_args.s_job_id.isdigit():
        return -1, 'err: --job_id is invalid '
    print('##### --job_id: %s' % o_args.s_job_id)

    # action
    if o_args.s_action not in ('normal', 'collect', 'replay'):
        return -1, 'err: --action is invalid '
    print('##### --action: %s' % o_args.s_action)

    # tasks
    if o_args.s_tasks:
        l_tmp = o_args.s_tasks.split(',')
        for i, l in enumerate(l_tmp):
            if l.isdigit():
                if int(l) > I_MAX_TASK_NUM or int(l) < I_MIN_TASK_NUM:
                    return -1, 'err: task_index should be in [0, %d], ' \
                               'but get %d' % (I_MAX_TASK_NUM, int(l)), \
                           d_tasks_params

                d_tasks_params[i] = {'type': 'single', 'start': int(l)}
                print('##### --task: %d' % int(l))
            else:
                if ':' not in l:
                    return -1, 'err: invalid task_index %s ' % l, \
                           d_tasks_params

                l_range = l.split(':')

                if len(l_range) != 2:
                    return -1, 'err: invalid task_index %s ' % l, \
                           d_tasks_params

                if not l_range[0].isdigit() or not l_range[1].isdigit():
                    return -1, 'err: task_index should be in [0, %d], ' \
                               'but get %s' % (I_MAX_TASK_NUM, l), \
                           d_tasks_params

                d_tasks_params[i] = {'type': 'range',
                                     'start': int(l_range[0]),
                                     'end': int(l_range[1])}
                print('##### --tasks: %d to %d' % (int(l_range[0]),
                                                   int(l_range[1])))

        return 0, '', d_tasks_params
    else:
        return -1, 'err: o_args.s_tasks is null', d_tasks_params


def read_task_info(s_path_file):
    r"""
        func:
            read task_info.txt
        Parameters:
            -- s_path_file: local path of task_info.txt
        Returns:
            -- i_retcode: return code  0, -1
            -- i_retmsg: return msg
            -- d_task_info: task info: [index] [task_name]
        Note:
            N/A
    """
    l_tmp = []
    d_task_info = {}

    if not os.path.exists(s_path_file):
        return -1, 'err: task_info.txt does not exists', d_task_info

    try:
        with open(s_path_file, 'r') as f:
            l_tmp = f.readlines()

        if not l_tmp:
            return -1, 'err: task_info.txt is empty', d_task_info

        for s_line in l_tmp:
            s_line = s_line.strip()
            if 'task_name' in s_line:
                continue

            l_task = s_line.split()  # l_task = 1 52 astar_*.00693044
            d_task_info[l_task[0].strip()] = {'task_name': l_task[2].strip(),
                                              'task_url': l_task[3].strip()}
        print('##### task_number: %d' % len(d_task_info))
        return 0, '', d_task_info
    except Exception as err:
        return -1, 'err: %s' % err, d_task_info


def check_ret(i_ret_code, s_ret_msg, i_last=0):
    if i_ret_code:
        print(s_ret_msg)
        exit(-1)
    elif i_last == 1:
        print(s_ret_msg)
        exit(0)


def exec_task(s_start, s_end, s_action):
    r"""
        func:
            exec
        Parameters:
            -- s_start   : start index
            -- s_end     : end index
            -- s_action  : normal, collect, replay
        Returns:
            N/A
        Note:
            N/A
    """

    if s_action == 'normal':
        s_trace = ''
    elif s_action == 'collect':
        s_trace = '--trace-collect'
    else:
        s_trace = '--trace-replay'

    s_task_cpt = '--gcpt_path %s' % S_TASK_CPT

    if s_start == s_end:
        s_cmd = 'python3 %s %s ' \
                '--rerun %s %s' \
                % (S_FILE_SCRIPT, s_task_cpt, s_start, s_trace)
    else:
        s_cmd = 'python3 %s %s ' \
                '--rerun_start %s --rerun_end %s %s' \
                % (S_FILE_SCRIPT, s_task_cpt, s_start, s_end, s_trace)

    print('##### exec_cmd: %s' % s_cmd)
    os.system(s_cmd)


def task_exec(s_repo, d_tasks_params, d_task_info, s_action, s_job_id):
    r"""
        func:
            download input -> start tasks
        Parameters:
            -- s_repo             : repo
            -- d_tasks_params     : task params {'type': single or range,
                                                'start':, 'end': }
            -- d_task_info        : task info [index]{task_name, task_url}
            -- s_action           : normal, collect, replay
            -- s_job_id           : job_id
        Returns:
            -- i_ret_code         : return code  0, -1
            -- i_ret_msg          : return msg
        Note:
            N/A
    """

    # s1: download
    if s_action == 'replay':
        i = int(s_job_id)
        s_zip_name = '%d.zip' % i
        s_prefix = S_URL_PREFIX + '/'
        s_zip_url = s_prefix + s_zip_name

        s_tmp_download_repo = s_prefix + S_COLLECT_REPO_PREFIX \
                              + str(math.ceil(i/2))
        s_zip_url = s_tmp_download_repo + \
                    S_COLLECT_DIR + '/' + s_zip_name

        s_cmd_download = 'rm -rf ./%s; wget %s' % (s_zip_name, s_zip_url)
        s_cmd_unzip = 'unzip %s' % s_zip_name

        print('##### --jobs: %d' % i)
        print(s_cmd_download)
        print(s_cmd_unzip)
        exec_download(s_cmd_download, s_cmd_unzip)

    # download simpoint
    for i_count, key in enumerate(d_tasks_params):
        if d_tasks_params[key]['type'] == 'single':
            s_task_index = str(d_tasks_params[key]['start'])
            if s_task_index not in d_task_info:
                return -1, \
                       'err: d_tasks_params[][start] ' \
                       'not in d_task_info %s' % key

            # s_task_name = d_task_info[s_task_index]['task_name']
            # astar_biglakes_0_0.000252016
            s_task_url = d_task_info[s_task_index]['task_url']

            s_cmd_download = 'curl %s/%s%s --create-dirs  -o .%s ' \
                             '>/dev/null 2>&1' \
                             % (S_URL_PREFIX, s_repo,
                                s_task_url, s_task_url)
            print('##### download_cmd: %s' % s_cmd_download)
            os.system(s_cmd_download)
            # todo: md5 check

        elif d_tasks_params[key]['type'] == 'range':
            s_task_index_start = str(d_tasks_params[key]['start'])
            s_task_index_end = str(d_tasks_params[key]['end'])

            for i in range(int(s_task_index_start), int(s_task_index_end)):
                print(i,d_task_info[str(i)], s_task_index_end)
                if s_task_index_start not in d_task_info \
                        or str(int(s_task_index_end) - 1) \
                        not in d_task_info:
                    return -1, \
                           'err: d_tasks_params[][start] or ' \
                           '[end] not in d_task_info %s' % key

                s_task_url = d_task_info[str(i)]['task_url']
                s_cmd_download = 'curl %s/%s%s --create-dirs -o .%s ' \
                                 '>/dev/null 2>&1' \
                                 % (S_URL_PREFIX, s_repo,
                                    s_task_url, s_task_url)
                print('##### download_cmd: %s' % s_cmd_download)
                os.system(s_cmd_download)
                # todo: md5 check
        else:
            return -1, 'err: unknown d_tasks_params type %s' \
                   % d_tasks_params[key]['type']

    # s2: exec
    process_list = []
    print('\n')
    for i_count, key in enumerate(d_tasks_params):
        if d_tasks_params[key]['type'] == 'single':
            s_start = str(d_tasks_params[key]['start'])
            s_end = s_start
        else:  # range
            s_start = str(d_tasks_params[key]['start'])
            s_end = str(d_tasks_params[key]['end'])

        p = Process(target=exec_task, args=(s_start, s_end, s_action))
        process_list.append(p)
        p.start()

    for p in process_list:
        p.join()

    return 0, ''


def action_upload(s_repo, s_dst_path, s_src_file):

    s_ret_msg = ''
    s_cmd = 'curl --request PUT ' \
            '-u zhuoli:bf53afe05e6411ed8aad3e4b4729d578 ' \
            '--url https://mirrors.tencent.com/repository/generic/%s%s ' \
            '--upload-file %s ' \
            '>/dev/null 2>&1' % (s_repo, s_dst_path, s_src_file)

    print('\n##### upload_cmd: %s' % s_cmd)
    i_ret_code = os.system(s_cmd)

    if i_ret_code:
        s_ret_msg = 'err: upload failed'
        print("error: %s %s" % (s_cmd, s_ret_msg))

    return i_ret_code, s_ret_msg


def output_upload(o_args):
    r"""
        func:
            upload output
        Parameters:
            -- s_repo             : repo
        Returns:
            -- i_ret_code         : return code  0, -1
            -- i_ret_msg          : return msg
        Note:
            N/A
    """

    # s1: rm trace
    if o_args.s_action == 'replay':
        s_cmd_rm = 'rm -rf %s/*/collect/system.' \
                   'cpu.branch_trace.trace.branch_trace' % S_OUTPUT_DIR
        print(s_cmd_rm)
        os.system(s_cmd_rm)

    # s2: zip
    s_zip_name = o_args.s_job_id + '.zip'
    s_cmd = 'rm -rf %s; zip -r %s %s/* ' % (s_zip_name, s_zip_name,
                                            S_OUTPUT_DIR)
    print('##### zip_cmd: %s' % s_cmd)
    os.system(s_cmd)  # todo

    # s3: upload
    s_src_file = s_zip_name
    if o_args.s_action == 'normal':
        s_repo = S_REPO_OUTPUT
        s_dst_path = o_args.s_repo_output
    elif o_args.s_action == 'collect':
        s_repo = o_args.s_repo
        s_dst_path = S_TRACE
    elif o_args.s_action == 'replay':
        s_repo = S_REPO_OUTPUT
        s_dst_path = o_args.s_repo_output
    else:
        raise Exception('err: unknown type')

    return action_upload(s_repo, s_dst_path, s_src_file)


def run():

    # s1: init
    print('\n################################ step1.1: parse cmd')
    o_args = parse_args()
    i_ret_code, s_ret_msg, d_tasks_params = check_and_get_args(o_args)
    check_ret(i_ret_code, s_ret_msg)

    print('\n################################ step1.2: read task')
    # [k: index] [v: task_name]
    i_ret_code, s_ret_msg, d_task_info = read_task_info(S_FILE_TASK_INFO)
    check_ret(i_ret_code, s_ret_msg)

    # s2: exec
    print('\n################################ step2: parse')
    i_ret_code, s_ret_msg = task_exec(o_args.s_repo, d_tasks_params,
                                      d_task_info, o_args.s_action,
                                      o_args.s_job_id)
    check_ret(i_ret_code, s_ret_msg)

    # s3: output
    print('\n################################ step3: upload output')
    i_ret_code, s_ret_msg = output_upload(o_args)
    check_ret(i_ret_code, s_ret_msg, 1)


if __name__ == "__main__":
    run()
