// 3DSync.ini <-> the configurator's config object, with no page in sight:
// index.js builds the object step by step and hands it to toIni() at the end,
// and an imported file goes through fromIni() into the same object.  Kept
// free of jQuery and the DOM so tests/configurator runs it under Node.
//
//   config = {
//     remotes: [{ uid, type, label, role, fields, auth, entries, carry,
//                 sealedSection, excluded, driveFolders }],
//     folders: [{ name, path, recursive, direction, filter }],
//     filter: '',      // [Filter] Extensions=
//     sync: { compare, compareUpTo, other },  // [Sync], see below
//     extras: []       // sections the page has no form for, as read
//   }
//
// remote.entries are the section's lines as the 3DS reads them, without
// Role= and Only=, which follow from role and excluded (lowercased folder
// names it does not sync).  fields are the editor's inputs by id, auth the
// OAuth login.  carry holds what an imported file had that the form does not
// produce: sealed values, which are never shown, and keys it has no field
// for.  A folder's filter of '' means it takes config.filter.
//
// A Google Drive remote signs in either with 3DSync's own client (no
// ClientSecret= in the file; the 3DS refreshes through the site's Worker, and
// the scope is drive.file) or, in expert mode, with the user's own client.
// fields['gdrive-expert-mode'] says which.  driveFolders maps a folder name
// (lowercased) to the Drive folder chosen for it, { id, title }, written as
// Folder.<name>= in the remote's section.
//
// sync is [Sync]: compare is CompareConflicts= (false: a conflict on SMB, FTP
// or WebDAV is always asked about, without downloading the remote copy to
// see whether it is the same file), compareUpTo is CompareUpTo= as typed (KB;
// '' for the 3DS's default of 4096, '0' for no limit), and other holds the
// section's other lines, written back as they were.  The section is only
// written when it says something.
(function (root) {
    'use strict';

    const TYPES = {
        dropbox: { section: 'Dropbox', name: 'Dropbox' },
        googledrive: { section: 'GoogleDrive', name: 'Drive' },
        smb: { section: 'SMB', name: 'SMB' },
        ftp: { section: 'FTP', name: 'FTP' },
        webdav: { section: 'WebDAV', name: 'WebDAV' }
    };
    const DEFAULT_DROPBOX_APP_KEY = 'y88zxdudhuoo41q';

    // Sealed by the 3DS on first start ("enc:v1:…"); opens only on that
    // console, in the section it was sealed in.
    const SEALED_PREFIX = 'enc:v1:';
    // What the 3DS seals, as isSealableSecret() in source/utils/secrets.cpp.
    const SEALABLE = {
        dropbox: ['token', 'refreshtoken', 'appsecret'],
        googledrive: ['token', 'clientsecret', 'refreshtoken'],
        smb: ['password'], ftp: ['password'], webdav: ['password']
    };
    // INI key (lowercased) -> editor input, per type.
    const INI_FIELDS = {
        dropbox: { appkey: 'dropbox-client-id' },
        googledrive: { clientid: 'gdrive-client-id', clientsecret: 'gdrive-client-secret', folderid: 'gdrive-folder-id' },
        smb: { server: 'smb-server', share: 'smb-share', user: 'smb-user', password: 'smb-password',
            domain: 'smb-domain', path: 'smb-path' },
        ftp: { host: 'ftp-host', port: 'ftp-port', user: 'ftp-user', password: 'ftp-password', path: 'ftp-path',
            tls: 'ftp-tls', mode: 'ftp-mode', verifytls: 'ftp-cert', pinnedkey: 'ftp-pinned-key',
            cacert: 'ftp-ca-cert' },
        webdav: { url: 'webdav-url', user: 'webdav-user', password: 'webdav-password', verifytls: 'webdav-cert',
            pinnedkey: 'webdav-pinned-key', cacert: 'webdav-ca-cert' }
    };
    // Inputs whose value follows from the INI value through a table, as
    // VerifyTLS=false picks "don't check" in the certificate select.  Another
    // value is kept as it was.  PinnedKey= and CACert= pick their own entries
    // of that select (certMode() below).
    const VERIFY_TLS = { 'false': 'off', no: 'off', '0': 'off', off: 'off',
        'true': 'public', yes: 'public', '1': 'public', on: 'public' };
    const MAPPED_VALUES = { 'ftp-cert': VERIFY_TLS, 'webdav-cert': VERIFY_TLS };
    // The editor's selects; a value outside them is kept as it was instead.
    const SELECT_OPTIONS = {
        'ftp-tls': ['none', 'try', 'require', 'implicit'],
        'ftp-mode': ['passive', 'active']
    };
    const PATH_SECTIONS = [
        { section: 'Paths', recursive: true, direction: 'both' },
        { section: 'ShallowPaths', recursive: false, direction: 'both' },
        { section: 'UploadPaths', recursive: true, direction: 'upload' },
        { section: 'UploadShallowPaths', recursive: false, direction: 'upload' }
    ];
    const SECRET_KEY = /^(password|clientsecret|appsecret|refreshtoken|token)$/i;
    const MASK = '••••••••';

    function isSealed(value) {
        return typeof value === 'string' && value.indexOf(SEALED_PREFIX) === 0;
    }

    function sectionName(remote) {
        return TYPES[remote.type].section + (remote.label ? ':' + remote.label : '');
    }

    // What the 3DS calls it: "SMB", or "SMB (backup)" for a labelled one.
    function displayName(remote) {
        let name = TYPES[remote.type].name;
        return remote.label ? name + ' (' + remote.label + ')' : name;
    }

    function syncsFolder(remote, name) {
        return !(remote.excluded || {})[name.toLowerCase()];
    }

    // The folders a remote is limited to, or null when it syncs all of them.
    // With a single remote there is nothing to route.
    function onlyFolders(config, remote) {
        if (config.remotes.length < 2) return null;
        let names = config.folders.map(function (f) { return f.name; });
        let kept = names.filter(function (name) { return syncsFolder(remote, name); });
        return kept.length === names.length ? null : kept;
    }

    // Values kept from an imported file that the form does not produce.
    // Something typed or logged in over one wins; a blank field leaves the
    // kept value in place.
    function withCarried(entries, remote) {
        let carry = remote.carry || [];
        let carried = function (key) {
            return carry.some(function (c) { return c[0].toLowerCase() === key.toLowerCase(); });
        };
        let out = entries.filter(function (e) { return e[1] !== '' || !carried(e[0]); });
        carry.forEach(function (c) {
            if (!out.some(function (e) { return e[0].toLowerCase() === c[0].toLowerCase(); })) out.push(c);
        });
        return out;
    }

    // --- Writing ---
    // masked is the preview: secrets hidden, and a CA certificate, kilobytes
    // of base64 on one line, cut to its start.
    function line(key, value, masked) {
        if (masked && SECRET_KEY.test(key)) return key + '=' + MASK;
        if (masked && /^cacert$/i.test(key) && value.length > 40)
            return key + '=' + value.slice(0, 32) + '… (' + value.length + ' characters)';
        return key + '=' + value;
    }

    function sectionText(name, pairs, masked) {
        return (name === '' ? '' : '[' + name + ']\n')
            + pairs.map(function (kv) { return line(kv[0], kv[1], masked) + '\n'; }).join('');
    }

    // Folder.<name>= for every folder the remote syncs that has a Drive
    // folder chosen; one for a folder no longer configured is not written.
    function driveFolderPairs(config, remote) {
        let chosen = remote.driveFolders || {};
        return config.folders.filter(function (f) {
            return chosen[f.name.toLowerCase()] && syncsFolder(remote, f.name);
        }).map(function (f) { return ['Folder.' + f.name, chosen[f.name.toLowerCase()].id]; });
    }

    function remoteText(config, remote, masked) {
        let pairs = remote.entries.slice();
        if (remote.type === 'googledrive') pairs = pairs.concat(driveFolderPairs(config, remote));
        if (remote.role !== 'sync') pairs.push(['Role', remote.role]);
        let only = onlyFolders(config, remote);
        if (only !== null) pairs.push(['Only', only.join(', ')]);
        return sectionText(sectionName(remote), pairs, masked);
    }

    // masked: passwords, secrets and tokens shown as dots, for the preview.
    function toIni(config, masked) {
        let parts = [];
        // Lines that came before any section must stay before the first one.
        let extras = config.extras || [];
        let head = extras.filter(function (x) { return x.name === ''; });
        let tail = extras.filter(function (x) { return x.name !== ''; });
        let headText = head.map(function (x) { return sectionText('', x.pairs, masked); }).join('');
        config.remotes.forEach(function (remote) { parts.push(remoteText(config, remote, masked)); });
        PATH_SECTIONS.forEach(function (kind) {
            let pairs = config.folders.filter(function (f) {
                return f.recursive === kind.recursive && f.direction === kind.direction;
            }).map(function (f) { return [f.name, f.path]; });
            if (pairs.length) parts.push(sectionText(kind.section, pairs, false));
        });
        // Written only when filled: no [Filter] section means all files.
        let filters = [];
        if (config.filter) filters.push(['Extensions', config.filter]);
        config.folders.forEach(function (f) { if (f.filter) filters.push([f.name, f.filter]); });
        if (filters.length) parts.push(sectionText('Filter', filters, false));
        let sync = config.sync || {};
        let syncPairs = [];
        if (sync.compare === false) syncPairs.push(['CompareConflicts', 'false']);
        if (sync.compareUpTo) syncPairs.push(['CompareUpTo', sync.compareUpTo]);
        syncPairs = syncPairs.concat(sync.other || []);
        if (syncPairs.length) parts.push(sectionText('Sync', syncPairs, false));
        tail.forEach(function (x) { parts.push(sectionText(x.name, x.pairs, masked)); });
        return (headText ? headText + '\n' : '') + parts.join('\n');
    }

    // --- Reading ---
    // The way inih reads it on the 3DS: ';' or '#' starts a comment line,
    // '=' or ':' ends the key, a ';' after whitespace ends the value, and an
    // indented line continues the previous value.  A section named twice is
    // one section.  sealIniText() on the 3DS follows the same rules; if
    // inih's change on a resync, all three must.
    function parseIni(text) {
        let sections = [];
        let byName = {};
        let current = null;
        let last = null;
        let badLines = [];
        let stripComment = function (v) {
            let m = /\s;/.exec(v);
            return (m ? v.slice(0, m.index) : v).replace(/\s+$/, '');
        };
        let open = function (name) {
            let key = name.toLowerCase();
            if (!byName[key]) {
                byName[key] = { name: name, pairs: [] };
                sections.push(byName[key]);
            }
            return byName[key];
        };
        text.replace(/^﻿/, '').split(/\r?\n/).forEach(function (raw, i) {
            let rawLine = raw.replace(/\s+$/, '');
            let start = rawLine.replace(/^\s+/, '');
            if (start === '' || start[0] === ';' || start[0] === '#') return;
            if (start.length < rawLine.length && last) {
                last[1] += '\n' + stripComment(start);
                return;
            }
            last = null;
            if (start[0] === '[') {
                let end = start.indexOf(']');
                if (end < 0) badLines.push(i + 1);
                else current = open(start.slice(1, end).trim());
                return;
            }
            let at = start.search(/[=:]/);
            if (at < 0) {
                badLines.push(i + 1);
                return;
            }
            last = [start.slice(0, at).replace(/\s+$/, ''), stripComment(start.slice(at + 1).replace(/^\s+/, ''))];
            (current || open('')).pairs.push(last);
        });
        return { sections: sections, badLines: badLines };
    }

    function typeOfSection(name) {
        let parts = name.split(':');
        let type = parts[0].toLowerCase();
        return Object.prototype.hasOwnProperty.call(TYPES, type) && parts.length <= 2 ? type : null;
    }

    function readRemote(section, type, uid, report) {
        let label = section.name.indexOf(':') >= 0 ? section.name.slice(section.name.indexOf(':') + 1) : '';
        let remote = {
            uid: uid, type: type, label: label, role: 'sync', fields: {}, auth: null,
            entries: [], carry: [], sealedSection: section.name.toLowerCase(), excluded: {}, only: null,
            driveFolders: {}
        };
        let name = displayName(remote);
        let fields = INI_FIELDS[type];
        let oauth = type === 'dropbox' || type === 'googledrive';
        let refreshToken = null;
        section.pairs.forEach(function (pair) {
            let key = pair[0].toLowerCase();
            let value = pair[1];
            if (key === 'role') {
                let role = value.toLowerCase();
                if (role === 'backup' || role === 'source' || role === 'sync') remote.role = role;
                else report.problems.push(name + ': Role=' + value + ' is unknown, so it syncs both ways.');
                return;
            }
            if (key === 'only') {
                remote.only = value.split(/[\s,]+/).filter(Boolean).map(function (n) { return n.toLowerCase(); });
                return;
            }
            // Written again from driveFolders, for the folders that still exist.
            if (type === 'googledrive' && key.indexOf('folder.') === 0 && key.length > 7) {
                remote.driveFolders[key.slice(7)] = { id: value, title: '' };
                return;
            }
            remote.entries.push(pair);
            let sealed = isSealed(value) && SEALABLE[type].indexOf(key) >= 0;
            let input = fields[key];
            let options = input && SELECT_OPTIONS[input];
            let ticks = input && MAPPED_VALUES[input];
            let fits = input && (ticks ? Object.prototype.hasOwnProperty.call(ticks, value.toLowerCase())
                : !options || options.indexOf(value.toLowerCase()) >= 0);
            let formKey = !!input || (oauth && key === 'refreshtoken');
            if (sealed || !formKey || (input && !fits)) remote.carry.push(pair);
            if (sealed) report.sealed++;
            let plain = sealed ? '' : value;
            if (fits) remote.fields[input] = ticks ? ticks[value.toLowerCase()] : options ? value.toLowerCase() : plain;
            if (key === 'refreshtoken') refreshToken = plain;
        });
        if (type === 'ftp' || type === 'webdav') certMode(remote, type, name, report);
        if (type === 'dropbox') {
            let appKey = remote.fields['dropbox-client-id'] || DEFAULT_DROPBOX_APP_KEY;
            remote.fields['dropbox-expert-mode'] = appKey !== DEFAULT_DROPBOX_APP_KEY;
            if (appKey === DEFAULT_DROPBOX_APP_KEY) delete remote.fields['dropbox-client-id'];
            if (refreshToken !== null) remote.auth = { clientId: appKey, clientSecret: '', refreshToken: refreshToken };
        } else if (type === 'googledrive') {
            // No client of its own (not even a sealed secret): 3DSync's.
            let own = remote.entries.some(function (e) { return /^client(id|secret)$/i.test(e[0]); });
            remote.fields['gdrive-expert-mode'] = own;
            if (refreshToken !== null) {
                remote.auth = own ? {
                    clientId: remote.fields['gdrive-client-id'] || '',
                    clientSecret: remote.fields['gdrive-client-secret'] || '', refreshToken: refreshToken
                } : { shared: true, clientId: '', clientSecret: '', refreshToken: refreshToken };
            }
        }
        if (oauth && !remote.auth)
            report.problems.push(name + ' has no refresh token: kept as it is, but log in again to change it.');
        if (label !== '' && !/^[A-Za-z0-9_-]+$/.test(label))
            report.problems.push(name + ': the label may only hold letters, digits, - and _; change it when you edit it.');
        return remote;
    }

    // The certificate select of a WebDAV or FTP remote, from the three keys
    // the 3DS reads: PinnedKey= first, as there, then CACert=, then
    // VerifyTLS=.  The lines themselves stay in entries, written as read.
    function certMode(remote, type, name, report) {
        let f = remote.fields;
        let pin = f[type + '-pinned-key'], ca = f[type + '-ca-cert'];
        let off = f[type + '-cert'] === 'off';
        if (pin) f[type + '-cert'] = 'key';
        else if (ca) f[type + '-cert'] = 'ca';
        else if (!f[type + '-cert']) f[type + '-cert'] = 'public';
        if (pin && ca)
            report.problems.push(name + ' has PinnedKey= and CACert=: the 3DS uses only PinnedKey=, and so does '
                + 'the form.');
        if ((pin || ca) && off)
            report.problems.push(name + ' has VerifyTLS=false with ' + (pin ? 'PinnedKey=' : 'CACert=')
                + ': the 3DS checks the certificate, as the form shows.');
        if (pin && pinnedKeyProblem(pin))
            report.problems.push(name + ': PinnedKey= is not a SHA-256 hash; the 3DS leaves the remote out.');
        if (ca && !pin && caCertProblem(ca))
            report.problems.push(name + ': CACert= is not a certificate; the 3DS leaves the remote out.');
    }

    // --- Server certificates (WebDAV, FTP) ---
    // The same rules as parsePinnedKeys() and parseCaCerts() in
    // source/utils/certtrust.cpp, so the page refuses what the 3DS would.

    function base64Bytes(b64) {
        if (b64 === '' || b64.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(b64)) return null;
        try {
            let bin = atob(b64);
            let bytes = new Uint8Array(bin.length);
            for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
            return bytes;
        } catch (e) {
            return null;
        }
    }

    function bytesBase64(bytes) {
        let bin = '';
        for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
        return btoa(bin);
    }

    // PinnedKey=: SHA-256 hashes separated by commas, each 64 hex digits
    // (colons and spaces allowed, as browsers show them) or sha256// and
    // base64.  null when the 3DS takes it.
    function pinnedKeyProblem(value) {
        let items = value.split(',').filter(function (item) { return item.trim() !== ''; });
        if (!items.length) return 'Paste the server\'s SHA-256 key hash';
        let bad = items.some(function (item) {
            let s = item.replace(/[\s:]/g, '');
            if (/^sha256\/\//i.test(s)) {
                let bytes = base64Bytes(s.slice(8));
                return !bytes || bytes.length !== 32;
            }
            return !/^[0-9a-f]{64}$/i.test(s);
        });
        return bad ? 'Not a SHA-256 hash: 64 hex digits (colons allowed), or sha256// and base64' : null;
    }

    // One DER element filling the buffer, a SEQUENCE as every certificate
    // is: what the 3DS checks before mbedTLS reads it.
    function isOneDerSequence(der) {
        if (der.length < 2 || der[0] !== 0x30) return false;
        let len = der[1], header = 2;
        if (len & 0x80) {
            let n = len & 0x7f;
            if (n === 0 || n > 4 || der.length < 2 + n) return false;
            len = 0;
            for (let i = 0; i < n; i++) len = len * 256 + der[2 + i];
            header += n;
        }
        return header + len === der.length;
    }

    function caItems(value) {
        return value.replace(/-----(BEGIN|END) CERTIFICATE-----/g, ',').split(',')
            .map(function (item) { return item.replace(/\s/g, ''); })
            .filter(function (item) { return item !== ''; });
    }

    // CACert=: null when the 3DS takes it.
    function caCertProblem(value) {
        let items = caItems(value);
        if (!items.length) return 'No certificate';
        let bad = items.some(function (b64) {
            let der = base64Bytes(b64);
            return !der || !isOneDerSequence(der);
        });
        return bad ? 'Not a certificate' : null;
    }

    // A CA certificate file as chosen in the form, read as bytes: PEM (one
    // or more BEGIN CERTIFICATE blocks; anything around them, such as
    // openssl's text dump, is skipped) or a single DER certificate.  Returns
    // { value, count } with value the CACert= line, base64 DER separated by
    // commas, or { error }.
    function caCertFromFile(bytes) {
        let text = '';
        for (let i = 0; i < bytes.length; i++) text += String.fromCharCode(bytes[i]);
        let blocks = text.match(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g);
        let items;
        if (blocks) items = blocks.map(function (b) { return caItems(b)[0] || ''; });
        else if (/-----BEGIN /.test(text))
            return { error: 'This file holds no certificate (a private key, perhaps?). Choose the CA\'s certificate.' };
        else items = [bytesBase64(bytes)];
        let value = items.join(',');
        if (caCertProblem(value))
            return { error: 'Not a certificate file. Choose the CA\'s certificate (.pem, .crt or .cer).' };
        return { value: value, count: items.length };
    }

    // The 3DS reads CompareConflicts= as on for anything it does not know, and
    // says so at startup; the page does the same, and keeps a CompareUpTo= it
    // would refuse in the field, where Confirm points it out.
    const SWITCH_VALUES = { 'true': true, yes: true, '1': true, on: true,
        'false': false, no: false, '0': false, off: false };

    function readSync(section, sync, report) {
        section.pairs.forEach(function (pair) {
            let key = pair[0].toLowerCase();
            let value = pair[1].trim();
            if (key === 'compareconflicts') {
                let on = SWITCH_VALUES[value.toLowerCase()];
                if (value === '' || on === undefined) {
                    if (value !== '')
                        report.problems.push('[Sync] CompareConflicts=' + value + ' is neither true nor false, so '
                            + 'conflicts are compared.');
                    sync.compare = true;
                } else sync.compare = on;
            } else if (key === 'compareupto') sync.compareUpTo = value;
            else sync.other.push(pair);
        });
    }

    // Returns { config, report }.  report: remotes and folders read, how many
    // values were sealed, the sections kept as they were, and every line
    // that could not be used, for the page to list.  config is null when the
    // file holds neither remotes nor folders.
    function fromIni(text) {
        let parsed = parseIni(text);
        let report = { remotes: 0, folders: 0, sealed: 0, kept: [], problems: [] };
        if (parsed.badLines.length)
            report.problems.push('Line ' + parsed.badLines.join(', ') + ' could not be read and was left out.');
        let config = { remotes: [], folders: [], filter: '', sync: { compare: true, compareUpTo: '', other: [] },
            extras: [] };
        let filterPairs = [];
        parsed.sections.forEach(function (section) {
            let key = section.name.toLowerCase();
            let type = typeOfSection(section.name);
            let kind = PATH_SECTIONS.find(function (k) { return k.section.toLowerCase() === key; });
            if (type) config.remotes.push(readRemote(section, type, config.remotes.length + 1, report));
            else if (kind) {
                section.pairs.forEach(function (pair) {
                    config.folders.push({ name: pair[0], path: pair[1], recursive: kind.recursive,
                        direction: kind.direction, filter: '' });
                });
            } else if (key === 'filter') filterPairs = section.pairs;
            else if (key === 'sync') readSync(section, config.sync, report);
            else if (section.pairs.length) {
                config.extras.push(section);
                report.kept.push(section.name === '' ? 'the lines before the first section' : '[' + section.name + ']');
            }
        });
        if (config.remotes.length === 0 && config.folders.length === 0) {
            report.problems.unshift('No remotes or folders found: is this a 3DSync.ini?');
            return { config: null, report: report };
        }

        let names = config.folders.map(function (f) { return f.name.toLowerCase(); });
        config.remotes.forEach(function (remote) {
            if (remote.only !== null) {
                names.forEach(function (n) { if (remote.only.indexOf(n) < 0) remote.excluded[n] = true; });
                remote.only.forEach(function (n) {
                    if (names.indexOf(n) < 0)
                        report.problems.push(displayName(remote) + ': Only= names "' + n + '", which is no folder here.');
                });
            }
            delete remote.only;
            Object.keys(remote.driveFolders).forEach(function (n) {
                if (names.indexOf(n) < 0) {
                    report.problems.push(displayName(remote) + ': Folder.' + n + '= names no folder here and was left out.');
                    delete remote.driveFolders[n];
                }
            });
        });
        filterPairs.forEach(function (pair) {
            let key = pair[0].toLowerCase();
            let folder = config.folders.find(function (f) { return f.name.toLowerCase() === key; });
            if (key === 'extensions') config.filter = pair[1];
            else if (folder) folder.filter = pair[1];
            else report.problems.push('[Filter] ' + pair[0] + '= names no folder and was left out.');
        });
        report.remotes = config.remotes.length;
        report.folders = config.folders.length;
        return { config: config, report: report };
    }

    let api = {
        TYPES: TYPES, DEFAULT_DROPBOX_APP_KEY: DEFAULT_DROPBOX_APP_KEY, INI_FIELDS: INI_FIELDS,
        isSealed: isSealed, sectionName: sectionName, displayName: displayName, syncsFolder: syncsFolder,
        onlyFolders: onlyFolders, withCarried: withCarried, toIni: toIni, parseIni: parseIni, fromIni: fromIni,
        pinnedKeyProblem: pinnedKeyProblem, caCertProblem: caCertProblem, caCertFromFile: caCertFromFile
    };
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
    else root.IniFile = api;
})(this);
