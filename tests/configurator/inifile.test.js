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

test('VerifyTLS=false picks "don\'t check", and is written back', () => {
    let { config } = IniFile.fromIni('[WebDAV]\nUrl=https://nas/dav/\nVerifyTLS=False\n[FTP]\nHost=h\nVerifyTLS=maybe\n');
    let [webdav, ftp] = config.remotes;
    assert.equal(webdav.fields['webdav-cert'], 'off');
    assert.deepEqual(webdav.carry, []);
    // A value the select cannot show is kept as it was.
    assert.equal(ftp.fields['ftp-cert'], 'public');
    assert.deepEqual(ftp.carry, [['VerifyTLS', 'maybe']]);
    assert.match(IniFile.toIni(config, false), /\[WebDAV\]\nUrl=https:\/\/nas\/dav\/\nVerifyTLS=False\n/);
});

// --- Server certificates: PinnedKey= and CACert= ---

// A self-signed EC certificate (CN=nas.local), as tests/test_certtrust.cpp.
const CERT_B64 =
    'MIIBfTCCASOgAwIBAgIUDuaYMN3Z2l7FIDsXuwma49fNmYQwCgYIKoZIzj0EAwIwFDESMBAGA1UEAwwJ' +
    'bmFzLmxvY2FsMB4XDTI2MTEwOTE5MDU0MVoXDTM2MTEwNjE5MDU0MVowFDESMBAGA1UEAwwJbmFzLmxv' +
    'Y2FsMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEgwVJmXz+c1oXyG3XyLcTJO6Ki3VU/sarBVVNTPYH' +
    'HKjIMN7SzEpNlCorf64VhMBdMdlrJG4JxnrvbMrnHXZDxqNTMFEwHQYDVR0OBBYEFDW072mtvcv9p4us' +
    'UqQP8JARYdUCMB8GA1UdIwQYMBaAFDW072mtvcv9p4usUqQP8JARYdUCMA8GA1UdEwEB/wQFMAMBAf8w' +
    'CgYIKoZIzj0EAwIDSAAwRQIgIakhQamB3hx+eo+t1lnGs4vrb9A1vLp5+mc79bB0NzgCIQC9FWsyCKzk' +
    'RqRKz5DUnw8bDGij2A41yvkh2MG66K7Ghg==';
const KEY_HEX = 'b444c9f346c9c75a699584fe4ecc694e121c795b05621aaa0984f57c5eaafe07';

function bytesOf(text) {
    return Uint8Array.from(Buffer.from(text, 'binary'));
}

test('a pinned key is accepted in every form the 3DS reads', () => {
    let colons = KEY_HEX.toUpperCase().match(/../g).join(':');
    [KEY_HEX, colons, colons.replace(/:/g, ' '), 'sha256//tETJ80bJx1pplYT+TsxpThIceVsFYhqqCYT1fF6q/gc=',
        KEY_HEX + ', ' + colons].forEach(function (value) {
        assert.equal(IniFile.pinnedKeyProblem(value), null, value);
    });
    [KEY_HEX.slice(1), KEY_HEX + '0', 'sha256//tETJ80bJx1pplYT+Tsxp', '', ',', KEY_HEX + ',abc',
        'da39a3ee5e6b4b0d3255bfef95601890afd80709'].forEach(function (value) {
        assert.ok(IniFile.pinnedKeyProblem(value), value);
    });
});

test('a CA file becomes one line of base64 DER, from PEM or DER', () => {
    let pem = '-----BEGIN CERTIFICATE-----\n' + CERT_B64.match(/.{1,64}/g).join('\n') + '\n-----END CERTIFICATE-----\n';
    assert.deepEqual(IniFile.caCertFromFile(bytesOf(pem)), { value: CERT_B64, count: 1 });
    // A chain, with openssl's text around it.
    let two = IniFile.caCertFromFile(bytesOf('subject=CN=nas.local\n' + pem + pem));
    assert.deepEqual(two, { value: CERT_B64 + ',' + CERT_B64, count: 2 });
    let der = Uint8Array.from(Buffer.from(CERT_B64, 'base64'));
    assert.deepEqual(IniFile.caCertFromFile(der), { value: CERT_B64, count: 1 });
});

