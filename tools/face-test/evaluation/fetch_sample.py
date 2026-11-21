#!/usr/bin/env python3
"""fetch_sample.py: pulls a sample from an Immich server to measure the face
engine against: for the people with the most pictures, previews, the faces
Immich found in them (boxes and its person clusters) and when they were
taken. Reads only (GETs and metadata searches). IMMICH_URL, IMMICH_TOKEN
(a session token or API key's bearer) and FACES_WORK (where the sample
goes: sample/img, sample/faces, sample/taken.json, sample/ppm) are taken
from the environment. The pictures are somebody's: keep them out of the
repository. Needs Pillow."""
import json, os, sys, urllib.request

BASE = os.environ['IMMICH_URL'].rstrip('/') + '/api'
TOKEN = os.environ['IMMICH_TOKEN']
OUT = os.environ.get('FACES_WORK', '.').rstrip('/') + '/' + 'sample'
os.makedirs(OUT + '/img', exist_ok=True)
os.makedirs(OUT + '/faces', exist_ok=True)


def call(method, path, body=None, raw=False):
    request = urllib.request.Request(BASE + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={'Authorization': 'Bearer ' + TOKEN, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=120) as response:
        data = response.read()
    return data if raw else json.loads(data)


people = []
page = 1
while len(people) < 150:
    result = call('GET', '/people?size=50&page=%d&withHidden=false' % page)
    people += result['people']
    if not result.get('hasNextPage'):
        break
    page += 1
for person in people:
    person['count'] = call('GET', '/people/%s/statistics' % person['id'])['assets']
people.sort(key=lambda p: -p['count'])
print('people', len(people), [p['count'] for p in people[:30]])
json.dump([{'id': p['id'], 'count': p['count']} for p in people],
    open(OUT + '/people.json', 'w'))

assets = {}
for rank, person in enumerate(people[:120]):
    want = 36 if rank < 16 else 3
    if person['count'] < 2:
        continue
    ids = []
    page = 1
    while page is not None and len(ids) < 1000:
        result = call('POST', '/search/metadata', {'personIds': [person['id']],
            'type': 'IMAGE', 'size': 250, 'page': page})['assets']
        ids += [item['id'] for item in result['items']]
        page = int(result['nextPage']) if result.get('nextPage') else None
    step = max(len(ids) / want, 1)
    picked = [ids[int(i * step)] for i in range(min(want, len(ids)))]
    for asset in picked:
        assets[asset] = True
    print('person', rank, person['count'], 'assets', len(ids), 'picked', len(picked), flush=True)

print('assets', len(assets), flush=True)
done = 0
for asset in assets:
    image = '%s/img/%s.jpg' % (OUT, asset)
    faces = '%s/faces/%s.json' % (OUT, asset)
    try:
        if not os.path.exists(faces):
            json.dump(call('GET', '/faces?id=' + asset), open(faces, 'w'))
        if not os.path.exists(image):
            data = call('GET', '/assets/%s/thumbnail?size=preview' % asset, raw=True)
            open(image + '.part', 'wb').write(data)
            os.rename(image + '.part', image)
    except Exception as error:
        print('failed', asset[:8], error, flush=True)
    done += 1
    if done % 100 == 0:
        print(done, flush=True)
print('done', done)

taken = {}
for asset in assets:
    try:
        d = call('GET', '/assets/' + asset)
        taken[asset] = d.get('localDateTime') or d.get('fileCreatedAt')
    except Exception as error:
        print('failed', asset[:8], error, flush=True)
json.dump(taken, open(OUT + '/taken.json', 'w'))

# facescan_host reads PPM
from PIL import Image
os.makedirs(OUT + '/ppm', exist_ok=True)
for asset in assets:
    image = '%s/img/%s.jpg' % (OUT, asset)
    ppm = '%s/ppm/%s.ppm' % (OUT, asset)
    if os.path.exists(image) and not os.path.exists(ppm):
        Image.open(image).convert('RGB').save(ppm)
