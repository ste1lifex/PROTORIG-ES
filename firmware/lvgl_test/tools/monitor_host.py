"""Render the real monitor LVGL page and verify protocol + watch power restoration.
Does not open USB, flash or simulate hardware sleep. Requires gcc; uses SDK LVGL.
"""
from pathlib import Path
import os, subprocess, concurrent.futures, hashlib
from dev_env import get as dev_get
ROOT=Path(__file__).resolve().parents[1]
HOST=ROOT/'tools/host'
OUT=ROOT/'tools/monitor_output'
_SDK_BASE = dev_get('BL_SDK_BASE')
if not _SDK_BASE:
    raise SystemExit('缺少 BL_SDK_BASE：设置环境变量，或写入 firmware/dev-env.json（见 firmware/README.md）')
SDK=Path(_SDK_BASE)/'components/graphics/lvgl_v9'
CC=dev_get('HOST_CC', 'gcc')
UI=ROOT.parent/'eez_ui/src/ui'
OUT.mkdir(exist_ok=True)
flags=['-O1','-std=c99','-DPROTORIG_HOST_TEST','-DLV_KCONFIG_IGNORE','-DLV_CONF_PATH="'+(HOST/'lv_conf.h').as_posix()+'"']
for inc in (HOST,ROOT,UI,SDK): flags+=['-I'+str(inc)]
sources=[p for p in SDK.rglob('*.c') if 'port' not in p.relative_to(SDK).parts]
sources+=list((ROOT/'fonts_gen').glob('*.c'))+list(UI.glob('*.c'))
sources += [ROOT/p for p in ['pc_monitor.c','monitor_protocol.c','eez_ui_glue.c','power.c']]+[HOST/'monitor_test.c']
def compile_one(p):
    obj=OUT/(hashlib.sha1(str(p).encode()).hexdigest()[:14]+'.o')
    if not str(p).startswith(str(SDK)) or not obj.exists():
        r=subprocess.run([CC,*flags,'-c',str(p),'-o',str(obj)],capture_output=True,text=True)
        if r.returncode: raise RuntimeError(str(p)+'\n'+r.stderr)
    return obj
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool: objects=list(pool.map(compile_one,sources))
rsp=OUT/'link.rsp';rsp.write_text('\n'.join('"'+p.as_posix()+'"' for p in objects),encoding='utf-8')
exe=OUT/'monitor_test.exe'
subprocess.run([CC,'@'+str(rsp),'-lm','-o',str(exe)],check=True)
subprocess.run([str(exe)],cwd=OUT,check=True)
try:
    from PIL import Image
    for p in OUT.glob('monitor-*.ppm'): Image.open(p).save(p.with_suffix('.png'))
except ImportError:
    print('Pillow unavailable; PPM previews retained.')
