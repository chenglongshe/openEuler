#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

function usage {

cat <<EOM
Usage: $(basename "$0") [OPTION]...

  -d          RoCE Device Name (e.g. bnxt_re0, bnxt_re_bond0)
  -i          Ethernet Interface Name (e.g. p1p1 or for bond,
		specify slave interfaces like -i p6p1 -i p6p2)
  -m [1-3]    1 - PFC only
	      2 - CC only
	      3 - PFC + CC mode
  -v	      1 - Enable priority vlan
  -r [0-7]    RoCE Packet Priority
  -s VALUE    RoCE Packet DSCP Value
  -c [0-7]    RoCE CNP Packet Priority
  -p VALUE    RoCE CNP Packet DSCP Value
  -b VALUE    RoCE Bandwidth percentage for ETS configuration - Default is 50%
  -t [2]      Default mode (Only RoCE v2 is supported - Input Ignored)
  -u [1-2]    Utility to configure QoS settings
	      1 - Use bnxtqos utility (default)
              2 - Use lldptool
  -h          display help
EOM
exit 2
}

MAX_INTERFACE_COUNT=2

while getopts ":r:s:c:p:i:d:v:m:b:t:u:h" o; do
    case "${o}" in
        r)
            r=${OPTARG}
            ;;
        s)
            s=${OPTARG}
            ;;
        c)
            c=${OPTARG}
            ;;
        p)
            p=${OPTARG}
            ;;
        i)
            i+=${OPTARG}" "
            ;;
        d)
            d=${OPTARG}
            ;;
        v)
            v=${OPTARG}
            ;;
        m)
            m=${OPTARG}
            ;;
        b)
            b=${OPTARG}
            ;;
	#ignoring whatever the user passes for RoCE mode
        t)
            t=2
            ;;
	u)
	    u=${OPTARG}
	    ;;
        h)
            usage
            ;;

        *)
            usage
            ;;
    esac
done
shift $((OPTIND-1))

if [ -z "${i}" ] || [ -z "${r}" ]; then
    usage
fi

if [ -z "${d}" ]
then
	sysfspath="/sys/class/infiniband/"
	for rdev in `ls $sysfspath`
	do
		pr=`echo $rdev|awk '{if($0 ~ /bnxt_/) print "yes"; else print "no";}'`
		if [ "$pr" == "no" ]
		then
			continue
		fi
		dpath="$sysfspath/$rdev/ports/1/gid_attrs/ndevs/0"
		ipiface=`cat $dpath`
		if [ "$ipiface " == "${i}" ]
		then
			d=$rdev
			break
		fi
	done
	if [ "${d}" == "" ]
	then
		echo "Could not locate RDMA device for ${i}interface."
		echo "Try using -d option explicitly."
		exit -1
	fi
	echo "Using RDMA device ${d} for interface ${ipiface}"
fi

if [ -z "${u}" ] || [ "$u" == 1 ]
then
	USE_BNXTQOS=1
elif [ "$u" == 2 ]
then
	USE_BNXTQOS=0
else
	echo "Invalid utility selected (-u option)"
	usage
	exit 1
fi

if [ $USE_BNXTQOS -eq 1 ]
then
	type bnxtqos >/dev/null 2>&1 ||
	       { echo >&2 "bnxtqos utility is not installed.  Aborting."; exit 1; }
else
	type lldptool >/dev/null 2>&1 ||
	       { echo >&2 "lldptool is not installed.  Aborting."; exit 1; }
fi

EN_ROCE_DSCP=0
#s p r c needs to be checked for format conversion
scale='^[0-9]+$'
hexscale='^(0[xX])[0-9a-fA-F]+$'

if [ ! -z "${s}" ]
then
	if [[ $s =~ $scale ]]
	then
		tmp=`printf "0x%x" $s`
		s=$tmp
	else
		if ! [[ $s =~ $hexscale ]]
		then
			echo Invalid RoCE DSCP value
			exit -1
		fi
	fi
EN_ROCE_DSCP=1
fi

EN_CNP_DSCP=0
if [ ! -z "${p}" ]
then
	if [[ $p =~ $scale ]]
	then
		tmp=`printf "0x%x" $p`
		p=$tmp
	else
		if ! [[ $p =~ $hexscale ]]
		then
			echo Invalid RoCE CNP packet DSCP value
			exit -1
		fi
	fi
EN_CNP_DSCP=1
fi


