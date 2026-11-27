#!/usr/bin/env python3
"""Count the lines of code in C and Python files.

	defw_loc.py FILE...

A line of code is a line that holds something other than whitespace or a
comment. A docstring counts as a comment, and a preprocessor line counts as
code.

This counted both versions of DEFw for the comparison of v1 with v2, whose
line counts are in docs/comparison_report_v2.md. v1 is gone, so what is left
is the counting, which defw2_spank_flow_check uses.
"""

import ast
import io
import sys
import tokenize


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


def main(argv):
	if not argv:
		sys.stderr.write(__doc__.split('\n\n')[1] + '\n')
		return 2
	for path in argv:
		print('{:>8}  {}'.format(count_file(path), path))
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
