import os
import subprocess
import sys

idf_py = r'C:\Espressif\python_env\idf5.3_py3.11_env\Scripts\python.exe'
env = os.environ.copy()
env['IDF_PATH'] = r'C:\Espressif\frameworks\esp-idf-v5.3.5'
env['IDF_PYTHON_ENV_PATH'] = r'C:\Espressif\python_env\idf5.3_py3.11_env'
env['PATH'] = ';'.join([
    r'C:\Espressif\python_env\idf5.3_py3.11_env\Scripts',
    r'C:\Espressif\tools\xtensa-esp-elf\esp-13.2.0_20250707\xtensa-esp-elf\bin',
    r'C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64',
    r'C:\Espressif\tools\cmake\3.30.2\bin',
    r'C:\Espressif\tools\ninja\1.12.1',
    env.get('PATH', '')
])

ninja_bin = r'C:\Espressif\tools\ninja\1.12.1\ninja.exe'
cmd = [ninja_bin, '-C', r'D:\Games\E\retro-go-pro\j2me\j2me-go\build']
print("Running command:", " ".join(cmd))
res = subprocess.run(cmd, env=env)
if res.returncode == 0:
    bin_path = r'D:\Games\E\retro-go-pro\j2me\j2me-go\build\j2me-go.bin'
    if os.path.exists(bin_path):
        size = os.path.getsize(bin_path)
        print(f"\nSUCCESS: Built {bin_path} ({size:,} bytes)")
sys.exit(res.returncode)
