#! /usr/bin/env python3

import argparse
import json
import os
import random
import shutil
import signal
import subprocess
import time
from multiprocessing import Process, Queue, cpu_count
import sys
import perf
import spec_score
import socket
import time
import logging

CPU_COUNT = cpu_count()

def get_current_nodeip():
    hostname = socket.gethostname()
    ip = socket.gethostbyname(hostname)
    return ip


def get_gcpt_range(gcpt_all):

    start_job = 0
    end_job = len(gcpt_all)
    s_job_range = f"start job: {start_job}; end job: {end_job}, \
    the last one is not included\n"

    logging.info(s_job_range)
    print(s_job_range)

    return gcpt_all


class GCPT(object):

    STATE_NONE = 0
    STATE_RUNNING = 1
    STATE_FINISHED = 2
    STATE_ABORTED = 3

    def __init__(self, base_path, benchspec, point, weight):
        self.base_path = base_path
        self.benchspec = benchspec
        self.point = point
        self.weight = weight
        self.state = self.STATE_NONE
        self.num_cycles = -1
        self.num_instrs = -1
        self.ipc = -1
        self.num_seconds = -1

    def get_path(self):
        dir_name = self.__str__()
        print(f"dir_name: {dir_name}")
        bin_dir = os.path.join(self.base_path, dir_name, "0")
        print(f"bin_dir: {bin_dir}")
        bin_file = list(os.listdir(bin_dir))
        bin_path = os.path.join(bin_dir, bin_file[0])
        print(f"bin_path: {bin_path}")
        assert (os.path.isfile(bin_path))
        return bin_path

    def __str__(self):
        return "_".join([self.benchspec, self.point, str(self.weight)])

    def result_path(self, base_path):
        return os.path.join(base_path, self.__str__())

    def err_path(self, base_path):
        return os.path.join(self.result_path(base_path), "m5.err")

    def stats_path(self, base_path):
        return os.path.join(self.result_path(base_path), "stats.txt")

    def out_path(self, base_path):
        return os.path.join(self.result_path(base_path), "m5.out")

    def get_state(self, base_path):
        self.state = self.STATE_NONE
        if os.path.exists(self.out_path(base_path)):
            self.state = self.STATE_RUNNING
            with open(self.out_path(base_path)) as f:
                for line in f:
                    if "ABORT at pc" in line or \
                       "FATAL:" in line or "Error:" in line:
                        self.state = self.STATE_ABORTED
                    elif "max instruction count" in line or \
                        "GOOD TRAP" in line:
                        self.state = self.STATE_FINISHED
                    else:
                        if "cycleCnt = " in line:
                            cycle_cnt_str = line.split("cycleCnt =")[
                                1].split(", ")[0]
                            self.num_cycles = int(
                                cycle_cnt_str.replace(",", "").strip())
                        if "instrCnt = " in line:
                            instr_cnt_str = line.split("instrCnt =")[
                                1].split(", ")[0]
                            self.num_instrs = int(
                                instr_cnt_str.replace(",", "").strip())
                        if "Host time spent" in line:
                            second_cnt_str = line.split("Host time spent:")[
                                1].replace("ms", "")
                            self.num_seconds = int(
                                second_cnt_str.replace(",", "").strip()) / 1000
        else:
            logging.info(f"m5.out not exist: {self.out_path(base_path)}")
        return self.state

    def get_simulation_cps(self):
        return int(round(self.num_cycles / self.num_seconds))

    def get_ipc(self):
        return round(self.num_instrs / self.num_cycles, 3)

    def state_str(self):
        state_strs = ["S_NONE", "S_RUNNING", "S_FINISHED", "S_ABORTED"]
        return state_strs[self.state]

    def show(self, base_path, benchId, worker_ip):
        self.get_state(base_path)
        attributes = {
            "instrCnt": self.num_instrs,
            "cycleCnt": self.num_cycles,
            "totalIPC": f"{self.get_ipc():.3f}",
            "simSpeed": self.get_simulation_cps()
        }
        attributes_str = ", ".join(
            map(lambda k: f"{k:>8} = {str(attributes[k]):>9}",
                attributes))

        logging.info(
            f"GCPT[{benchId}] running on {worker_ip}: \
            {str(self):>50}: {self.state_str():>10}")


