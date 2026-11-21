"""recognition.py NAME [--quality Q]: embeddings of results/NAME.jsonl against
Immich's person clusters: how often the rules a program could use give a
face the right person when it only knows that person's faces from other
days, and what grouping by links does. Immich splits some people over
several clusters; sample/merges.json may say which of its clusters are one
person and which are small children, by rank in number of faces:
{"merge": {"13": 1}, "small": [3, 6]}. Aggregate numbers only."""
import json, os, sys
import numpy as np
B = os.environ.get('FACES_WORK', '.').rstrip('/') + '/'
name = sys.argv[1]
min_q = float(sys.argv[sys.argv.index('--quality') + 1]) if '--quality' in sys.argv else 0.0
taken = json.load(open(B + 'sample/taken.json'))
notes = json.load(open(B + 'sample/merges.json')) if os.path.exists(B + 'sample/merges.json') else {}
merge = {int(k): int(v) for k, v in notes.get('merge', {}).items()}


def iou(a, b):
    w = min(a[2], b[2]) - max(a[0], b[0]); h = min(a[3], b[3]) - max(a[1], b[1])
    if w <= 0 or h <= 0: return 0
    return w * h / ((a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - w * h)


import collections
count = collections.Counter()
rows = [json.loads(line) for line in open(B + 'results/%s.jsonl' % name)]
for r in rows:
    asset = os.path.basename(r['image'])[:-4]
    for f in json.load(open(B + 'sample/faces/%s.json' % asset)):
        if f.get('person'):
            count[f['person']['id']] += 1
order = [p for p, _ in count.most_common()]
canon = {p: order[merge.get(i, i)] for i, p in enumerate(order)}
small = {order[i] for i in notes.get('small', []) if i < len(order)}

faces = []
for r in rows:
    asset = os.path.basename(r['image'])[:-4]
    truth = []
    for f in json.load(open(B + 'sample/faces/%s.json' % asset)):
        sx = r['width'] / f['imageWidth']; sy = r['height'] / f['imageHeight']
        truth.append(([f['boundingBoxX1'] * sx, f['boundingBoxY1'] * sy, f['boundingBoxX2'] * sx, f['boundingBoxY2'] * sy],
            (f.get('person') or {}).get('id')))
    for i, f in enumerate(r['faces']):
        if not f['emb'] or f['score'] < 0.7 or f['quality'] < min_q: continue
        for box, person in truth:
            if person and iou(box, f['box']) >= 0.3:
                faces.append(dict(emb=np.array(f['emb'], np.float32), person=canon.get(person, person), baby=person in small, image=asset, day=taken[asset][:10]))
                break
E = np.stack([f['emb'] for f in faces]); S = E @ E.T
person = np.array([f['person'] for f in faces]); day = np.array([f['day'] for f in faces])
baby = np.array([f['baby'] for f in faces]); image = np.array([f['image'] for f in faces])
n = len(faces)
other_day = day[:, None] != day[None, :]
people = sorted(set(person))
index = {p: i for i, p in enumerate(people)}
pid = np.array([index[p] for p in person])
member = np.zeros((len(people), n), bool); member[pid, np.arange(n)] = True
recognisable = np.array([bool((member[pid[i]] & other_day[i]).any()) for i in range(n)])
print('%d faces (quality >= %.2f), %d people after merging; recognisable from another day: %d; of babies/toddlers: %d' % (n, min_q, len(people), recognisable.sum(), (recognisable & baby).sum()))
same = person[:, None] == person[None, :]
upper = np.triu(np.ones((n, n), bool), 1)
certain = S[upper & ~same & (image[:, None] == image[None, :])]
print('same-picture pairs of different people: %d; at or above 0.45: %d, 0.50: %d, 0.55: %d, 0.60: %d, 0.65: %d' % ((len(certain),) + tuple(int((certain >= t).sum()) for t in (.45, .5, .55, .6, .65))))

# per face and person: best similarity, mean of the best three, centroid similarity (faces of other days only)
best = np.full((n, len(people)), -1.0); top3 = np.full((n, len(people)), -1.0); cent = np.full((n, len(people)), -1.0)
for i in range(n):
    for k in range(len(people)):
        gallery = member[k] & other_day[i]
        if not gallery.any(): continue
        s = np.sort(S[i, gallery])[::-1]
        best[i, k] = s[0]
        top3[i, k] = s[:3].mean() if len(s) >= 3 else -1
        c = E[gallery].mean(axis=0); c /= np.linalg.norm(c) + 1e-9
        cent[i, k] = E[i] @ c if gallery.sum() >= 3 else -1


def report(title, score, thresholds):
    print(title)
    choice = score.argmax(axis=1); value = score.max(axis=1)
    for t in thresholds:
        assigned = value >= t
        right = assigned & (choice == pid)
        wrong = assigned & (choice != pid)
        print('   %.2f: assigned %4d = %5.1f%% of recognisable, right %.2f%%, wrong %3d (babies/toddlers %d, others %d); without babies: right %.2f%%' % (
            t, assigned.sum(), 100 * (assigned & recognisable).sum() / recognisable.sum(), 100 * right.sum() / max(assigned.sum(), 1), wrong.sum(),
            (wrong & baby).sum(), (wrong & ~baby).sum(), 100 * (right & ~baby).sum() / max((assigned & ~baby).sum(), 1)))


report('nearest face of a person', best, (0.40, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70))
report('mean of the three nearest faces of a person', top3, (0.40, 0.45, 0.50, 0.55, 0.60, 0.65))
report('centre of a person (3 faces or more)', cent, (0.40, 0.45, 0.50, 0.55, 0.60, 0.65))
# the margin to the second person
second = np.sort(best, axis=1)[:, -2]
choice = best.argmax(axis=1); value = best.max(axis=1)
for t, m in ((0.55, 0.05), (0.55, 0.10), (0.60, 0.05), (0.60, 0.10), (0.65, 0.05)):
    assigned = (value >= t) & (value - second >= m)
    right = assigned & (choice == pid); wrong = assigned & (choice != pid)
    print('nearest >= %.2f and %.2f above the next person: assigned %4d = %5.1f%%, right %.2f%%, wrong %d (babies %d)' % (
        t, m, assigned.sum(), 100 * (assigned & recognisable).sum() / recognisable.sum(), 100 * right.sum() / max(assigned.sum(), 1), wrong.sum(), (wrong & baby).sum()))

# The rule of ClassifyFaceMatch(): certain at 0.70, or 0.60 and 0.10 clear of the next person.
same_rule = (value >= 0.70) | ((value >= 0.60) & (value - np.maximum(second, 0) >= 0.10))
maybe_rule = (value >= 0.45) & ~same_rule
right = same_rule & (choice == pid); wrong = same_rule & (choice != pid)
print('ClassifyFaceMatch same: assigned %d = %.1f%% of recognisable, right %.2f%%, wrong %d (babies %d)' % (
    same_rule.sum(), 100 * (same_rule & recognisable).sum() / recognisable.sum(), 100 * right.sum() / max(same_rule.sum(), 1), wrong.sum(), (wrong & baby).sum()))
right = maybe_rule & (choice == pid)
print('ClassifyFaceMatch maybe: %d more faces asked about = %.1f%% of recognisable, the suggestion right for %.1f%%' % (
    maybe_rule.sum(), 100 * (maybe_rule & recognisable).sum() / recognisable.sum(), 100 * right.sum() / max(maybe_rule.sum(), 1)))
nobody = ~same_rule & ~maybe_rule
print('no match: %d faces, of which recognisable %d' % (nobody.sum(), (nobody & recognisable).sum()))

# Grouping faces nobody has named: every pair at or above a threshold is a link.
for t in (0.50, 0.55, 0.60, 0.65, 0.70):
    parent = list(range(n))
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]; x = parent[x]
        return x
    for i, j in np.argwhere(np.triu(S >= t, 1)):
        parent[find(i)] = find(j)
    groups = {}
    for i in range(n): groups.setdefault(find(i), []).append(i)
    multi = [g for g in groups.values() if len(g) >= 2]
    pure = 0; mixed = 0; mixed_adults = 0
    for g in multi:
        counts = {}
        for i in g: counts[pid[i]] = counts.get(pid[i], 0) + 1
        pure += max(counts.values())
        if len(counts) > 1:
            mixed += 1
            if sum(1 for k, c in counts.items() if not baby[[i for i in g if pid[i] == k][0]]) > 1: mixed_adults += 1
    grouped = sum(len(g) for g in multi)
    big = sorted(multi, key=len, reverse=True)[:5]
    # how the two largest people are split
    top = [k for k, _ in sorted(((k, (pid == k).sum()) for k in range(len(people))), key=lambda t: -t[1])[:6]]
    pieces = [sum(1 for g in multi if sum(1 for i in g if pid[i] == k) >= 2) for k in top]
    print('links >= %.2f: %d groups of 2 or more with %d faces (%.0f%%), %d single; faces in the right group %.2f%%; groups with more than one person %d (not counting babies: %d); the six largest people are in %s groups' % (
        t, len(multi), grouped, 100 * grouped / n, n - grouped, 100 * pure / max(grouped, 1), mixed, mixed_adults, pieces))
