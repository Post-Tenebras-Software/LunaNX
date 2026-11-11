#!/usr/bin/env python3
"""Prepare store archives/metadata locally. No submission or network calls."""
import base64
import hashlib
import html
import json
import math
from pathlib import Path
import shutil
import struct
import zipfile
from check import ROOT, VERSION, digest, require, validate

REPO = 'https://github.com/Post-Tenebras-Software/LunaNX'


def png_size(path):
    data=path.read_bytes()
    require(data[:8]==b'\x89PNG\r\n\x1a\n', 'Invalid PNG')
    return struct.unpack('>II',data[16:24])


def prepare():
    validate()
    release=ROOT/'releases'/('v'+VERSION)
    target=ROOT/'dist/store'
    require(release.is_dir(), 'Package the audited release first')
    require(not target.exists(), 'Store output exists; refusing to overwrite')
    spec=json.loads((ROOT/'distribution/hb-appstore/submission.json').read_text())
    require(spec['version']==VERSION and spec['url']==REPO, 'Submission/release identity mismatch')
    for field,minimum,maximum in [('title',3,24),('author',3,24),('contact',8,30),('short',10,40)]:
        require(minimum<=len(spec[field])<=maximum, 'Submission field length: '+field)
    require(spec['ai']=='mostly' and set(spec['aiparts'])=={'art','text'}, 'Review AI disclosure')
    ai_note='\n\nAI use: Mostly AI-generated. AI also used for: icon/banner art, description text.'
    details=spec['long']+ai_note
    require(len(details)<=1024 and len(spec['changelog'])>=20,'Description/changelog length')
    require(png_size(ROOT/spec['icon'])==(256,150),'Store icon must be 256x150')
    require(png_size(ROOT/spec['banner'])==(848,208),'Store banner must be 848x208')
    target.mkdir(parents=True)
    with zipfile.ZipFile(release/f'LunaNX-v{VERSION}-switch.zip') as archive:
        files={n:archive.read(n) for n in archive.namelist()}
    require(files['switch/LunaNX/LunaNX.nro']==(release/'LunaNX.nro').read_bytes(),'Install NRO mismatch')
    require(all(not Path(n).is_absolute() and '..' not in Path(n).parts and '\\' not in n for n in files),'Unsafe archive path')
    require(not any(any(s in n for s in ['login.bin','recent-games.json','shortcut.cfg','video.cfg']) for n in files),
            'A store update must never replace saved account/settings/history files')
    manifest=''.join('E: '+n+'\n' for n in sorted(files))
    info={'name':'LunaNX','title':spec['title'],'author':spec['author'],'category':spec['category'],
          'version':VERSION,'license':spec['license'],'url':REPO,'description':spec['short'],
          'details':details,'changelog':spec['changelog'],'updated':'10/11/2026',
          'binary':'/switch/LunaNX/LunaNX.nro','screens':1,
          'extracted':math.ceil(sum(map(len,files.values()))/1024)}
    repo=target/'repo';package=repo/'packages/LunaNX';package.mkdir(parents=True)
    (repo/'zips').mkdir()
    (package/'info.json').write_text(json.dumps(info,indent=2)+'\n')
    (package/'manifest.install').write_text(manifest)
    shutil.copy2(ROOT/spec['icon'],package/'icon.png')
    shutil.copy2(ROOT/spec['banner'],package/'screen.png')
    store_zip=release/f'LunaNX-v{VERSION}-hb-appstore.zip'
    require(not store_zip.exists(),'Refusing to overwrite store ZIP')
    with zipfile.ZipFile(store_zip,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for name,data in files.items():archive.writestr(name,data)
        archive.writestr('manifest.install',manifest)
        archive.writestr('info.json',json.dumps(info,indent=2)+'\n')
    with zipfile.ZipFile(store_zip) as archive:
        require(archive.testzip() is None,'Corrupt store ZIP')
        require(set(archive.namelist())==set(files)|{'manifest.install','info.json'},'Unexpected store files')
        require(archive.read('manifest.install').decode()==manifest,'Store manifest mismatch')
    shutil.copy2(store_zip,repo/'zips/LunaNX.zip')
    entry={**info,'filesize':math.ceil(store_zip.stat().st_size/1024),
           'md5':hashlib.md5(store_zip.read_bytes()).hexdigest(),'sha256':digest(store_zip)}
    (repo/'repo.json').write_text(json.dumps({'packages':[entry]},indent=2)+'\n')
    # The prepared form payload is reviewable and uses the exact observed public form shape.
    payload={'package':'LunaNX','console':'switch','submitter':spec['contact'],'type':'new',
             'info':{k:html.escape(v) if k=='details' else v for k,v in info.items()
                     if k in ['title','author','category','version','url','license','description','details']},
             'changes':html.escape(spec['changelog']),
             'assets':[{'data':'data:image/png;base64,'+base64.b64encode((ROOT/spec[key]).read_bytes()).decode(),
                        'format':'base64','type':kind} for key,kind in [('icon','icon'),('banner','screen')]]}
    (target/'submission-payload.json').write_text(json.dumps(payload,indent=2)+'\n')
    shutil.copy2(ROOT/'distribution/hb-appstore/submission.json',target/'submission-fields.json')
    shutil.copytree(ROOT/'distribution/sphaira',target/'sphaira')
    with zipfile.ZipFile(target/'custom-store.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for p in sorted(repo.rglob('*')):
            if p.is_file():archive.write(p,p.relative_to(repo))
    # All added release assets participate in the same checksum inventory.
    (release/'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.relative_to(release).as_posix()}\n'
        for p in sorted(release.iterdir()) if p.is_file() and p.name!='SHA256SUMS'))
    print('Prepared HB App Store ZIP, submission fields/payload, Sphaira catalog entry and static custom store.')
    print('Nothing submitted, uploaded or approved.')


if __name__=='__main__':prepare()
