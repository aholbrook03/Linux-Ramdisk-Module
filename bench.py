# bench.py
# Andrew Holbrook
#
# Program for benchmarking the ramdisk module.

from random import randint
import pickle

fp = open("/mnt/hdd/f1", "w+")
fp2 = open("/dev/zero", "r")

for i in range(0, 1000000):
	if (i % 1000) == 0:
		print i
	nbytes = randint(1, 1024)
	offset = randint(1, 1024*1024)

	fp.seek(offset)

	if randint(0, 1):
		for i in range(0, nbytes):
			fp.write("A")
	else:
		buf = fp.read(nbytes)



fp2.close()
fp.close()
