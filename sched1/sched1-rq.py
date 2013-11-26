#!/usr/bin/python

import sys
import math

if len(sys.argv) == 1:
    print >> sys.stderr, "Usage: sched1-rq.py <requests-output>"
    sys.exit(1)

rq_file = file(sys.argv[1])

for l in rq_file:
    wl_name, step, rq_id, tid, start, end, flags = l.split(',')
    
    print '%.1f' % (float(int(end) - int(start)) / 1000.)