EN_ROCE_PRI=0
if [ ! -z "${r}" ]
then

	if [[ $r =~ $scale ]]
	then
		tmp=`printf "0x%x" $r`
		r=$tmp
	else
		if ! [[ $r =~ $hexscale ]]
		then
			echo Invalid RoCE packet priority
			exit -1
		fi
	fi
EN_ROCE_PRI=1
fi


EN_CNP_PRI=0
if [ ! -z "${c}" ]
then
	if [[ $c =~ $scale ]]
	then
		tmp=`printf "0x%x" $c`
		c=$tmp
	else
		if ! [[ $c =~ $hexscale ]]
		then
			echo Invalid RoCE CNP packet priority
			exit -1
		fi
	fi
EN_CNP_PRI=1
fi


if_count=0

for interface in $i
do
if_count=`expr $if_count + 1`
done

if [ $if_count -gt $MAX_INTERFACE_COUNT ];
then
	echo "Number of interfaces more than supported"
	exit 1;
fi

INF_NAME1=`echo $i |cut -d ' ' -f1`
INF_NAME2=`echo $i |cut -d ' ' -f2`

if [[ "$INF_NAME1" == "$INF_NAME2" ]];
then
	INF_NAME2=
fi

if [ -z $m ]
then
	ENABLE_PFC=1
	ENABLE_CC=1
elif [ "$m" == "1" ]
then
	ENABLE_PFC=1
	ENABLE_CC=0
elif [ "$m" == "2" ]
then
	ENABLE_CC=1
	ENABLE_PFC=0
elif [ "$m" == "3" ]
then
	ENABLE_PFC=1
	ENABLE_CC=1
else
	echo "Invalid value for mode (-m option)"
	exit 1;
fi

ENABLE_DSCP=0
echo ENABLE_PFC = $ENABLE_PFC ENABLE_CC = $ENABLE_CC

if [ $EN_ROCE_DSCP -eq 1 ] || [ $EN_CNP_DSCP -eq 1 ]
then
	ENABLE_DSCP=1
fi

ENABLE_PRI=0
if [ $EN_ROCE_PRI -eq 1 ] || [ $EN_CNP_PRI -eq 1]
then
	ENABLE_PRI=1
fi

ENABLE_DSCP_BASED_PFC=1
if [ "$v" == "1" ]
then
	ENABLE_DSCP_BASED_PFC=0
fi

echo ENABLE_DSCP = $ENABLE_DSCP ENABLE_DSCP_BASED_PFC = $ENABLE_DSCP_BASED_PFC

DEV_NAME=$d
ROCE_DSCP=$s
ROCE_PRI=$r
ROCE_CNP_DSCP=$p
ROCE_CNP_PRI=$c
ROCE_BW=$b

if [ -z $ROCE_BW ]
then
	ROCE_BW=50
fi

if [ $ROCE_BW -lt 100 ]
then
	L2_BW=`expr 100 - $ROCE_BW`
else
	L2_BW=50
	ROCE_BW=50
fi

# Only RoCE v2 is supported
ROCE_MODE=2

echo L2 $L2_BW RoCE $ROCE_BW

echo "Using Ethernet interface $INF_NAME1  $INF_NAME2 and RoCE interface $DEV_NAME"

CNP_SERVICE_TYPE=0
if test -f "/sys/kernel/debug/bnxt_re/$DEV_NAME/info"; then
	CNP_SERVICE_TYPE= \
		`cat /sys/kernel/debug/bnxt_re/$DEV_NAME/info|grep fw_service_prof_type_sup|awk '{print $3}'`
fi

# Define priority 2 tc mapping
pri2tc=""

for i in `seq 0 7`;
do
    if [ $EN_ROCE_PRI -eq 1 ] && [ $i -eq `printf "%d" $ROCE_PRI` ]; then
        pri2tc+=",$i:1"
    elif [ $EN_CNP_PRI -eq 1 ] && [ $i -eq `printf "%d" $ROCE_CNP_PRI` ]  \
	    && [ $CNP_SERVICE_TYPE -eq 1 ] && [ $ENABLE_CC -eq 1 ]; then
       pri2tc+=",$i:2"
    else
        pri2tc+=",$i:0"
    fi
done

pri2tc=${pri2tc:1}

