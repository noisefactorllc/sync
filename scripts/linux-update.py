#!/usr/bin/python3 -I
"""Constrained APT maintenance for the system-owned Sync package."""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

PACKAGE = 'noisedeck-sync'
CONFIG = '/etc/noisedeck-sync/update.json'
MARKER = '/var/lib/noisedeck-sync-update/transaction.json'
ACTIVATION = '/var/lib/noisedeck-sync-update/activation.json'
LOCK = '/run/noisedeck-sync-update.lock'
MUTEX = '/run/noisedeck-sync-update-maintenance.lock'
KEY = '/usr/share/keyrings/noisedeck-sync-archive-keyring.gpg'
FINGERPRINT = '/usr/share/noisedeck-sync/apt-key-fingerprint'
SOURCE = '/etc/apt/sources.list.d/noisedeck-sync.sources'
HELPER = '/usr/libexec/noisedeck-sync-update'
TIMER_DEPENDENCY = '/etc/systemd/system/timers.target.d/noisedeck-sync.conf'
SOURCE_CONTENT = 'Types: deb\nURIs: https://downloads.noisefactor.io/apt/sync/\nSuites: noble-preview\nComponents: main\nArchitectures: amd64\nSigned-By: '+KEY+'\nCheck-Valid-Until: yes\n'
PIN_CONTENT = 'Package: noisedeck-sync noisedeck-sync-archive-keyring\nPin: origin downloads.noisefactor.io\nPin-Priority: 100\n\nPackage: *\nPin: origin downloads.noisefactor.io\nPin-Priority: -1\n'
HOOK_CONTENT = 'Unattended-Upgrade::Package-Blacklist { "noisedeck-sync"; "noisedeck-sync-archive-keyring"; };\nDPkg::Pre-Install-Pkgs { "'+HELPER+' apt-hook"; };\nDPkg::Tools::Options::'+HELPER+'::Version "1";\n'
VERSION = re.compile(r'[0-9][0-9A-Za-z.+:~\-]*\Z')
DIGEST = re.compile(r'[0-9a-f]{64}\Z')
APT_OPTIONS = ['-o', 'Dir::Etc::sourcelist='+SOURCE, '-o', 'Dir::Etc::sourceparts=-',
               '-o', 'Dir::State::lists=/var/lib/noisedeck-sync-update/apt/lists',
               '-o', 'Dir::Cache::pkgcache=/var/cache/noisedeck-sync-update/pkgcache.bin',
               '-o', 'Dir::Cache::srcpkgcache=/var/cache/noisedeck-sync-update/srcpkgcache.bin',
               '-o', 'APT::Get::AllowUnauthenticated=false',
               '-o', 'Acquire::AllowInsecureRepositories=false',
               '-o', 'Acquire::AllowDowngradeToInsecureRepositories=false',
               '-o', 'Acquire::Check-Valid-Until=true',
               '-o', 'DPkg::Lock::Timeout=0']

class Deferred(RuntimeError):
    pass

def run(argv):
    # Never time out a dpkg write. Its process-held exclusion and durable marker
    # remain until completion; bounded metadata/download work runs before writes.
    timeout = (None if '--no-download' in argv and 'install' in argv else
               1800 if '--download-only' in argv else 300)
    try:
        completed = subprocess.run(argv, check=True, text=True, capture_output=True,
                                   timeout=timeout, env={'PATH':'/usr/sbin:/usr/bin:/sbin:/bin',
                                   'LC_ALL':'C', 'DEBIAN_FRONTEND':'noninteractive'})
        return completed.stdout
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise Deferred('command failed: '+argv[0]+'; '+str(error)) from error

