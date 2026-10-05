#!/usr/bin/env python3
"""Check the line counter the comparison's line-count criterion rests on.

The counter has to tell code from comment where the two look alike: a
comment marker inside a string, a string that spans lines, a docstring
against any other string, a backslash at the end of a line. Then it counts
both versions of DEFw, so a source file that belongs to no area fails here
rather than going uncounted.

	defw2_loc_check.py <DEFw checkout>
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'bench'))

import defw_loc  # noqa: E402

failures = []


def check(what, ok):
	print('{:<64} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)


C_SOURCE = r'''/*
 * A block comment
 */
#include <stdio.h>

// A line comment
static const char *s = "/* not a comment */";	/* but this is */
static const char *u = "/* opens nothing";
static char c = '"';
static const char *t = "a string \
that goes on";
#define TWO(x) \
	((x) + \
	 (x))

int main(void) { /* one */ return 0; /* two */ }
/* a comment that ends */ int x;
'''
C_CODE = {4, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17}

PY_SOURCE = '''"""A module's docstring,
over two lines."""
import os

# A comment


def f(x):
	"""A function's docstring."""
	text = """not a docstring,
	a string"""
	return x  # and a comment


class C:
	\'\'\'A class's docstring.\'\'\'
	value = '#not a comment'
'''
PY_CODE = {3, 8, 10, 11, 12, 15, 17}


def main(argv):
	if len(argv) != 1:
		raise SystemExit('usage: defw2_loc_check.py <DEFw checkout>')
	found = defw_loc.c_code_lines(C_SOURCE)
	check('C: code lines are the ones that hold code', found == C_CODE)
	if found != C_CODE:
		print('  expected {}, found {}'.format(sorted(C_CODE),
						      sorted(found)))
	found = defw_loc.python_code_lines(PY_SOURCE)
	check('Python: docstrings and comments are not code',
	      found == PY_CODE)
	if found != PY_CODE:
		print('  expected {}, found {}'.format(sorted(PY_CODE),
						      sorted(found)))

	root = argv[0]
	try:
		v1 = defw_loc.count_version(root, ('src', 'python'),
					    defw_loc.V1_AREAS,
					    defw_loc.V1_LEFT_OUT)
		v2 = defw_loc.count_version(root, ('src2',), defw_loc.V2_AREAS,
					    defw_loc.V2_LEFT_OUT)
	except SystemExit as unplaced:
		print('  ' + str(unplaced))
		v1 = v2 = {'lines': 0, 'files': {}, 'areas': []}
	check('both versions count, every file in an area',
	      v1['lines'] > 0 and v2['lines'] > 0)
	check('no test, benchmark or example is counted',
	      not any(path.startswith(defw_loc.V2_LEFT_OUT +
				      defw_loc.V1_LEFT_OUT)
		      for version in (v1, v2) for path in version['files']))
	check('the subset leaves out exactly the areas that say why',
	      all(row['equivalent'] == (row['left_out_because'] is None)
		  for version in (v1, v2) for row in version['areas']))
	print('LOC CHECK ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
