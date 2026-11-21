"""run.py NAME [facescan_host options]: runs facescan_host (FACESCAN_HOST, default
build-host/face-test/facescan_host; tools/face-test/build-host.sh makes it)
over the sample in FACES_WORK in JOBS processes, writes results/NAME.jsonl.
--models DIR says where the networks are (default: models)."""
import glob, os, subprocess, sys
B = os.environ.get('FACES_WORK', '.').rstrip('/') + '/'
name = sys.argv[1]
extra = sys.argv[2:]
models = 'models'
if '--models' in extra:
    models = extra[extra.index('--models') + 1]
    del extra[extra.index('--models'):extra.index('--models') + 2]
pictures = sorted(glob.glob(B + 'sample/ppm/*.ppm'))
if os.environ.get('LIMIT'):
    pictures = pictures[:int(os.environ['LIMIT'])]
jobs = int(os.environ.get('JOBS', '8'))
os.makedirs(B + 'results', exist_ok=True)
procs = []
for j in range(jobs):
    part = pictures[j::jobs]
    listfile = B + 'results/%s.%d.list' % (name, j)
    open(listfile, 'w').write('\n'.join(part) + '\n')
    out = open(B + 'results/%s.%d.jsonl' % (name, j), 'w')
    procs.append((subprocess.Popen([os.environ.get('FACESCAN_HOST', 'build-host/face-test/facescan_host'), '--models', models, '--list', listfile] + extra, stdout=out), out, listfile))
with open(B + 'results/%s.jsonl' % name, 'w') as merged:
    for j, (proc, out, listfile) in enumerate(procs):
        proc.wait(); out.close()
        merged.write(open(out.name).read())
        os.remove(out.name); os.remove(listfile)
print(name, sum(1 for _ in open(B + 'results/%s.jsonl' % name)), 'pictures')
