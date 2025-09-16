source /home/name/common/env.sh

rm -rf result/test*
ssh root@$CLIENT_OBSERVE rm -rf /home/name/redis/client/test*
nr=$1

sh runc_parallel.sh 1
sleep 3

for i in $(seq 1 $nr); do
sh runc_parallel.sh $((i * 4))
sleep 3
done
