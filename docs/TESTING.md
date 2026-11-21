# Testing Photos

## On the build host

```
make check-host        # 54 tests: core (library, people, edits, EXIF, images), faces, PTP
tools/test-sync.sh     # 9 tests: the sync engine against tools/fake-immich.py
tools/raw-host.sh test --release   # the RAW library on CC0 samples (downloads them)
```

All run under AddressSanitizer and UBSan.

## In a VM

`tools/vm/vm-up.sh` boots the current air/OS x86_64 image (prepared once with
an SSH key, see `tools/vm/env.sh`) with a data disk at `/Data`;
`tools/vm/deploy.sh` copies a cross build to `/Data/photos`.

```
tools/make-fixtures.py /somewhere/fixtures      # generated pictures, a camera card layout
tools/vm/scp.sh /somewhere/fixtures vm:/Data/
tools/vm/ssh.sh 'cd /Data/photos && PHOTOS_LIBRARY=/Data/lib PHOTOS_SETTINGS_DIR=/Data/settings ./photos_tool import /Data/fixtures'
tools/vm/app.sh                                  # starts Photos on that library, never uploading
tools/vm/hey.sh state                            # what the window shows
tools/vm/hey.sh group mode=0                     # years
tools/vm/hey.sh openphoto id=25
tools/vm/hey.sh editphoto; tools/vm/hey.sh editvalues 'json={"exposure":0.5}'; tools/vm/hey.sh editdone
python3 tools/vm/qmp.py shot /tmp/shot.png
```

A phone: `python3 tools/vm/qmp.py hmp "device_add usb-mtp,bus=xhci.0,rootdir=DIR,readonly=on,id=phone"`
(and `device_del phone`); `tools/ptp-test/vm-test.sh` runs the camera layer's
own tests with `ptpls`.

## Rules

- Tests that sign in to a **real Immich server** run with `PHOTOS_NO_UPLOAD=1`
  (`tools/vm/app.sh` sets it). Upload and delete are tested against
  `tools/fake-immich.py` only.
- No personal pictures in the repository: fixtures are generated.