def load_all_gcpt(gcpt_path, json_path, bench_name, state_filter=None,
                  xs_path=None, sorted_by=None):
    perf_filter = [
        ("l3cache_mpki_load", lambda x: float(x) < 3),
        ("branch_prediction_mpki", lambda x: float(x) > 5),
    ]
    perf_filter = None
    all_gcpt = []
    print(f"gcpt_path: {gcpt_path}")
    print(f"bench_name: {bench_name}")
    print(f"state_filter: {state_filter}")

    with open(json_path) as f:
        data = json.load(f)

    counter = 0
    for benchspec in data:
        for point in data[benchspec]:
            weight = data[benchspec][point]
            gcpt = GCPT(gcpt_path, benchspec, point, weight)
            # print(f"creating gcpt: {gcpt}")
            if state_filter is None and perf_filter is None:
                if bench_name == 'all' or benchspec == bench_name:
                    all_gcpt.append(gcpt)
                    # print("all_gcpt appending")
                continue
            perf_match, state_match = True, True
            if state_filter is not None:
                state_match = False
                perf_base_path = get_perf_base_path(xs_path)

                if gcpt.get_state(perf_base_path) in state_filter:
                    state_match = True

                if gcpt.get_state(perf_base_path) == 0:
                    logging.info(
                        f"Reporting: {benchspec}_{point}_{weight}: \
                        state = NONE")
                elif gcpt.get_state(perf_base_path) == 1:
                    logging.info(
                        f"Reporting: {benchspec}_{point}_{weight}: \
                        state = RUNNING")
                elif gcpt.get_state(perf_base_path) == 2:
                    logging.info(
                        f"Reporting: {benchspec}_{point}_{weight}: \
                        state = FINISHED")
                elif gcpt.get_state(perf_base_path) == 3:
                    logging.info(
                        f"Reporting: {benchspec}_{point}_{weight}: \
                        state = ABORTED")

            if state_match and perf_filter is not None:
                perf_path = gcpt.err_path(get_perf_base_path(xs_path))
                counters = perf.gem5PerfCounters(perf_path)
                print(f"get_all_mainip(): {get_all_manip()}")
                counters.add_manip(get_all_manip())
                for fit in perf_filter:
                    if not fit[1](counters[fit[0]]):
                        perf_match = False

            if perf_match and state_match:
                if bench_name == 'all' or benchspec == bench_name:
                    all_gcpt.append(gcpt)

            counter = counter + 1

    if sorted_by is not None:
        all_gcpt = sorted(all_gcpt, key=sorted_by)

    dump_jobs = True
    counter = 0
    if dump_jobs:
        for gcpt in all_gcpt:
            if state_filter is None:
                counter = counter + 1

    with open(args.gcpt_path + '/task_info.txt', 'w') as f:
        for i, l in enumerate(all_gcpt):
            s_task_info = "%d, %s_%s_%s\n" % \
                          (i, l.benchspec, l.point, l.weight)
            f.write(s_task_info)

    print("task_info.txt in %s\n" % (args.gcpt_path + '/task_info.txt'))

    return all_gcpt


def get_perf_base_path(xs_path):
    return os.path.join(xs_path, args.outdir)


