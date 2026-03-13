#!/bin/bash
set -euo pipefail

cd syscall_table_discoverer
./load.sh

cd ..

cd kernel
./compile.sh
./install.sh

cd ..