#!/usr/bin/env python3
"""Development only: retags the device engine's arm64 objects for the arm64 iOS Simulator, so the
real engine (not a stand-in) runs in the simulator. The result is never linked into a device build."""
import pathlib, struct, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
source = root / 'Vendor/OrcaBridgeCore.xcframework/ios-arm64/libOrcaBridgeCore.a'
output = root / 'Vendor/OrcaBridgeCoreSimulator/libOrcaBridgeCore.a'
LC_BUILD_VERSION, PLATFORM_IOS, PLATFORM_IOSSIMULATOR = 0x32, 2, 7
with tempfile.TemporaryDirectory() as work:
    subprocess.run(['ar', 'x', source], cwd=work, check=True)
    subprocess.run(['chmod', '-R', 'u+rw', '.'], cwd=work, check=True)
    objects = []
    for file in pathlib.Path(work).iterdir():
        if file.suffix != '.o':
            continue
        data = bytearray(file.read_bytes())
        ncmds = struct.unpack_from('<I', data, 16)[0]
        offset, patched = 32, False
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', data, offset)
            if cmd == LC_BUILD_VERSION and struct.unpack_from('<I', data, offset + 8)[0] == PLATFORM_IOS:
                struct.pack_into('<I', data, offset + 8, PLATFORM_IOSSIMULATOR)
                patched = True
            offset += size
        if not patched:
            raise SystemExit(f'No iOS build version in {file.name}')
        file.write_bytes(data)
        objects.append(file.name)
    output.parent.mkdir(exist_ok=True)
    output.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'libtool', '-static', '-no_warning_for_no_symbols', '-o', output, *sorted(objects)], cwd=work, check=True)
print('Wrote', output, len(objects), 'objects')
