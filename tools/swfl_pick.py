# Selective SWFL extraction for the wired-CarPlay study, using IDEmu's own reader.
# IDEMU: path of an IDEmu checkout (default: ../../emulators/IDEmu beside this repo)
import os, sys, re
sys.path.insert(0, os.environ.get('IDEMU', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'emulators', 'IDEmu')))
from idemu import swfl
src, out = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else 'pick'
PAT = re.compile(r'(^|/)(bin/(vdev-medialaunch|start_usb\.sh|start_vdev_medialaunch\.sh|io-usb-dcd)|etc/umass-enum[^/]*|lib/devu-iap2ncm[^/]*|opt/conn/.*|opt/sys/bin/evo-medialaunch|opt/sys/etc/starter\.cfg|opt/mm/bin/NBTMediaMainApp|opt/hmi/ID5/data/ro/app/hmicarplay.*)$')
if mode == 'summary':
    s = swfl.summarize(src); print(s['files'], s['bytes'], s['cpus']); print(sorted(s['roots'].items(), key=lambda x: -x[1])[:15]); sys.exit()
res = swfl.extract(src, out, lambda tp, n, m: bool(PAT.search(tp + '/' + n)))
print(len(res), 'files', sum(v for _, v in res), 'bytes')
