#!/bin/bash
$GEM5_BASE_DIR/build/RISCV/gem5.opt  \
    --outdir=m5out/dhrystone \
    --debug-file=cache.output \
    configs/example/fs.py \
    --xiangshan-system \
    --cpu-type=DerivO3CPU \
    --bp-type=DecoupledBPUWithFTB \
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
    --num-cpus=1 \
    --generic-rv-cpt=$GEM5_SCRIPTS_DIR/scripts/bin/dhrystone/dhrystone-riscv64-xs-flash.bin   \
    --raw-cpt
