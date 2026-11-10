#!/bin/bash
# Runs scripts/phase0.sh on one 24-core Xeon node. From the repository root, after
# a full build:
#   mkdir -p runs && sbatch scripts/phase0.sbatch
# Slurm cannot create the directory named in --output, so `runs` must exist first.
#SBATCH --partition=long
#SBATCH --constraint="xcnc|xcnd"
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=24
#SBATCH --mem=0
#SBATCH --time=1:30:00
#SBATCH --job-name=canmeh-phase0
#SBATCH --output=runs/slurm-%j.log

cd "${SLURM_SUBMIT_DIR:?run this through sbatch, or set SLURM_SUBMIT_DIR and SLURM_JOB_ID}" || exit 1
exec scripts/phase0.sh "results/phase0-$SLURM_JOB_ID" "$PWD/build/phase0-work-$SLURM_JOB_ID"
