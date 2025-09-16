nr=$1
DIST=4

dir=/home/name/redis_image/test_inbalance_new/result
echo "****************"
cat ${dir}/test_1/log_specific | grep throughput | awk '{print $3}'
for i in $(seq 1 $nr); do
cat ${dir}/test_$((i * DIST))/log_specific | grep throughput | awk '{print $3}'
done
echo "****************"
cat ${dir}/test_1/log_specific | grep -A 2 "latency summary" | tail -n 1 | awk '{print $1}'
for i in $(seq 1 $nr); do
cat ${dir}/test_$((i * DIST))/log_specific | grep -A 2 "latency summary" | tail -n 1 | awk '{print $1}'
done
echo "****************"
cat ${dir}/test_1/log_specific | grep -A 2 "latency summary" | tail -n 1 | awk '{print $5}'
for i in $(seq 1 $nr); do
cat ${dir}/test_$((i * DIST))/log_specific | grep -A 2 "latency summary" | tail -n 1 | awk '{print $5}'
done
