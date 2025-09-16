nr=$1

source /home/name/common/env.sh
dir=/home/name/redis/client/test_$((nr + 1))

rm -rf $dir
mkdir $dir

for (( i=1; i<=$nr; i++ ))
do
    if [ $((i % 4)) -eq xx ]; then
        redis-benchmark -h ${UNREACHABLE_IPS[$i]} -p 6379 -c aa -d bb -n dd -r 10000000  -t get --threads cc > $dir/log_${i} &
#        redis-benchmark -h ${UNREACHABLE_IPS[$((80-i))]} -p 6379 -c aa -d bb -n dd -r 10000000  -t get --threads cc > $dir/log_${i} &
fi
done
wait
