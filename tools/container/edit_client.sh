#!/bin/bash

source /home/name/common/env.sh

connection=40
data_len=3
thread=50
data_num=2000000
data_num_large=200000000

mod=0
for IP in "${CLIENT[@]}"
do
echo $IP
    cp run_bk.sh run.sh
    echo $mod
    sed -i "s|xx|$mod|g" run.sh
    sed -i "s|aa|$connection|g" run.sh
    sed -i "s|bb|$data_len|g" run.sh
    sed -i "s|cc|$thread|g" run.sh
    sed -i "s|dd|$data_num_large|g" run.sh

    ssh root@$IP mkdir -p /home/name/common
    scp /home/name/common/env.sh root@$IP:/home/name/common
    scp run.sh root@$IP:/home/name/redis/client/
    mod=$((mod + 1))
done

cp run_bk_117.sh run_117.sh
sed -i "s|aa|$connection|g" run_117.sh
sed -i "s|bb|$data_len|g" run_117.sh
sed -i "s|cc|$thread|g" run_117.sh
sed -i "s|dd|$data_num|g" run_117.sh
scp run_117.sh root@$CLIENT_OBSERVE:/home/name/redis/client/run.sh
