#!/usr/bin/env python3
"""Does an install of DEFw v2 run with nothing but its own prefix?

It installs src2 from a build tree into a scratch prefix, then runs the
compat smoke test from that install: the installed defw2-dirsvc, the
installed defw2-python and the installed defw2 package, with
LD_LIBRARY_PATH and PYTHONPATH cleared. So the binaries have to find
libdefw2, Margo and libfabric through their RPATHs, the extension has to
find libdefw2 relative to itself, and the launcher has to find the package
the install put beside it. A process started over ssh gets exactly that
bare environment.

	defw2_install_smoke.py --build DIR --cmake PATH --python PATH
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

failures = []


def check(what, ok, detail=None):
	print('{:<62} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)
		if detail:
			print('    ' + str(detail)[-3000:])


def main(argv):
	parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
	parser.add_argument('--build', required=True,
			    help='the src2 build directory')
	parser.add_argument('--cmake', required=True)
	parser.add_argument('--python', required=True)
	args = parser.parse_args(argv)

	prefix = os.path.join(os.getcwd(), 'install-smoke')
	shutil.rmtree(prefix, ignore_errors=True)
	result = subprocess.run(
		[args.cmake, '-DCMAKE_INSTALL_PREFIX=' + prefix, '-P',
		 os.path.join(args.build, 'cmake_install.cmake')],
		capture_output=True, text=True)
	check('src2 installs into a scratch prefix', result.returncode == 0,
	      result.stdout + result.stderr)

	launcher = os.path.join(prefix, 'bin', 'defw2-python')
	dirsvc = os.path.join(prefix, 'bin', 'defw2-dirsvc')
	extension = glob.glob(os.path.join(
		prefix, 'lib*', 'python*', 'site-packages', 'defw2',
		'_defw2*.so'))
	check('with the launcher, the directory and the binding',
	      os.access(launcher, os.X_OK) and os.access(dirsvc, os.X_OK) and
	      len(extension) == 1 and
	      glob.glob(os.path.join(prefix, 'lib*', 'python*',
				     'site-packages', 'defw2', 'compat',
				     '_v1', 'defw.py')),
	      (launcher, dirsvc, extension))

	env = {key: value for key, value in os.environ.items()
	       if key not in ('LD_LIBRARY_PATH', 'PYTHONPATH')}
	for path in [dirsvc] + extension:
		result = subprocess.run(['ldd', path], capture_output=True,
					text=True, env=env)
		found = [line.split('=>')[1].split('(')[0].strip()
			 for line in result.stdout.splitlines()
			 if 'libdefw2' in line and '=>' in line]
		check('{} loads the libdefw2 installed beside it'.format(
			os.path.basename(path)),
		      'not found' not in result.stdout and len(found) == 1 and
		      os.path.realpath(found[0]).startswith(
			      os.path.realpath(prefix) + os.sep),
		      result.stdout)
	env['DEFW2_PYTHON'] = args.python
	env.setdefault('DEFW_PATH', os.path.normpath(
		os.path.join(HERE, '..', '..')))
	result = subprocess.run(
		[launcher, os.path.join(HERE, 'defw2_compat_smoke.py'),
		 '--dirsvc', dirsvc, '--launcher', launcher],
		capture_output=True, text=True, timeout=600, env=env)
	sys.stdout.write(''.join('    ' + line + '\n'
				 for line in result.stdout.splitlines()
				 if 'FAILED' in line))
	check('the compat smoke passes from the install, with no library '
	      'or Python path', result.returncode == 0 and
	      'COMPAT SMOKE PASSED' in result.stdout,
	      result.stdout[-2000:] + result.stderr[-2000:])

	print('INSTALL SMOKE ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
