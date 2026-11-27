#!/usr/bin/env python3
"""Run the comparison campaign: every workload, on both versions of DEFw.

Phase 3 ends with one campaign, from one build in one session: W1 to W7
rerun, W1 to W4 on each provider Phase 0 paired, and W5 to W7 on ofi+tcp.
The service and the clients get CPUs of their own, as on separate nodes,
and each workload's headline pair runs once more with the CPUs shared, as
Phase 0 and Phase 2 ran. This drives the harnesses beside it and gathers
their reports, a comparison of them, and the line counts, in one directory.

It runs on the host of a QFw-SLURM-Cluster, the cluster the measurements
are made on, because placing containers on CPUs is the host's to do:

	defw_campaign.py --prefix /workspace/qfw-container-base/qfw-install \\
		--out shared-dir/defw2-baselines/2026-10-06-campaign

--plan lists the runs without making them, and --quick runs every one
with fewer calls, which tests the campaign rather than measuring anything.

The CPUs. The Docker VM needs ten. For W1 to W4 the node running the
harness gets CPUs 0 to 7, and the launcher puts the service on 4 to 7 and
the clients on 0 to 3. For W5 and W6 the clients' node gets 0 to 3 and the
QPM's node 4 to 7. For W7 the compute nodes get 0 to 3 and the nodes the
site's QPMs run on 4 to 7. Every other container waits on 8 and 9. Each
container gets back what it had when the campaign ends, however it ends.

A run that fails is tried once more, and the campaign keeps both tries,
since a run that fails now and then is a result too: v1 at eight clients
sometimes loses a client's connection while it warms up. A failed try's
run directory goes under failed/, so the comparison reads only the runs
that passed.

Before it starts, it removes the shared-memory regions Mercury's na+sm
left in /dev/shm for processes that no longer exist, which a process that
is killed cannot remove itself. A container's /dev/shm is 64 MiB, and once
it fills, libfabric's sm2 refuses to start and v1 falls back to tcp.

W5 and W6 start a QFw plane of their own for every run, with the fake IQM
QPM given eight slots as in Phase 2. W7 runs qfw_qiskit_simple.sh under
salloc against the site planes, which have to be running already, the v1
plane from site.yaml and the v2 plane from site-defw2.yaml.
"""

import argparse
import os
import re
import shlex
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone

BENCH_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BENCH_DIR)

import defw_bench_common as common  # noqa: E402

V1_TRANSPORTS = ('tcp', 'ofi+tcp', 'ofi+sm2')
V2_TRANSPORTS = ('ofi+tcp', 'na+sm')
NEEDED_CPUS = 10
SERVICE_CPUS = '4-7'
CLIENT_CPUS = '0-3'
OTHER_CPUS = '8-9'
RUN_TIMEOUT_S = 3 * 3600
W7_SITE = {1: '/etc/openqse/qfw/site.yaml',
	   2: '/etc/openqse/qfw/site-defw2.yaml'}

# The plane W5 and W6 run against: QFw's local profile, with the fake IQM
# QPM given the eight slots its device profile declares, and the log
# levels Phase 0 ran v1 at.
PLANE_RUNTIME = """\
resolver:
  scope-order:
    - local
local-services:
  start-prte: true
  start-dirsvc: true
  start-qpm: true
  dirsvc:
    name: qfw-local-dirsvc
    bind-host: 127.0.0.1
    port: auto
  service-manifest: {manifest}
"""
PLANE_SERVICES = """\
mpi-launch:
  launcher: mpirun
  allow-run-as-root: auto
  export-env:
    - LD_LIBRARY_PATH
  bind-to: core
  map-by: ppr:1:l3cache
  mca:
    btl: ^tcp,ofi,vader,openib
    pml: ^ucx
    mtl: ofi
    opal_common_ofi_provider_include: shm+cxi:linkx
services:
  - name: fake-iqm
    credential-mode: no-secret
    module: svc_fake_iqm_qpm
    load-modules: svc_fake_iqm_qpm,api_launcher
    agent-prefix: qpm_fake_iqm
    target: group1-head
    assigned-hosts: {slots}
    assigned-hosts-env: QFW_QPM_ASSIGNED_HOSTS
    device-id: fake-iqm-20q
    log-level: error
    py-log-level: critical
    provider-launch:
      type: internal
"""

