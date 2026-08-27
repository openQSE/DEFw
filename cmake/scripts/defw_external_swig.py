#!/usr/bin/env python3

import argparse
import os
import shlex
import subprocess
import sys
import sysconfig
from pathlib import Path

import yaml


SWIG_FLAGS = [
	"-threads",
	"-python",
	"-includeall",
	"-D__x86_64__",
	"-D__arch_lib__",
	"-D_LARGEFILE64_SOURCE=1",
]


def run(cmd):
	print(" ".join(shlex.quote(str(part)) for part in cmd))
	subprocess.run(cmd, check=True)


def compiler_include_dirs(cc):
	cmd = [cc, "-xc", "-E", "-v", os.devnull]
	result = subprocess.run(cmd, text=True, capture_output=True, check=False)
	lines = result.stderr.splitlines()
	includes = []
	collect = False
	for line in lines:
		if "#include <...> search starts here:" in line:
			collect = True
			continue
		if "End of search list." in line:
			break
		if collect:
			path = line.strip()
			if path and Path(path).exists():
				includes.append(path)
	return includes


def load_entries(config):
	with open(config, "r", encoding="utf-8") as cfg:
		data = yaml.safe_load(cfg)
	if not isinstance(data, dict):
		raise ValueError(f"{config} does not contain a YAML mapping")
	try:
		entries = data["defw"]["swigify"]
	except KeyError as exc:
		raise ValueError(f"{config} is missing defw.swigify") from exc
	if not isinstance(entries, list):
		raise ValueError(f"{config} defw.swigify must be a list")
	return entries


def entry_files(entry):
	root = Path(entry["path"])
	if "files" in entry:
		files = []
		for file_name in entry["files"]:
			file_path = Path(file_name)
			if not file_path.is_absolute():
				file_path = root / file_path
			files.append(file_path)
		return files
	return sorted(root.glob("*.h"))


def read_text_files(paths):
	chunks = []
	for path in paths:
		with open(path, "r", encoding="utf-8") as file_obj:
			chunks.append(file_obj.read())
	return chunks


def list_paths(entry, key):
	return [str(Path(path)) for path in entry.get(key, [])]


def pkg_config_paths(entry):
	if entry.get("pkg_config", True) is False:
		return [], []
	if entry.get("include_dirs") or entry.get("library_dirs"):
		return [], []

	name = entry["name"]
	result = subprocess.run(
		["pkg-config", "--variable=prefix", name],
		text=True,
		capture_output=True,
		check=False,
	)
	if result.returncode != 0:
		msg = result.stderr.strip() or result.stdout.strip()
		raise RuntimeError(
			f"pkg-config could not find {name}; add include_dirs and "
			f"library_dirs to the SWIG config. {msg}"
		)
	prefix = Path(result.stdout.strip())
	return [str(prefix / "include")], [str(prefix / "lib")]


def write_interface(entry, files, output_dir):
	module = "c" + entry["name"]
	interface_path = output_dir / f"{module}.i"
	ignore = entry.get("ignore", [])
	addendums = read_text_files(entry.get("addendums", []))
	typemaps = read_text_files(entry.get("typemaps", []))

	with open(interface_path, "w", encoding="utf-8") as intf:
		intf.write(f"%module {module}\n")
		intf.write('%include "cwstring.i"\n')
		intf.write('%rename("%(strip:[__])s", regexmatch$name="__.*") "";\n')
		intf.write("%{\n")
		for chunk in addendums:
			intf.write(chunk)
			if not chunk.endswith("\n"):
				intf.write("\n")
		for header in files:
			intf.write(f'#include "{header}"\n')
		intf.write("%}\n")
		for chunk in typemaps:
			intf.write(chunk)
			if not chunk.endswith("\n"):
				intf.write("\n")
		intf.write("typedef long long ssize_t;\n")
		intf.write("typedef unsigned long long uint64_t;\n")
		intf.write("typedef unsigned int uint32_t;\n")
		intf.write("typedef unsigned short uint16_t;\n")
		intf.write("typedef unsigned char uint8_t;\n")
		for symbol in ignore:
			intf.write(f"%ignore {symbol};\n")
		for header in files:
			intf.write(f'%include "{header}"\n')
	return interface_path


def library_flags(entry):
	flags = []
	for library_dir in entry.get("library_dirs", []):
		flags.append("-L" + str(Path(library_dir)))
		for origin in ("$ORIGIN", str(Path(library_dir))):
			flags.append("-Wl,-rpath," + origin)
	for lib in entry.get("libs", []):
		flags.append("-l" + lib)
	return flags


def python_library_flags():
	flags = []
	lib_dir = sysconfig.get_config_var("LIBDIR")
	ldlibrary = sysconfig.get_config_var("LDLIBRARY")
	if lib_dir:
		flags.append("-L" + lib_dir)
	if ldlibrary and ldlibrary.startswith("lib"):
		name = Path(ldlibrary).stem[3:]
		flags.append("-l" + name)
	return flags


def build_entry(args, entry, system_includes):
	output_dir = Path(args.binary_dir)
	source_dir = Path(args.source_dir)
	output_dir.mkdir(parents=True, exist_ok=True)
	files = entry_files(entry)
	if not files:
		raise ValueError(f"no headers found for external SWIG entry {entry}")

	pkg_includes, pkg_libs = pkg_config_paths(entry)
	include_dirs = [
		str(source_dir / "src"),
		str(source_dir / "swig" / "typemaps"),
	]
	include_dirs += list_paths(entry, "include_dirs") + pkg_includes
	library_dirs = list_paths(entry, "library_dirs") + pkg_libs
	if pkg_libs:
		entry = dict(entry)
		entry["library_dirs"] = library_dirs

	interface_path = write_interface(entry, files, output_dir)
	module = "c" + entry["name"]
	wrapper_c = output_dir / f"{module}_wrap.c"
	swig_cmd = [
		args.swig,
		*SWIG_FLAGS,
		*(f"-I{path}" for path in include_dirs),
		*(f"-I{path}" for path in system_includes),
		"-outdir",
		str(output_dir),
		"-o",
		str(wrapper_c),
		str(interface_path),
	]
	run(swig_cmd)

	python_include = sysconfig.get_config_var("INCLUDEPY")
	extension = output_dir / f"_{module}.so"
	cc_cmd = [
		args.cc,
		"-g",
		"-Wall",
		"-fPIC",
		"-shared",
		*(f"-I{path}" for path in include_dirs),
		*(["-I" + python_include] if python_include else []),
		str(wrapper_c),
		"-o",
		str(extension),
		*library_flags(entry),
		*python_library_flags(),
	]
	run(cc_cmd)


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--config", required=True)
	parser.add_argument("--binary-dir", required=True)
	parser.add_argument("--source-dir", required=True)
	parser.add_argument("--swig", required=True)
	parser.add_argument("--cc", required=True)
	args = parser.parse_args()

	system_includes = compiler_include_dirs(args.cc)
	for entry in load_entries(args.config):
		build_entry(args, entry, system_includes)


if __name__ == "__main__":
	try:
		main()
	except Exception as exc:
		print(f"external SWIG generation failed: {exc}", file=sys.stderr)
		sys.exit(1)