def candidate_metadata(text, version):
    paragraphs = [p for p in text.strip().split('\n\n') if p.strip()]
    if len(paragraphs) != 1:
        raise Deferred('candidate metadata is ambiguous')
    fields = {}
    for line in paragraphs[0].splitlines():
        if line.startswith((' ', '\t')): continue
        if ': ' not in line: raise Deferred('candidate metadata is malformed')
        key, value = line.split(': ', 1)
        if key in fields: raise Deferred('duplicate candidate field')
        fields[key] = value
    if (fields.get('Package') != PACKAGE or fields.get('Version') != version or
            fields.get('Architecture') != 'amd64' or not DIGEST.fullmatch(fields.get('SHA256',''))):
        raise Deferred('candidate identity, architecture, version, or digest mismatch')
    return fields['SHA256']

def validate_simulation(text, version):
    installs = []
    for line in text.splitlines():
        if line.startswith('Remv '): raise Deferred('transaction removes a package')
        if line.startswith(('Inst ', 'Conf ')):
            match = re.fullmatch(r'(Inst|Conf) ([^ ]+)(?: \[[^\]]+\])? \(([^ ]+) .+\)', line)
            if not match or match[2] != PACKAGE or match[3] != version:
                raise Deferred('transaction changes an unrelated package or version')
            if match[1] == 'Inst': installs.append(match[2])
    if installs != [PACKAGE]: raise Deferred('transaction is not one exact Sync upgrade')

def monitored_download(argv, monitor, timeout=1800):
    # The download process is stopped while checking ownership and activity.
    # 250 ms is a polling target, not a measured zero-interference guarantee.
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen(argv, stdout=output, stderr=output,
            start_new_session=True, env={'PATH':'/usr/sbin:/usr/bin:/sbin:/bin',
                                        'LC_ALL':'C','DEBIAN_FRONTEND':'noninteractive'})
        def signal_group(value):
            try: os.killpg(process.pid,value)
            except ProcessLookupError: pass
        try:
            deadline=time.monotonic()+timeout
            while process.poll() is None:
                if time.monotonic() >= deadline: raise Deferred('package download timed out')
                signal_group(signal.SIGSTOP)
                monitor()
                signal_group(signal.SIGCONT)
                time.sleep(0.25)
            if process.returncode != 0: raise Deferred('package download failed')
        except BaseException:
            signal_group(signal.SIGCONT)
            signal_group(signal.SIGTERM)
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                signal_group(signal.SIGKILL); process.wait()
            raise

def run_download(argv, monitor):
    # This is the only cancellable package command: it cannot start dpkg.
    if argv[0] != '/usr/bin/apt-get' or '--download-only' not in argv or '--no-download' in argv:
        raise Deferred('invalid cancellable package command')
    monitored_download(argv,monitor)