# Removes the na+sm regions of processes that are gone, and says how many
# it removed and how much of /dev/shm is free. Mercury names each region
# after the process that made it.
SHM_SWEEP = """\
n=0
for f in /dev/shm/na_sm-*; do
	pid=${f#/dev/shm/na_sm-}
	pid=${pid%%-*}
	if [ -e "$f" ] && [ ! -d "/proc/$pid" ]; then
		rm -f "$f" && n=$((n + 1))
	fi
done
echo "removed $n free $(df -k /dev/shm | awk 'NR == 2 {print $4}')"
"""

# Variables that would change what a harness loads, cleared before QFw is
# activated, so every command starts from the same place.
CLEARED = ('QFW_PREFIX QFW_BIN_PATH QFW_LIBEXEC_DIR QFW_SHARE_DIR '
	   'QFW_SITE_CONFIG DEFW_PREFIX DEFW_PATH DEFW_CONFIG_PATH PYTHONPATH '
	   '_QFW_ACTIVE DEFW_EXTERNAL_SERVICES_PATH '
	   'DEFW_EXTERNAL_SERVICE_APIS_PATH QFW_DEFW_VERSION DEFW_TRANSPORT '
	   'DEFW_OFI_PROVIDER')


class Run:
	"""One launcher run. layout says where its processes go: 'local' and
	'qpm' give the service and the clients CPUs of their own, and
	'shared' leaves every container as it was."""

	def __init__(self, workload, version, transport, clients, layout,
		     client='c', service='c', events=False, payload=None,
		     calls=None, timeout=None, retries=None):
		self.workload = workload
		self.version = version
		self.transport = transport
		self.clients = clients
		self.layout = layout
		self.client = client
		self.service = service
		self.events = events
		self.payload = payload
		self.calls = calls
		# The launcher's own limit on the run, and tries after a
		# failure, when they are not the campaign's.
		self.timeout = timeout
		self.retries = retries

	@property
	def qpm(self):
		return common.WORKLOADS[self.workload].get('qpm', False)

	@property
	def resolve(self):
		return common.WORKLOADS[self.workload].get('resolve', False)

	def name(self):
		parts = ['v{}'.format(self.version), self.workload]
		if self.events:
			parts.append('events')
		if self.qpm:
			parts.append(self.client)
		elif (self.client, self.service) != ('c', 'c'):
			parts.append('{}-client-{}-service'.format(
				self.client, self.service))
		if self.payload:
			parts.append(self.payload)
		parts += [self.transport, 'c{}'.format(self.clients),
			  self.layout]
		return '-'.join(parts)

	def describe(self):
		return dict(vars(self))


def plan(quick):
	"""Every run of the campaign, in the order they run."""
	runs = []
	for layout in ('local', 'shared'):
		runs += echo_runs(layout, quick)
	for layout in ('qpm', 'shared'):
		runs += qpm_runs(layout, quick)
	return runs


def pairing(layout):
	"""Each version's providers and the client counts for a layout. With
	the CPUs shared, a workload keeps only its headline pair, eight
	clients on ofi+tcp, as Phase 0 and Phase 2 measured."""
	if layout == 'shared':
		return {1: ('ofi+tcp',), 2: ('ofi+tcp',)}, (8,)
	return {1: V1_TRANSPORTS, 2: V2_TRANSPORTS}, (1, 8)


def echo_runs(layout, quick):
	"""W1 to W4, against each version's echo service and directory."""
	transports, counts = pairing(layout)
	fewer = {1: 300, 2: 1000} if quick else {1: None, 2: None}
	runs = []
	for workload in ('W1', 'W2', 'W4'):
		for clients in counts:
			for version, kinds in transports.items():
				runs += [Run(workload, version, kind, clients,
					     layout, calls=fewer[version])
					 for kind in kinds]
	if layout != 'shared':
		runs += python_runs(layout, fewer[2])
	runs += bulk_runs(layout, quick)
	return runs


def python_runs(layout, calls):
	"""W1 with Python on either side, eight clients against the Python
	service, and W4 from Python."""
	runs = []
	sides = (('python', 'c', 1), ('c', 'python', 1),
		 ('python', 'python', 1), ('c', 'python', 8))
	for transport in V2_TRANSPORTS:
		for client, service, clients in sides:
			runs.append(Run('W1', 2, transport, clients, layout,
					client=client, service=service,
					calls=calls))
		runs.append(Run('W4', 2, transport, 1, layout, client='python',
				calls=calls))
	return runs


