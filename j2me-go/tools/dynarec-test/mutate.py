import subprocess, sys, os, re
os.environ.setdefault('PROJ', os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
src = open(os.environ['PROJ'] + '/main/nanojava/src/vm/dynarec.c').read()
MUT = [
 ("lsub borrow operand order", "xj_saltu(&E, R_TMP2, ral, rbl);\n        xj_sub(&E, R_TMP, ral, rbl);", "xj_saltu(&E, R_TMP2, rbl, ral);\n        xj_sub(&E, R_TMP, ral, rbl);"),
 ("bounds check removed", "    chk_bounds(rar, rix, ex);\n    switch (op) {\n    case 0x33: xj_add(&E, R_TMP, rix, rar); break;", "    switch (op) {\n    case 0x33: xj_add(&E, R_TMP, rix, rar); break;"),
 ("cached locals not written back at exit", "    for (int k = 0; k < A.n_cached; k++)\n        xj_s32i(&E, A.lreg_of[A.cached[k]], R_LV, A.cached[k] * 4);\n    xj_s32i(&E, R_BUD, R_T, offsetof(Thread, budget));\n    xj_movi(&E, R_T, x->code);", "    xj_s32i(&E, R_BUD, R_T, offsetof(Thread, budget));\n    xj_movi(&E, R_T, x->code);"),
 ("ifgt/ifle inverted (imm 0)", "if (c == C_GT) xj_blt(&E, R_TMP, ra, 0); else xj_bge(&E, R_TMP, ra, 0);", "if (c == C_GT) xj_bge(&E, R_TMP, ra, 0); else xj_blt(&E, R_TMP, ra, 0);"),
 ("dispatch leaf jumps to previous root", "        emit_jump_pc(s_roots[lo]);\n        xj_movi(&E, R_T, JIT_DEOPT);", "        emit_jump_pc(s_roots[lo > 0 ? lo - 1 : lo]);\n        xj_movi(&E, R_T, JIT_DEOPT);"),
 ("flush_all does not resync REG_SP", "    if (adj) emit_addi32(R_SP, R_SP, adj * 4);\n}\n\n/* a stack entry", "    (void)adj;\n}\n\n/* a stack entry"),
 ("spill forgets to store", "            store_slot(&vs[i], mem_depth + i);\n            rfree(vs[i].reg);", "            rfree(vs[i].reg);"),
 ("spill stores to wrong slot", "            store_slot(&vs[i], mem_depth + i);\n            rfree(vs[i].reg);", "            store_slot(&vs[i], mem_depth + i + 1);\n            rfree(vs[i].reg);"),
 ("local store does not detach pending reads", "static void detach_local(int idx)\n{", "static void detach_local(int idx)\n{\n    return;"),
 ("budget register not stored at exit", "    xj_s32i(&E, R_BUD, R_T, offsetof(Thread, budget));\n    xj_movi(&E, R_T, x->code);", "    xj_movi(&E, R_T, x->code);"),
 ("backward goto skips budget check", "        if (off <= 0) budget_check(pc);\n        flush_all();\n        emit_jump_pc(pc + off);", "        flush_all();\n        emit_jump_pc(pc + off);"),
 ("x / -1 returns x", "if (op == 0x6c) xj_neg(&E, d, ra); else xj_movi(&E, d, 0);\n        patch_j(p_j, E.pos);", "if (op == 0x6c) xj_mov(&E, d, ra); else xj_movi(&E, d, 0);\n        patch_j(p_j, E.pos);"),
 ("divide-by-zero check removed", "        branch_exit(0, rb, ex);                              /* divide by zero */", "        (void)ex;"),
 ("aastore accepts wrong class", "p_ok = E.pos; xj_beq(&E, R_TMP, R_TMP2, 0);", "p_ok = E.pos; xj_bne(&E, R_TMP, R_TMP2, 0);"),
 ("dup_x1 wrong order", "{ VEnt c = clone_ent(&v1); push(c); push(v2); push(v1); } break;", "{ VEnt c = clone_ent(&v1); push(v2); push(v1); push(c); } break;"),
 ("i2l sign extension missing", "xj_srai(&E, d, ra, 31);\n        push(a);", "xj_srli(&E, d, ra, 15);\n        push(a);"),
 ("null check removed on getfield", "    VEnt ops[1] = { o };\n    int ex = snap_exit(JIT_DEOPT, pc, ops, 1);\n    chk_null(ro, ex);\n    if (op == 0xcb) {", "    VEnt ops[1] = { o };\n    (void)snap_exit(JIT_DEOPT, pc, ops, 1);\n    if (op == 0xcb) {"),
 ("lcmp low word compared signed", "xj_movi(&E, R_TMP, -1); p[n++] = E.pos; xj_bltu(&E, ral, rbl, 0);", "xj_movi(&E, R_TMP, -1); p[n++] = E.pos; xj_blt(&E, ral, rbl, 0);"),
 ("iushr const >=16 uses sra", "    else xj_extui(&E, rd, ra, sa, 32 - sa);\n}", "    else xj_srai(&E, rd, ra, sa);\n}"),
 ("char/short element address uses x4", "    case 0x34: case 0x35: xj_addx2(&E, R_TMP, rix, rar); break;", "    case 0x34: case 0x35: xj_addx4(&E, R_TMP, rix, rar); break;"),
 ("putfield2 hi/lo swapped", "    xj_s32i(&E, rvl, ro, off);\n    if (wide) xj_s32i(&E, rvh, ro, off + 4);", "    xj_s32i(&E, wide ? rvh : rvl, ro, off);\n    if (wide) xj_s32i(&E, rvl, ro, off + 4);"),
 ("tableswitch index bias", "if (lo >= -32767 && lo <= 32767) emit_addi32(R_TMP, rk, -lo);", "if (lo >= -32767 && lo <= 32767) emit_addi32(R_TMP, rk, -lo + 1);"),
 ("lookupswitch pivot direction", "    xj_blt(&E, rk, R_TMP2, 2);                                /* below the pivot: the left half follows */", "    xj_bge(&E, rk, R_TMP2, 2);                                /* below the pivot: the left half follows */"),
 ("exit stores wrong f->pc", "emit_l32r(R_TMP, lit_const((uint32_t)(uintptr_t)(CC + x->pc)));", "emit_l32r(R_TMP, lit_const((uint32_t)(uintptr_t)(CC + x->pc + 1)));"),
 ("exit f->sp off by one slot", "    emit_addi32(R_TMP, R_SP, s_bias + adj * 4);\n    xj_s32i(&E, R_TMP, R_F, offsetof(Frame, sp));", "    emit_addi32(R_TMP, R_SP, s_bias + adj * 4 + 4);\n    xj_s32i(&E, R_TMP, R_F, offsetof(Frame, sp));"),
 ("fcmpg/fcmpl unordered result swapped", "xj_movi(&E, R_TMP, op == 0x95 ? -1 : 1);", "xj_movi(&E, R_TMP, op == 0x95 ? 1 : -1);"),
 ("i2b uses 16-bit sext", "if (op == 0x91) xj_sext(&E, d, ra, 7);", "if (op == 0x91) xj_sext(&E, d, ra, 15);"),
 ("ladd carry dropped", "        xj_saltu(&E, R_TMP2, R_TMP, ral);\n        int d0 = ralloc(), d1 = ralloc();\n        xj_add(&E, d1, rah, rbh);", "        xj_movi(&E, R_TMP2, 0);\n        int d0 = ralloc(), d1 = ralloc();\n        xj_add(&E, d1, rah, rbh);"),
 ("ishr const uses logical shift", "    else if (op == 0x7a) xj_srai(&E, rd, ra, sa);", "    else if (op == 0x7a) xj_srli(&E, rd, ra, sa & 15);"),
 ("iinc on cached local does not detach", "static void local_inc(int idx, int c)\n{\n    detach_local(idx);", "static void local_inc(int idx, int c)\n{"),
 ("conditional-branch operands released early (flush clobbers)", "    flush_all();                                              /* operands stay in their registers */", "    rel(&a); if (!bimm) rel(&b);\n    flush_all();"),
]
only = [int(a) for a in sys.argv[1:]]
res = []
for i, (name, old, new) in enumerate(MUT):
    if only and i not in only: continue
    if src.count(old) != 1:
        res.append((i, name, "PATTERN-MISSING %d" % src.count(old))); continue
    D = '/tmp/mut%d' % i
    os.makedirs(D, exist_ok=True)
    open(D + '/dynarec.c', 'w').write(src.replace(old, new))
    for f in ('dynarec.h', 'xjit_emit.h', 'xjit_exec.h', 'xjit_exec.c', 'vm.h'):
        subprocess.run(['cp', os.environ['PROJ'] + '/main/nanojava/src/vm/' + f, D + '/'])
    PROJ = os.environ['PROJ']; SRCD = PROJ + '/main/nanojava/src'
    cmd = ['gcc', '-g', '-O1', '-shared', '-fPIC', '-DJIT_VERBOSE=1', '-I' + D, '-I' + SRCD + '/pal', '-I' + SRCD,
           '-o', D + '/m.so', D + '/dynarec.c', 'xjit_exec_host.c', 'stubs2.c']
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = subprocess.run(['python3', 'drive.py', '0', '400', D + '/m.so'], capture_output=True, text=True).stdout
    m = re.search(r"\{'ok': (\d+), 'FAIL': (\d+), 'rejected': (\d+)\}", out)
    res.append((i, name, ("caught: %s of 400 programs fail" % m.group(2)) if m and int(m.group(2)) > 0 else ("NOT CAUGHT " + (m.group(0) if m else out[-200:]))))
for r in res: print("%2d %-45s %s" % r)
