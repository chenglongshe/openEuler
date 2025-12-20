nr=$1

source /home/name/common/env.sh
sh client_kill.sh
function cleanup() {
ssh root@$CLIENT_OBSERVE "rm -rf /home/name/redis/client/test_*"
for IP in "${CLIENT[@]}"
do
    ssh root@$IP "rm -rf /home/name/redis/client/test_*"
done
}

function copy() {
scp -r root@$CLIENT_OBSERVE:/home/name/redis/client/test_$((nr + 1)) result
for IP in "${CLIENT[@]}"
do
    scp -r root@$IP:/home/name/redis/client/test_$((nr + 1)) result &> /dev/null
done
}

function stress() {
nr=$1
for IP in "${CLIENT[@]}"
do
echo -n $IP
    ssh root@$IP "sh /home/name/redis/client/run.sh $nr" &
echo done
done
}

nr=$((nr - 1))

cleanup
stress $nr
sleep 2
ssh root@$CLIENT_OBSERVE "sh /home/name/redis/client/run.sh $((nr + 1))"

sleep 15
mkdir result
copy
cat result/test_$((nr + 1))/log_specific | grep -A 4 throughput
