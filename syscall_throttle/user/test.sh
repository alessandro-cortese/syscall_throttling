#!/bin/bash

# registra syscall getpid (39)
sudo ./scthctl addsys 39

# registra il tuo uid effettivo
sudo ./scthctl adduid $(id -u)

# limita a 5/s e accendi
sudo ./scthctl setmax 5
sudo ./scthctl on

# lancia il carico 
./tester_getpid
./tester_openat