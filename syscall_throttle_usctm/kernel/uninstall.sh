#!/bin/bash
echo "[*] uninstalling lkm..."
sudo rmmod scth_mod
make clean
echo "[*] lkm removed"