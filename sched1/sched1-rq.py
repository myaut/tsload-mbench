#!/usr/bin/python

import sys
import math

RQF_FINISHED = 8

tstime2us = lambda t: float(t) / 1000.
tstime2sec = lambda t: float(t) / 1000000000.

if len(sys.argv) == 1:
    print >> sys.stderr, "Usage: sched1-rq.py <requests-output>"
    sys.exit(1)

rq_file = file(sys.argv[1])

experiment_start = None

for l in rq_file:
    wl_name, step, rq_id, tid, sched, start, end, flags = l.split(',')    
    flags = int(flags, 16)
    
    if not (flags & RQF_FINISHED):
        continue
    
    end = int(end)
    start = int(start)
    
    if experiment_start is None:
        experiment_start = start
    
    print '%.3f,%.1f' % (tstime2sec(start - experiment_start), tstime2us(end - start))