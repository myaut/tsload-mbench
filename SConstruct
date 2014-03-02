import os
import sys

from SCons.Action import ActionFactory
from SCons.Script.SConscript import SConsEnvironment

PathJoin = os.path.join
PathBaseName = os.path.basename
PathExists = os.path.exists

from SCons.Errors import StopError

SConsEnvironment.Chmod = ActionFactory(os.chmod,
        lambda dest, mode: 'Chmod("%s", 0%o)' % (dest, mode)) 

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

env['MACH'] = ''

# Append path to build tools
sys.path.append(PathJoin(env['TSLOADPATH'], 'tools', 'build'))

# Load tsload SConscripts
SConscript(PathJoin(env['TSLOADPATH'], 'SConscript.env.py'), 'env')
SConscript(PathJoin(env['TSLOADPATH'], 'SConscript.plat.py'), 'env')
SConscript(PathJoin(env['TSLOADPATH'], 'SConscript.install.py'), 'env')
SConscript(PathJoin(env['TSLOADPATH'], 'SConscript.etrace.py'), 'env')

# ------------
# MODULES

modules = ['bigmem', 'llc']

for mod in modules:
    variant_dir = env.BuildDir(PathJoin('mbench', mod))
    
    SConscript(PathJoin(mod, 'SConscript'), 'env',
               variant_dir = variant_dir)