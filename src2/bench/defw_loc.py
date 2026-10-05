#!/usr/bin/env python3
"""Count the lines of code DEFw owns, v1's and v2's, for the comparison.

The design's criterion is DEFw-owned lines of code for equivalent function,
v2 fewer than v1's C and Python combined. This gives both counts the
criterion needs: every line each version owns, and the subset that does
what the other version does too. Each version's area table names what the
subset leaves out and why, so the judgement stays visible.

	defw_loc.py [--root DEFW] [--json PATH] [--files]

A line of code is a line that holds something other than whitespace or a
comment. A docstring counts as a comment, and a preprocessor line counts as
code. Tests, benchmarks, examples and generated files are left out of both
versions, and so are build files and shell wrappers, which neither version
counts as code.
"""

import argparse
import ast
import io
import json
import os
import subprocess
import sys
import tokenize

# Each version's areas, in order, as (name, equivalent, paths). A file
# belongs to the first area with a path that is its own or a directory
# above it. equivalent is False for an area of function the other version
# does not have, with the reason the table gives for leaving it out.
V1_AREAS = (
	('C transport', True, ('src/defw_transport',)),
	('C embedded Python', True, ('src/defw_python',)),
	('C runtime', True, ('src/',)),
	('telnet shell', 'v2 has no shell',
	 ('python/infra/defw_telnet', 'python/infra/defw_cmd.py')),
	('Python infrastructure', True, ('python/infra/',)),
	('directory service', True,
	 ('python/services/svc_dirsvc/', 'python/service-apis/api_dirsvc/')),
	('events API', True, ('python/service-apis/api_events/',)),
	('launcher service', 'v2 starts no services',
	 ('python/services/svc_launcher/',
	  'python/service-apis/api_launcher/')),
	('test services', 'test subjects, as v2\'s echo is',
	 ('python/services/svc_test_', 'python/service-apis/api_test_')),
	('experiments', 'a test framework', ('python/experiments/',)),
)
V1_LEFT_OUT = ('python/tests/',)

V2_AREAS = (
	('echo service', 'a test subject, as v1\'s test services are',
	 ('src2/services/echo/', 'src2/rpc/defw2_echo_client.c',
	  'src2/include/defw2/defw2_echo.h',
	  'src2/bindings/python/defw2/_echo.py')),
	('typed QPM API', 'v1 carries QFw\'s QPM API as Python QFw owns',
	 ('src2/qpm/', 'src2/include/defw2/defw2_qpm.h',
	  'src2/bindings/python/defw2/_qpm.py')),
	('compat', 'it runs v1\'s code on v2, which v1 needs no help to do',
	 ('src2/bindings/python/defw2/compat/',)),
	('telemetry', 'v1 carries trace context and records nothing',
	 ('src2/telemetry/', 'src2/include/defw2/defw2_telemetry.h')),
	('directory', True,
	 ('src2/dir/', 'src2/services/dirsvc/',
	  'src2/include/defw2/defw2_dir.h',
	  'src2/bindings/python/defw2/_dir.py')),
	('events', True,
	 ('src2/event/', 'src2/include/defw2/defw2_event.h',
	  'src2/bindings/python/defw2/_event.py')),
	('documents', True,
	 ('src2/rpc/defw2_doc.c', 'src2/include/defw2/defw2_doc.h',
	  'src2/bindings/python/defw2/_doc.py')),
	('service hosting', True,
	 ('src2/host/', 'src2/include/defw2/defw2_service.h',
	  'src2/bindings/python/defw2/_service.py')),
	('RPC and wire', True, ('src2/rpc/', 'src2/include/defw2/')),
	('runtime', True, ('src2/core/',)),
	('Python binding', True, ('src2/bindings/python/',)),
)
V2_LEFT_OUT = ('src2/tests/', 'src2/bench/', 'src2/examples/')

SOURCES = ('.c', '.h', '.py')
# What v1's build writes beside its sources: SWIG's wrappers and the
# Python modules over them. git leaves them out as untracked, and a tree
# without git skips them by name.
GENERATED = ('_wrap.c', 'cdefw_agent.py', 'cdefw_global.py')


def c_code_lines(text):
	"""Line numbers of a C file that hold code, comments and whitespace
	aside. String and character literals are code, so a comment marker
	inside one is not a comment."""
	lines = set()
	number = 1
	state = 'code'
	i = 0
	while i < len(text):
		ch = text[i]
		pair = text[i:i + 2]
		if ch == '\n':
			number += 1
			if state == 'line':
				state = 'code'
		elif state == 'code':
			if pair == '/*':
				state = 'block'
				i += 1
			elif pair == '//':
				state = 'line'
			elif not ch.isspace():
				lines.add(number)
				if ch in '"\'':
					state = ch
		elif state == 'block':
			if pair == '*/':
				state = 'code'
				i += 1
		elif state in '"\'':
			lines.add(number)
			if ch == '\\':
				i += 1
				if text[i:i + 1] == '\n':
					number += 1
			elif ch == state:
				state = 'code'
		i += 1
	return lines


