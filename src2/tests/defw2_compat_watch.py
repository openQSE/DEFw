#!/usr/bin/env python3
"""A v1 process that follows the directory by its peer events, for
defw2_compat_smoke.py, which stops the directory under it.

It runs under defw2-python. It prints the directory's runtime ID once it
listens, then each peer event as it hears it, one JSON line each, until
its standard input closes.

	defw2-python defw2_compat_watch.py
"""

import json
import sys

import defw_workers
from defw2.compat import _state


def heard(event):
	print(json.dumps({'event_type': event['event_type'],
			  'runtime_id': event['remote_runtime_id'],
			  'reason': event['reason']}), flush=True)


# Adding the listener asks the directory at once, so the watch knows it
# before anything stops it.
defw_workers.add_peer_event_listener(heard)
print(json.dumps({'watching': _state.directory().v2.runtime_id()}),
      flush=True)
sys.stdin.read()
