import contextlib
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode = True
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / 'scripts/linux-update.py'
spec = importlib.util.spec_from_file_location('sync_linux_update', SCRIPT)
module = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(module)
except FileNotFoundError:
    module = None

class FakeRunner:
    def __init__(self):
        self.calls = []
        self.responses = {}
        self.before_install = None
    def __call__(self, argv):
        self.calls.append(argv)
        if '--no-download' in argv and self.before_install is not None:
            self.before_install()
        for key, response in self.responses.items():
            if key in argv:
                if isinstance(response, Exception):
                    raise response
                return response
        return ''

class LinuxUpdateTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(module, 'safe Linux maintenance engine is missing')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runner = FakeRunner()
        self.engine = module.Maintenance(self.root, self.runner, os.getuid(),
            lambda argv, monitor: (monitor(), self.runner(argv)))
        self.write('/usr/lib/os-release', 'ID=ubuntu\nVERSION_ID="24.04"\n')
    def write(self, path, data):
        path = self.root / path.lstrip('/')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data)
        path.chmod(0o644)
        return path
    def test_simulation_rejects_unrelated_changes_and_bad_version(self):
        good = 'Inst noisedeck-sync [1.0] (1.1 Noisefactor Sync:noble-preview [amd64])\nConf noisedeck-sync (1.1 Noisefactor Sync:noble-preview [amd64])\n'
        module.validate_simulation(good, '1.1')
        for extra in ['Inst linux-image-generic (6.0 Ubuntu [amd64])\n', 'Remv libssl3t64 [3.0]\n', 'Inst noisedeck-sync (1.0 origin [amd64])\n']:
            with self.subTest(extra=extra), self.assertRaises(module.Deferred):
                module.validate_simulation(good + extra, '1.1')
        with self.assertRaises(module.Deferred):
            module.validate_simulation('unparseable output', '1.1')
    def test_enrollment_needs_packaged_verified_keyring(self):
        with self.assertRaisesRegex(module.Deferred, 'keyring|missing'):
            self.engine.enable(1000)
        self.assertFalse((self.root/'etc/apt/sources.list.d/noisedeck-sync.sources').exists())
        self.assertEqual(self.runner.calls, [])
    def test_enrollment_scopes_repository_and_does_not_create_symlinks(self):
        self.write('/usr/share/keyrings/noisedeck-sync-archive-keyring.gpg', 'fixture-keyring')
        self.write('/usr/share/noisedeck-sync/apt-key-fingerprint', 'A'*40+'\n')
        self.runner.responses['--show-keys'] = 'pub:::::::::\nfpr:::::::::'+'A'*40+':\n'
        self.runner.responses['--print-architecture'] = 'amd64\n'
        self.engine.enable(1000)
        source = (self.root/'etc/apt/sources.list.d/noisedeck-sync.sources').read_text()
        self.assertIn('Signed-By: /usr/share/keyrings/noisedeck-sync-archive-keyring.gpg', source)
        self.assertNotIn('Trusted:', source)
        pins = (self.root/'etc/apt/preferences.d/noisedeck-sync').read_text()
        self.assertIn('Package: *\nPin: origin downloads.noisefactor.io\nPin-Priority: -1', pins)
        self.assertEqual(json.loads((self.root/'etc/noisedeck-sync/update.json').read_text())['users'], [1000])
        self.assertTrue((self.root/'run/noisedeck-sync-update.lock').is_file())
        self.assertFalse(any(path.is_symlink() for path in self.root.rglob('*')))
        self.assertFalse(any('enable' in call for call in self.runner.calls))
    def test_unknown_enrollment_fields_and_user_write_permission_are_rejected(self):
        config = self.write('/etc/noisedeck-sync/update.json', '{"schema":1,"enabled":true,"users":[1000],"command":"bad"}')
        with self.assertRaises(module.Deferred): self.engine.config()
        config.write_text('{"schema":1,"enabled":true,"users":[1000]}')
        config.chmod(0o666)
        with self.assertRaises(module.Deferred): self.engine.config()
    def test_apt_hook_rejects_sync_payload_without_live_maintenance_lock(self):
        deb = self.write('/var/cache/apt/archives/noisedeck-sync_1.1_amd64.deb', 'payload')
        self.runner.responses['--field'] = 'noisedeck-sync\n1.1\namd64\n'
        with self.assertRaises(module.Deferred): self.engine.apt_hook([str(deb)])
    def test_apt_hook_ignores_unrelated_payload_and_rejects_unparseable_input(self):
        deb = self.write('/var/cache/apt/archives/other.deb', 'other')
        self.runner.responses['--field'] = 'other\n1\namd64\n'
        self.engine.apt_hook([str(deb)])
        with self.assertRaises(module.Deferred): self.engine.apt_hook(['relative.deb'])
    def test_apply_defers_disabled_enrollment_before_apt_or_service_commands(self):
        self.write('/etc/noisedeck-sync/update.json', '{"schema":1,"enabled":false,"users":[1000]}')
        with self.assertRaisesRegex(module.Deferred, 'disabled'):
            self.engine.apply(automatic=True)
        self.assertEqual(self.runner.calls, [])
    def test_candidate_metadata_requires_fixed_identity_and_digest(self):
        valid = 'Package: noisedeck-sync\nVersion: 1.1\nArchitecture: amd64\nSHA256: '+'a'*64+'\n'
        self.assertEqual(module.candidate_metadata(valid, '1.1'), 'a'*64)
        for invalid in [valid.replace('amd64', 'arm64'), valid.replace('1.1','1.0'), valid.replace('a'*64,'deadbeef'), valid+valid]:
            with self.assertRaises(module.Deferred): module.candidate_metadata(invalid,'1.1')
    def test_production_cli_has_no_fixture_root_or_command_override(self):
        result = subprocess.run(['/usr/bin/env', 'python3', '-I', str(SCRIPT), 'status', '--root', str(self.root)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)

    def ready_transaction(self):
        self.write('/proc/'+str(os.getpid())+'/stat',str(os.getpid())+' (test) S '+'0 '*18+'42\n')
        self.write('/etc/noisedeck-sync/update.json', '{"schema":1,"enabled":true,"users":[1000]}')
        for path in (module.SOURCE, '/etc/apt/preferences.d/noisedeck-sync', '/etc/apt/apt.conf.d/52noisedeck-sync', module.LOCK):
            self.write(path, '')
        self.write(module.SOURCE, module.SOURCE_CONTENT)
        self.write('/etc/apt/preferences.d/noisedeck-sync', module.PIN_CONTENT)
        self.write('/etc/apt/apt.conf.d/52noisedeck-sync', module.HOOK_CONTENT)
        self.write('/usr/share/keyrings/noisedeck-sync-archive-keyring.gpg', 'fixture-keyring')
        self.write('/usr/share/noisedeck-sync/apt-key-fingerprint', 'A'*40+'\n')
        self.runner.responses['--show-keys'] = 'pub:::::::::\nfpr:::::::::'+'A'*40+':\n'
        self.runner.responses['--print-architecture'] = 'amd64\n'
        self.runner.responses['policy'] = '  Candidate: 1.1\n'
        deb = self.write('/var/cache/apt/archives/sync.deb','sync')
        self.runner.responses['show'] = 'Package: noisedeck-sync\nVersion: 1.1\nArchitecture: amd64\nSHA256: '+hashlib.sha256(b'sync').hexdigest()+'\n'
        self.runner.responses['--field'] = 'Package: noisedeck-sync\nVersion: 1.1\nArchitecture: amd64\n'
        info = self.engine.path(module.LOCK).stat()
        self.write('/proc/locks','1: FLOCK ADVISORY WRITE '+str(os.getpid())+' '+
            format(os.major(info.st_dev),'x')+':'+format(os.minor(info.st_dev),'x')+':'+str(info.st_ino)+' 0 EOF\n')
        # Model APT's boundary, keeping the real hook validation and durable
        # marker transition in every successful transaction fixture.
        self.runner.before_install = lambda: self.engine.apt_hook([str(deb)])
        self.runner.responses['--showformat=${Version}'] = '1.0'
        self.runner.responses['--showformat=${Status} ${Version}'] = 'install ok installed 1.1'
        self.runner.responses['--simulate'] = 'Inst noisedeck-sync [1.0] (1.1 Noisefactor Sync:noble-preview [amd64])\nConf noisedeck-sync (1.1 Noisefactor Sync:noble-preview [amd64])\n'
        self.engine.managed = lambda users: {}
        self.engine.instances = lambda: {}

    def test_apply_uses_exact_sync_package_and_removes_marker_after_success(self):
        self.ready_transaction()
        self.assertEqual(self.engine.apply(), 'installed 1.1')
        installs = [call for call in self.runner.calls if '--no-download' in call]
        self.assertEqual(len(installs), 1)
        self.assertEqual(installs[0][-2:], ['install', 'noisedeck-sync=1.1'])
        self.assertIn('--no-remove', installs[0])
        self.assertIn('DPkg::Pre-Install-Pkgs::=/usr/libexec/noisedeck-sync-update apt-hook', installs[0])
        self.assertFalse((self.root/module.MARKER.lstrip('/')).exists())

    def managed_transaction(self):
        self.ready_transaction()
        self.engine.managed=lambda users: {123:1000}
        self.engine.probe=lambda uid,pid: None
        self.engine.provider_baseline=lambda uid,pid: ['ndi']
        calls=[]
        self.engine.service=lambda uid,operation: calls.append((uid,operation)) or ''
        waited=[]
        self.engine.wait_activation=lambda uid,version,expected: waited.append((uid,version,expected))
        class Channel:
            def setblocking(self,flag): pass
            def recv(self,size,flags): raise BlockingIOError
        @contextlib.contextmanager
        def reservation(uid,pid,digest): yield Channel()
        self.engine.reservation=reservation
        return calls,waited

    def test_pre_dpkg_lock_failure_restores_the_unchanged_daemon(self):
        calls,waited=self.managed_transaction()
        def busy(): raise module.Deferred('APT could not acquire its package lock')
        self.runner.before_install=busy
        with self.assertRaisesRegex(module.Deferred,'package lock'):
            self.engine.apply()
        self.assertIn((1000,['start']),calls)
        self.assertEqual(waited,[(1000,'1.0',['ndi'])])
        self.assertFalse(self.engine.path(module.MARKER).exists())
        self.assertFalse(self.engine.path(module.ACTIVATION).exists())

    def test_hook_rejection_before_dpkg_restores_the_unchanged_daemon(self):
        calls,waited=self.managed_transaction()
        hook=self.runner.before_install
        def changed_payload():
            self.write('/var/cache/apt/archives/sync.deb','corrupt')
            hook()
        self.runner.before_install=changed_payload
        with self.assertRaisesRegex(module.Deferred,'payload differs'):
            self.engine.apply()
        self.assertIn((1000,['start']),calls)
        self.assertEqual(waited,[(1000,'1.0',['ndi'])])
        self.assertFalse(self.engine.path(module.MARKER).exists())

    def test_unproven_preinstall_state_never_restarts_daemons(self):
        for change in ('installing','missing','malformed','changed-identity','unsafe-permissions'):
            with self.subTest(change=change):
                calls,waited=self.managed_transaction()
                hook=self.runner.before_install
                def ambiguous():
                    path=self.engine.path(module.MARKER)
                    if change == 'installing': hook()
                    elif change == 'missing': path.unlink()
                    elif change == 'malformed': path.write_text('{')
                    elif change == 'unsafe-permissions': path.chmod(0o666)
                    else:
                        marker=json.loads(path.read_text()); marker['digest']='b'*64
                        path.write_text(json.dumps(marker))
                    raise module.Deferred('uncertain package outcome')
                self.runner.before_install=ambiguous
                with self.assertRaisesRegex(module.Deferred,'uncertain package outcome'):
                    self.engine.apply()
                self.assertNotIn((1000,['start']),calls)
                self.assertEqual(waited,[])
                self.assertFalse(self.engine.path(module.ACTIVATION).exists())
                # Isolate each failure injection, never clear real machine state.
                self.engine.path(module.MARKER).unlink(missing_ok=True)

    def test_unknown_process_or_unavailable_reservation_never_downloads_or_stops(self):
        self.ready_transaction()
        def unknown(users): raise module.Deferred('unknown Sync process')
        self.engine.managed = unknown
        with self.assertRaises(module.Deferred): self.engine.apply()
        self.assertFalse(any('--download-only' in call or 'stop' in call or '--no-download' in call for call in self.runner.calls))

    def test_payload_failure_leaves_durable_repair_marker_and_does_not_restart(self):
        self.ready_transaction()
        self.runner.responses['--no-download'] = module.Deferred('dpkg failed during extraction')
        with self.assertRaises(module.Deferred): self.engine.apply()
        marker = json.loads((self.root/module.MARKER.lstrip('/')).read_text())
        self.assertEqual(marker['phase'], 'installing')
        self.assertFalse(any('start' in call for call in self.runner.calls))

    def test_existing_transaction_or_dpkg_repair_stops_before_download(self):
        self.ready_transaction()
        self.runner.responses['--audit'] = 'Package configuration incomplete\n'
        with self.assertRaisesRegex(module.Deferred, 'repair'):
            self.engine.apply()
        self.assertFalse(any('--download-only' in call for call in self.runner.calls))

    def test_install_hook_rejects_unrelated_dependency_in_sync_transaction(self):
        self.ready_transaction()
        sync = self.write('/var/cache/apt/archives/sync.deb','sync')
        other = self.write('/var/cache/apt/archives/other.deb','other')
        import hashlib
        self.write(module.MARKER,json.dumps({'phase':'installing','version':'1.1','digest':hashlib.sha256(b'sync').hexdigest()}))
        original = self.engine.run
        def metadata(argv):
            if '--field' in argv:
                return ('noisedeck-sync\n1.1\namd64\n' if str(sync) in argv else 'other\n1\namd64\n')
            return original(argv)
        self.engine.run = metadata
        with self.engine.lock(module.LOCK):
            with self.assertRaisesRegex(module.Deferred, 'unrelated'):
                self.engine.apt_hook([str(sync),str(other)])

    def test_apt_queries_cannot_reuse_unrelated_repository_lists_or_binary_cache(self):
        self.ready_transaction()
        self.engine.apply()
        for call in self.runner.calls:
            if call[0] in ('/usr/bin/apt-get','/usr/bin/apt-cache'):
                self.assertIn('Dir::State::lists=/var/lib/noisedeck-sync-update/apt/lists', call)
                self.assertIn('Dir::Cache::pkgcache=/var/cache/noisedeck-sync-update/pkgcache.bin', call)
                self.assertIn('Dir::Cache::srcpkgcache=/var/cache/noisedeck-sync-update/srcpkgcache.bin', call)

    def test_modified_source_cannot_redirect_the_privileged_update(self):
        self.ready_transaction()
        self.write(module.SOURCE, 'Types: deb\nURIs: https://unrelated.example/apt/\nTrusted: yes\n')
        with self.assertRaisesRegex(module.Deferred, 'configuration'):
            self.engine.apply()
        self.assertFalse(any(call[0] == '/usr/bin/apt-get' for call in self.runner.calls))

    def test_stale_marker_does_not_block_unrelated_apt_transaction(self):
        self.write(module.MARKER, '{"phase":"installing"}')
        deb=self.write('/var/cache/apt/archives/other.deb','other')
        self.runner.responses['--field']='other\n1\namd64\n'
        self.engine.apt_hook([str(deb)])

    def test_manual_apply_still_works_when_automatic_timer_is_disabled(self):
        self.ready_transaction()
        self.write('/etc/noisedeck-sync/update.json', '{"schema":1,"enabled":false,"users":[1000]}')
        self.assertEqual(self.engine.apply(), 'installed 1.1')

    def test_restart_failure_attempts_every_user_and_keeps_pending_state(self):
        calls=[]
        def service(uid,operation):
            calls.append((uid,operation))
            if uid==1000 and operation==['start']: raise module.Deferred('start failed')
            return ''
        self.engine.service=service
        self.engine.wait_activation=lambda uid,version,expected: None
        self.write('/var/lib/noisedeck-sync-update/activation.json',json.dumps({'schema':1,'version':'1.1','pending':[1000,1001],'errors':{},
            'expected':{'1000':[],'1001':['ndi']}}))
        with self.assertRaisesRegex(module.Deferred,'restart pending'):
            self.engine.restart_pending()
        self.assertIn((1001,['start']),calls)
        state=json.loads((self.root/'var/lib/noisedeck-sync-update/activation.json').read_text())
        self.assertEqual(state['pending'],[1000])

    def test_hook_rejects_shared_daemon_lock_but_accepts_maintenance_exclusive_lock(self):
        self.ready_transaction()
        deb=self.write('/var/cache/apt/archives/sync.deb','sync')
        self.runner.responses['--field']='noisedeck-sync\n1.1\namd64\n'
        self.write(module.MARKER,json.dumps({'phase':'installing','version':'1.1',
            'digest':hashlib.sha256(b'sync').hexdigest(),'maintenance_pid':os.getpid(),'maintenance_start':'42'}))
        self.write('/proc/'+str(os.getpid())+'/stat',str(os.getpid())+' (test) S '+'0 '*18+'42\n')
        lockpath=self.root/module.LOCK.lstrip('/')
        info=lockpath.stat()
        self.write('/proc/locks','1: FLOCK ADVISORY WRITE '+str(os.getpid())+' '+
            format(os.major(info.st_dev),'x')+':'+format(os.minor(info.st_dev),'x')+':'+str(info.st_ino)+' 0 EOF\n')
        with lockpath.open() as lock:
            fcntl.flock(lock,fcntl.LOCK_SH)
            with self.assertRaisesRegex(module.Deferred,'exclusive|maintenance'):
                self.engine.apt_hook([str(deb)])
        with self.engine.lock(module.LOCK): self.engine.apt_hook([str(deb)])

    def hook_marker(self):
        self.ready_transaction()
        marker={'schema':1,'phase':'pre-install','version':'1.1','previous':'1.0',
                'digest':hashlib.sha256(b'sync').hexdigest(),'users':[1000],
                'maintenance_pid':os.getpid(),'maintenance_start':'42'}
        self.write(module.MARKER,json.dumps(marker))
        return marker,self.engine.path('/var/cache/apt/archives/sync.deb')

    def test_authenticated_hook_durably_marks_payload_writes_before_returning(self):
        marker,deb=self.hook_marker()
        with self.engine.lock(module.LOCK):
            self.engine.apt_hook([str(deb)])
            marker['phase']='installing'
            self.assertEqual(json.loads(self.engine.path(module.MARKER).read_text()),marker)
            # Both global configuration and the fixed install option can invoke it.
            self.engine.apt_hook([str(deb)])

    def test_hook_accepts_only_descendants_of_the_recorded_live_maintenance_process(self):
        marker,deb=self.hook_marker()
        parent=marker['maintenance_pid']
        child=parent+100000
        self.write('/proc/'+str(child)+'/stat',str(child)+' (apt hook) S '+str(parent)+' '+'0 '*17+'43\n')
        with self.engine.lock(module.LOCK), patch.object(module.os,'getpid',return_value=child):
            self.engine.apt_hook([str(deb)])
        self.assertEqual(json.loads(self.engine.path(module.MARKER).read_text())['phase'],'installing')
        for case in ('unrelated','reused-pid','cycle','missing-parent'):
            with self.subTest(case=case):
                self.write(module.MARKER,json.dumps(marker))
                ancestor=0 if case == 'unrelated' else child if case == 'cycle' else parent+1 if case == 'missing-parent' else parent
                self.write('/proc/'+str(child)+'/stat',str(child)+' (apt hook) S '+str(ancestor)+' '+'0 '*17+'43\n')
                if case == 'reused-pid':
                    self.write('/proc/'+str(parent)+'/stat',str(parent)+' (new owner) S '+'0 '*18+'99\n')
                else:
                    self.write('/proc/'+str(parent)+'/stat',str(parent)+' (owner) S '+'0 '*18+'42\n')
                with self.engine.lock(module.LOCK), patch.object(module.os,'getpid',return_value=child):
                    with self.assertRaises(module.Deferred): self.engine.apt_hook([str(deb)])
                self.assertEqual(json.loads(self.engine.path(module.MARKER).read_text())['phase'],'pre-install')

    def test_withdrawn_offer_or_new_reboot_requirement_never_stops_for_install(self):
        for changed in ('withdrawn','reboot'):
            with self.subTest(changed=changed):
                self.ready_transaction(); self.runner.calls.clear()
                def download(argv,monitor):
                    monitor(); self.runner(argv)
                    if changed == 'withdrawn': self.runner.responses['policy']='  Candidate: (none)\n'
                    else: self.write('/run/reboot-required','')
                self.engine.download=download
                with self.assertRaises(module.Deferred): self.engine.apply()
                self.assertFalse(any('--no-download' in call or 'stop' in call for call in self.runner.calls))
                self.assertFalse((self.root/module.MARKER.lstrip('/')).exists())

    def test_download_cancellation_terminates_only_its_subprocess_group(self):
        marker=self.root/'download-pid'
        def busy(): raise module.Deferred('active session')
        with self.assertRaisesRegex(module.Deferred,'active session'):
            module.monitored_download([sys.executable,'-c','import time; time.sleep(60)'],busy,timeout=1)
        with self.assertRaisesRegex(module.Deferred,'invalid cancellable'):
            module.run_download(['/usr/bin/apt-get','install','noisedeck-sync=1.1'],lambda:None)

    def test_activation_requires_current_version_active_process_and_prior_usable_providers(self):
        self.engine.service=lambda uid,operation: 'MainPID=123\nActiveState=active\n'
        self.engine.instances=lambda: {123:1000}
        response={'version':1,'type':'status','runningVersion':'1.1',
                  'providers':[{'id':'ndi','selected':True,'available':True,'healthy':True}]}
        @contextlib.contextmanager
        def status(uid,pid,request): yield None,response
        self.engine.control_request=status
        self.assertTrue(self.engine.activation_ready(1000,'1.1',['ndi']))
        response['runningVersion']='1.0'
        self.assertFalse(self.engine.activation_ready(1000,'1.1',['ndi']))
        response['runningVersion']='1.1'; response['providers'][0]['healthy']=False
        self.assertFalse(self.engine.activation_ready(1000,'1.1',['ndi']))
        response['providers']=[]
        self.assertFalse(self.engine.activation_ready(1000,'1.1',['ndi']))
        response['type']='error'
        with self.assertRaises(module.Deferred): self.engine.activation_ready(1000,'1.1',[])
        response['type']='status'
        self.engine.instances=lambda: {}
        self.assertFalse(self.engine.activation_ready(1000,'1.1',[]))

    def test_provider_unusable_before_maintenance_does_not_hold_activation_pending(self):
        # NDI is selected by default and unavailable without its runtime.
        self.engine.service=lambda uid,operation: 'MainPID=123\nActiveState=active\n'
        self.engine.instances=lambda: {123:1000}
        response={'version':1,'type':'status','runningVersion':'1.1',
                  'providers':[{'id':'ndi','selected':True,'available':False,'healthy':False}]}
        @contextlib.contextmanager
        def status(uid,pid,request): yield None,response
        self.engine.control_request=status
        self.assertEqual(self.engine.provider_baseline(1000,123),[])
        self.assertTrue(self.engine.activation_ready(1000,'1.1',[]))
        response['providers']=[]
        self.assertTrue(self.engine.activation_ready(1000,'1.1',[]))

    def test_legacy_activation_record_recovers_without_inventing_a_provider_baseline(self):
        # The old updater keeps running after its package is replaced and writes
        # this schema-1 journal. The newly installed helper must resume it.
        self.write(module.ACTIVATION,'{"schema":1,"version":"1.1","pending":[1000],"errors":{}}')
        self.engine.service=lambda uid,operation: 'MainPID=123\nActiveState=active\n'
        self.engine.instances=lambda: {123:1000}
        response={'version':1,'type':'status','runningVersion':'1.1',
                  'providers':[{'id':'ndi','selected':True,'available':True,'healthy':True}]}
        @contextlib.contextmanager
        def status(uid,pid,request): yield None,response
        self.engine.control_request=status
        self.engine.restart_pending()
        self.assertFalse(self.engine.path(module.ACTIVATION).exists())

    def test_legacy_activation_still_defers_unhealthy_or_empty_provider_state(self):
        self.engine.service=lambda uid,operation: 'MainPID=123\nActiveState=active\n'
        self.engine.instances=lambda: {123:1000}
        response={'version':1,'type':'status','runningVersion':'1.1'}
        @contextlib.contextmanager
        def status(uid,pid,request): yield None,response
        self.engine.control_request=status
        original_wait=self.engine.wait_activation
        self.engine.wait_activation=lambda uid,version,expected: original_wait(uid,version,expected,timeout=0.001)
        for providers in ([], [{'id':'ndi','selected':True,'available':False,'healthy':False}]):
            with self.subTest(providers=providers):
                response['providers']=providers
                self.write(module.ACTIVATION,'{"schema":1,"version":"1.1","pending":[1000],"errors":{}}')
                with self.assertRaisesRegex(module.Deferred,'restart pending'):
                    self.engine.restart_pending()
                state=json.loads(self.engine.path(module.ACTIVATION).read_text())
                self.assertEqual(state['pending'],[1000])
                self.assertNotIn('expected',state)

    def test_new_activation_records_are_versioned_and_require_expected_provider_state(self):
        self.engine.stage_activation([1000],'1.1',{1000:['ndi']})
        state=json.loads(self.engine.path(module.ACTIVATION).read_text())
        self.assertEqual(state['schema'],2)
        self.assertEqual(state['expected'],{'1000':['ndi']})
        for schema,extra in ((2,{}),(1,{'expected':{}})):
            self.write(module.ACTIVATION,json.dumps({'schema':schema,'version':'1.1','pending':[1000],'errors':{},**extra}))
            with self.assertRaisesRegex(module.Deferred,'invalid activation record'):
                self.engine.restart_pending()

    def test_apply_restores_the_providers_each_daemon_could_use_before_maintenance(self):
        self.ready_transaction()
        self.engine.managed=lambda users: {123:1000}
        self.engine.probe=lambda uid,pid: None
        calls=[]
        def service(uid,operation):
            calls.append((uid,operation)); return ''
        self.engine.service=service
        class Channel:
            def setblocking(self,flag): pass
            def recv(self,size,flags): raise BlockingIOError
        @contextlib.contextmanager
        def reservation(uid,pid,digest): yield Channel()
        self.engine.reservation=reservation
        response={'version':1,'type':'status','runningVersion':'1.0','providers':[
            {'id':'ndi','selected':True,'available':True,'healthy':True},
            {'id':'camera','selected':True,'available':False,'healthy':False}]}
        @contextlib.contextmanager
        def status(uid,pid,request): yield None,response
        self.engine.control_request=status
        waited=[]
        self.engine.wait_activation=lambda uid,version,expected: waited.append((uid,version,expected))
        self.assertEqual(self.engine.apply(),'installed 1.1')
        self.assertIn((1000,['stop']),calls)
        self.assertEqual(waited,[(1000,'1.1',['ndi'])])
        self.assertFalse((self.root/module.ACTIVATION.lstrip('/')).exists())

    def test_package_removal_does_not_leave_enrollment_requiring_a_missing_boot_lock(self):
        self.ready_transaction()
        self.engine.remove_enrollment()
        self.assertFalse((self.root/module.CONFIG.lstrip('/')).exists())
        self.assertFalse((self.root/module.SOURCE.lstrip('/')).exists())

if __name__ == '__main__': unittest.main()
