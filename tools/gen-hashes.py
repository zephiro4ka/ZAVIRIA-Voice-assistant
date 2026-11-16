# Записывает контрольные суммы файлов программы в ZAVIRIA/app/IntegrityHashes.inc.
# Запускать после обновления llama.cpp, onnxruntime или версии cloudflared в tools/installer.iss.
import hashlib, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.join(ROOT, 'ZAVIRIA')


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


entries = {}
release = os.path.join(PROJECT, 'x64', 'Release')
for name in sorted(os.listdir(release)):
    if name.startswith('onnxruntime') and name.endswith('.dll'):
        entries[name] = sha(os.path.join(release, name))
engine = os.path.join(PROJECT, 'third_party', 'llama.cpp')
for folder, _, files in os.walk(engine):
    for name in sorted(files):
        if name.lower().endswith(('.exe', '.dll')):
            full = os.path.join(folder, name)
            entries['llama.cpp\\' + os.path.relpath(full, engine)] = sha(full)
pinned = re.search(r"cloudflared-windows-amd64\.exe', 'cloudflared\.exe', '([0-9a-f]{64})'", open(os.path.join(ROOT, 'tools', 'installer.iss'), encoding='utf-8-sig').read())
entries['cloudflared.exe'] = pinned.group(1)

with open(os.path.join(PROJECT, 'app', 'IntegrityHashes.inc'), 'w', encoding='utf-8', newline='\n') as out:
    for path, value in entries.items():
        out.write('{ L"%s", "%s" },\n' % (path.replace('\\', '\\\\'), value))
print(len(entries), 'files')