def bulk_runs(layout, quick):
	"""W3 from one client. v1 has no bulk path over tcp, and its sm2
	moves a payload inline at well under a MiB a second, so it makes five
	calls of 16 MiB and none of 256 MiB, which would take six minutes a
	call."""
	payloads = ('16MiB',) if layout == 'shared' else \
		('1MiB', '16MiB') if quick else ('1MiB', '16MiB', '256MiB')
	transports = pairing(layout)[0]
	runs = []
	for payload, version, kind in [
			(payload, version, kind) for payload in payloads
			for version, kinds in transports.items()
			for kind in kinds]:
		if kind == 'tcp' or (kind, payload) == ('ofi+sm2', '256MiB'):
			continue
		slow = (kind, payload) == ('ofi+sm2', '16MiB')
		runs.append(Run('W3', version, kind, 1, layout,
				payload=payload,
				calls=5 if slow or quick else None))
	return runs


def qpm_runs(layout, quick):
	"""W5, polling and on events, and W6, on ofi+tcp. v1 at eight clients
	makes ten jobs a client, since a hundred did not finish in half an
	hour, and v1's W6 takes twenty seconds a job. v1's QPM at eight
	clients can also lose completions and spin at a whole CPU, so such a
	run gets ten minutes, and no second try that would do the same."""
	clients = (8,) if layout == 'shared' else (1, 8)
	typed = () if layout == 'shared' else ('c', 'python')
	runs = []
	for events in (False, True):
		for count in clients:
			v1_jobs = 5 if quick else None if count == 1 else 10
			busy = count > 1
			runs.append(Run('W5', 1, 'ofi+tcp', count, layout,
					client='qfw', events=events,
					calls=v1_jobs,
					timeout=600 if busy else None,
					retries=0 if busy else None))
			runs += [Run('W5', 2, 'ofi+tcp', count, layout,
				     client=client, events=events,
				     calls=50 if quick else None)
				 for client in typed + ('qfw',)]
	runs.append(Run('W6', 1, 'ofi+tcp', 1, layout, client='qfw',
			calls=1 if quick else None))
	for client in typed + ('qfw',):
		runs.append(Run('W6', 2, 'ofi+tcp', 1, layout, client=client,
				calls=2 if quick else None))
	return runs


class Cluster:
	"""The cluster's containers, and where their processes may run."""

	def __init__(self, args):
		self.args = args
		self.original = {}
		shape = '{{.HostConfig.NanoCpus}} {{.HostConfig.CpusetCpus}}'
		for name in docker('ps', '--format', '{{.Names}}').split():
			fields = docker('inspect', name, '--format', shape)
			nano, _, cpuset = fields.strip().partition(' ')
			self.original[name] = (int(nano), cpuset)
		self.cpus = int(docker('info', '--format', '{{.NCPU}}'))
		self.layout = 'shared'

	def check(self):
		for name in (self.args.node, self.args.qpm_node,
			     self.args.controller):
			if name not in self.original:
				raise SystemExit('no running container ' + name)
		if self.cpus < NEEDED_CPUS:
			raise SystemExit(
				'the Docker VM has {} CPUs, and the service '
				'and the clients need {} to have their '
				'own'.format(self.cpus, NEEDED_CPUS))

	def assigned(self, layout):
		"""The CPUs, and the CPU quota, each container gets in a layout.
		A quota of None leaves the container's own."""
		quota = 4 * 10 ** 9
		if layout == 'local':
			return {self.args.node: ('0-7', 2 * quota)}
		if layout == 'qpm':
			return {self.args.node: (CLIENT_CPUS, quota),
				self.args.qpm_node: (SERVICE_CPUS, quota)}
		places = {}
		for name in self.original:
			if re.fullmatch(r'c\d+', name):
				places[name] = (CLIENT_CPUS, None)
			elif name.endswith('-head') or '-worker-' in name:
				places[name] = (SERVICE_CPUS, None)
		return places

	def place(self, layout):
		"""Put every container where layout says. 'shared' is where
		they were."""
		if layout == self.layout:
			return
		if layout == 'shared':
			self.restore()
			return
		assigned = self.assigned(layout)
		for name in self.original:
			update(name, *assigned.get(name, (OTHER_CPUS, None)))
		self.layout = layout

	def restore(self):
		every = '0-{}'.format(self.cpus - 1)
		for name, (nano, cpuset) in self.original.items():
			try:
				update(name, cpuset or every, nano or None)
			except subprocess.CalledProcessError as exc:
				print('could not restore', name, exc.stderr,
				      file=sys.stderr)
		self.layout = 'shared'


