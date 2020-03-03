import sys

f = open(sys.argv[1], "r")
totcycles = 0 
totcount = 0
cycles = []
counts = []
for line in f:
	data = line.strip().split()
	if data[0] == "totcycles":
		totcycles += int(data[1])	
		cycles.append(int(data[1]))
	else:
		totcount += int(data[1])
		counts.append(int(data[1]))

'''
for c in cycles:
	print(c)
for c in counts:
	print(c)
'''

for cycle, count in zip(cycles,counts):
	print(cycle, ",", count)
