#!/usr/bin/env python3
"""Execute the actual installer with a local GitHub fixture and an isolated HOME.

Also runs unchanged on Linux to exercise its real mv/sha256sum/tar utilities.
No production installer URL or environment override is introduced for testing.
"""
import hashlib
import io
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tarfile
import tempfile

tool = Path(sys.argv[0]).name
if tool in ("curl", "uname", "ldd", "mv", "sha256sum", "head"):
    fixture = Path(os.environ["INSTALLER_FIXTURE"])
    if tool == "uname":
        print(os.environ.get("INSTALLER_ARCH", "x86_64"))
    elif tool == "ldd":
        if os.environ.get("INSTALLER_LIBC") == "musl":
            print("musl libc")
            sys.exit(1)  # musl's loader prints its version with a nonzero status.
        print("ldd GNU libc")
    elif tool == "head":
        if sys.platform != 'darwin' or sys.argv[1] != '-c':
            os.execv('/usr/bin/head', ['head'] + sys.argv[1:])
        # BSD head over-reads pipes and discards the excess. The installer
        # runs on Linux, whose GNU/BusyBox head reads only the requested bytes.
        remaining = int(sys.argv[2])
        while remaining:
            chunk = os.read(0, min(65536, remaining))
            if not chunk:
                break
            sys.stdout.buffer.write(chunk)
            remaining -= len(chunk)
    elif tool == "sha256sum":
        for line in Path(sys.argv[-1]).read_text().splitlines():
            digest, name = line.split("  ", 1)
            if hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
                print(name + ": FAILED", file=sys.stderr)
                sys.exit(1)
            print(name + ": OK")
    elif tool == "mv":
        if sys.platform != "darwin":
            os.execv("/bin/mv", ["mv"] + sys.argv[1:])
        # macOS lacks GNU mv -T. Match rename semantics here; Linux uses real mv.
        try:
            os.rename(sys.argv[-2], sys.argv[-1])
        except OSError:
            sys.exit(1)
    else:
        args = sys.argv[1:]
        urls = [a for a in args if a.startswith("https://")]
        assert len(urls) == 1, args
        url = urls[0]
        with (fixture / "requests").open("a") as log:
            log.write(url + "\n")
        # The download must stay on the tag selected by the Mac.
        base = "https://github.com/shih-liang/nativepipe/releases/download/"
        assert url.startswith(base), url
        tag, name = url[len(base):].split("/", 1)
        if tag == "offline":
            print("curl: HTTP 404", file=sys.stderr)
            sys.exit(22)
        source = fixture / tag / name
        if not source.is_file():
            print("curl: HTTP 404", file=sys.stderr)
            sys.exit(22)
        if "-o" in args:
            shutil.copyfile(source, args[args.index("-o") + 1])
        else:
            sys.stdout.buffer.write(source.read_bytes())
            if (fixture / tag / "download-failed").exists():
                print("curl: interrupted download", file=sys.stderr)
                sys.exit(18)
    sys.exit(0)

