#!/usr/bin/python

import sys
import math

usage_str = '''Usage: sched1-mark.py [-f] [-v] [-d] [-t] <sched1-trace-file>

-v    -  Be verbose (print outliers)
-d    -  Dump transition times to files
-t    -  Trace to stdout
-f    -  Filter outliers'''

if len(sys.argv) < 2:
    print >> sys.stderr, usage_str
    sys.exit(1)

trace = file(sys.argv.pop())

STATE_LIST = ['sched', 'vcs', 'futex', 'futex2', 'ping', 'pong', 'pop', 'tick', 'sys_exec']

EV_START = 0
EV_END = 1
EV_DISCARD = 2

P_PING = 0
P_PONG = 1
P_TICK = 2
P_SYS  = 3

do_dump = '-d' in sys.argv
do_trace = '-t' in sys.argv
do_warn = '-v' in sys.argv
do_filter = '-f' in sys.argv

def is_outlier(state, delta):
    if state == 'ping':
        return delta > 5100. 
    elif state == 'pong':
        return delta > 1100.
    elif state == 'tick':
        return delta > 4100.
    
    return delta > 100.

# Checks if ping thread being preempted by system thread (or some other thread)
# also ignore IDLE thread (0), cause it is always VCS
def is_preempt(tid, current_tid):
    global proc
    return (tid > 0 and current_tid > 0) and             \
           ((tid in proc and current_tid not in proc) or \
            (tid not in proc and current_tid in proc))

class Process:
    tid = None
    
    def __init__(self, tid, p_type):
        self.states = []
        self.p_type = p_type
        self.tid = tid
        
        self.s_times = dict((state, 0) 
                            for state 
                            in STATE_LIST)
        self.times = dict((state, []) 
                            for state 
                            in STATE_LIST)
        self.requests = dict((state, 0) 
                             for state 
                             in STATE_LIST)
    
    def add_time(self, ev_type, state, tm, rq_id):
        tm = float(tm) / 1000.
        delta = None
        
        if ev_type == EV_START:
            if state not in self.states:
                self.states.append(state)
            
            if state in self.s_times:
                self.s_times[state] = tm
                
                if rq_id > 0:
                    self.requests[state] = rq_id
        else:
            if ev_type == EV_END and           \
                    state in self.times and    \
                    state in self.states and    \
                    state in self.requests:
                
                if rq_id > 0:
                    if rq_id != self.requests[state]:
                        return None
                    else:
                        self.requests[state] = 0
                
                delta = tm - self.s_times[state]
                self.times[state].append(delta)
                
                if do_trace:
                    print ev_type, state, delta
            
            try:
                self.states.remove(state)
            except ValueError:
                pass
        
        return delta        

pong = None
tick = Process(-1, P_TICK)
sys  = Process(-2, P_SYS)

proc = { -1: tick,
         -2: sys }

STATE_TABLE = [# Involuntary/voluntary scheduling conducted by __schedule() 
               ('IVCS',           EV_START, 'sched',  P_PING),
               ('CPU ON',         EV_END,   'sched',  P_PING),
               
               ('VCS',            EV_START, 'vcs',    P_PONG),
               ('CPU ON',         EV_END,   'vcs',    P_PONG),
               
               # When ping puts request to pong queue, it uses OS futex
               # mechanizm. Measure both time between syscall and waking
               # up pong thread (futex) and between leaving CPU and placing
               # onto  RQ (futex2)
               ('PING SEND PONG', EV_START, 'futex',  P_PONG),
               ('WAKEUP',         EV_END,   'futex',  P_PONG),
               
               ('PING SEND PONG', EV_START, 'futex2', P_PING),
               ('YIELD',          EV_END,   'futex2', P_PING),
               
               # Measure time spent by calculating matrices (pong request
               # time. If context have been switched during that, discard it. 
               ('RQ START',       EV_START,   'ping', P_PING),
               ('CPU ON',         EV_DISCARD, 'ping', P_PING),
               ('RQ FINISH',      EV_END,     'ping', P_PING),
               
               # Measure time that pong really spends in sleep timer
               ('PONG SLEEP',     EV_START, 'pong',  P_PONG),
               ('WAKEUP',         EV_END,   'pong',  P_PONG),
               
               # Measure CPU overheads made by pong thread
               ('PONG WAKEUP',    EV_START, 'pop',  P_PONG),
               ('PONG POP',       EV_END,   'pop',  P_PONG),
               
               # Measure system tick jitter
               ('TICK',           EV_END,   'tick',  P_TICK),
               ('TICK',           EV_START, 'tick',  P_TICK),
               
               # Sometimes ping thread being preempted by some thread
               # Most threads being migrated to another CPUs, but some
               # system threads are bound to CPU0, account them to
               ('CPU ON',         EV_START, 'sys_exec', P_SYS),
               ('CPU ON',         EV_END,   'sys_exec', P_SYS),]

for line in trace:
    tm, event, tid, current_tid, rq_id = line.strip().split(',')
    
    tm = int(tm)
    tid = int(tid)
    current_tid = int(current_tid)
    rq_id = int(rq_id)
    
    if event == 'PING START':
        proc[tid] = Process(tid, P_PING)
        continue
    elif event == 'PONG START':
        if pong is not None:
            print >> sys.stderr, 'Only one PONG thread supported'
            sys.exit(1)
        
        pong = Process(tid, P_PONG)
        proc[tid] = pong
        
        continue
    
    for ev_name, ev_type, state, p_type in STATE_TABLE:
        if ev_name != event:
            continue
        
        current_pr = None
        if ev_name == 'PING SEND PONG' and state in ['futex', 'pong2']:
            pr = pong
        elif p_type == P_TICK and ev_name == 'TICK':
            pr = tick
        elif p_type == P_SYS and is_preempt(tid, current_tid):            
            if ev_type == EV_START and sys.tid != tid:
                sys.tid = current_tid
                pr = sys
            elif ev_type == EV_END and sys.tid == tid:
                sys.tid = -2
                pr = sys
            else:
                continue
        elif p_type in [P_PING, P_PONG]:
            pr = proc.get(tid, None)
            current_pr = proc.get(current_tid, None)
        else:
            continue
        
        for p in [pr, current_pr]:  
            if p is not None:
                delta = p.add_time(ev_type, state, tm, rq_id)
                
                if do_warn and delta is not None and is_outlier(state, delta):
                    print 'OUTLIER "%s" rq: %d time: %d p: %d(%s) D=%.1f' % (ev_name, rq_id, tm, p.tid, state, delta)
                    # print p.requests
                
                
print '%-8s %5s %8s %9s %9s %9s' % ('STATE', 'TID', 'N', 'MEAN', 'VAR', 'SD')

for state in STATE_LIST:
    for p in proc.values():
        times = p.times[state]
        
        if times:
            mean = sum(times) / len(times)
            
            # Filter outliers
            if do_filter:
                times = filter(lambda t: not is_outlier(state, t), times)
            
            if do_dump:
                f = file('%s-%d.txt' % (state, p.tid), 'w')
                for t in times:            
                    print >> f,  t
            
            N    = len(times)
            mean = sum(times) / N
            var  = sum(map(lambda t: (t - mean) ** 2, times)) / N
            sd   = math.sqrt(var)
            
            print '%-8s %5s %8d %9.3f %9.3f %9.3f' % (state, p.tid, N, mean, var, sd)