class Maintenance:
    def __init__(self, root=Path('/'), runner=run, owner=0, downloader=None):
        # Dependencies are passed only by in-process tests, never by CLI or environment.
        self.root, self.run, self.owner = Path(root), runner, owner
        self.download = downloader or run_download

    def path(self, absolute):
        return self.root / absolute.lstrip('/')

    def trusted(self, absolute, directory=False):
        path = self.path(absolute)
        current = self.root
        for piece in Path(absolute).parts[1:]:
            current /= piece
            try: info = current.lstat()
            except FileNotFoundError as error: raise Deferred('missing trusted file: '+absolute) from error
            if stat.S_ISLNK(info.st_mode) or info.st_uid != self.owner or info.st_mode & 0o022:
                raise Deferred('unsafe ownership or permissions: '+absolute)
        wanted = stat.S_ISDIR if directory else stat.S_ISREG
        if not wanted(info.st_mode): raise Deferred('unexpected file type: '+absolute)
        return path

    def mkdir(self, absolute):
        path = self.path(absolute)
        current = self.root
        for piece in Path(absolute).parts[1:]:
            current /= piece
            try: current.mkdir(mode=0o755)
            except FileExistsError: pass
            info = current.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != self.owner or info.st_mode & 0o022:
                raise Deferred('unsafe directory: '+str(current))
        return path

    def write(self, absolute, text):
        path = self.path(absolute)
        self.mkdir(str(Path(absolute).parent))
        if path.exists() or path.is_symlink(): self.trusted(absolute)
        descriptor, temporary = tempfile.mkstemp(prefix='.sync-update-', dir=path.parent)
        try:
            with os.fdopen(descriptor, 'w') as stream:
                stream.write(text)
                stream.flush()
                os.fchmod(stream.fileno(), 0o644)
                os.fsync(stream.fileno())
            os.replace(temporary, path)
            parent = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try: os.fsync(parent)
            finally: os.close(parent)
        finally:
            if os.path.exists(temporary): os.unlink(temporary)

    def remove(self, absolute):
        if self.path(absolute).exists():
            self.trusted(absolute).unlink()
            parent = os.open(self.path(absolute).parent, os.O_RDONLY | os.O_DIRECTORY)
            try: os.fsync(parent)
            finally: os.close(parent)

    def config(self):
        try: value = json.loads(self.trusted(CONFIG).read_text())
        except (ValueError, OSError) as error: raise Deferred('invalid enrollment') from error
        if (set(value) != {'schema','enabled','users'} or value['schema'] != 1 or
                type(value['enabled']) is not bool or type(value['users']) is not list or
                not value['users'] or any(type(uid) is not int or uid <= 0 or uid >= 2**32-1 for uid in value['users']) or
                len(value['users']) != len(set(value['users']))):
            raise Deferred('invalid enrollment')
        return value

    def check_host(self):
        values = dict(line.split('=',1) for line in self.trusted('/usr/lib/os-release').read_text().splitlines() if '=' in line)
        if values.get('ID','').strip('"') != 'ubuntu' or values.get('VERSION_ID','').strip('"') != '24.04':
            raise Deferred('automatic maintenance requires Ubuntu 24.04')
        if self.run(['/usr/bin/dpkg', '--print-architecture']).strip() != 'amd64':
            raise Deferred('automatic maintenance requires amd64')

    def verify_key(self):
        self.trusted(KEY)
        fingerprint = self.trusted(FINGERPRINT).read_text().strip()
        if not re.fullmatch(r'[A-F0-9]{40}', fingerprint): raise Deferred('invalid packaged keyring fingerprint')
        result = self.run(['/usr/bin/gpg', '--batch', '--no-options', '--with-colons', '--show-keys', KEY])
        primary = []
        pending = False
        for line in result.splitlines():
            fields = line.split(':')
            if fields[0] == 'pub': pending = True
            elif fields[0] == 'sub': pending = False
            elif fields[0] == 'fpr' and pending and len(fields) > 9:
                primary.append(fields[9]); pending = False
        if primary != [fingerprint]: raise Deferred('packaged keyring fingerprint mismatch')

    def enable(self, uid):
        if type(uid) is not int or uid <= 0 or uid >= 2**32-1: raise Deferred('invalid managed UID')
        self.verify_key()
        self.check_host()
        users = self.config()['users'] if self.path(CONFIG).exists() else []
        users = sorted(set(users+[uid]))
        self.mkdir('/run')
        descriptor = os.open(self.path(LOCK), os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o644) if not self.path(LOCK).exists() else None
        if descriptor is not None: os.close(descriptor)
        self.trusted(LOCK)
        self.write(SOURCE, SOURCE_CONTENT)
        self.write('/etc/apt/preferences.d/noisedeck-sync', PIN_CONTENT)
        self.write('/etc/apt/apt.conf.d/52noisedeck-sync', HOOK_CONTENT)
        self.write(CONFIG, json.dumps({'schema':1,'enabled':True,'users':users})+'\n')
        # A regular target drop-in provides boot activation without creating enable symlinks.
        self.write(TIMER_DEPENDENCY, '[Unit]\nWants=noisedeck-sync-update.timer\n')
        self.run(['/usr/bin/systemctl', 'daemon-reload'])
        self.run(['/usr/bin/systemctl', 'start', 'noisedeck-sync-update.timer'])

    def disable(self):
        config = self.config(); config['enabled'] = False
        self.write(CONFIG,json.dumps(config)+'\n')
        self.remove(TIMER_DEPENDENCY)
        self.run(['/usr/bin/systemctl','stop','noisedeck-sync-update.timer'])
        self.run(['/usr/bin/systemctl','daemon-reload'])

    def remove_enrollment(self):
        if self.path(CONFIG).exists(): self.disable()
        for path in (SOURCE, '/etc/apt/preferences.d/noisedeck-sync',
                     '/etc/apt/apt.conf.d/52noisedeck-sync', CONFIG):
            self.remove(path)

    @contextlib.contextmanager
    def lock(self, name, create=False):
        if create:
            self.mkdir('/run')
            try:
                descriptor = os.open(self.path(name), os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o644)
                os.close(descriptor)
            except FileExistsError: pass
        self.trusted(name)
        descriptor = os.open(self.path(name), os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            try: fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error: raise Deferred('Sync maintenance lock is busy') from error
            yield
        finally: os.close(descriptor)

    def service(self, uid, operation):
        user = pwd.getpwuid(uid).pw_name
        unit = [] if operation == ['daemon-reload'] else ['noisedeck-sync.service']
        return self.run(['/usr/bin/systemctl','--user','--machine='+user+'@.host', *operation, *unit])

    def instances(self):
        found = {}
        for path in self.path('/proc').iterdir():
            if not path.name.isdigit(): continue
            try:
                comm = (path/'comm').read_text().strip()
                executable = os.readlink(path/'exe')
                if comm != 'syncd' and Path(executable.removesuffix(' (deleted)')).name != 'syncd': continue
                status = (path/'status').read_text()
                uids = re.search(r'^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)$', status, re.M)
                if not uids or len(set(uids.groups())) != 1 or executable != '/usr/bin/syncd':
                    raise Deferred('unknown Sync process ownership or executable')
                found[int(path.name)] = int(uids[1])
            except (FileNotFoundError, ProcessLookupError): continue
            except PermissionError as error: raise Deferred('cannot inspect process ownership') from error
        return found

    def managed(self, users):
        actual = self.instances()
        managed = {}
        for uid in users:
            runtime = self.path('/run/user/'+str(uid))
            if not runtime.exists(): continue
            info = runtime.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != uid or info.st_mode & 0o077:
                raise Deferred('unsafe managed user runtime directory')
            result = self.service(uid, ['show','--property=MainPID','--value']).strip()
            if not result.isdigit(): raise Deferred('cannot identify managed service PID')
            pid = int(result)
            if pid:
                if actual.get(pid) != uid: raise Deferred('managed service identity mismatch')
                group = self.path('/proc/'+str(pid)+'/cgroup').read_text()
                if '/user-'+str(uid)+'.slice/user@'+str(uid)+'.service/' not in group or not group.strip().endswith('/noisedeck-sync.service'):
                    raise Deferred('unknown Sync service cgroup')
                managed[pid] = uid
        if actual != managed: raise Deferred('unmanaged Sync process blocks maintenance')
        return managed

    @contextlib.contextmanager
    def control_request(self, uid, pid, request):
        path = self.path('/run/user/'+str(uid)+'/noisedeck-sync/control.sock')
        info = path.lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != uid or info.st_mode & 0o077:
            raise Deferred('unsafe control socket')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(2)
            connection.connect(str(path))
            peer_pid, peer_uid, _ = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if (peer_pid,peer_uid) != (pid,uid): raise Deferred('control socket peer mismatch')
            body = json.dumps(request).encode()
            connection.sendall(struct.pack('!I',len(body))+body)
            def receive(count):
                value = b''
                while len(value) < count:
                    part = connection.recv(count-len(value))
                    if not part: raise Deferred('maintenance channel closed')
                    value += part
                return value
            length = struct.unpack('!I',receive(4))[0]
            if not 0 < length <= 4096: raise Deferred('invalid maintenance response length')
            response = json.loads(receive(length))
            yield connection, response

    def probe(self, uid, pid):
        with self.control_request(uid,pid,{'version':1,'command':'update-probe'}) as (_,response):
            if response != {'version':1,'type':'update','status':'idle'}:
                raise Deferred('download deferred: active or unknown daemon workload')

    @staticmethod
    def usable_providers(response):
        providers = response.get('providers')
        if response.get('version') != 1 or response.get('type') != 'status' or not isinstance(providers,list):
            raise Deferred('invalid daemon status')
        return sorted({provider['id'] for provider in providers
                       if isinstance(provider,dict) and type(provider.get('id')) is str and
                       provider.get('selected') is True and provider.get('available') is True and
                       provider.get('healthy') is True})

    def provider_baseline(self, uid, pid):
        with self.control_request(uid,pid,{'version':1,'command':'update-status'}) as (_,response):
            return self.usable_providers(response)

    @contextlib.contextmanager
    def reservation(self, uid, pid, digest):
        generation = time.monotonic_ns()
        with self.control_request(uid,pid,{'version':1,'command':'update-reserve',
                'generation':generation,'digest':digest}) as (connection,response):
            if (response.get('version') != 1 or response.get('type') != 'update' or
                    response.get('status') != 'reserved' or response.get('generation') != generation or
                    type(response.get('token')) is not int or not 0 < response['token'] < 2**64):
                raise Deferred('daemon refused maintenance: '+str(response.get('code','invalid reservation')))
            yield connection

    def check_host_maintenance(self):
        if self.path('/run/reboot-required').exists(): raise Deferred('operating system restart is pending')
        if self.run(['/usr/bin/dpkg','--audit']).strip(): raise Deferred('dpkg repair is required')
        self.check_conffiles()

    def check_conffiles(self):
        output = self.run(['/usr/bin/dpkg-query','--show','--showformat=${Conffiles}',PACKAGE])
        for line in output.splitlines():
            values = line.split()
            if len(values) != 2 or not values[0].startswith('/') or not re.fullmatch('[0-9a-f]{32}',values[1]):
                raise Deferred('conffile requires administrator review')
            data = self.trusted(values[0]).read_bytes()
            if hashlib.md5(data).hexdigest() != values[1]: raise Deferred('modified conffile requires administrator review')

    def process_start(self, pid):
        if type(pid) is not int or pid <= 0: raise Deferred('invalid maintenance process identity')
        value=self.path('/proc/'+str(pid)+'/stat').read_text().rsplit(') ',1)
        if len(value) != 2: raise Deferred('unknown maintenance process identity')
        fields=value[1].split()
        if len(fields) < 20 or not fields[19].isdigit(): raise Deferred('unknown maintenance process start')
        return fields[19]

    def exclusive_lock_owner(self, pid):
        info=self.trusted(LOCK).stat()
        for line in self.path('/proc/locks').read_text().splitlines():
            fields=line.split()
            if len(fields) < 8 or fields[1:4] != ['FLOCK','ADVISORY','WRITE'] or fields[4] != str(pid): continue
            device=fields[5].split(':')
            if len(device) != 3: continue
            try: identity=(int(device[0],16),int(device[1],16),int(device[2]))
            except ValueError: continue
            if identity == (os.major(info.st_dev),os.minor(info.st_dev),info.st_ino): return True
        return False

    def apt_hook(self, paths):
        packages = []
        for name in paths:
            if not name.startswith('/') or '\x00' in name: raise Deferred('invalid APT hook payload path')
            output = self.run(['/usr/bin/dpkg-deb','--field',name,'Package','Version','Architecture'])
            # Multiple dpkg-deb fields are prefixed by their names.
            values = [line.split(': ',1)[-1] for line in output.strip().splitlines()]
            if len(values) != 3: raise Deferred('invalid APT hook package metadata')
            packages.append((name,values))
        if not any(values[0] == PACKAGE for _,values in packages): return
        if len(packages) != 1:
            raise Deferred('coordinated transaction contains an unrelated package or duplicate')
        self.check_host_maintenance()
        for name,values in packages:
            marker = json.loads(self.trusted(MARKER).read_text())
            if marker.get('phase') != 'installing' or marker.get('version') != values[1] or values[2] != 'amd64':
                raise Deferred('Sync package requires coordinated maintenance')
            archive = Path(name)
            try: relative = archive.relative_to(self.root)
            except ValueError as error: raise Deferred('APT archive is outside the system root') from error
            if relative.parent != Path('var/cache/apt/archives'):
                raise Deferred('coordinated APT payload is outside the package cache')
            self.trusted('/'+str(relative))
            if hashlib.sha256(archive.read_bytes()).hexdigest() != marker.get('digest'):
                raise Deferred('APT payload differs from reserved package')
            self.trusted(LOCK)
            descriptor = os.open(self.path(LOCK), os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
            try:
                try: fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
                except BlockingIOError:
                    pid=marker.get('maintenance_pid')
                    if (self.process_start(pid) != marker.get('maintenance_start') or
                            not self.exclusive_lock_owner(pid)):
                        raise Deferred('exclusive lock does not belong to the recorded maintenance process')
                    continue
                raise Deferred('exclusive installation lock is not held by maintenance')
            finally: os.close(descriptor)

    def stage_activation(self, users, version, usable):
        if users:
            pending=sorted(set(users))
            self.write(ACTIVATION,json.dumps({'schema':1,'version':version,'pending':pending,'errors':{},
                'expected':{str(uid):usable[uid] for uid in pending}})+'\n')

    def activation_ready(self, uid, version, expected):
        fields=dict(line.split('=',1) for line in self.service(uid,
            ['show','--property=MainPID','--property=ActiveState']).splitlines() if '=' in line)
        if fields.get('ActiveState') != 'active' or not fields.get('MainPID','').isdigit(): return False
        pid=int(fields['MainPID'])
        if pid <= 0 or self.instances().get(pid) != uid: return False
        with self.control_request(uid,pid,{'version':1,'command':'update-status'}) as (_,response):
            if response.get('runningVersion') != version: return False
            # Restore what the daemon could do before maintenance. A selected
            # provider that was already unusable, such as NDI without its
            # runtime, must not hold this and every later update pending.
            return set(expected) <= set(self.usable_providers(response))

    def wait_activation(self, uid, version, expected, timeout=60):
        deadline=time.monotonic()+timeout
        consecutive=0
        while time.monotonic() < deadline:
            try: ready=self.activation_ready(uid,version,expected)
            except (Deferred,OSError,ValueError): ready=False
            consecutive=consecutive+1 if ready else 0
            if consecutive >= 3: return
            time.sleep(0.5)
        raise Deferred('daemon version and provider health did not become ready')

    def restart_pending(self):
        if not self.path(ACTIVATION).exists(): return
        state=json.loads(self.trusted(ACTIVATION).read_text())
        if (not isinstance(state,dict) or set(state) != {'schema','version','pending','errors','expected'} or
                state['schema'] != 1 or not VERSION.fullmatch(state['version']) or
                not isinstance(state['pending'],list) or not isinstance(state['errors'],dict) or
                any(type(uid) is not int or uid <= 0 or uid >= 2**32-1 for uid in state['pending']) or
                not isinstance(state['expected'],dict) or
                any(not isinstance(state['expected'].get(str(uid)),list) or
                    any(type(name) is not str for name in state['expected'][str(uid)])
                    for uid in state['pending'])):
            raise Deferred('invalid activation record')
        for uid in tuple(state['pending']):
            try:
                self.service(uid,['daemon-reload'])
                self.service(uid,['start'])
                self.wait_activation(uid,state['version'],state['expected'][str(uid)])
                state['pending'].remove(uid)
                state['errors'].pop(str(uid),None)
            except (Deferred,OSError) as error:
                state['errors'][str(uid)]=str(error)[:300]
            self.write(ACTIVATION,json.dumps(state)+'\n')
        if state['pending']:
            raise Deferred('installed '+state['version']+'; restart pending for users '+
                           ','.join(str(uid) for uid in state['pending']))
        self.remove(ACTIVATION)

    def apply(self, automatic=False):
        config = self.config()
        if automatic and not config['enabled']: raise Deferred('automatic maintenance is disabled')
        self.check_host(); self.verify_key()
        for path, expected in ((SOURCE,SOURCE_CONTENT),
                ('/etc/apt/preferences.d/noisedeck-sync',PIN_CONTENT),
                ('/etc/apt/apt.conf.d/52noisedeck-sync',HOOK_CONTENT)):
            if self.trusted(path).read_text() != expected:
                raise Deferred('enrollment configuration was changed; administrator review is required')
        with self.lock(MUTEX, create=True):
            if self.path(MARKER).exists(): raise Deferred('incomplete transaction requires administrator repair')
            self.restart_pending()
            self.check_host_maintenance()
            self.mkdir('/var/lib/noisedeck-sync-update/apt/lists')
            self.mkdir('/var/cache/noisedeck-sync-update')
            self.run(['/usr/bin/apt-get',*APT_OPTIONS,'-o','APT::Update::Error-Mode=any','update'])
            policy = self.run(['/usr/bin/apt-cache',*APT_OPTIONS,'policy',PACKAGE])
            selected = re.search(r'^\s*Candidate: (\S+)$',policy,re.M)
            if not selected or not VERSION.fullmatch(selected[1]): raise Deferred('no eligible signed candidate')
            version = selected[1]
            installed = self.run(['/usr/bin/dpkg-query','--show','--showformat=${Version}',PACKAGE]).strip()
            if version == installed: return 'current'
            self.run(['/usr/bin/dpkg','--compare-versions',version,'gt',installed])
            digest = candidate_metadata(self.run(['/usr/bin/apt-cache',*APT_OPTIONS,'show',PACKAGE+'='+version]),version)
            managed = self.managed(config['users'])
            stopped = []
            installing = False
            marker = {'schema':1,'phase':'prepared','version':version,'previous':installed,'digest':digest,'users':list(managed.values()),
                      'maintenance_pid':os.getpid(),'maintenance_start':self.process_start(os.getpid())}
            transaction = ['/usr/bin/apt-get',*APT_OPTIONS,'--only-upgrade','--no-remove','--no-install-recommends','--assume-yes','install',PACKAGE+'='+version]
            validate_simulation(self.run([*transaction[:1], '--simulate', *transaction[1:]]),version)
            def download_idle():
                if automatic and not self.config()['enabled']:
                    raise Deferred('automatic maintenance was disabled during download')
                if self.managed(config['users']) != managed:
                    raise Deferred('Sync process inventory changed during download')
                for pid,uid in managed.items(): self.probe(uid,pid)
            download_idle()
            self.download([*transaction[:1], '--download-only', *transaction[1:]], download_idle)
            # A long download must not install a withdrawn or changed offer, or
            # reuse an earlier host preflight after unrelated administrator work.
            self.run(['/usr/bin/apt-get',*APT_OPTIONS,'-o','APT::Update::Error-Mode=any','update'])
            fresh_policy=self.run(['/usr/bin/apt-cache',*APT_OPTIONS,'policy',PACKAGE])
            fresh=re.search(r'^\s*Candidate: (\S+)$',fresh_policy,re.M)
            if not fresh or fresh[1] != version:
                raise Deferred('candidate was withdrawn or replaced during download')
            fresh_digest=candidate_metadata(self.run(['/usr/bin/apt-cache',*APT_OPTIONS,'show',PACKAGE+'='+version]),version)
            if fresh_digest != digest: raise Deferred('same-version candidate bytes changed')
            self.check_host_maintenance()
            download_idle()
            usable = {uid:self.provider_baseline(uid,pid) for pid,uid in managed.items()}
            with contextlib.ExitStack() as stack:
                channels = [stack.enter_context(self.reservation(uid,pid,digest)) for pid,uid in managed.items()]
                if self.managed(config['users']) != managed: raise Deferred('Sync process inventory changed')
                # A closed channel means the atomic reservation has gone away.
                for channel in channels:
                    channel.setblocking(False)
                    try:
                        if channel.recv(1, socket.MSG_PEEK) == b'': raise Deferred('maintenance reservation was lost')
                    except BlockingIOError: pass
                self.write(MARKER,json.dumps(marker)+'\n')
                try:
                    for uid in managed.values():
                        stopped.append(uid); self.service(uid,['stop'])
                    if self.instances(): raise Deferred('Sync process remained after stop')
                    with self.lock(LOCK):
                        # Reserve against new daemon startup for the entire package transaction.
                        if self.instances(): raise Deferred('Sync process started during maintenance')
                        validate_simulation(self.run([*transaction[:1],'--simulate',*transaction[1:]]),version)
                        marker['phase'] = 'installing'; self.write(MARKER,json.dumps(marker)+'\n')
                        installing = True
                        self.run([*transaction[:1],'--no-download',*transaction[1:]])
                        actual = self.run(['/usr/bin/dpkg-query','--show','--showformat=${Status} ${Version}',PACKAGE]).strip()
                        if actual != 'install ok installed '+version: raise Deferred('installed package state did not verify')
                        marker['phase']='installed'; self.write(MARKER,json.dumps(marker)+'\n')
                        self.stage_activation(stopped,version,usable)
                        self.remove(MARKER)
                    self.restart_pending()
                    return 'installed '+version
                except Exception as original:
                    if not installing:
                        self.stage_activation(stopped,installed,usable)
                        self.remove(MARKER)
                        try: self.restart_pending()
                        except Deferred as recovery:
                            raise Deferred(str(original)+'; '+str(recovery)) from original
                    # Once dpkg may have written, keep the marker and never launch mixed files.
                    raise

    def status(self):
        if not self.path(CONFIG).exists(): return {'enabled':False,'state':'not-enrolled'}
        config=self.config()
        state='enabled' if config['enabled'] else 'disabled'
        result={'enabled':config['enabled'],'state':state,'users':config['users']}
        if self.path(ACTIVATION).exists():
            activation=json.loads(self.trusted(ACTIVATION).read_text())
            result.update(state='restart-pending',installedVersion=activation['version'],
                          pendingUsers=activation['pending'],errors=activation['errors'])
        if self.path(MARKER).exists(): result['state']='repair-required'
        return result

def main(argv):
    parser=argparse.ArgumentParser(prog='syncctl update')
    commands=parser.add_subparsers(dest='command',required=True)
    enable=commands.add_parser('enable'); enable.add_argument('--user',required=True)
    apply=commands.add_parser('apply'); apply.add_argument('--automatic',action='store_true')
    for command in ('status','disable','apt-hook','remove'): commands.add_parser(command)
    args=parser.parse_args(argv)
    if args.command != 'status' and os.geteuid() != 0:
        parser.error('this operation requires administrator privileges')
    engine=Maintenance()
    try:
        if args.command == 'disable': engine.disable()
        elif args.command in ('enable','remove'):
            with engine.lock(MUTEX, create=True):
                if args.command == 'enable': engine.enable(pwd.getpwnam(args.user).pw_uid)
                else: engine.remove_enrollment()
        elif args.command == 'apply': print(engine.apply(automatic=args.automatic))
        elif args.command == 'apt-hook': engine.apt_hook([line.rstrip('\n') for line in sys.stdin])
        else: print(json.dumps(engine.status(),sort_keys=True))
        return 0
    except (Deferred,OSError,ValueError,KeyError) as error:
        print('syncctl update: deferred: '+str(error),file=sys.stderr)
        return 1

if __name__ == '__main__': sys.exit(main(sys.argv[1:]))
