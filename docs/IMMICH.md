# Photos and Immich

Photos keeps a library in sync with one account on one Immich server (tested
with server 3.4). Sign in under *Settings › Immich* with the server's address,
your e-mail address and password. The password is used once: what is kept
(in `~/config/settings/Photos/settings.json`, readable by you only) is the
session the server hands out. An API key works too (`photos_tool apikey`).

A pass runs when Photos starts, every 15 minutes (*Settings*), after an import
and when you click *Immich* at the bottom of the sidebar.

## From the server to Photos

Photos asks the server's sync stream what changed since the last pass, so a
pass that finds nothing new takes a fraction of a second, and the first one
lists everything (23,000 photos: about a minute).

| On the server | In Photos |
| --- | --- |
| a new photo | appears in the library with its thumbnail. The **original stays on the server** until you open the photo at full size, edit it, export it or hand it to PhotoCraft; *Settings › Keep the originals of all Immich photos on this computer* fetches them all |
| the same file is in the library already (same checksum) | it is the same photo: nothing is copied either way |
| favourite changed | follows |
| moved to the trash | moves to *Recently Deleted* here (and back, if restored there) |
| deleted for good | a photo whose original is only on the server disappears; one whose original is here moves to *Recently Deleted*, where you can still keep it |
| videos, hidden and locked items | not listed |

## From Photos to the server

| In Photos | On the server |
| --- | --- |
| a photo that is not on the server (imported from a camera, a folder…) | uploaded: the original file, with its capture date and favourite state. The server is asked first, by checksum, whether it has the file |
| favourite changed | follows |
| moved to *Recently Deleted* | nothing yet |
| deleted for good | **nothing**, unless *Photos deleted here for good also go to the trash on Immich* is on: then the server's copy moves to its trash (never deleted outright). Either way the photo does not come back to this library |
| edits and PhotoCraft versions | stay here: the server keeps the original |

Uploading can be turned off (*Send photos that are not on the server to
Immich*); the library then only receives.

## Another server or account

A library belongs to one account. Signing in to another server or as someone
else forgets what the library knew about the old one: photos whose originals
are here stay (and are uploaded to the new account), photos that were only on
the old server go.

## For tests

`tools/fake-immich.py` is a stand-in server (see its header);
`tools/test-sync.sh` runs the engine against it on the build host. Programs run
with `PHOTOS_NO_UPLOAD=1` never upload, whatever the settings say: use it for
any test that signs in to a real server.
