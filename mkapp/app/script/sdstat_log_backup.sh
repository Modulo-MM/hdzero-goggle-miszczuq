#!/bin/sh

# Source dir: rc.sh passes the previous boot's log dir it set aside
# (/mnt/app/log.prev) and runs this in the background; default is the live dir.
logpath_src=${1:-/mnt/app/log}
logpath_dst=/mnt/extsd/log

function log_backup()
{
        src=$1
        dst=$2
        let index=-1

        if [ ! -d "${dst}" ]
        then
		echo "mkdir ${dst}"
                mkdir ${dst}
        fi

		#get the max index value
        for file in ${dst}/*.log.*
        do
                if [ -f "$file" ]
                then
                        #let start=${#dst}+16
                        #index_s=${file: $start}
                        #let index_i=${index_s}
                        let index_i=${file##*.}

                        if [ "${index_i}" -gt "${index}" ]
                        then
                                index=${index_i}
                        fi

                        #echo "$file"
                        #echo "index = ${index_s} ${index_i} ${index}"
                fi
        done

		let index+=1
        echo "index_next = ${index}"

        for file in ${src}/*.log
        do
                if [ -f "$file" ]
                then
                        name=${file##*/}
                        cp ${file} ${dst}/${name}.${index}
                        echo "cp ${file} ${dst}/${name}.${index}"
						#rm -f ${file}
                        let index+=1
                fi
        done
}

# Nothing to back up: skip the slow index scan of the SD card's log dir.
set -- ${logpath_src}/*.log
if [ ! -e "$1" ]; then
    if [ "$logpath_src" != "/mnt/app/log" ]; then
        rm -rf ${logpath_src}
    fi
    exit
fi

#if grep -qs '/mnt/extsd' /proc/mounts; then 
if df | grep -qs '/mnt/extsd'; then
    echo "sdcard mounted."         
else                    
    echo "sdcard not mounted."
    exit
fi  

#copy /mnt/app/log/* to /mnt/extsd/log if sdcard mounted
log_backup $logpath_src $logpath_dst

#remove all log files
if [ "$logpath_src" != "/mnt/app/log" ]; then
    rm -rf ${logpath_src}
else
    rm -f ${logpath_src}/*
fi