script = sys.stdin.read()
installer = Path(sys.argv[1]).read_bytes()
upload = "--upload" in sys.argv
assert ("NATIVEPIPE INSTALLER" if upload else "| sh -s") in script
assert "tar -xzf" not in script, "Installation policy must stay out of SSH arguments"
with tempfile.TemporaryDirectory(prefix="nativepipe-installer-") as directory:
    root = Path(directory)
    fixture = root / "github"
    fixture.mkdir()
    test_home = root / "user account"
    test_home.mkdir()
    scratch = root / "temporary"
    scratch.mkdir()
    binary = root / "bin"
    binary.mkdir()
    utilities = ["curl", "uname", "ldd", "mv"]
    if upload and sys.platform == 'darwin':
        utilities.append('head')
    if shutil.which("sha256sum") is None:  # Stock macOS has shasum instead.
        utilities.append("sha256sum")
    for name in utilities:
        shutil.copyfile(__file__, binary / name)
        (binary / name).chmod(0o755)
    env = dict(os.environ, HOME=str(test_home), INSTALLER_FIXTURE=str(fixture),
               INSTALLER_ARCH="x86_64", INSTALLER_LIBC="gnu",
               TMPDIR=str(scratch),
               PATH=str(binary) + os.pathsep + os.environ["PATH"])
    asset = "nativepipe-compositor-x86_64-gnu.tar.gz"

    def release(version, executable=True):
        target = fixture / version
        target.mkdir()
        (target / "install-compositor.sh").write_bytes(installer)
        with tarfile.open(target / asset, "w:gz") as archive:
            files = {"lib/version": version.encode()}
            if executable:
                # The upload prelude must leave following stdin bytes intact.
                status = 71 if version == 'bad-runtime' else 0
                check = f'if [ "${{1:-}}" = --check-runtime ]; then exit {status}; fi\n'
                check += 'read marker\n[ "$marker" = AFTER_ARCHIVE ] || exit 97\n'
                files["nativepipe-wayland"] = ("#!/bin/sh\n" + check + "printf '%s\\n' '" + version + "'\n").encode()
                files["libexec/np-open"] = b"#!/bin/sh\nexit 0\n"
            for name, data in files.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 0o755 if name in ("nativepipe-wayland", "libexec/np-open") else 0o644
                archive.addfile(info, io.BytesIO(data))
        digest = hashlib.sha256((target / asset).read_bytes()).hexdigest()
        (target / "SHA256SUMS").write_text(digest + "  " + asset + "\n")
        (fixture / "latest").write_text(version)
        return digest

    def resolved_script():
        release_url = "https://github.com/shih-liang/nativepipe/releases/download/" + (fixture / "latest").read_text()
        return "release=" + shlex.quote(release_url) + "\n" + script

    def invoke(success=True, cached=None, truncate=False, declaration=None, leading_zeros=False):
        if upload:
            process = subprocess.Popen(["/bin/sh", "-c", script], env=env,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            assert process.stdout.readline() == b'NATIVEPIPE INSTALLER\n'
            process.stdin.write(f'{len(installer)}\n'.encode() + installer)
            process.stdin.flush()
            target = f'NATIVEPIPE TARGET {env["INSTALLER_ARCH"]} {env["INSTALLER_LIBC"]}\n'.encode()
            assert process.stdout.readline() == target
            directory = fixture / (fixture / 'latest').read_text()
            data = (directory / asset).read_bytes()
            digest = (directory / 'SHA256SUMS').read_text().split()[0]
            declared_size = str(len(data)) if declaration is None else declaration
            if leading_zeros:
                declared_size = '000' + declared_size
            process.stdin.write(f'{digest} {declared_size}\n'.encode())
            process.stdin.flush()
            response = process.stdout.readline()
            if cached is not None:
                assert response == (b'NATIVEPIPE CACHED\n' if cached else b'NATIVEPIPE UPLOAD\n'), response
            payload = data if response == b'NATIVEPIPE UPLOAD\n' else b''
            payload = payload[:len(payload)//2] if truncate else payload + b'AFTER_ARCHIVE\n'
            out, err = process.communicate(input=payload, timeout=20)
            result = subprocess.CompletedProcess(process.args, process.returncode, out.decode(), err.decode())
        else:
            result = subprocess.run(["/bin/sh", "-c", resolved_script()], env=env,
                                    input="AFTER_ARCHIVE\n", capture_output=True, text=True, timeout=20)
        assert (result.returncode == 0) == success, result.stderr
        if success:
            assert result.stdout == (fixture / "latest").read_text() + "\n", result
        return result

    installation = test_home / ".local/share/nativepipe/compositor"
    current = installation / "current"
    # An old PATH installation must not bypass an explicitly requested update.
    (binary / "nativepipe-wayland").write_text("#!/bin/sh\nexit 99\n")
    (binary / "nativepipe-wayland").chmod(0o755)
    first = release("v1")
    invoke(cached=False, leading_zeros=upload)
    first_path = current.resolve()
    assert first_path.name == first
    requests_before = 0 if upload else (fixture / "requests").read_text().count(asset)
    invoke(cached=True)
    if upload:
        assert not (fixture / 'requests').exists(), 'Bundled installation contacted GitHub'
    else:
        assert (fixture / "requests").read_text().count(asset) == requests_before
    second = release("v2")
    invoke()
    assert current.resolve().name == second
    assert (first_path / "lib/version").read_text() == "v1", "Running version was modified"
    release("corrupt")
    with (fixture / "corrupt" / asset).open("ab") as archive:
        archive.write(b"corruption")
    invoke(success=False)
    assert current.resolve().name == second
    release('bad-runtime')
    invoke(success=False)
    assert current.resolve().name == second, 'An incompatible runtime replaced the active version'
    release("missing-executable", executable=False)
    invoke(success=False)
    assert current.resolve().name == second
    release("bad-manifest")
    (fixture / "bad-manifest/SHA256SUMS").write_text("not-a-digest  " + asset + "\n")
    invoke(success=False)
    assert current.resolve().name == second
    if upload:
        release('truncated')
        assert 'interrupted' in invoke(success=False, truncate=True).stderr
        # Large declarations reach the streaming reader; no fixture allocates
        # their declared bytes. The tiny payload must then fail completeness.
        for size in ('536870913', '2147483648', '9223372036854775808'):
            failure = invoke(success=False, cached=False, truncate=True, declaration=size)
            assert 'interrupted' in failure.stderr, (size, failure)
            assert current.resolve().name == second
        for size in ('', '0', '000', '-1', '+1', '1.0', '0x20', '1 extra', '9' * 300):
            failure = invoke(success=False, declaration=size)
            assert 'invalid' in failure.stderr, (size, failure)
            assert current.resolve().name == second
        for data in (b'0\n', b'65537\n', b'invalid\n', b'100\nshort script'):
            result = subprocess.run(["/bin/sh", "-c", script], env=env, input=data,
                                    capture_output=True, timeout=20)
            assert result.returncode != 0 and b'NATIVEPIPE TARGET' not in result.stdout, result
            assert current.resolve().name == second
    else:
        for version, data in (("empty-installer", b""), ("partial-installer", installer.split(b'\ninstall_compositor "$@"')[0])):
            release(version)
            (fixture / version / 'install-compositor.sh').write_bytes(data)
            invoke(success=False)
            assert current.resolve().name == second
        release('interrupted-installer')
        (fixture / 'interrupted-installer/install-compositor.sh').write_bytes(b'exit 0\n')
        (fixture / 'interrupted-installer/download-failed').touch()
        assert 'interrupted download' in invoke(success=False).stderr
        release('interrupted-with-valid-output')
        executable = shlex.quote(str(current.resolve() / 'nativepipe-wayland'))
        (fixture / 'interrupted-with-valid-output/install-compositor.sh').write_text(f"printf '%s\\n' {executable}\n")
        (fixture / 'interrupted-with-valid-output/download-failed').touch()
        assert 'didn’t download completely' in invoke(success=False).stderr
        release('missing-installer')
        (fixture / 'missing-installer/install-compositor.sh').unlink()
        assert 'HTTP 404' in invoke(success=False).stderr
        (fixture / "latest").write_text("offline")
        assert "HTTP 404" in invoke(success=False).stderr
    assert current.resolve().name == second
    third = release("v3")
    if upload:
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=3) as workers:
            list(workers.map(lambda _: invoke(), range(3)))
    else:
        children = [subprocess.Popen(["/bin/sh", "-c", resolved_script()], env=env,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(3)]
        for child in children:
            out, err = child.communicate(input=b'AFTER_ARCHIVE\n', timeout=20)
            assert child.returncode == 0 and out == b"v3\n", err.decode()
    assert current.resolve().name == third
    assert (current / "lib/version").read_text() == "v3"
    for arch, libc in (("aarch64", "gnu"), ("aarch64", "musl"), ("x86_64", "musl")):
        env.update(INSTALLER_ARCH=arch, INSTALLER_LIBC=libc)
        asset = f"nativepipe-compositor-{arch}-{libc}.tar.gz"
        digest = release(f"target-{arch}-{libc}")
        invoke(cached=False)
        assert current.resolve().name == digest
    (binary / "bash").write_text("#!/bin/sh\necho 'bash is unavailable' >&2\nexit 127\n")
    (binary / "bash").chmod(0o755)
    release("without-bash")
    invoke(cached=False)
    assert not list(installation.glob(".install-*")), "Staging directories leaked"
    leftovers = {path.name for path in scratch.iterdir()}
    if sys.platform == "darwin":
        # Apple's python3 launcher creates this toolchain cache in TMPDIR.
        leftovers.discard("xcrun_db")
    assert not leftovers, f"Installer bootstrap temporary files leaked: {leftovers}"
    print("PASS", "bundled upload" if upload else "curl | sh", "install, cached reconnect, update, immutable running version, corrupt archive, missing executable, invalid manifest, installer failure, stdin handoff, interruption/offline, concurrent publication, four architecture/libc targets, no Bash dependency")
