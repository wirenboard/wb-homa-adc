#!/bin/bash
### BEGIN INIT INFO
# Required-Start: $remote_fs mosquitto
# Required-Stop: $remote_fs
### END INIT INFO

USER_CONFFILE="/etc/wb-homa-adc.conf"
DYN_CONFFILE="/tmp/wb-homa-adc.do-not-edit.conf"
SYS_CONFFILE="/tmp/wb-homa-adc.sys.conf"
TMP_USR_CONFFILE="/tmp/wb-homa-adc.usr.conf"
ENV_FILE="/etc/wb-homa-adc.env"

#~ [ -r /etc/default/$NAME ] && . /etc/default/$NAME
. /lib/init/vars.sh
VERBOSE="yes"
. /lib/lsb/init-functions

if [[ ! -d /sys/firmware/devicetree/base/wirenboard/analog-inputs ]]; then
	CONFFILE=${USER_CONFFILE}
else
	source "/usr/lib/wb-utils/common.sh"
	wb_source "of"
	wb_source "wb_env_of"
	WB_ENV_CACHE="${WB_ENV_CACHE:-/var/run/wb_env.cache}"

	declare -A OF_IIODEV_LINKS
	of_find_iio_dev_links

	WB_OF_ROOT="/wirenboard"
	node="${WB_OF_ROOT}/analog-inputs"

	ADCSYSCONF='{\n  "iio_channels": ['
	first=1
	for ch_name in  $(of_node_children "$node" | sort); do
		phandle=$(of_get_prop_ulong "$node/$ch_name" "iio-device")
		divider_r1=$(of_get_prop_ulong "$node/$ch_name" "divider-r1-ohms")
		divider_r2=$(of_get_prop_ulong "$node/$ch_name" "divider-r2-ohms")
		
		multiplier=$(echo "scale=5; ($divider_r1 + $divider_r2) / $divider_r2" | bc)

		ITEM="{ \
			\"match_iio\" : \"${OF_IIODEV_LINKS[$phandle]}\", \
			\"id\" : \"$ch_name\", \
			\"channel_number\" : \"$(of_get_prop_str "$node/$ch_name" "iio-channel-name")\", \
			\"voltage_multiplier\" : $multiplier , \
			\"averaging_window\" : $(of_has_prop "$node/$ch_name" "averaging-window" && echo -n $(of_get_prop_ulong "$node/$ch_name" "averaging-window") || echo -n 1),"

		if $(of_has_prop "$node/$ch_name" "decimal-places"); then
			ITEM="$ITEM\"decimal_places\" : $(of_get_prop_ulong "$node/$ch_name" "decimal-places"),"
		fi

		ITEM="$ITEM \
			\"_sort_key\" : $(of_has_prop "$node/$ch_name" "sort-order" && echo -n $(of_get_prop_ulong "$node/$ch_name" "sort-order") || echo -n 0) \
		}"

		if (( first )); then
			first=0
		else
			ADCSYSCONF="$ADCSYSCONF,"
		fi

		ADCSYSCONF="$ADCSYSCONF\n$ITEM"
	done
	ADCSYSCONF="$ADCSYSCONF\n]}"
	echo -e $ADCSYSCONF > /tmp/ADCSYSCONF
	echo -e $ADCSYSCONF | jq ".iio_channels = (.iio_channels | sort_by(._sort_key) | map(del(._sort_key)))" > ${SYS_CONFFILE}

	sed 's#//.*##' "$USER_CONFFILE"  > ${TMP_USR_CONFFILE}

	jq '.[0] as $first | .[1] as $second | $first * $second | .iio_channels |= ( $first.iio_channels + $second.iio_channels | [reduce .[] as $o ({}; .[[$o["match_iio"], $o["channel_number"]] | tostring] += $o ) | .[]] ) ' -s  ${SYS_CONFFILE} ${TMP_USR_CONFFILE} > ${DYN_CONFFILE}
	CONFFILE=${DYN_CONFFILE}
fi
echo "CONFFILE=${CONFFILE}" > ${ENV_FILE}
modprobe -r ti_ads1015; 
modprobe ti_ads1015; 


:
