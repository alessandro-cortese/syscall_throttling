#!/bin/bash
set -euo pipefail

cd ../user
sudo ./scthctl resetstats
sudo ./scthctl addprog tester_getpid
sudo ./scthctl addsys 39
sudo ./scthctl setmax 5
sudo ./scthctl on

./tester_getpid >/dev/null &
pid=$!
sleep 2
./scthctl stats

sudo ./scthctl setmax 5000
./scthctl stats   # current ancora 5, next 5000
sleep 2
./scthctl stats   # current 5000, next 5000

kill $pid