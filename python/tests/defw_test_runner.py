#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import subprocess
import sys
import textwrap

import yaml


TESTS_DIR = Path(__file__).resolve().parent
PYTHON_DIR = TESTS_DIR.parent
DEFW_ROOT = PYTHON_DIR.parent
DEFAULT_CONFIG_DIR = TESTS_DIR / "configs"
DEFAULT_CONFIG_PATH = PYTHON_DIR / "config" / "defw_generic.yaml"
DEFAULT_MODULES = [
	"svc_resmgr",
	"api_resmgr",
]


def list_configs():
	return sorted(p.name for p in DEFAULT_CONFIG_DIR.glob("*.yaml"))


def resolve_config_path(config_name):
	path = Path(config_name)
	if path.is_file():
		return path.resolve()
	if path.suffix != ".yaml":
		path = DEFAULT_CONFIG_DIR / f"{config_name}.yaml"
	else:
		path = DEFAULT_CONFIG_DIR / path.name
	if path.is_file():
		return path.resolve()
	raise FileNotFoundError(f"Unable to find config '{config_name}'")


def load_config(path):
	with open(path, "r", encoding="utf-8") as stream:
		config = yaml.safe_load(stream) or {}
	if not isinstance(config, dict):
		raise ValueError("Config must be a YAML mapping")
	return config


def normalize_scripts(config):
	scripts = config.get("scripts", [])
	if not isinstance(scripts, list) or not scripts:
		raise ValueError("Config must define a non-empty scripts list")
	for script in scripts:
		if not isinstance(script, str) or not script.strip():
			raise ValueError("Each script entry must be a non-empty string")
	return scripts


def join_modules(config):
	modules = list(DEFAULT_MODULES)
	for module in config.get("modules", []):
		if module not in modules:
			modules.append(module)
	return ",".join(modules)


def build_environment(config):
	master = config.get("master", {})
	env = os.environ.copy()
	defw_path = str(DEFW_ROOT)
	src_path = str(DEFW_ROOT / "src")
	agent_name = master.get("agent_name", "master-resmgr")
	listen_port = str(master.get("listen_port", 25100))
	telnet_port = str(master.get("telnet_port", 25101))
	log_dir = str(master.get(
		"log_dir",
		Path("/tmp") / f"defw-{config.get('suite', 'suite')}-runner",
	))
	report_mode = str(master.get("report_mode", "both"))
	log_level = str(master.get("log_level", "DEBUG"))
	experiment_port_base = str(master.get(
		"experiment_port_base",
		int(listen_port) + 10,
	))

	env["LD_LIBRARY_PATH"] = (
		f"{src_path}{os.pathsep}{env['LD_LIBRARY_PATH']}"
		if env.get("LD_LIBRARY_PATH")
		else src_path
	)
	env["DEFW_PATH"] = defw_path
	env["DEFW_CONFIG_PATH"] = str(
		Path(master.get("config_path", DEFAULT_CONFIG_PATH)).resolve()
	)
	env["DEFW_AGENT_NAME"] = agent_name
	env["DEFW_AGENT_TYPE"] = "resmgr"
	env["DEFW_SHELL_TYPE"] = "cmdline"
	env["DEFW_LISTEN_PORT"] = listen_port
	env["DEFW_TELNET_PORT"] = telnet_port
	env["DEFW_PARENT_NAME"] = agent_name
	env["DEFW_PARENT_HOSTNAME"] = master.get("parent_hostname", "127.0.0.1")
	env["DEFW_PARENT_ADDR"] = master.get("parent_addr", "127.0.0.1")
	env["DEFW_PARENT_PORT"] = listen_port
	env["DEFW_LOG_DIR"] = log_dir
	env["DEFW_LOG_LEVEL"] = log_level
	env["DEFW_REPORT_MODE"] = report_mode
	env["DEFW_EXPERIMENT_PORT_BASE"] = experiment_port_base
	env["DEFW_ONLY_LOAD_MODULE"] = join_modules(config)

	for key, value in config.get("env", {}).items():
		env[str(key)] = str(value)
	return env


def build_python_command(config):
	suite = config.get("suite")
	if not isinstance(suite, str) or not suite.strip():
		raise ValueError("Config must define a suite name")
	scripts = normalize_scripts(config)
	indented_scripts = ",\n".join(f"    {script!r}" for script in scripts)
	return textwrap.dedent(
		f"""
		import defw

		suite = defw.experiments[{suite!r}].scripts
		for script_name in [
		{indented_scripts}
		]:
		    print(suite[script_name].run())
		defw.dumpGlobalTestResults()
		"""
	).strip()


def build_command(config):
	return [
		str(DEFW_ROOT / "src" / "defwp"),
		"-c",
		build_python_command(config),
	]


def parse_args():
	parser = argparse.ArgumentParser(
		description="Run DEFw experiments from a YAML config",
	)
	parser.add_argument(
		"config",
		nargs="?",
		help="Config name from python/tests/configs or a YAML path",
	)
	parser.add_argument(
		"--list-configs",
		action="store_true",
		help="List available built-in configs and exit",
	)
	parser.add_argument(
		"--dry-run",
		action="store_true",
		help="Print the resolved command and environment, then exit",
	)
	return parser.parse_args()


def main():
	args = parse_args()
	if args.list_configs:
		for name in list_configs():
			print(name)
		return 0
	if not args.config:
		raise SystemExit("A config name or path is required")

	config_path = resolve_config_path(args.config)
	config = load_config(config_path)
	env = build_environment(config)
	cmd = build_command(config)

	if args.dry_run:
		print(f"config: {config_path}")
		print("command:")
		print("  " + " ".join(cmd[:2]) + " <embedded-python>")
		print("environment:")
		for key in [
			"DEFW_PATH",
			"DEFW_CONFIG_PATH",
			"DEFW_AGENT_NAME",
			"DEFW_AGENT_TYPE",
			"DEFW_SHELL_TYPE",
			"DEFW_LISTEN_PORT",
			"DEFW_TELNET_PORT",
			"DEFW_PARENT_NAME",
			"DEFW_PARENT_HOSTNAME",
			"DEFW_PARENT_ADDR",
			"DEFW_PARENT_PORT",
			"DEFW_LOG_DIR",
			"DEFW_LOG_LEVEL",
			"DEFW_REPORT_MODE",
			"DEFW_EXPERIMENT_PORT_BASE",
			"DEFW_ONLY_LOAD_MODULE",
		]:
			print(f"  {key}={env[key]}")
		return 0

	completed = subprocess.run(cmd, env=env, cwd=str(DEFW_ROOT))
	return completed.returncode


if __name__ == "__main__":
	sys.exit(main())
