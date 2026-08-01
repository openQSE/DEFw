#!/usr/bin/env python3

import importlib


def main():
	module = importlib.import_module("cdefw_external_swig_fixture")
	if module.defw_external_swig_fixture_name() != "external-swig-fixture":
		raise AssertionError("external SWIG fixture name mismatch")
	if module.defw_external_swig_fixture_add(2, 5) != 7:
		raise AssertionError("external SWIG fixture add mismatch")


if __name__ == "__main__":
	main()
