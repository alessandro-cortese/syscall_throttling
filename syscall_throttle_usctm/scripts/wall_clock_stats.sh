#!/bin/bash
set -euo pipefail

cd ../user

sudo ./scthctl on
sudo ./scthctl resetstats
sudo ./scthctl setmax 5
./scthctl stats
sleep 2
./scthctl stats