#!/bin/bash
echo "$0"
echo "$1"
if [ -z "$1" ]; then 
	echo "please input flash image name"
else 
	python swdflash.py --bin="$1" --addr=0x0
fi
