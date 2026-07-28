#!/bin/bash

for (( k=1; k<=16; k*=2 ))
do 
   ./run_experiment.sh mode=zns size=32GiB k=$k trace=../traces/tr_occ90_10.txt
done

./run_experiment.sh mode=conv size=32GiB trace=../traces/tr_occ90_10.txt

chown $SUDO_USER:$SUDO_USER *.txt
chown $SUDO_USER:$SUDO_USER *.csv