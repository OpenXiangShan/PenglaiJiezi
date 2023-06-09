./build/RISCV/gem5.debug  configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=DecoupledBPU --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BingoPrefetcher --num-cpus=1 -I=40000000 --generic-rv-cpt=bzip2_chicken_109600000000_0.015136_5600001000_.gz --branch-trace-collect --branch-trace-file branch.trace

./util/decode_branch_trace.py ./m5out/system.cpu.branch_trace.trace.branch_trace branch.trace

./build/RISCV/gem5.opt --outdir=/data/zql/Simulator/gem5-woa/m5out/bzip2_chicken_109600000000_0.015136/collect ./configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=DecoupledBPU --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BingoPrefetcher --num-cpus=1 -I=40000000 --warmup-insts-no-switch=20000000 --generic-rv-cpt=./nemu_take_simpoint_cpt_06/bzip2_chicken_109600000000_0.015136/0/_5600001000_.gz --branch-trace-collect --branch-trace-file bzip2_chicken_109600000000_0.trace


# collect hello world trace
./build/RISCV/gem5.debug --outdir=m5out/hello ./configs/example/se.py --cpu-type=DerivO3CPU --bp-type=LTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-size=8GB --l2-hwp-type=BingoPrefetcher -c tests/test-progs/hello/bin/riscv/linux/hello.nofpu --num-cpus=1  --branch-trace-collect --branch-trace-file hello.trace

# replay hello world trace
./build/RISCV/gem5.debug --outdir=m5out/hello ./configs/example/se.py --cpu-type=DerivO3CPU --bp-type=LTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-size=8GB --l2-hwp-type=BingoPrefetcher -c tests/test-progs/hello/bin/riscv/linux/hello.nofpu --num-cpus=1  --branch-trace-replay --branch-trace-file m5out/hello/sy^Cem.cpu.branch_trace.trace.hello.trace

#collect dhrystone
 ./build/RISCV/gem5.debug --outdir=m5out/dhrystone ./configs/example/se.py --cpu-type=DerivO3CPU --bp-type=LTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-size=8GB --l2-hwp-type=BingoPrefetcher -c  /data/zql/riscv_software/dhrystone/v2.2/dry2_riscv_static  --num-cpus=1  --branch-trace-collect --branch-trace-file dhrystone.trace

#replay dhrystone
./build/RISCV/gem5.opt  ./configs/example/se.py --cpu-type=DerivO3CPU --bp-type=LTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-size=8GB --l2-hwp-type=BingoPrefetcher -c tests/test-progs/dhrystone/bin/dry2_riscv_static_1000   --num-cpus=1  --branch-trace-replay --branch-trace-file ./m5out/dhrystone/trace/system.cpu.branch_trace.trace.dhrystone_1000.trace 



# bzip collect
./build/RISCV/gem5.debug --debug-flags=AddrRanges,ExecAll,Fetch,Commit  ./configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=DecoupledBPU --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BingoPrefetcher --num-cpus=1 -I=40000000 --warmup-insts-no-switch=20000000 --generic-rv-cpt=bzip2_chicken_109600000000_0.015136_5600001000_.gz  --branch-trace-collect --branch-trace-file bzip2_chicken_109600000000_0.trace  &>bzip_org.log 
# bzip replay
 ./build/RISCV/gem5.debug --debug-flags=AddrRanges,Commit,Fetch,ExecAll configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=DecoupledBPU --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BingoPrefetcher --num-cpus=1 -I=40000000 --warmup-insts-no-switch=20000000 --generic-rv-cpt=bzip2_chicken_109600000000_0.015136_5600001000_.gz  --branch-trace-replay --branch-trace-file ./m5out/bzip2_chicken_109600000000_0.015136/collect/system.cpu.branch_trace.trace.bzip2_chicken_109600000000_0.trace &>bzip.log


# bzip collect
./build/RISCV/gem5.debug --debug-flags=TLB,TLBVerbose,AddrRanges,ExecAll,Fetch,Commit --outdir=m5out/bzip_temp/collect ./configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=DecoupledBPU --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BingoPrefetcher --num-cpus=1 -I=1000  --generic-rv-cpt=bzip2_chicken_109600000000_0.015136_5600001000_.gz  &>bzip_org.log 
# bzip replay
 ./build/RISCV/gem5.debug --debug-flags=TLB,TLBVerbose,AddrRanges,Commit,Fetch,ExecAll --outdir=m5out/bzip_temp/replay configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=DecoupledBPU --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BingoPrefetcher --num-cpus=1 -I=1000  --generic-rv-cpt=bzip2_chicken_109600000000_0.015136_5600001000_.gz  --branch-trace-replay --branch-trace-file m5out/bzip_temp/collect/system.cpu.branch_trace.trace.bzip.trace &>bzip_wrong.log
