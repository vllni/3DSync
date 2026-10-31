// docs/static/js/inifile.js under Node: the configurator's config object to
// 3DSync.ini and back.  Run with `node --test tests/configurator/*.test.js`.
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const IniFile = require('../../docs/static/js/inifile.js');

function remote(type, extra) {
    return Object.assign({ uid: 1, type: type, label: '', role: 'sync', fields: {}, auth: null,
        entries: [], carry: [], excluded: {} }, extra);
}

test('a config is written in the order the 3DS expects', () => {
    let config = {
        remotes: [
            remote('smb', { entries: [['Server', 'nas'], ['Share', 'saves'], ['Password', 'pw']] }),
            remote('smb', { uid: 2, label: 'backup', role: 'backup', entries: [['Server', 'nas2'], ['Share', 'b']],
                excluded: { shots: true } })
        ],
        folders: [
            { name: 'saves', path: '/roms/saves', recursive: true, direction: 'both', filter: '' },
            { name: 'shots', path: '/luma/screenshots', recursive: false, direction: 'upload', filter: '*' }
        ],
        filter: 'sav',
        extras: [{ name: 'Security', pairs: [['SealCredentials', 'false']] }]
    };
    assert.equal(IniFile.toIni(config, false), [
        '[SMB]', 'Server=nas', 'Share=saves', 'Password=pw', '',
        '[SMB:backup]', 'Server=nas2', 'Share=b', 'Role=backup', 'Only=saves', '',
        '[Paths]', 'saves=/roms/saves', '',
        '[UploadShallowPaths]', 'shots=/luma/screenshots', '',
        '[Filter]', 'Extensions=sav', 'shots=*', '',
        '[Security]', 'SealCredentials=false', ''
    ].join('\n'));
    assert.match(IniFile.toIni(config, true), /^Password=•+$/m);
});

test('a single remote gets no Only=, whatever it excludes', () => {
    let config = { remotes: [remote('ftp', { entries: [['Host', 'h']], excluded: { saves: true } })],
        folders: [{ name: 'saves', path: '/s', recursive: true, direction: 'both', filter: '' }], filter: '', extras: [] };
    assert.doesNotMatch(IniFile.toIni(config, false), /Only=/);
});

test('the file is read the way inih reads it', () => {
    let parsed = IniFile.parseIni('﻿; comment\n# also\n[SMB]\nServer : nas ; inline\n'
        + 'Password=a;b\n  continued\nnot a pair\n[smb]\nShare=s\n');
    assert.deepEqual(parsed.sections, [{ name: 'SMB', pairs: [['Server', 'nas'], ['Password', 'a;b\ncontinued'],
        ['Share', 's']] }]);
    assert.deepEqual(parsed.badLines, [7]);
});

test('sealed values are never shown, and are written back unchanged', () => {
    let text = '[Dropbox]\nAppKey=y88zxdudhuoo41q\nRefreshToken=enc:v1:DBX\n\n'
        + '[SMB:Backup]\nServer=nas\nShare=b\nPassword=enc:v1:SMB\nRole=backup\nOnly=saves\n\n'
        + '[Paths]\nsaves=/roms/saves\nCheckpoint=/3ds/Checkpoint\n';
    let { config, report } = IniFile.fromIni(text);
    assert.equal(report.sealed, 2);
    let [dropbox, smb] = config.remotes;
    assert.equal(dropbox.auth.refreshToken, '');
    assert.equal(smb.fields['smb-password'], '');
    assert.ok(!Object.values(smb.fields).some(IniFile.isSealed));
    assert.deepEqual(smb.excluded, { checkpoint: true });
    assert.equal(IniFile.toIni(config, false), text);
});

test('the editor keeps a carried value unless something replaces it', () => {
    let smb = remote('smb', { carry: [['Password', 'enc:v1:SMB'], ['Encrypt', 'yes']] });
    assert.deepEqual(IniFile.withCarried([['Server', 'nas'], ['Password', '']], smb),
        [['Server', 'nas'], ['Password', 'enc:v1:SMB'], ['Encrypt', 'yes']]);
    assert.deepEqual(IniFile.withCarried([['Server', 'nas'], ['Password', 'new']], smb),
        [['Server', 'nas'], ['Password', 'new'], ['Encrypt', 'yes']]);
});

test('plain secrets, selects and unknown keys', () => {
    let { config } = IniFile.fromIni('[FTP]\nHost=h\nPassword=pw\nTLS=Require\nMode=weird\nEncrypt=yes\n');
    let ftp = config.remotes[0];
    assert.equal(ftp.fields['ftp-password'], 'pw');
    assert.equal(ftp.fields['ftp-tls'], 'require');
    assert.equal(ftp.fields['ftp-mode'], undefined);
    assert.deepEqual(ftp.carry, [['Mode', 'weird'], ['Encrypt', 'yes']]);
});

test('every problem is reported, and the rest still imports', () => {
    let { config, report } = IniFile.fromIni('Debug=1\n[Dropbox]\nToken=old\n[SMB:my share]\nServer=n\nShare=s\n'
        + 'Role=mirror\nOnly=saves, nothere\n[Paths]\nsaves=/s\n[Filter]\ntypo=sav\n');
    assert.equal(report.remotes, 2);
    assert.equal(report.problems.length, 5);
    assert.deepEqual(report.kept, ['the lines before the first section']);
    assert.match(IniFile.toIni(config, false), /^Debug=1\n\n\[Dropbox\]\nToken=old\n/);
});

test('a file with neither remotes nor folders is not a config', () => {
    let { config, report } = IniFile.fromIni('[Something]\na=b\n');
    assert.equal(config, null);
    assert.match(report.problems[0], /No remotes or folders/);
});