def python_code_lines(text):
	"""Line numbers of a Python file that hold code. A docstring, the
	string that opens a module, class or function, is a comment."""
	docstrings = set()
	for node in ast.walk(ast.parse(text)):
		if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef,
				     ast.AsyncFunctionDef)) and node.body:
			first = node.body[0]
			if isinstance(first, ast.Expr) and \
			   isinstance(first.value, ast.Constant) and \
			   isinstance(first.value.value, str):
				docstrings.update(range(first.lineno,
							first.end_lineno + 1))
	skipped = (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE,
		   tokenize.INDENT, tokenize.DEDENT, tokenize.ENCODING,
		   tokenize.ENDMARKER)
	lines = set()
	for token in tokenize.generate_tokens(io.StringIO(text).readline):
		if token.type not in skipped:
			lines.update(range(token.start[0], token.end[0] + 1))
	return lines - docstrings


def count_file(path):
	"""The lines of code in one C or Python file."""
	with open(path, encoding='utf-8') as stream:
		text = stream.read()
	if path.endswith('.py'):
		return len(python_code_lines(text))
	return len(c_code_lines(text))


def tracked_files(root, prefixes):
	"""The C and Python files git tracks under prefixes, so a build tree
	or a generated wrapper beside them is never counted. A tree that is
	not a git checkout, such as an exported one, is walked instead."""
	done = subprocess.run(['git', '-C', root, 'ls-files', '--', *prefixes],
			      capture_output=True, text=True)
	if done.returncode == 0 and done.stdout:
		paths = done.stdout.splitlines()
	else:
		paths = walked_files(root, prefixes)
	return sorted(path for path in paths if path.endswith(SOURCES) and
		      not path.endswith(GENERATED))


def walked_files(root, prefixes):
	paths = []
	for prefix in prefixes:
		for directory, subdirectories, names in os.walk(
				os.path.join(root, prefix)):
			subdirectories[:] = [name for name in subdirectories
					     if name != '__pycache__']
			paths += [os.path.relpath(os.path.join(directory, name),
						  root) for name in names]
	return paths


def area_of(path, areas):
	for name, _, paths in areas:
		if path.startswith(paths):
			return name
	return None


def count_version(root, prefixes, areas, left_out):
	"""Lines per area, and each file's own count."""
	totals = {name: {'files': 0, 'lines': 0} for name, _, _ in areas}
	files = {}
	for path in tracked_files(root, prefixes):
		if path.startswith(left_out):
			continue
		area = area_of(path, areas)
		if area is None:
			raise SystemExit('{} belongs to no area'.format(path))
		lines = count_file(os.path.join(root, path))
		files[path] = (area, lines)
		totals[area]['files'] += 1
		totals[area]['lines'] += lines
	rows = []
	for name, equivalent, _ in areas:
		rows.append({'area': name, 'files': totals[name]['files'],
			     'lines': totals[name]['lines'],
			     'equivalent': equivalent is True,
			     'left_out_because': (None if equivalent is True
						  else equivalent)})
	return {
		'areas': rows,
		'lines': sum(row['lines'] for row in rows),
		'equivalent_lines': sum(row['lines'] for row in rows
					if row['equivalent']),
		'files': files,
	}


def git_revision(root):
	out = subprocess.run(['git', '-C', root, 'rev-parse', 'HEAD'],
			     capture_output=True, text=True)
	return out.stdout.strip() or None if out.returncode == 0 else None


def print_version(title, counted, show_files):
	print(title)
	print('  {:<24} {:>6} {:>8}  {}'.format('area', 'files', 'lines',
						 'left out of the subset'))
	for row in counted['areas']:
		print('  {:<24} {:>6} {:>8}  {}'.format(
			row['area'], row['files'], '{:,}'.format(row['lines']),
			row['left_out_because'] or ''))
	print('  {:<24} {:>6} {:>8}'.format(
		'all', sum(row['files'] for row in counted['areas']),
		'{:,}'.format(counted['lines'])))
	print('  {:<24} {:>6} {:>8}'.format(
		'equivalent function', '',
		'{:,}'.format(counted['equivalent_lines'])))
	if show_files:
		for path, (area, lines) in sorted(counted['files'].items()):
			print('    {:>6}  {}  ({})'.format(lines, path, area))
	print()


def main(argv):
	parser = argparse.ArgumentParser(
		description='Count the lines of code DEFw v1 and v2 own.')
	parser.add_argument(
		'--root', default=os.path.dirname(os.path.dirname(
			os.path.dirname(os.path.abspath(__file__)))),
		help='the DEFw checkout (default: the one this is in)')
	parser.add_argument('--json', help='also write the counts here')
	parser.add_argument('--files', action='store_true',
			    help='list every file with its count')
	args = parser.parse_args(argv)

	v1 = count_version(args.root, ('src', 'python'), V1_AREAS, V1_LEFT_OUT)
	v2 = count_version(args.root, ('src2',), V2_AREAS, V2_LEFT_OUT)
	print('Lines of code, comments and blank lines aside, at {}\n'.format(
		git_revision(args.root)))
	print_version('v1: src and python, without {}'.format(
		', '.join(V1_LEFT_OUT)), v1, args.files)
	print_version('v2: src2, without {}'.format(', '.join(V2_LEFT_OUT)),
		      v2, args.files)
	print('v2 against v1: {:,} against {:,} in all, {:,} against {:,} '
	      'for equivalent function'.format(
		      v2['lines'], v1['lines'], v2['equivalent_lines'],
		      v1['equivalent_lines']))
	if args.json:
		with open(args.json, 'w', encoding='utf-8') as stream:
			json.dump({'revision': git_revision(args.root),
				   'v1': v1, 'v2': v2}, stream, indent=2)
			stream.write('\n')
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
