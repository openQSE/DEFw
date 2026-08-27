import sys
from pathlib import Path

def find_venv_sitepackages():
	exe = Path(sys.executable)
	venv_root = exe.parent.parent

	cur = exe.parent
	while cur != cur.parent:
		if (cur / "pyvenv.cfg").exists():
			venv_root = cur
			break
		cur = cur.parent

	version = f"{sys.version_info.major}.{sys.version_info.minor}"

	site_packages = venv_root / "lib" / f"python{version}" / "site-packages"
	return site_packages

def add_venv_sitepackages():
	sp = str(find_venv_sitepackages())
	sys.path.insert(0, sp)
