"""Synthetic test fixtures only; never imported by the production entry point."""
import os
from pathlib import Path
import secrets
import stat
import uuid

from config_broker.core import BrokerError, ConfigBroker
from config_broker.protocol import dispatch
from relay.runtime import Runtime


def pair_device(store, device, token=None, now=None):
    token = token or secrets.token_hex(32)
    identifier = str(uuid.uuid4())
    code, _ = store.identity.issue_pairing_code(now=now)
    store.identity.create_enrollment(device, code, identifier, token, now)
    store.identity.activate_enrollment(identifier, token, now)
    store.identity.finalize_enrollment(identifier, token, now)
    return token


def resolve_data_fixture(store, streams, device, recording, present, clock):
    """Use the actual reconciliation operation; no ledger in these codec fixtures.

    HTTP administrator authorization and configuration pins have separate runtime tests.
    """
    runtime = object.__new__(Runtime)
    import threading
    runtime._mutex, runtime._active = threading.RLock(), None
    runtime.store, runtime.streams, runtime._now = store, streams, clock
    from relay.auth import UploadAuthorization
    runtime.auth = UploadAuthorization(store)
    runtime._settle = lambda _key: None
    return runtime.reconcile_job(device, recording, present)


def test_ownership(info, mode):
    if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != mode:
        raise BrokerError('unsafe_permissions')
    if stat.S_ISREG(info.st_mode) and info.st_nlink != 1:
        raise BrokerError('unsafe_permissions')


class LocalBroker:
    """Exercises the real RPC allowlist and core with explicit test ownership."""
    def __init__(self, directory, fault_hook=None):
        self.core = ConfigBroker(directory, ownership_checker=test_ownership, fault_hook=fault_hook)

    def call(self, method, **params):
        result = dispatch(self.core, {'version': 1, 'id': 'test-only', 'method': method, 'params': params})
        if not result['ok']:
            raise BrokerError(result['error']['code'])
        return result['result']

    def __getattr__(self, name):
        return lambda *args, **kwargs: getattr(self.core, name)(*args, **kwargs)

    def close(self):
        self.core.close()


def ready_broker(directory):
    broker = LocalBroker(directory)
    epoch = broker.begin_maintenance()['generation']
    broker.reconcile(epoch, {})
    broker.end_maintenance(epoch)
    return broker
