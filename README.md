# About

JIEZI Simulator is a high-performance RISC-V performance simulator developed by Tencent Penglai Laboratory. The current version of Spec cpu 2006 is 13.9 points/GHz, 12.2 points/GHz specint 2006, 15.6 points/GHz specfp 2006. We have gone through two stages:

1. The micro-architecture of key performance components are aligned with the micro-architecture of Xiangshan Nanhu CPU.
2. Then continue performance optimization and micro-architecture exploration base on 1.

Performance optimization and microarchitectural exploration including:

- **Front-end**: Implemented Enhanced SC Predictor,  Loop Predictor, and RAS Predictor based on speculative chain-list stack  and commit stack,  and Icache performance-related optimization.
- **Back-end**: Analyze and optimize the configuration of out-of-order components such as LSQ and ROB. Implemented a mixed RMAP and HBMAP solution to enhance the rename table recovery solution. Merge the move eliminate feature in XS-gem5, and fix a bug which a long-term occupation of physical registers that not be released.
- **Memory System**: Implimented the Bingo and SPP Prefetcher, which are in mixed Cache Level and prefetch to Current or Low Level Cache.

Key performance component alignments including:

- **Front-end**: Align Decoupled BPU, fetch bundle-based uBTB, BTB, TAGE, ITAGE, and FTQ components, align 4-stage instruction fetch pipeline stages and instruction prefetch performance.
- **Back-end**: Align the function, quantity, and delay of execution units such as Branch Unit and ALU, and align the width of instruction issue and commit;
- **Memory system**: Align the behavior and specifications of the Memory Voilation Predictor Storeset, and align some of the cache configuration parameters and performance  statistics.
  
  The front-end is contributed by **yuweiyan (严余伟)** and **henglong (龙衡)**, the back-end is contributed by **denniskhu (胡凯)** and **zhuoli (李卓)**, and the memory system is contributed by **qianlnzhang (张乾龙)** and **deweichchen (陈德炜)**.

# A Short Doc

## GCC & libboost

- Use GCC > 9.4.0.
- Install libboost.

## Building gem5

- [official building docs](https://www.gem5.org/documentation/general_docs/building)
- git clone GEM5
- DRAMSim3
  
  ```shell
  cd ext/dramsim3
  git clone https://github.com/umd-memsys/DRAMsim3.git
  cd DRAMsim3 && mkdir build
  cd build
  cmake ..
  make
  
  Notes:
      - Must rebuild gem5 after install DRAMSim3
      - Must use DRAMSim3 with our costumized config
  
  gem5.opt ... --mem-type=DRAMsim3 --dramsim3-ini=$gem5_home/xiangshan_DDR4_8Gb_x8_2400.ini ...
  ```
- mold linker(5 times faster than the default linker)
  
  ```shell
  wget https://github.com/rui314/mold/releases/download/v1.10.1/mold-1.10.1-x86_64-linux.tar.gz
  tar zxvf mold-1.10.1-x86_64-linux.tar.gz
  cp -r mold-1.10.1-x86_64-linux/bin/* /usr/local/bin
  cp -r mold-1.10.1-x86_64-linux/lib/* /usr/local/lib/
  cp -r mold-1.10.1-x86_64-linux/libexec/* /usr/local/libexec/
  cp -r mold-1.10.1-x86_64-linux/share/* /usr/local/share/
  ```
- xs-env
  
  ```shell
  https://xiangshan-doc.readthedocs.io/zh_CN/latest/tools/xsenv/
  ```
- gem5
  
  ```shell
  python3 $(which scons-3) build/RISCV/gem5.opt  --linker=mold -j`nproc`
  or 
  bash scripts/build.sh
  ```

## Bringup

```shell
bash scripts/run_dhrystone.sh   # with print "Dhrystone PASS"
  bash scripts/run_helloWorld.sh  # with print "Hello world!"
```

## Produce RVGCpt checkpoints with NEMU

Please refer to [the checkpoint tutorial for Xiangshan](https://xiangshan-doc.readthedocs.io/zh_CN/latest/tools/simpoint/)
and [Build Linux kernel for Xiangshan](https://github.com/OpenXiangShan/XiangShan-doc/blob/main/tutorial/others/Linux%20Kernel%20%E7%9A%84%E6%9E%84%E5%BB%BA.md)

The process of SimPoint checkpointing includes ***3 individual steps***

1. SimPoint Profiling to get BBVs. (To save space, they often output in compressed formats such as **bbv.gz**.)
2. SimPoint clustering. You can also opt to Python and sk-learn to do k-means clustering. (In this step, what is typically obtained are the **positions** selected by SimPoint and their **weights**.)
3. Taking checkpoints according to clustering results. (In the RVGCpt process, this step generates the **checkpoints** that will be used for simulation.)

If you have problem generating SPECCPU checkpoints, following links might help you.

- [The video to build SPECCPU, put it in Linux, and run it in NEMU to get SimPoint BBVs](https://drive.google.com/file/d/1msr_YijlYN4rxpn71bod1LAoRWs5VtAL/view?usp=sharing) (step 1)
- [The document to do SimPoint clustering based on BBVs and take simpoint checkpoints](https://zhuanlan.zhihu.com/p/604396330) (step 2 & 3)

## Spec2006 preparation

```
bash scripts/run_spec2006_single.sh # print usage
```

Notes:

- set --rerun to 9999 for the first time to generate task_info.txt(index of each checkpoint) in --gcpt_path

## Running spec2006

### Single

```
bash scripts/run_spec2006_single.sh
```

### Range

```
bash scripts/run_spec2006_range.sh
```

### All

```
bash scripts/run_spec2006_all.sh
```

### Report

```
bash scripts/run_spec2006_report.sh
```

