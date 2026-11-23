import sys
sys.argv = ['drive.py', '0', '0']
exec(open('drive.py').read().split("if __name__")[0])
ALL = ['static','field','div','array','ternary','invoke','long','float','switch','yield']
def rate(feats, n=150, start=0):
    st = {'ok':0,'FAIL':0,'rejected':0}; first=None
    for seed in range(start, start+n):
        r, info, code = check_program(seed, 2, feats)
        st[r] += 1
        if r == 'FAIL' and first is None: first = (seed, info)
    return st, first
base = {k: False for k in ALL}
print("core only      ", rate(base)[0])
for k in ALL:
    f = dict(base); f[k] = True
    st, first = rate(f)
    print("core + %-8s" % k, st, ("first fail seed %d" % first[0]) if first else "")
