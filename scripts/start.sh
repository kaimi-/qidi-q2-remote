#!/bin/bash
set -e

echo "Start QD_Q2-client $(date "+%Y%m%d%H%M%S")"

export QD_REMOTE_INPUT_SOCK="${QD_REMOTE_INPUT_SOCK:-/run/qd2-remote-input.sock}"
export LD_PRELOAD="${LD_PRELOAD_OVERRIDE:-/home/mks/qd2_remote_input.so}"

exec taskset -c 0 /home/mks/QD_Q2/bin/client
