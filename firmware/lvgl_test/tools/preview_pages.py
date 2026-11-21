"""Render actual generated LVGL pages. Run eez_rebuild.ps1 after editing EEZ."""
from pathlib import Path
import subprocess
import sys
import shutil
here=Path(__file__).resolve().parent
subprocess.run([sys.executable,str(here/'host_preview.py')],check=True)
out=here.parents[1]/'eez_ui/preview'
out.mkdir(exist_ok=True)
for name in ('face','network','meets','settings'):
    shutil.copy2(here/'host_output'/(name+'.png'),out/('page_'+name+'.png'))
shutil.copy2(here/'host_output/terminal_overview.png',out/'preview_all.png')
print(out/'preview_all.png')
