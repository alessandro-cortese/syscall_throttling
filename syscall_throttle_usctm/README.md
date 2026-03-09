### System Call Throttling System - 2.0

sudo cat /sys/module/the_usctm/parameters/sys_call_table_address 
sudo cat /sys/module/the_usctm/parameters/sys_ni_syscall_address
sudo cat /sys/module/the_usctm/parameters/free_entries 

sys_call_table = ffffffffbc2004a0
sys_ni_syscall = ffffffffbaef0d00