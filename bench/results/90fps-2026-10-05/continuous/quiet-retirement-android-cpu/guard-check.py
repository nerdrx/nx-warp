#!/usr/bin/env python3
"""Check only the process guard; no device calls or fixture execution."""
import ast, tempfile
from pathlib import Path
from types import SimpleNamespace
source=Path(__file__).with_name('run-device.py').read_text()
tree=ast.parse(source)
fn=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='require_idle_processes')
module=ast.fix_missing_locations(ast.Module(body=[fn],type_ignores=[]))
with tempfile.TemporaryDirectory() as d:
 for name in ['org.meumeu.wivrn.nx','org.meumeu.wivrn.nx.warp','org.meumeu.wivrn.github','org.meumeu.wivrn.nx:decoder','nx_quiet_gate','/data/local/tmp/nx_quiet_gate','com.android.systemui','']:
  calls=[]
  def adb(*args):
   calls.append(args)
   return SimpleNamespace(returncode=0,stdout='PID NAME\n'+('123 '+name+'\n' if name else ''))
  namespace={'adb':adb,'a':SimpleNamespace(output=Path(d))}
  exec(compile(module,'run-device.py guard','exec'),namespace)
  denied=False
  try: namespace['require_idle_processes']()
  except SystemExit as e:
   denied=True;assert str(e).startswith('Refusing CPU test:')
  assert denied==(name.startswith('org.meumeu.wivrn') or name.rsplit('/',1)[-1]=='nx_quiet_gate')
  assert calls==[('shell','ps','-A','-o','PID,NAME')]
 for status,text in [(1,''),(0,''),(0,'unrecognized option')]:
  namespace={'adb':lambda *args:SimpleNamespace(returncode=status,stdout=text),'a':SimpleNamespace(output=Path(d))}
  exec(compile(module,'run-device.py guard','exec'),namespace)
  try: namespace['require_idle_processes']()
  except AssertionError: pass
  else: raise AssertionError('Failed-open process inventory')
print('11 process-guard cases pass; no device or fixture execution.')