ethtool $INF_NAME1
ethtool -i $INF_NAME1
ethtool -A $INF_NAME1 rx off tx off
if [ "$INF_NAME2" != "" ];
then
    ethtool $INF_NAME2
    ethtool -i $INF_NAME2
    ethtool -A $INF_NAME2 rx off tx off
fi

bnxt_qos_rem_app_tlvs() {
	INF_NAME=$1
	j=0
	for i in `bnxtqos -dev=$INF_NAME get_qos \
		|grep -e "Priority:" -e "Sel:" -e DSCP -e UDP -e "Ethertype:"|awk -F":" '{ print $2}'`
	do
		if [ $i == 0x8915 ]
		then
			i=35093
	fi

	if [ $j -eq 0 ]
	then
		APP_0=$i
	else
		APP_0=$APP_0,$i
	fi

	j=`expr $j + 1`

	if [ $j -eq 3 ]
	then
		bnxtqos -dev=$INF_NAME set_apptlv -d app=$APP_0
		j=0
	fi
	done
}

lldptool_rem_app_tlvs() {
	INF_NAME=$1
	for i in `lldptool -t -i $INF_NAME -V APP -c app \
		|awk -F"(" '{ print $2}'|awk -F")" '{ print $1}'| sed "1 d"`
	do
		lldptool -T -i $INF_NAME -V APP -d app=$i
	done
}

bnxt_qos_pgm_pfc_ets() {
	INF_NAME=$1
	echo "Setting pfc/ets on $INF_NAME"
	bnxt_qos_rem_app_tlvs $INF_NAME
	# bnxtqos requires nvm cfg 155,255,269 and 270 to be disabled
	if [ $CNP_SERVICE_TYPE -eq 1 ]
	then
		bnxtqos -dev=$INF_NAME set_ets  \
			tsa=0:ets,1:ets,2:strict,3:strict,4:strict,5:strict,6:strict,7:strict  \
			priority2tc=$pri2tc tcbw=$L2_BW,$ROCE_BW
	else
		bnxtqos -dev=$INF_NAME set_ets tsa=0:ets,1:ets  \
			priority2tc=$pri2tc tcbw=$L2_BW,$ROCE_BW
	fi

	if [ ! -z "$ROCE_PRI" ] && [ $ENABLE_PFC -eq 1 ]
	then
		bnxtqos -dev=$INF_NAME set_pfc enabled=`printf "%d" $ROCE_PRI`
		sleep 1
		bnxtqos -dev=$INF_NAME set_apptlv app=`printf "%d" $ROCE_PRI`,3,4791
		sleep 1
	else
		bnxtqos -dev=$INF_NAME set_pfc enabled=none
	fi

	if [ $ENABLE_DSCP_BASED_PFC -eq 1 ] || [ $EN_ROCE_DSCP -eq 1 ]
	then
		bnxtqos -dev=$INF_NAME set_apptlv  \
			app=`printf "%d" $ROCE_PRI`,5,`printf "%d" $ROCE_DSCP`
		sleep 1
	fi
	if [ $ENABLE_CC -eq 1 ] &&  [ $CNP_SERVICE_TYPE -eq 1 ]
	then
		bnxtqos -dev=$INF_NAME set_apptlv  \
			app=`printf "%d" $ROCE_CNP_PRI`,5,`printf "%d" $ROCE_CNP_DSCP`
		sleep 1
	fi
	sleep 1
	bnxtqos -dev=$INF_NAME get_qos
}

lldptool_pgm_pfc_ets() {
	INF_NAME=$1
	lldptool_rem_app_tlvs $INF_NAME
	lldptool -T -i $INF_NAME1 -V ETS-CFG  \
		tsa="0:ets,1:ets,2:strict,3:strict,4:strict,5:strict,6:strict,7:strict"  \
		up2tc=$pri2tc  tcbw=$L2_BW,$ROCE_BW,0,0,0,0,0,0
	if [ ! -z "$ROCE_PRI" ] && [ $ENABLE_PFC -eq 1 ]
	then
		lldptool -L -i $INF_NAME1 adminStatus=rxtx
		lldptool -T -i $INF_NAME1 -V PFC enabled=`printf "%d" $ROCE_PRI`
		lldptool -T -i $INF_NAME1 -V APP app="`printf "%d" $ROCE_PRI`,3,4791"
		if [ $ENABLE_DSCP_BASED_PFC -eq 1 ] || [ $EN_ROCE_DSCP -eq 1 ]
		then
			lldptool -T -i $INF_NAME1 -V APP \
			       	app="`printf "%d" $ROCE_PRI`,5,`printf "%d" $ROCE_DSCP`"
		fi
	else
		lldptool -T -i $INF_NAME1 -V PFC enabled=none
	fi
	if [ $ENABLE_CC -eq 1 ] &&  [ $CNP_SERVICE_TYPE -eq 1 ]
	then
		lldptool -T -i $INF_NAME1 -V APP  \
			app="`printf "%d" $ROCE_CNP_PRI`,5,`printf "%d" $ROCE_CNP_DSCP`"
		sleep 1
	fi
}

