#!/bin/bash
# Runs Phase 1 as a job array, one run per 24-core Xeon node and at most four at
# once: tasks 1-5 are the search at seeds 1-5, tasks 6-10 are best-of-N at seeds 1-5.
# From the repository root, after a full build:
#   mkdir -p runs && MODEL=<id> sbatch scripts/phase1.sbatch
# Slurm cannot create the directory named in --output, so `runs` must exist first.
#SBATCH --partition=long
#SBATCH --constraint="xcnc|xcnd"
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=24
#SBATCH --mem=0
#SBATCH --time=2:45:00
#SBATCH --array=1-10%4
#SBATCH --job-name=canmeh-phase1
#SBATCH --output=runs/slurm-%A_%a.log

cd "${SLURM_SUBMIT_DIR:?run this through sbatch, or set SLURM_SUBMIT_DIR, SLURM_JOB_ID and SLURM_ARRAY_TASK_ID}" || exit 1
index=${SLURM_ARRAY_TASK_ID:?this script runs as a job array}
if (( index <= 5 )); then
  arm=search
  seed=$index
else
  arm=best-of-n
  seed=$(( index - 5 ))
fi
exec scripts/phase1.sh "$arm" "$seed" "${PHASE1_ROOT:-results/phase1}/$arm-$seed" \
  "$PWD/build/phase1-work-$arm-$seed-$SLURM_JOB_ID"
