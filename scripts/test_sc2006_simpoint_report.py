# -*- coding:utf8 -*-
"""
    author: zhuoli@tencent.com
    time  : 2022/11/14 16:31
    desc  : report test SpecCpu2006 simpoint
"""
import os
import sys
import argparse
import time
import json
import math
from test_sc2006_simpoint import shell_system, check_ret, S_FILE_SCRIPT, \
    S_OUTPUT_DIR, S_URL_PREFIX, S_REPO_OUTPUT, \
    S_COLLECT_DIR, S_COLLECT_REPO_PREFIX, exec_download
from multiprocessing import Process


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
                    python3 test_sc2006_simpoint_report.py
                   --jobs_num 16
                   --repo_dir /qianlnzhang/GEM5/decoupled-tage-dev/0b/
                   --action replay'''

    o_parser = argparse.ArgumentParser(formatter_class=
                                       argparse.ArgumentDefaultsHelpFormatter,
                                       epilog=s_example)

    if len(sys.argv) <= 1:
        o_parser.print_help()
        sys.exit(1)

    o_parser.add_argument('-V', '--version', action='version',
                                 version='tools version: 1.0')

    o_parser.add_argument('--jobs_num', dest='i_jobs_num',
                          help="jobs number of landun",
                          type=int,
                          default=0)

    o_parser.add_argument('--repo_dir', dest='s_repo_dir',
                          help="$BK_CI_GIT_REPO_NAME/$BK_CI_GIT_REPO_BRANCH/"
                               "$BK_CI_GIT_REPO_HEAD_COMMIT_ID/",
                          type=str,
                          default='')

    o_parser.add_argument('--action', dest='s_action',
                          help="[normal, collect, replay]",
                          type=str,
                          default='normal')

    return o_parser.parse_args()


def check_args(o_args):
    r"""
        func:
            check
        Parameters:
            -- o_args
        Returns:
            -- i_ret_code: return code  0, -1
            -- i_ret_msg: return msg
        Note:
            N/A
    """

    # jobs_num
    if not o_args.i_jobs_num:
        return -1, 'err: invalid --jobs_num % ' % o_args.i_jobs_num
    print('##### --jobs_num: %s' % o_args.i_jobs_num)

    # repo
    if not o_args.s_repo_dir:
        return -1, 'err: invalid --repo % ' % o_args.s_repo_dir
    else:
        print('##### --repo: %s' % o_args.s_repo_dir)
        if o_args.s_repo_dir[-1] != '/':
            o_args.s_repo_dir += '/'

    # action
    if o_args.s_action not in ('normal', 'collect', 'replay'):
        return -1, 'err: --action is invalid '
    print('##### --action: %s' % o_args.s_action)

    return 0, ''


def download(i_jobs_num, s_action, s_repo_dir):
    r"""
        func:
            check
        Parameters:
            -- i_jobs_num
            -- s_action
            -- s_repo_dir
        Returns:
            -- i_ret_code: return code  0, -1
            -- i_ret_msg: return msg
        Note:
            https://mirrors.tencent.com/repository/generic/gem5_test_output
            /qianlnzhang/GEM5/decoupled-tage-dev/b82fad3fd1e457f9755/1.zip
    """

    l_process_list = []

    if s_action == 'normal' or s_action == 'replay':
        s_prefix = S_URL_PREFIX + '/' + S_REPO_OUTPUT + s_repo_dir
    elif s_action == 'collect':
        s_prefix = S_URL_PREFIX + '/'
    else:
        raise Exception('err: unknown type %s' % s_action)

    for i in range(1, i_jobs_num + 1):

        s_zip_name = '%d.zip' % i
        s_zip_url = s_prefix + s_zip_name

        if s_action == 'collect':
            s_tmp_download_repo = s_prefix + S_COLLECT_REPO_PREFIX \
                                  + str(math.ceil(i/2))
            s_zip_url = s_tmp_download_repo + \
                        S_COLLECT_DIR + '/' + s_zip_name

        s_cmd_download = 'rm -rf ./%s; wget %s' % (s_zip_name, s_zip_url)
        s_cmd_unzip = 'unzip %s' % s_zip_name

        print('##### --jobs: %d' % i)
        print(s_cmd_download)
        print(s_cmd_unzip)

        p = Process(target=exec_download, args=(s_cmd_download, s_cmd_unzip))
        l_process_list.append(p)
        p.start()

    for p in l_process_list:
        p.join()

    return 0, ''


def report(s_action):

    if s_action == 'normal':
        s_suffix = ''
    elif s_action == 'collect':
        s_suffix = '--trace-collect'
    else:  # replay
        s_suffix = '--trace-replay'

    s_cmd = 'python3 %s --report %s' % (S_FILE_SCRIPT, s_suffix)
    print('##### scmd: %s' % s_cmd)
    s_s_cmd_out, i_ret_code = shell_system(s_cmd)

    if i_ret_code:
        return -1, 'err: report failed %d' % i_ret_code
    else:
        return 0, s_s_cmd_out


def run():

    # s1: init
    print('\n################################ step1: parse cmd')
    o_args = parse_args()
    i_ret_code, s_ret_msg = check_args(o_args)
    check_ret(i_ret_code, s_ret_msg)

    # s2: download and unzip 1~N.zip
    print('\n################################ step2: download')
    i_ret_code, s_ret_msg = download(o_args.i_jobs_num, o_args.s_action,
                                     o_args.s_repo_dir)
    check_ret(i_ret_code, s_ret_msg)

    # s3: report
    print('\n################################ step3: report')
    i_ret_code, s_ret_msg = report(o_args.s_action)
    check_ret(i_ret_code, s_ret_msg)

    # s4: generate html
    s_report = "<p>" + s_ret_msg.replace("\n", "<br>") + "</p>"
    s_html = '''<!DOCTYPE html>
                <html lang="en">
                <body>
                %s
                </body>
                </html>''' % s_report

    with open('test.html', 'w') as f:
        f.writelines(s_html)

    # s5: report check simpoints >= 1061
    if '1061' in s_ret_msg:
        print(s_ret_msg)
        exit(0)
    else:
        print(s_ret_msg)
        exit(-1)


if __name__ == "__main__":
    run()
