source /home/name/common/env.sh

for IP in "${CLIENT[@]}"
do
    echo $IP
    ssh root@$IP "pgrep -f redis-benchmark | xargs kill -9" 
done
ssh root@$CLIENT_OBSERVE "pgrep -f redis-benchmark | xargs kill -9"
