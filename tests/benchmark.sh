#!/bin/sh
export MANAGER_IP=10.0.0.254

i=1
while [ "$i" -le 10 ]; do
    /mnt/utm/client_program
    i=$((i + 1))
done
