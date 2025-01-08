patchwork=(patchwork@huawei.com)
me=(qiaoyifan4@huawei.com)
pl=(yangerkun@huawei.com)
se=(
	yi.zhang@huawei.com
	houtao1@huawei.com
)
group=(
	chengzhihao1@huawei.com
	qiaoyifan4@huawei.com
)
release=(
	'me qiaoyifan4@huawei.com'
	'hulk-4.19 liuyongqiang13@huawei.com kernel@openeuler.org'
	'hulk-4.1 gongruiqi1@huawei.com xiangyang3@huawei.com'
	'hulk-4.4 zhengyejian1@huawei.com'
	'hulk-5.10-next chenjun102@huawei.com'
	'hulk-5.10 wanghai38@huawei.com'
	'rh7.2 tongtiangen@huawei.com'
	'rh7.3 wanghai38@huawei.com'
	'rh7.5 yebin10@huawei.com'
	'rh8.1 zhangchangzhong@huawei.com'
	'hulk-3.10 xiujianfeng@huawei.com'
	'OLK-5.10 zhangjialin11@huawei.com kernel@openeuler.org'
	'OLK-6.6 zhengzengkai@huawei.com kernel@openeuler.org'
	'next miaoxie@huawei.com weiyongjun1@huawei.com guohanjun@huawei.com huawei.libin@huawei.com yuehaibing@huawei.com johnny.chenyi@huawei.com'
	'SP1 zhangjialin11@huawei.com kernel@openeuler.org'
)

usage()
{
	echo "----- send patch or patchset ------"
	echo "$0 release_versionxxx -p patch/-d patchset dir"
	echo "availiable release:"
	for r in "${release[@]}"; do
		echo "    $r"
	done
}

if [ $# != 3 ]; then
	echo "wrong input"
	usage
	exit 1
fi

# which release to send
for r in "${release[@]}"; do
	tmp=($r)
	i=0
	for t in ${tmp[@]}; do
		if [ $i -eq 0 ]; then
			if [ $1 == $t ]; then
				echo "will send to $r"
				let i++
			else
				break
			fi
		elif [ $i -eq 1 ]; then
			cmd="$cmd-to $t"
			let i++
		else
			cmd="$cmd -to $t"
		fi
	done
	if [ $i -ne 0 ]; then
		break
	fi
done

# always cc me pl and se
cmd="$cmd -cc $me -cc $pl"
for j in ${se[@]}; do
	cmd="$cmd -cc $j"
done

# if send to patchwork
if [ $1 != me ]; then
	cmd="$cmd -to $patchwork"
	for g in ${group[@]}; do
		cmd="$cmd -cc $g"
	done
fi

cmd="$cmd --suppress-cc=all"
# -p : xxx.patch
# -d : xxx/*.patch
if [ $2 == "-p" ]; then
	cmd="git send-email $cmd $3"
elif [ $2 == "-d" ]; then
	cmd="git send-email $cmd $3/*.patch"
else
	echo "unknow patch"
fi

echo "$cmd"
$cmd