def xs_run(workloads, gem5_dir, gem5_base_arg, threads):

    emu_path = os.path.join(gem5_dir, "build/RISCV/gem5.opt")
    config_path = os.path.join(gem5_dir, "configs/example/fs.py")

    proc_count, finish_count = 0, 0
    max_pending_proc = threads
    pending_proc, error_proc = [], []
    free_cores = list(range(max_pending_proc))

    # skip CI cores
    ci_cores = []  # list(range(0, 64))# + list(range(32, 48))
    for core in list(map(lambda x: x // threads, ci_cores)):
        if core in free_cores:
            free_cores.remove(core)
            max_pending_proc -= 1
    logging.info("Free cores:%s", free_cores)
    print("Free cores:%s", free_cores)
    try:
        while len(workloads) > 0 or len(pending_proc) > 0:
            has_pending_workload = len(workloads) > 0 and len(
                pending_proc) >= max_pending_proc
            has_pending_proc = len(pending_proc) > 0
            if has_pending_workload or has_pending_proc:
                finished_proc = list(
                    filter(lambda p: p[1].poll() is not None, pending_proc))
                for workload, proc, core in finished_proc:
                    logging.info(f"{workload} has finished")
                    print((f"{workload} has finished"))
                    sys.stdout.flush()
                    pending_proc.remove((workload, proc, core))
                    free_cores.append(core)
                    if proc.returncode != 0:
                        logging.info(
                            f"[ERROR] {workload} exits with code \
                            {proc.returncode}")
                        print(
                            f"[ERROR] {workload} exits with code \
                            {proc.returncode}")
                        sys.stdout.flush()
                        error_proc.append(workload)
                        continue
                    finish_count += 1
                if len(finished_proc) == 0:
                    time.sleep(1)
            can_launch = max_pending_proc - len(pending_proc)
            for workload in workloads[:can_launch]:
                if len(pending_proc) < max_pending_proc:
                    allocate_core = free_cores[0]

                    workload_path = workload.get_path()
                    print(f"wkload_path: {workload_path}")
                    cpt_para = "--generic-rv-cpt=" + workload_path

                    perf_base_path = get_perf_base_path(gem5_dir)
                    result_path = workload.result_path(perf_base_path)
                    if not os.path.exists(result_path):
                        os.makedirs(result_path, exist_ok=True)
                    stdout_file = workload.out_path(perf_base_path)
                    stderr_file = workload.err_path(perf_base_path)
                    stats_file = workload.stats_path(perf_base_path)
                    out_dir = "--outdir=" + result_path

                    # print(f"result_path: {result_path}")
                    print(f"stdout_file: {stdout_file}")
                    print(f"stderr_file: {stderr_file}")
                    print(f"stats_file: {stats_file}")

                    with open(stdout_file, "w") as stdout, \
                    open(stderr_file, "w") as stderr:
                        random_seed = random.randint(0, 9999)
                        run_cmd = [emu_path] + [out_dir] + \
                                    [config_path] + gem5_base_arg + [cpt_para]

                        cmd_str = " ".join(run_cmd)
                        print(f"cmd_str:{cmd_str}")
                        logging.info(f"cmd {proc_count}: {cmd_str}")
                        proc = subprocess.Popen(
                            run_cmd, stdout=stdout, stderr=stderr,
                            preexec_fn=os.setsid)
                    pending_proc.append((workload, proc, allocate_core))
                    free_cores = free_cores[1:]
                    proc_count += 1
            workloads = workloads[can_launch:]
    except KeyboardInterrupt:
        logging.info("Interrupted. Exiting all programs ...")
        logging.info("Not finished:")
        for i, (workload, proc, _) in enumerate(pending_proc):
            os.killpg(os.getpgid(proc.pid), signal.SIGINT)
            logging.info(f"  ({i + 1}) {workload}")
        logging.info("Not started:")
        for i, workload in enumerate(workloads):
            logging.info(f"  ({i + 1}) {workload}")

    if len(error_proc) > 0:
        logging.info("Errors:")
        for i, workload in enumerate(error_proc):
            logging.info(f"  ({i + 1}) {workload}")


def get_all_manip():
    all_manip = []
    ipc = perf.PerfManip(
        name="IPC",
        counters=[f"cpu.totalIpc"],
        func=lambda totalIpc: totalIpc * 1.0
    )
    all_manip.append(ipc)
    return all_manip


def get_total_inst(simpoint_profiling_path, benchspec, spec_version, isa):
    if spec_version == 2006:
        if isa == "rv64gcb":
            base_path = simpoint_profiling_path
            filename = benchspec + ".log"
            bench_path = os.path.join(base_path, filename)
        else:
            logging.info("Unknown ISA\n")
            return None
    else:
        logging.info("Unknown SPEC version\n")
        return None

    f = open(bench_path)
    for line in f:
        if "total guest instructions" in line:
            f.close()
            return int(line.split("instructions = ")[1].replace("\x1b[0m", ""))
    return None


def xs_report_ipc(xs_path, gcpt_queue, result_queue):

    while not gcpt_queue.empty():
        gcpt = gcpt_queue.get()
        # print(f"Processing {str(gcpt)}...")
        perf_path = gcpt.stats_path(get_perf_base_path(xs_path))
        # print(f"perf_path: {perf_path}")
        counters = perf.gem5PerfCounters(perf_path)
        # print(f"counters: {counters.get_counters()}")
        counters.add_manip(get_all_manip())
        # when the spec has not finished, IPC may be None
        if counters["IPC"] is not None:
            result_queue.put(
                [gcpt.benchspec, [float(gcpt.weight), float(counters["IPC"])]])
        else:
            logging.info("IPC not found in", gcpt.benchspec,
                         gcpt.point, gcpt.weight)


def xs_report(all_gcpt, xs_path, profiling_path, dir_spec_reftime,
              spec_version, isa, num_jobs):
    # frequency/GHz
    frequency = 2
    gcpt_ipc = dict()
    keys = list(map(lambda gcpt: gcpt.benchspec, all_gcpt))
    for k in keys:
        gcpt_ipc[k] = []

    # multi-threading for processing the performance counters
    gcpt_queue = Queue()

    for gcpt in all_gcpt:
        gcpt_queue.put(gcpt)

    result_queue = Queue()
    process_list = []

    for _ in range(num_jobs):
        p = Process(target=xs_report_ipc, args=(
            xs_path, gcpt_queue, result_queue))
        process_list.append(p)
        p.start()

    for p in process_list:
        p.join()

    while not result_queue.empty():
        result = result_queue.get()
        gcpt_ipc[result[0]].append(result[1])

    print("=================== Coverage ==================")
    spec_time = {}

    for benchspec in gcpt_ipc:
        print(f"benchspec:{benchspec}")
        total_weight = sum(map(lambda info: info[0], gcpt_ipc[benchspec]))
        total_cpi = sum(
            map(lambda info: info[0] / info[1],
                gcpt_ipc[benchspec])) / total_weight
        num_instr = get_total_inst(profiling_path,
                                   benchspec, spec_version, isa)
        num_seconds = total_cpi * num_instr / (frequency * (10 ** 9))
        print(f"{benchspec:>25} coverage: {total_weight:.2f}")
        spec_name = benchspec.split("_")[0]
        spec_time[spec_name] = spec_time.get(spec_name, 0) + num_seconds

    spec_score.get_spec_score(dir_spec_reftime,
                              spec_time, spec_version, frequency)
    print(f"Number of Checkpoints: {len(all_gcpt)}")
    print(f"SPEC CPU Version: SPEC CPU{spec_version}, {isa}")


def parse_argument():

    parser = argparse.ArgumentParser(description="autorun script for xs")
    parser.add_argument('--gcpt_path', metavar='gcpt_path', type=str,
                         required=True,  help='path to gcpt checkpoints')

    parser.add_argument('--gem5', type=str, required=True,
                        help='path to gem5: example:/data/Simulator/gem5')

    parser.add_argument('--json_path', type=str, required=True,
                        help='summary file name')

    parser.add_argument('--outdir', type=str,
                        default="simpoint_cpt_06_output",
                        help='dir to output, will appear in gem5/')

    parser.add_argument('--rerun', default=-1, type=int,
                        help='rerun one specific bench')

    parser.add_argument('--rerun_start', default=-1, type=int,
                        help='specify some benchIDs to rerun, \
                        start ID is included')

    parser.add_argument('--rerun_end', default=-1, type=int,
                        help='specify some benchIDs to rerun, \
                        end ID is NOT included')

    parser.add_argument('--report', '-R', action='store_true',
                        default=False, help='report only')

    parser.add_argument('--bench', default="all", type=str,
                        help='the benchmark name, i.e.: \
                        all/mcf/gcc_g23')

    parser.add_argument('--ref', default=None, type=str,
                        help='path to ref')

    parser.add_argument('--warmup', '-W', default=20000000,
                        type=int, help="warmup instr count")

    parser.add_argument('--max-instr', '-I', default=40000000,
                        type=int, help="max instr count")

    parser.add_argument('--threads', '-T', default=CPU_COUNT - 1,
                        type=int, help="number of emu threads")

    parser.add_argument('--version', default=2006,
                        type=int, help='SPEC version')

    parser.add_argument('--dir_spec_reftime',
                        type=str,
                        help='''directory of spec reftime''')

    parser.add_argument('--dir_profiling',
                        type=str,
                        help='''directory of nemu profiling log, for example:
                        ./simpoint/spec06_rv64gcb_o2_20m/logs/profiling''')

    parser.add_argument('--isa', default="rv64gcb",
                        type=str, help='ISA version')

    parser.add_argument('--jobs', '-j', default=1, type=int,
                        help="processing files in 'j' threads for report")

    args = parser.parse_args()

    return args

max
def init(args):

    # step1: mkdir output dir
    log_dir = args.gem5 + '/' + args.outdir
    if not os.path.exists(log_dir):
        os.mkdir(log_dir)

    # step2: set ref
    if args.ref is None:
        args.ref = args.gem5

    # step3: init base arg
    gem5_base_arg = ["--caches", "--l2cache",
                     "--xiangshan-system", "--cpu-type=DerivO3CPU",
                    "--mem-type=DRAMsim3", "--dramsim3-ini=" +
                    args.gem5 + "/xiangshan_DDR4_8Gb_x8_2400.ini",
                    "--mem-size=8GB", "--cacheline_size=64",
                    "--l1i_size=64kB", "--l1i_assoc=8",
                    "--l1d_size=64kB", "--l1d_assoc=8",
                    "--l1d-hwp-type=SignaturePathPrefetcherToLowerLevel",
                    "--l2_size=1MB", "--l2_assoc=8",
                    "--l2-hwp-type=SignaturePathPrefetcher",
                    "--l3cache", "--l3_size=6MB", "--l3_assoc=6",
                    "--enable-difftest",
                    "--warmup-insts-no-switch=20000010",
                    "--maxinsts=40000010"]

    # step4: log
    current_time = time.strftime("%Y.%m.%d.%H.%M.%S", time.localtime())
    log_file = get_current_nodeip() + '-' + current_time + '.log'
    report_log_file = get_current_nodeip() + '-' + current_time \
                      + '-report' + '.log'

    if args.report:
        fname = report_log_file
    else:
        fname = log_file

    logging.basicConfig(filename=log_dir + '/' + fname,
                        format='[%(asctime)s-%(filename)s]:  %(message)s',
                        level=logging.INFO, filemode='a',
                        datefmt='%Y-%m-%d%I:%M:%S %p')
    print(f"log_file:  {log_dir}/{fname}")

    # step5: print args
    for k in args.__dict__:
        logging.info(k + ": " + str(args.__dict__[k]))

    # step6: load gcpt
    gcpt_all = load_all_gcpt(args.gcpt_path, args.json_path,
                             args.bench)
    logging.info("All:  %s", len(gcpt_all))

    return log_dir, gcpt_all, gem5_base_arg


if __name__ == "__main__":

    # step1:
    args = parse_argument()

    # step2: init
    log_dir, gcpt_all, gem5_base_arg = init(args)

    # step3: get checkpoint
    if (args.rerun != -1):
        gcpt = gcpt_all[args.rerun:args.rerun + 1]
    elif (args.rerun_start != -1 and args.rerun_end != -1
          and args.rerun_end > args.rerun_start):
        gcpt = gcpt_all[args.rerun_start:args.rerun_end]
    else:
        gcpt = get_gcpt_range(gcpt_all)

    # step4: report or run
    if args.report:
        gcpt = load_all_gcpt(args.gcpt_path, args.json_path, args.bench,
                             state_filter=[GCPT.STATE_FINISHED],
                             xs_path=args.ref,
                             sorted_by=lambda x: x.benchspec.lower())
        print(f"gcpt: {gcpt}")
        xs_report(gcpt, args.ref, args.dir_profiling, args.dir_spec_reftime,
                  args.version, args.isa, args.jobs)
    else:
        logging.info("For Current Node:  %d jobs", len(gcpt))
        logging.info("First: %s", gcpt[0])
        logging.info("Last: %s", gcpt[-1])
        xs_run(gcpt, args.gem5, gem5_base_arg, args.threads)