test('a CA file that is no certificate is refused with a reason', () => {
    assert.match(IniFile.caCertFromFile(bytesOf('-----BEGIN PRIVATE KEY-----\nMIGH\n-----END PRIVATE KEY-----\n')).error,
        /no certificate/);
    assert.match(IniFile.caCertFromFile(bytesOf('hello')).error, /Not a certificate/);
    // Cut short: still base64, but the DER length says otherwise.
    assert.ok(IniFile.caCertProblem(CERT_B64.slice(0, 400)));
    assert.equal(IniFile.caCertProblem(CERT_B64), null);
});

test('PinnedKey= and CACert= pick their entry of the select, and round-trip', () => {
    let text = '[WebDAV]\nUrl=https://nas/dav/\nPinnedKey=' + KEY_HEX + '\n\n[FTP]\nHost=h\nCACert=' + CERT_B64 + '\n';
    let { config, report } = IniFile.fromIni(text);
    let [webdav, ftp] = config.remotes;
    assert.equal(webdav.fields['webdav-cert'], 'key');
    assert.equal(webdav.fields['webdav-pinned-key'], KEY_HEX);
    assert.equal(ftp.fields['ftp-cert'], 'ca');
    assert.equal(ftp.fields['ftp-ca-cert'], CERT_B64);
    assert.deepEqual(report.problems, []);
    assert.equal(IniFile.toIni(config, false), text);
    // The preview cuts the certificate short; the download keeps it whole.
    assert.match(IniFile.toIni(config, true), /\nCACert=MIIBfTCCASOgAwIBAgIUDuaYMN3Z2l7F… \(516 characters\)\n/);
});

test('an import says what the 3DS makes of contradicting or broken certificate lines', () => {
    let { config, report } = IniFile.fromIni('[WebDAV]\nUrl=https://nas/dav/\nPinnedKey=' + KEY_HEX
        + '\nCACert=' + CERT_B64 + '\nVerifyTLS=false\n[FTP]\nHost=h\nPinnedKey=abc\n');
    assert.equal(config.remotes[0].fields['webdav-cert'], 'key');
    assert.equal(config.remotes[1].fields['ftp-cert'], 'key');
    assert.equal(report.problems.length, 3);
    assert.match(report.problems[0], /only PinnedKey=/);
    assert.match(report.problems[1], /VerifyTLS=false with PinnedKey=/);
    assert.match(report.problems[2], /FTP: PinnedKey= is not a SHA-256 hash/);
});

// --- Google Drive with 3DSync's own client, and the folders chosen for it ---

function folder(name, extra) {
    return Object.assign({ name: name, path: '/roms/' + name, recursive: true, direction: 'both', filter: '' }, extra);
}

test('a Drive remote of 3DSync\'s client writes its token and a Folder line per chosen folder', () => {
    let drive = remote('googledrive', { entries: [['RefreshToken', 'rt']],
        driveFolders: { gba: { id: 'F1', title: 'saves' }, gone: { id: 'F9', title: 'old' } } });
    let config = { remotes: [drive], folders: [folder('GBA'), folder('nds')], filter: '', extras: [] };
    let ini = IniFile.toIni(config, false);
    assert.match(ini, /\[GoogleDrive\]\nRefreshToken=rt\nFolder\.GBA=F1\n/);
    // No folder chosen for nds: the 3DS makes its own; nothing for a folder that is gone.
    assert.doesNotMatch(ini, /Folder\.nds|F9/);
    assert.doesNotMatch(ini, /ClientSecret/);
});

test('a folder left out of a Drive remote gets no Folder line there', () => {
    let drive = remote('googledrive', { entries: [['RefreshToken', 'rt']], excluded: { gba: true },
        driveFolders: { gba: { id: 'F1', title: 'saves' } } });
    let smb = remote('smb', { uid: 2, entries: [['Server', 'nas'], ['Share', 's']] });
    let config = { remotes: [drive, smb], folders: [folder('gba'), folder('nds')], filter: '', extras: [] };
    let ini = IniFile.toIni(config, false);
    assert.doesNotMatch(ini, /Folder\.gba/);
    assert.match(ini, /\[GoogleDrive\]\nRefreshToken=rt\nOnly=nds\n/);
});

