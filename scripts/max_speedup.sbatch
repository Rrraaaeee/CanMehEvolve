#!/bin/bash
# Runs the combined-patch, best-flag measurement on one 24-core Xeon node.
# From the repository root, after a full build:
#   mkdir -p runs && sbatch scripts/max_speedup.sbatch
# Slurm cannot create the directory named in --output, so `runs` must exist first.
#SBATCH --partition=long
#SBATCH --constraint="xcnc|xcnd"
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=24
#SBATCH --mem=0
#SBATCH --time=1:30:00
#SBATCH --job-name=canmeh-max
#SBATCH --output=runs/slurm-%j.log

cd "${SLURM_SUBMIT_DIR:?run this through sbatch}" || exit 1
exec scripts/max_speedup.sh "results/max-speedup-$SLURM_JOB_ID" \
  "$PWD/build/max-speedup-work-$SLURM_JOB_ID"
