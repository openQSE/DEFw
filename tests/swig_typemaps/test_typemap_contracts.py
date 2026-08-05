#!/usr/bin/env python3

import importlib
import subprocess
import sys


LEGACY_CHARPPP_WARNING = (
	"swig/python detected a memory leak of type 'char **', "
	"no destructor found"
)


def expect(condition, message):
	if not condition:
		raise AssertionError(message)


def expect_legacy_charppp_warning():
	script = "\n".join((
		"import gc",
		"import importlib",
		"module = importlib.import_module('defw_typemap_fixture')",
		"value = module.defw_typemap_make_compat_pointer()",
		"assert isinstance(value, tuple), repr(value)",
		"assert len(value) == 2 and value[0] == 0, repr(value)",
		"assert value[1] is not None, repr(value)",
		"del value",
		"gc.collect()",
	))
	result = subprocess.run(
		[sys.executable, "-c", script],
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
		text=True,
		check=False,
	)
	expect(result.returncode == 0,
	       "compat char*** child check failed: "
	       f"stdout={result.stdout!r} stderr={result.stderr!r}")
	output = result.stdout + result.stderr
	expect(LEGACY_CHARPPP_WARNING in output,
	       "compat char*** legacy warning was not emitted: "
	       f"stdout={result.stdout!r} stderr={result.stderr!r}")


def main():
	module = importlib.import_module("defw_typemap_fixture")

	compat = module.defw_typemap_make_compat_string()
	expect(compat == [0, "compat-string"] or compat == (0, "compat-string"),
	       f"unexpected compat char** result: {compat!r}")

	expect_legacy_charppp_warning()

	owned = module.defw_typemap_make_owned_string()
	expect(owned == [0, "owned-string"] or owned == (0, "owned-string"),
	       f"unexpected owned string result: {owned!r}")

	try:
		module.defw_typemap_make_null_owned_string()
	except MemoryError:
		pass
	else:
		raise AssertionError("NULL owned string did not raise MemoryError")

	items = module.defw_typemap_make_owned_string_list()
	expect(items == [0, ["alpha", "beta", "gamma"]] or
	       items == (0, ["alpha", "beta", "gamma"]),
	       f"unexpected owned string-list result: {items!r}")

	handle = module.defw_typemap_get_handle()
	expect(handle is not None, "opaque handle is None")
	expect("defw_typemap_handle" in repr(handle),
	       f"unexpected opaque handle repr: {handle!r}")


if __name__ == "__main__":
	main()
