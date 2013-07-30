import os
import sys

PathJoin = os.path.join
PathBaseName = os.path.basename
PathExists = os.path.exists

from SCons.Errors import StopError 

# Setup path to tsload
AddOption('--with-tsload',  dest='tsload', action="store", default='',
          metavar='DIR', help='Path to tsload sources directory')

if not GetOption('tsload'):
    raise StopError('Provide path to tsload by specifying --with-tsload option')

env = DefaultEnvironment(ENV = {'PATH': os.environ['PATH']})

env['DEBUG'] = True

env['TSLOADPATH'] = GetOption('tsload')
env['TSPROJECT'] = 'mbench'
env['TSVERSION'] = '0.1'
env['TSNAME'] =  'mbench-' + env['TSVERSION']

# Load tsload SConscripts
SConscript(PathJoin(env['TSLOADPATH'], 'SConscript.env.py'), 'env')
SConscript(PathJoin(env['TSLOADPATH'], 'SConscript.plat.py'), 'env')
SConscript(PathJoin(env['TSLOADPATH'], 'SConscript.install.py'), 'env')

# ------------
# MODULES

modules = ['sched1']

for mod in modules:
    variant_dir = env.BuildDir(PathJoin('mbench', mod))
    
    SConscript(PathJoin(mod, 'SConscript'), 'env',
               variant_dir = variant_dir)