if [ $USE_BNXTQOS -eq 1 ]
then
    SYSTEMCTL_STATUS=`command -v systemctl`
    if [ "$SYSTEMCTL_STATUS" == "" ];
    then
        echo "systemctl not found, install and re-run the script. exiting..."
        exit -1
    fi

    STATUS="$(systemctl is-active lldpad)"
    if [ "${STATUS}" = "active" ]; then
        #Stop lldpad
        echo "Disabling lldpad service, and using bnxtqos tool for configuration"
        systemctl stop lldpad.service
    else
        echo "check if lldpad service is running : no action needed"
    fi

    bnxt_qos_pgm_pfc_ets $INF_NAME1
    if [ "$INF_NAME2" != "" ];
    then
	bnxt_qos_pgm_pfc_ets $INF_NAME2
    fi
else
    IS_RUNNING=`ps -aef | grep lldpad | head -1 | grep "/usr/sbin/lldpad"`
    if [ "$IS_RUNNING" != " " ]
    then
        #Stop lldpad
        systemctl stop lldpad.service
    fi
    systemctl start lldpad.service
    sleep 1
    STATUS="$(systemctl is-active lldpad)"
    if [ "${STATUS}" != "active" ]; then
        echo "Failed to start lldpad service"
        exit 1
    fi

    echo "Setting up LLDP"
    lldptool_pgm_pfc_ets INF_NAME1
    if [ "$INF_NAME2" != "" ];
    then
	lldptool_pgm_pfc_ets INF_NAME2
    fi
    systemctl restart lldpad.service
fi

echo "Settings Default to use RoCE-v$ROCE_MODE"
mkdir -p /sys/kernel/config/rdma_cm/$DEV_NAME
echo "RoCE v2" > /sys/kernel/config/rdma_cm/$DEV_NAME/ports/1/default_roce_mode
if [ ! -z "$ROCE_DSCP" ]
then
	echo -n $((ROCE_DSCP << 2)) > /sys/kernel/config/rdma_cm/$DEV_NAME/ports/1/default_roce_tos
else
	echo -n 0 > /sys/kernel/config/rdma_cm/$DEV_NAME/ports/1/default_roce_tos
fi
PREVDIR=`pwd`
mkdir -p /sys/kernel/config/bnxt_re/$DEV_NAME
cd /sys/kernel/config/bnxt_re/$DEV_NAME/ports/1/cc/

#Disabling prio vlan insertion if dscp based pfc is enabled
if [ $ENABLE_DSCP_BASED_PFC -eq 1 ]
then
	echo -n 0x1 > disable_prio_vlan_tx
else
	echo -n 0x0 > disable_prio_vlan_tx
fi

if [ $ENABLE_CC -eq 1 ]
then
	echo "Setting up CC Settings"
	echo -n 0x1 > ecn_marking
	echo -n 0x1 > ecn_enable
	echo -n 1 > cc_mode
else
	echo -n 0x0 > ecn_marking
	echo -n 0x0 > ecn_enable
fi

if [ $CNP_SERVICE_TYPE != 1 ]
then
	if [ ! -z "$ROCE_CNP_PRI" ]
	then
		echo -n $ROCE_CNP_PRI > cnp_prio
	fi
	if [ ! -z "$ROCE_PRI" ]
	then
		echo -n $ROCE_PRI > roce_prio
	fi
fi

if [ $ENABLE_DSCP -eq 1 ]
then
	echo "Setting up DSCP/PRI"
	if [ ! -z "$ROCE_DSCP" ]
	then
		echo -n $ROCE_DSCP > roce_dscp
	fi
	if [ ! -z "$ROCE_CNP_DSCP" ]
	then
		echo -n $ROCE_CNP_DSCP > cnp_dscp
	fi
fi
echo -n 0x1 > apply

cd $PREVDIR

echo "Complete"