test('an imported Drive section without a client is 3DSync\'s, and its folders come back', () => {
    let r = IniFile.fromIni('[GoogleDrive]\nRefreshToken=rt\nFolder.gba=F1\nFolder.snes=F2\n\n[Paths]\ngba=/roms/gba\n');
    let drive = r.config.remotes[0];
    assert.equal(drive.fields['gdrive-expert-mode'], false);
    assert.deepEqual(drive.auth, { shared: true, clientId: '', clientSecret: '', refreshToken: 'rt' });
    assert.deepEqual(drive.driveFolders, { gba: { id: 'F1', title: '' } });
    // Folder lines are not kept as entries: they are written from driveFolders.
    assert.deepEqual(drive.entries, [['RefreshToken', 'rt']]);
    assert.ok(r.report.problems.some(function (p) { return /Folder\.snes/.test(p); }));
    // And written out the same way again.
    assert.match(IniFile.toIni(r.config, false), /\[GoogleDrive\]\nRefreshToken=rt\nFolder\.gba=F1\n/);
});

test('an imported Drive section with a client of its own stays in expert mode, sealed or not', () => {
    let own = IniFile.fromIni('[GoogleDrive]\nClientId=id\nClientSecret=sec\nRefreshToken=rt\n\n[Paths]\na=/a\n');
    assert.equal(own.config.remotes[0].fields['gdrive-expert-mode'], true);
    assert.deepEqual(own.config.remotes[0].auth, { clientId: 'id', clientSecret: 'sec', refreshToken: 'rt' });
    let sealed = IniFile.fromIni('[GoogleDrive]\nClientId=id\nClientSecret=enc:v1:AAAA\nRefreshToken=enc:v1:BBBB\n\n[Paths]\na=/a\n');
    assert.equal(sealed.config.remotes[0].fields['gdrive-expert-mode'], true);
    assert.equal(sealed.config.remotes[0].auth.refreshToken, '');
});

test('[Sync] is written only when it says something', () => {
    let base = { remotes: [remote('ftp', { entries: [['Host', 'h']] })],
        folders: [{ name: 'saves', path: '/s', recursive: true, direction: 'both', filter: '' }], filter: '', extras: [] };
    assert.doesNotMatch(IniFile.toIni(base, false), /\[Sync\]/);
    base.sync = { compare: true, compareUpTo: '', other: [] };
    assert.doesNotMatch(IniFile.toIni(base, false), /\[Sync\]/);
    base.sync = { compare: false, compareUpTo: '512', other: [['Later', 'x']] };
    assert.match(IniFile.toIni(base, false), /\n\[Sync\]\nCompareConflicts=false\nCompareUpTo=512\nLater=x\n$/);
});

test('[Sync] is read back into the form', () => {
    let { config, report } = IniFile.fromIni('[FTP]\nHost=h\n[Paths]\nsaves=/s\n'
        + '[Sync]\nCompareConflicts=Off\nCompareUpTo=0\nLater=x\n');
    assert.deepEqual(config.sync, { compare: false, compareUpTo: '0', other: [['Later', 'x']] });
    assert.deepEqual(report.kept, []);
    assert.match(IniFile.toIni(config, false), /\[Sync\]\nCompareConflicts=false\nCompareUpTo=0\nLater=x\n$/);

    let odd = IniFile.fromIni('[FTP]\nHost=h\n[Paths]\nsaves=/s\n[Sync]\nCompareConflicts=maybe\n');
    assert.equal(odd.config.sync.compare, true);
    assert.match(odd.report.problems.join(' '), /CompareConflicts=maybe/);

    let none = IniFile.fromIni('[FTP]\nHost=h\n[Paths]\nsaves=/s\n');
    assert.deepEqual(none.config.sync, { compare: true, compareUpTo: '', other: [] });
});
