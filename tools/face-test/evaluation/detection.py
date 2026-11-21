"""detection.py NAME...: the faces of results/NAME.jsonl against the boxes Immich
has for the same pictures: how many of Immich's faces were found, by size,
and how many detections Immich does not have, per score threshold."""
import json, os, sys
B = os.environ.get('FACES_WORK', '.').rstrip('/') + '/'
BUCKETS = [(0, 20), (20, 32), (32, 48), (48, 80), (80, 160), (160, 10000)]


def iou(a, b):
    w = min(a[2], b[2]) - max(a[0], b[0]); h = min(a[3], b[3]) - max(a[1], b[1])
    if w <= 0 or h <= 0: return 0
    return w * h / ((a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - w * h)


def load_truth(asset, width, height):
    out = []
    for f in json.load(open(B + 'sample/faces/%s.json' % asset)):
        sx = width / f['imageWidth']; sy = height / f['imageHeight']
        out.append(([f['boundingBoxX1'] * sx, f['boundingBoxY1'] * sy, f['boundingBoxX2'] * sx, f['boundingBoxY2'] * sy],
            (f.get('person') or {}).get('id')))
    return out


def evaluate(name, thresholds=(0.6, 0.65, 0.7, 0.75, 0.8, 0.85)):
    rows = [json.loads(l) for l in open(B + 'results/%s.jsonl' % name)]
    runs = sum(r['detect_runs'] for r in rows) / len(rows)
    ms = sum(r['detect_ms'] for r in rows) / len(rows)
    print('== %s: %d pictures, %.1f detector runs and %.0f ms per picture' % (name, len(rows), runs, ms))
    for t in thresholds:
        hit = [0] * len(BUCKETS); total = [0] * len(BUCKETS); extra = 0; extra_big = 0; found = 0
        for r in rows:
            asset = os.path.basename(r['image'])[:-4]
            truth = load_truth(asset, r['width'], r['height'])
            faces = [f for f in r['faces'] if f['score'] >= t]
            used = set()
            for box, person in truth:
                size = min(box[2] - box[0], box[3] - box[1])
                bucket = [i for i, (lo, hi) in enumerate(BUCKETS) if lo <= size < hi][0]
                total[bucket] += 1
                best, best_iou = None, 0.3
                for i, f in enumerate(faces):
                    if i in used: continue
                    v = iou(box, f['box'])
                    if v >= best_iou: best, best_iou = i, v
                if best is not None:
                    used.add(best); hit[bucket] += 1
            found += len(faces)
            for i, f in enumerate(faces):
                if i not in used:
                    extra += 1
                    if f['eyes'] >= 14: extra_big += 1
        cells = ' '.join('%4d/%-4d %3.0f%%' % (h, n, 100 * h / max(n, 1)) for h, n in zip(hit, total))
        print('  score>=%.2f recall %5.1f%%  by size %s  | found %d, not in Immich %d (eyes>=14px: %d)' % (
            t, 100 * sum(hit) / sum(total), cells, found, extra, extra_big))


print('size buckets (px, smaller side of Immich box):', BUCKETS)
for name in sys.argv[1:]:
    evaluate(name)