#bzip replay mpki
./build/RISCV/gem5.opt --outdir=m5out/bzip2_chicken_109600000000_0/mpki configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=DecoupledBPU --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BingoPrefetcher --num-cpus=1 -I=40000000 --warmup-insts-no-switch=20000000 --generic-rv-cpt=bzip2_chicken_109600000000_0.015136_5600001000_.gz --branch-trace-replay --branch-trace-file /data/zql/Simulator/gem5-woa/m5out/bzip2_chicken_109600000000_0/collect/system.cpu.branch_trace.trace.bzip2_chicken_109600000000_0.trace --branch-misspred-penalty 20 --branch-MPKI 900

#maprobe
./build/RISCV/gem5.opt --debug-flags=O3PipeView,O3CPUAll --debug-file=trace_new.out configs/example/se.py --cpu-type=DerivO3CPU --bp-type=LTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l1d-hwp-type=SignaturePathPrefetcher --l2-hwp-type=SignaturePathPrefetcher --num-cpus=1 -c tests/test-progs/maprobe/bin/maprobe

perf_calibrate: csr_read_cycle 16
perf_calibrate: csr_read_ninst 4
Memory throughput:
mem band width 3.615580 B/cycle (512 samples)
L1 latency:
range 0x1000B (5 iters) latency test
range 0x1000B (5 intrs) read latency 11.553125 (320 samples)
range 0x8000B (2 iters) latency test
range 0x8000B (2 intrs) read latency 10.197266 (1024 samples)
range 0x10000B (2 iters) latency test
range 0x10000B (2 intrs) read latency 10.080078 (2048 samples)

#debug maprobe
./build/RISCV/gem5.opt --debug-flags=Fetch,ExecAll --outdir=m5out/maprobe/normal  configs/example/se.py --cpu-type=DerivO3CPU --bp-type=LTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BOPPrefetcher --num-cpus=1  -c tests/test-progs/maprobe/bin/maprobe &>maprobe_normal.log

 ./build/RISCV/gem5.opt --debug-flags=ExecAll --outdir=m5out/maprobe/replay  configs/example/se.py --cpu-type=DerivO3CPU --bp-type=LTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BOPPrefetcher --num-cpus=1 --branch-trace-replay --branch-trace-file m5out/maprobe/collect/system.cpu.branch_trace.trace.maprobe.trace  -c tests/test-progs/maprobe/bin/maprobe &>maprobe_replay_exec.log

./build/RISCV/gem5.opt --debug-flags=ExecAll --outdir=m5out/maprobe/collect  configs/example/se.py --cpu-type=DerivO3CPU --bp-type=LTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BOPPrefetcher --num-cpus=1 --branch-trace-collect --branch-trace-file maprobe.trace  -c tests/test-progs/maprobe/bin/maprobe &>maprobe_collect_exec.log


#checkpoint
./build/RISCV/gem5.opt configs/example/fs.py --caches --l2cache --enable-difftest --xiangshan --cpu-type=DerivO3CPU --bp-type=LTAGE --indirect-bp-type=ITTAGE --mem-size=8GB --mem-type=DRAMsim3 --dramsim3-ini=./xiangshan_DDR4_8Gb_x8_2400.ini --cacheline_size=64 --l1i_size=64kB --l1i_assoc=8 --l1d_size=64kB --l1d_assoc=8 --l2_size=1MB --l2_assoc=8 --l2-hwp-type=SMSPrefetcher --l3cache --l3_size=6MB --l3_assoc=6 --warmup-insts-no-switch=20000010 --maxinsts=40000010 --generic-rv-cpt=/data/zql/benchmarks/simpoint_local/spec06_rv64gcb_o2_20m/take_cpt/GemsFDTD_1041040000000_0.022405/0/_1041040000000_.gz


#hello
./build/RISCV/gem5.opt configs/example/se.py --cpu-type=DerivO3CPU --bp-type=LTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=BOPPrefetcher --num-cpus=1 -c tests.bak/test-progs/hello/bin/riscv/linux/hello.nofpu


#cmd from xiangshan
build/RISCV/gem5.opt configs/example/fs.py --caches --l2cache --enable-difftest --xiangshan --cpu-type=DerivO3CPU --generic-rv-cpt=cpt.gz --bp-type=LTAGE --indirect-bp-type=ITTAGE --mem-size=8GB --mem-type=DRAMsim3 --dramsim3-ini xiangshan_DDR4_8Gb_x8_2400.ini --cacheline_size=64 --l1i_size=64kB --l1i_assoc=8 --l1d_size=64kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l2-hwp-type=SMSPrefetcher --l3cache --l3_size=6MB --l3_assoc=6 --warmup-insts-no-switch=20000010 --maxinsts=40000010


