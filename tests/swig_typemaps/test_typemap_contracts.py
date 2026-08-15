#!/usr/bin/env python3

import importlib


def expect(condition, message):
	if not condition:
		raise AssertionError(message)


def main():
	module = importlib.import_module("defw_typemap_fixture")

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
