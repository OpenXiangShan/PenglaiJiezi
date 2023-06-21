#!/bin/bash

#-----> base dir, the directory above scripts(i.e. GEM5/)
export GEM5_BASE_DIR=$(cd "$(dirname ${BASH_SOURCE[0]})/.."; pwd )

#-----> script dir, the directory above scripts(i.e. GEM5/scripts/)
export GEM5_SCRIPTS_DIR=${GEM5_BASE_DIR}/scripts

$GEM5_BASE_DIR/build/RISCV/gem5.opt  \
    --outdir=./outputhello \
    $GEM5_BASE_DIR/configs/example/se.py \
    --cpu-type=DerivO3CPU \
    --caches \
    --cacheline_size=64 \
    --l1i_size=128kB \
    --l1i_assoc=8 \
    --l1d_size=128kB \
    --l1d_assoc=8 \
    --l2cache \
    --l2_size=1MB \
    --l2_assoc=8 \
    --l3cache \
    --l3_size=6MB \
    --l3_assoc=6 \
    --mem-type=DDR4_2400_16x4 \
    --mem-size=8GB \
    --l2-hwp-type=BOPPrefetcher \
    --num-cpus=1 \
    -I=40000000 \
    -c=$GEM5_SCRIPTS_DIR/workload/hello.nofpu