# raw-cpt
./build/RISCV/gem5.opt configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=LTAGE --indirect-bp-type=ITTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=SMSPrefetcher --num-cpus=1 -I=40000000 --generic-rv-cpt=/data/zql/Simulator/GEM5-internal/tests/test-progs/mem-violation/build/mem-violation-riscv64-xs-flash.bin --raw-cpt

#debug ssit
./build.old/RISCV/gem5.opt --outdir=m5out/SSIT_old configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=LTAGE --indirect-bp-type=ITTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=SMSPrefetcher --num-cpus=1 -I=40000000 --generic-rv-cpt=tests/test-progs/mem-violation/build/mem-violation-riscv64-xs-flash.bin --raw-cpt

./build/RISCV/gem5.opt --outdir=m5out/SSIT_new configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=LTAGE --indirect-bp-type=ITTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=SMSPrefetcher --num-cpus=1 -I=40000000 --generic-rv-cpt=tests/test-progs/mem-violation/build/mem-violation-riscv64-xs-flash.bin --raw-cpt


./build/RISCV/gem5.opt --outdir=m5out/SSIT_new/ configs/example/fs.py --caches --l2cache --enable-difftest --xiangshan --cpu-type=DerivO3CPU --generic-rv-cpt=cpt.gz --bp-type=LTAGE --indirect-bp-type=ITTAGE --mem-size=8GB --mem-type=DRAMsim3 --dramsim3-ini xiangshan_DDR4_8Gb_x8_2400.ini --cacheline_size=64 --l1i_size=64kB --l1i_assoc=8 --l1d_size=64kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l2-hwp-type=SMSPrefetcher --l3cache --l3_size=6MB --l3_assoc=6 --warmup-insts-no-switch=20000010 --maxinsts=40000010 --generic-rv-cpt=/data/zql/benchmarks/simpoint_local/spec06_rv64gcb_o2_20m/take_cpt/GemsFDTD_1041040000000_0.022405/0/_1041040000000_.gz

./build.old/RISCV/gem5.opt --outdir=m5out/SSIT_old/ configs/example/fs.py --caches --l2cache --enable-difftest --xiangshan --cpu-type=DerivO3CPU --generic-rv-cpt=cpt.gz --bp-type=LTAGE --indirect-bp-type=ITTAGE --mem-size=8GB --mem-type=DRAMsim3 --dramsim3-ini xiangshan_DDR4_8Gb_x8_2400.ini --cacheline_size=64 --l1i_size=64kB --l1i_assoc=8 --l1d_size=64kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l2-hwp-type=SMSPrefetcher --l3cache --l3_size=6MB --l3_assoc=6 --warmup-insts-no-switch=20000010 --maxinsts=40000010 --generic-rv-cpt=/data/zql/benchmarks/simpoint_local/spec06_rv64gcb_o2_20m/take_cpt/GemsFDTD_1041040000000_0.022405/0/_1041040000000_.gz


#debug frontend
./build/RISCV/gem5.debug --outdir=m5out.xs  --debug-flags=O3CPUAll ./configs/example/se.py --cpu-type=O3CPU --caches -c ./tests/test-progs/hello/bin/riscv/linux/hello.nofpu>./m5out.xs/debug.log


#dhrystone fs
./build/RISCV/gem5.debug  configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=LTAGE --indirect-bp-type=ITTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=SMSPrefetcher --num-cpus=1 -I=40000000 --generic-rv-cpt=/data/zql/xs-env-1230/nexus-am/apps/dhrystone/build/dhrystone-riscv64-xs-flash.bin   --raw-cpt   --difftest-ref-so /data/zql/xs-env-github/NEMU/build/riscv64-nemu-interpreter-so

#maprobe
./build/RISCV/gem5.opt --outdir=m5out/maprobe configs/example/fs.py --xiangshan-system --enable-difftest --cpu-type=DerivO3CPU --bp-type=LTAGE --indirect-bp-type=ITTAGE --caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 --mem-type=DDR4_2400_16x4 --mem-size=8GB --l2-hwp-type=SMSPrefetcher --num-cpus=1 -I=40000000 --generic-rv-cpt=/data/zql/xs-env-projects/xs-env-nanhuV2/nexus-am/apps/maprobe/build/maprobe-riscv64-xs-flash.bin --raw-cpt --difftest-ref-so /data/zql/xs-env-projects/xs-env-github/NEMU/build/riscv64-nemu-interpreter-so

