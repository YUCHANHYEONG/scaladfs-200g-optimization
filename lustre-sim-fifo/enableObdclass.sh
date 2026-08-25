#!/bin/bash
echo "Running commands as a root user..."
make -j64
retval=$?
if [ $retval -ne 0 ]; then
	        echo "Make Failed with return $retval"
		exit $retval
fi
sudo insmod lustre/obdclass/obdclass.ko
sudo insmod lustre/ptlrpc/ptlrpc.ko
sudo insmod lustre/fld/fld.ko
sudo insmod lustre/osc/osc.ko
sudo insmod lustre/lov/lov.ko
sudo insmod lustre/mgc/mgc.ko
sudo insmod lustre/fid/fid.ko
sudo insmod lustre/lmv/lmv.ko  
sudo insmod lustre/mdc/mdc.ko
sudo insmod lustre/llite/lustre.ko
#mount -t lustre -O nochecksum 192.168.100.101@tcp2:/temp /mnt/client
mount -t lustre -O nochecksum 192.168.100.114@tcp2:/temp /mnt/client
lfs setstripe -S 1M -c -1 /mnt/client
echo "All done."
df -ht lustre
