"""defw2-python: run v1 Python on DEFw v2.

	defw2-python SCRIPT [ARGS...]
	defw2-python -m MODULE [ARGS...]
	defw2-python --serve MODULE [--execution-workers N]

The first two run a v1 client, as v1's defw-python did: the v1 names are
installed, then the script runs as __main__ with the arguments it was
given. The third serves a v1 QPM service module, the kind v1 loaded with
DEFW_ONLY_LOAD_MODULE, until it calls me.exit() or the process is told to
stop. Both read v2's environment, DEFW2_DIRSVC above all, which is where
the directory is.
"""

import os
import runpy
import sys

from . import install


def _usage():
	sys.stderr.write(__doc__.split('\n\n')[1] + '\n')
	return 2


def main(argv=None):
	argv = list(sys.argv[1:] if argv is None else argv)
	if not argv or argv[0] in ('-h', '--help'):
		return _usage()
	if argv[0] == '--serve':
		from ._serve import main as serve
		return serve(argv[1:])

	install()
	if argv[0] == '-m':
		if len(argv) < 2:
			return _usage()
		sys.argv = argv[1:]
		runpy.run_module(argv[1], run_name='__main__', alter_sys=True)
		return 0
	script = argv[0]
	sys.argv = argv
	sys.path.insert(0, os.path.dirname(os.path.abspath(script)))
	runpy.run_path(script, run_name='__main__')
	return 0


if __name__ == '__main__':
	sys.exit(main())
