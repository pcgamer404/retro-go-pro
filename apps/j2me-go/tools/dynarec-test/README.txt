Run from this folder (tools/dynarec-test).  Needs: gcc, python3.  The project root is found automatically.

Host-side verification of dynarec.c (no ESP32 needed).

  bash build.sh libdyn.so                                  build the real dynarec.c for the host (with JIT_VERBOSE)
  python3 drive.py START COUNT ./libdyn.so [SCALE]        differential test: N random programs, JIT vs reference
  python3 featscan.py                                      failure rate per language feature
  python3 bench.py ./libdyn.so                             instructions per iteration on six kernels
  python3 mutate.py [N ...]                                 inject bugs; the harness must catch each one

Files: xtemu.py (Xtensa subset emulator), refvm.py (reference interpreter), gen.py (assembler + program generator),
stubs2.c / xjit_exec_host.c (host replacements for VM services). x86-64 struct layouts differ from Xtensa; offsets are
read from the build so everything stays self-consistent.
