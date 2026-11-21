"""Compile actual LVGL firmware UI on Windows; render and exercise input paths.
Run with the project's Python. SDK/CC come from env vars or firmware/dev-env.json.
"""
from pathlib import Path
import os, subprocess, concurrent.futures, hashlib
from PIL import Image, ImageDraw, ImageFont
from dev_env import get as dev_get

_SDK_BASE = dev_get("BL_SDK_BASE")
if not _SDK_BASE:
    raise SystemExit("缺少 BL_SDK_BASE：设置环境变量，或写入 firmware/dev-env.json（见 firmware/README.md）")
SDK = Path(_SDK_BASE) / "components/graphics/lvgl_v9"
CC = dev_get("HOST_CC", "gcc")
ROOT=Path(__file__).resolve().parents[1]
HOST=ROOT/'tools/host'
OUT=ROOT/'tools/host_output'
UI=ROOT.parent/'eez_ui/src/ui'
OUT.mkdir(exist_ok=True)
flags=['-O1','-std=c99','-DLV_KCONFIG_IGNORE','-DLV_CONF_PATH="'+(HOST/'lv_conf.h').as_posix()+'"']
for inc in (HOST,ROOT,UI,SDK): flags+=['-I'+str(inc)]
sources=[p for p in SDK.rglob('*.c') if 'port' not in p.relative_to(SDK).parts]
sources+=list((ROOT/'fonts_gen').glob('*.c'))+list(UI.glob('*.c'))
sources += [ROOT/p for p in ['face_lvgl.c','watch_photo.c','eez_ui_glue.c','tp_lvgl.c','power.c']]+[HOST/'preview.c']
def compile_one(p):
    obj=OUT/(hashlib.sha1(str(p).encode()).hexdigest()[:14]+'.o')
    # Headers and generated object layout may change: rebuild application objects every run.
    if not str(p).startswith(str(SDK)) or not obj.exists() or obj.stat().st_mtime<max(p.stat().st_mtime,(HOST/'lv_conf.h').stat().st_mtime):
        r=subprocess.run([CC,*flags,'-c',str(p),'-o',str(obj)],capture_output=True,text=True)
        if r.returncode: raise RuntimeError(str(p)+'\n'+r.stderr)
    return obj
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool: objects=list(pool.map(compile_one,sources))
rsp=OUT/'link.rsp';rsp.write_text('\n'.join('"'+p.as_posix()+'"' for p in objects),encoding='utf-8')
exe=OUT/'preview.exe'
subprocess.run([CC,'@'+str(rsp),'-lm','-o',str(exe)],check=True)
before_run={p.name:p.stat().st_mtime_ns for p in OUT.glob('*.ppm')}
subprocess.run([str(exe)],cwd=OUT,check=True)
# The shared policy now always rounds X; mode 1 additionally asserts alignment.
# This is a repeatability/bounds check, NOT native-vs-rounded or QSPI validation.
reference={p.name:hashlib.sha256(p.read_bytes()).digest() for p in OUT.glob('*.ppm')
           if p.stat().st_mtime_ns != before_run.get(p.name)}
for mode in (1,):
    subprocess.run([str(exe),str(mode)],cwd=OUT,check=True)
    changed=[name for name,digest in reference.items()
             if hashlib.sha256((OUT/name).read_bytes()).digest()!=digest]
    if changed: raise AssertionError(f'flushmode {mode}: different pixels in {changed}')
    print(f'[flush-policy] mode={mode}: {len(reference)} repeat frames identical; bounds/buffer checks passed')
sheet=Image.new('RGB',(1056,1128),'#101310');dr=ImageDraw.Draw(sheet)
font=ImageFont.truetype(str(ROOT.parent/'font/ttf/HarmonyOS_Sans_SC_Medium.ttf'),20)
for i,(name,title) in enumerate([('face','00 / 协议源石容器'),('network','01 / 协议网络'),('meets','02 / 相遇记录'),('settings','03 / 终端设置')]):
    im=Image.open(OUT/(name+'.ppm'));im.save(OUT/(name+'.png'))
    x=24+(i%2)*528;y=42+(i//2)*558
    sheet.paste(im,(x,y));dr.text((x+110,y+490),title,font=font,fill='#ECEEE5')
sheet.save(OUT/'terminal_overview.png')
frames=[Image.open(OUT/f'motion_{i:02d}.ppm').convert('RGB') for i in range(60)]
palette=frames[0].quantize(colors=128)
frames=[im.quantize(palette=palette,dither=Image.Dither.NONE) for im in frames]
frames[0].save(OUT/'bezel_clock.gif',save_all=True,append_images=frames[1:],duration=100,loop=0,optimize=False)
Image.open(OUT/'midnight.ppm').save(OUT/'midnight.png')
print(OUT/'terminal_overview.png')