def docker(*arguments, timeout=60):
	return subprocess.run(['docker', *arguments], capture_output=True,
			      text=True, check=True, timeout=timeout).stdout


def update(name, cpuset, nano=None):
	"""Give a container CPUs, and a CPU quota when nano is not None."""
	quota = ['--cpus', str(nano / 1e9)] if nano is not None else []
	docker('update', '--cpuset-cpus', cpuset, *quota, name)


def quoted(command):
	return ' '.join(shlex.quote(str(word)) for word in command)


def run_logged(command, timeout):
	"""Run command with its output and errors together, as a log
	wants them."""
	return subprocess.run(command, stdout=subprocess.PIPE,
			      stderr=subprocess.STDOUT, text=True,
			      timeout=timeout)


class Campaign:
	def __init__(self, args, cluster):
		self.args = args
		self.cluster = cluster
		self.host_root, self.container_root = args.shared.split('=', 1)
		self.out = os.path.abspath(args.out)
		self.runs_dir = os.path.join(self.out, 'runs')
		self.failed_dir = os.path.join(self.out, 'failed')
		self.logs_dir = os.path.join(self.out, 'logs')
		self.results = []
		self.started = None
		self.plane_runtime = None
		self.shm = None

	def inside(self, host_path):
		"""A host path on the shared mount as the containers see it."""
		path = os.path.abspath(host_path)
		root = os.path.abspath(self.host_root)
		if os.path.commonpath([path, root]) != root:
			raise SystemExit('{} is not under {}, the shared '
					 'mount'.format(path, root))
		return self.container_root + path[len(root):]

	def preamble(self):
		"""QFw active from the installation under test."""
		prefix = shlex.quote(self.args.prefix)
		venv = shlex.quote(self.args.venv)
		root = shlex.quote(self.container_root)
		return ('unset {cleared}; export QFW_PREFIX={prefix}; '
			'source {prefix}/bin/qfw-activate --venv {venv} '
			'>/dev/null; export QFW_SHARED_ROOT={root}; '
			'export QFW_RUN_BASE_DIR={root}/qfw-bench-runs; '
			'export DEFW_PY_LOGLEVEL=critical; '.format(
				cleared=CLEARED, prefix=prefix, venv=venv,
				root=root))

	def execute(self, node, script, log, timeout=RUN_TIMEOUT_S):
		"""Run script in node with QFw active, its output in log."""
		with open(log, 'a', encoding='utf-8') as stream:
			stream.write('$ {}\n'.format(script))
			stream.flush()
			done = run_logged(['docker', 'exec', node, 'bash', '-c',
					   self.preamble() + script], timeout)
			stream.write(done.stdout)
		return done

	def write_plane_config(self):
		directory = os.path.join(self.out, 'plane')
		os.makedirs(directory, exist_ok=True)
		slots = [self.args.qpm_node] + [
			'fake-iqm-slot-{}'.format(i) for i in range(1, 8)]
		manifest = os.path.join(directory, 'services.yaml')
		with open(manifest, 'w', encoding='utf-8') as stream:
			stream.write(PLANE_SERVICES.format(
				slots=','.join(slots)))
		runtime = os.path.join(directory, 'runtime.yaml')
		with open(runtime, 'w', encoding='utf-8') as stream:
			stream.write(PLANE_RUNTIME.format(
				manifest=self.inside(manifest)))
		self.plane_runtime = self.inside(runtime)

	def plane_up(self, version, log):
		"""A fresh plane serving the fake IQM QPM, and its run."""
		if version == 2:
			select = 'export QFW_DEFW_VERSION=2; '
		else:
			select = ('export DEFW_TRANSPORT=ofi '
				  'DEFW_OFI_PROVIDER=tcp; ')
		setup = quoted(['qfw-setup', '--runtime-config',
				self.plane_runtime, '--service-id', 'fake-iqm'])
		done = self.execute(self.args.qpm_node, select + setup +
				    ' 2>/tmp/defw-campaign-plane.err | tail -1',
				    log, timeout=600)
		lines = done.stdout.strip().splitlines()
		if lines and lines[-1].startswith('/'):
			return lines[-1]
		return None

	def plane_down(self, run, log):
		self.execute(self.args.qpm_node, quoted(
			['qfw-teardown', '--keep-run-dir', '--run-dir', run]) +
			' >/dev/null 2>&1', log, timeout=600)

	def launcher(self, run, plane):
		"""The launcher command for run."""
		args = self.args
		bench = self.inside(BENCH_DIR)
		if run.version == 1 and not run.qpm:
			command = ['python3', bench + '/v1/defw1_bench.py']
		else:
			command = ['python3', bench + '/v2/defw2_bench.py',
				   '--client', run.client,
				   '--bin-dir', args.prefix + '/bin']
			if not run.qpm and not run.resolve:
				command += ['--service', run.service]
		command += [run.workload, '--transport', run.transport,
			    '--clients', run.clients,
			    '--defw-revision', args.revision,
			    '--image', args.image]
		if run.payload:
			command += ['--payload', run.payload]
		if run.calls:
			command += ['--calls', run.calls]
		if run.timeout:
			command += ['--timeout', run.timeout]
		if run.events:
			command.append('--events')
		if plane:
			command += ['--qfw-run-dir', plane]
		if run.layout == 'local':
			command += ['--service-cpus', SERVICE_CPUS]
		if run.layout in ('local', 'qpm'):
			command += ['--client-cpus', CLIENT_CPUS]
		return command

	def script(self, run, plane):
		"""Run the launcher on node-local storage, as the harnesses ask,
		then keep its run directory on the shared mount."""
		tmp = '/tmp/defw-campaign/' + run.name()
		command = self.launcher(run, plane) + ['--out', tmp]
		return ('rm -rf {tmp}; mkdir -p {tmp}; {command}; rc=$?; '
			'cp -r {tmp}/*/ {keep}/ 2>/dev/null; '
			'echo "kept $(ls {tmp})"; rm -rf {tmp}; '
			'exit $rc'.format(tmp=tmp, command=quoted(command),
					  keep=shlex.quote(
						  self.inside(self.runs_dir))))

	def measure(self, run):
		"""Make run, and once more if it fails."""
		retries = self.args.retries if run.retries is None \
			else run.retries
		for attempt in range(1 + retries):
			result = self.attempt(run, attempt)
			if not result['failure']:
				break
		return result

	def attempt(self, run, attempt):
		suffix = '-retry{}'.format(attempt) if attempt else ''
		log = os.path.join(self.logs_dir, run.name() + suffix + '.log')
		plane = None
		started = time.time()
		try:
			if run.qpm:
				plane = self.plane_up(run.version, log)
				if plane is None:
					why = 'the plane did not start'
					return self.record(run.describe(),
							   run.name() + suffix,
							   why, started, log)
			done = self.execute(self.args.node,
					    self.script(run, plane), log)
			kept = re.findall(r'^kept (\S+)$', done.stdout, re.M)
			failure = None if done.returncode == 0 else \
				'exit {}'.format(done.returncode)
			result = run.describe()
			result['run_dir'] = kept[-1] if kept else None
			result['attempt'] = attempt
			if failure and result['run_dir']:
				self.set_aside(result['run_dir'])
			return self.record(result, run.name() + suffix, failure,
					   started, log)
		except subprocess.TimeoutExpired:
			return self.record(run.describe(), run.name() + suffix,
					   'timed out', started, log)
		finally:
			if plane is not None:
				self.plane_down(plane, log)

	def set_aside(self, run_dir):
		"""A failed try's report, out of the comparison's way."""
		os.makedirs(self.failed_dir, exist_ok=True)
		source = os.path.join(self.runs_dir, run_dir)
		target = os.path.join(self.failed_dir, run_dir)
		if os.path.isdir(source):
			os.rename(source, target)

	def record(self, result, name, failure, started, log, note=None):
		result.update({'name': name, 'failure': failure,
			       'seconds': round(time.time() - started, 1),
			       'log': os.path.relpath(log, self.out)})
		self.results.append(result)
		print('{:<56} {:>7.1f} s  {}'.format(
			name, result['seconds'], failure or note or 'ok'),
		      flush=True)
		self.save()
		return result

	def w7(self, version, backend):
		"""qfw_qiskit_simple.sh under salloc, against a site plane."""
		layout = self.cluster.layout
		name = 'v{}-W7-{}-{}'.format(version, backend, layout)
		log = os.path.join(self.logs_dir, name + '.log')
		select = 'export QFW_SITE_CONFIG={}; '.format(W7_SITE[version])
		if version == 2:
			select += 'export QFW_DEFW_VERSION=2; '
		inner = (self.preamble() + select +
			 'export QFW_EXAMPLE_SERVICE_MODE=site; '
			 'export QFW_EXAMPLE_BACKEND={}; '
			 'cd "$QFW_SHARE_DIR/examples"; start=$(date +%s%N); '
			 './qfw_qiskit_simple.sh 4; rc=$?; '
			 'echo "W7 rc=$rc ms=$(( ($(date +%s%N) - start) '
			 '/ 1000000 ))"; exit $rc'.format(backend))
		salloc = ('salloc --nodes=1 --ntasks=1 --time=00:15:00 '
			  'bash -c ' + shlex.quote(inner))
		started = time.time()
		done = run_logged(['docker', 'exec', self.args.controller,
				   'bash', '-c', salloc], 1800)
		with open(log, 'a', encoding='utf-8') as stream:
			stream.write(done.stdout)
		found = re.findall(r'W7 rc=(\d+) ms=(\d+)', done.stdout)
		passed = done.returncode == 0 and found and found[-1][0] == '0'
		result = {'workload': 'W7', 'version': version,
			  'backend': backend, 'layout': layout,
			  'ms': int(found[-1][1]) if found else None}
		self.record(result, name, None if passed else
			    'exit {}'.format(done.returncode), started, log,
			    '{} ms'.format(result['ms']))

	def save(self):
		containers = {name: {'nano_cpus': nano, 'cpuset': cpuset}
			      for name, (nano, cpuset)
			      in self.cluster.original.items()}
		common.write_json(os.path.join(self.out, 'campaign.json'), {
			'started': self.started,
			'arguments': vars(self.args),
			'containers': containers,
			'shm': self.shm,
			'results': self.results,
		}, indent=2)

	def sweep_shm(self):
		"""Clear the regions killed processes left in /dev/shm."""
		self.shm = {}
		for node in (self.args.node, self.args.qpm_node):
			out = docker('exec', node, 'bash', '-c',
				     SHM_SWEEP).split()
			self.shm[node] = {'removed': int(out[1]),
					  'free_kib': int(out[3])}
			print('{}: removed {} stale na+sm regions, {} MiB of '
			      '/dev/shm free'.format(node, out[1],
						     int(out[3]) // 1024))

	def run(self, runs, w7_runs):
		self.started = datetime.now(timezone.utc).isoformat()
		for directory in (self.runs_dir, self.logs_dir):
			os.makedirs(directory, exist_ok=True)
		self.sweep_shm()
		self.write_plane_config()
		order = {'local': 0, 'qpm': 1, 'shared': 2}
		for run in sorted(runs, key=lambda r: order[r.layout]):
			self.cluster.place(run.layout)
			self.measure(run)
		backends = self.args.w7_backends.split(',')
		for layout, times in (('w7', w7_runs), ('shared', 1)):
			if not w7_runs:
				break
			self.cluster.place(layout)
			for backend in backends:
				for version in (1, 2):
					for _ in range(times):
						self.w7(version, backend)

	def report(self):
		"""The comparison of every report, and the line counts."""
		compare = [os.path.join(BENCH_DIR, 'defw_bench_compare.py'),
			   self.runs_dir, '--json',
			   os.path.join(self.out, 'compare.json')]
		loc = [os.path.join(BENCH_DIR, 'defw_loc.py'), '--json',
		       os.path.join(self.out, 'loc.json')]
		for command, name in ((compare, 'compare.txt'),
				      (loc, 'loc.txt')):
			done = subprocess.run([sys.executable, *command],
					      capture_output=True, text=True)
			path = os.path.join(self.out, name)
			with open(path, 'w', encoding='utf-8') as stream:
				stream.write(done.stdout + done.stderr)


def interrupted(signum, frame):
	raise KeyboardInterrupt


def parse_args(argv):
	shared = os.path.dirname(os.path.dirname(os.path.dirname(
		os.path.dirname(BENCH_DIR))))
	parser = argparse.ArgumentParser(
		description='Run the DEFw v1 against v2 comparison campaign.')
	parser.add_argument(
		'--prefix', required=True,
		help='the QFw installation built with DEFw v2, as the '
		'containers see it')
	parser.add_argument(
		'--out', help='the campaign directory, on the shared mount '
		'(default: defw2-baselines/<date>-campaign there)')
	parser.add_argument(
		'--venv', default='/workspace/qfw-container-base/qfw-venv')
	parser.add_argument(
		'--shared', default=shared + '=/workspace/qfw-container-base',
		help='HOST=CONTAINER, the shared mount as each side sees it '
		'(default: %(default)s)')
	parser.add_argument(
		'--node', default='c1',
		help='where the harness runs (default: c1)')
	parser.add_argument(
		'--qpm-node', default='fake-iqm-head',
		help='where W5 and W6 start their QPM (default: fake-iqm-head)')
	parser.add_argument(
		'--controller', default='slurmctld',
		help='where W7 calls salloc (default: slurmctld)')
	parser.add_argument(
		'--revision', help='the DEFw revision the installation was '
		'built from (default: this checkout\'s)')
	parser.add_argument(
		'--image', help='the cluster image, kept in each report '
		'(default: the one --node runs)')
	parser.add_argument(
		'--only', help='the workloads to run, such as W1,W5')
	parser.add_argument(
		'--w7-runs', type=int, default=3,
		help='W7 runs a version and backend with CPUs of their own, '
		'0 for none (default: 3)')
	parser.add_argument('--w7-backends', default='nwqsim,fake-iqm')
	parser.add_argument(
		'--retries', type=int, default=1,
		help='tries after a run fails (default: 1)')
	parser.add_argument(
		'--quick', action='store_true',
		help='every run with fewer calls, to test the campaign itself')
	parser.add_argument(
		'--plan', action='store_true', help='list the runs and stop')
	return parser.parse_args(argv)


def main(argv):
	args = parse_args(argv)
	runs = plan(args.quick)
	if args.only:
		wanted = set(args.only.split(','))
		runs = [run for run in runs if run.workload in wanted]
		if 'W7' not in wanted:
			args.w7_runs = 0
	if args.quick:
		args.w7_runs = min(args.w7_runs, 1)
		args.w7_backends = args.w7_backends.split(',')[0]
	if args.plan:
		for run in runs:
			print('{:<56} calls {}'.format(run.name(),
						       run.calls or 'default'))
		print('{} runs, and W7 {} times a version and backend'.format(
			len(runs), args.w7_runs))
		return 0

	args.revision = args.revision or subprocess.run(
		['git', '-C', BENCH_DIR, 'rev-parse', 'HEAD'],
		capture_output=True, text=True).stdout.strip()
	args.out = args.out or os.path.join(
		args.shared.split('=', 1)[0], 'defw2-baselines',
		datetime.now().strftime('%Y-%m-%d') + '-campaign')
	cluster = Cluster(args)
	cluster.check()
	args.image = args.image or docker(
		'inspect', args.node, '--format', '{{.Config.Image}}').strip()
	campaign = Campaign(args, cluster)
	# Stopped by a signal, the campaign still puts every container back.
	signal.signal(signal.SIGTERM, interrupted)
	try:
		campaign.run(runs, args.w7_runs)
	finally:
		cluster.restore()
		campaign.save()
	campaign.report()
	# A run that passed on a later try is not a failed run.
	passed = {re.sub(r'-retry\d+$', '', r['name'])
		  for r in campaign.results if not r['failure']}
	failed = sorted({re.sub(r'-retry\d+$', '', r['name'])
			 for r in campaign.results if r['failure']} - passed)
	print('{} tries, {} runs failed{}'.format(
		len(campaign.results), len(failed),
		': ' + ', '.join(failed) if failed else ''))
	print('campaign in {}'.format(campaign.out))
	return 1 if failed else 0


if __name__ == '__main__':
	try:
		sys.exit(main(sys.argv[1:]))
	except KeyboardInterrupt:
		sys.exit(130)
