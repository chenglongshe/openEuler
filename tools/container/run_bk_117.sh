nr=$1
IP=ip_observe // todo

#sleep 8
rm -rf /home/name/redis/client/test_$nr
mkdir /home/name/redis/client/test_$nr
redis-benchmark -h $IP -p 6379 -c aa -d bb -n dd -r 10000000  -t get --threads cc > /home/name/redis/client/test_$nr/log_specific
