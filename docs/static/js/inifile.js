// 3DSync.ini <-> the configurator's config object, with no page in sight:
// index.js builds the object step by step and hands it to toIni() at the end,
// and an imported file goes through fromIni() into the same object.  Kept
// free of jQuery and the DOM so tests/configurator runs it under Node.
//
//   config = {
//     remotes: [{ uid, type, label, role, fields, auth, entries, carry,
//                 sealedSection, excluded }],
//     folders: [{ name, path, recursive, direction, filter }],
//     filter: '',      // [Filter] Extensions=
//     extras: []       // sections the page has no form for, as read
//   }
//
// remote.entries are the section's lines as the 3DS reads them, without
// Role= and Only=, which follow from role and excluded (lowercased folder
// names it does not sync).  fields are the editor's inputs by id, auth the
// OAuth login.  carry holds what an imported file had that the form does not
// produce: sealed values, which are never shown, and keys it has no field
// for.  A folder's filter of '' means it takes config.filter.
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
            tls: 'ftp-tls', mode: 'ftp-mode' },
        webdav: { url: 'webdav-url', user: 'webdav-user', password: 'webdav-password' }
    };
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
    function line(key, value, masked) {
        return key + '=' + (masked && SECRET_KEY.test(key) ? MASK : value);
    }

    function sectionText(name, pairs, masked) {
        return (name === '' ? '' : '[' + name + ']\n')
            + pairs.map(function (kv) { return line(kv[0], kv[1], masked) + '\n'; }).join('');
    }

    function remoteText(config, remote, masked) {
        let pairs = remote.entries.slice();
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
            entries: [], carry: [], sealedSection: section.name.toLowerCase(), excluded: {}, only: null
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
            remote.entries.push(pair);
            let sealed = isSealed(value) && SEALABLE[type].indexOf(key) >= 0;
            let input = fields[key];
            let options = input && SELECT_OPTIONS[input];
            let fits = input && (!options || options.indexOf(value.toLowerCase()) >= 0);
            let formKey = !!input || (oauth && key === 'refreshtoken');
            if (sealed || !formKey || (input && !fits)) remote.carry.push(pair);
            if (sealed) report.sealed++;
            let plain = sealed ? '' : value;
            if (fits) remote.fields[input] = options ? value.toLowerCase() : plain;
            if (key === 'refreshtoken') refreshToken = plain;
        });
        if (type === 'dropbox') {
            let appKey = remote.fields['dropbox-client-id'] || DEFAULT_DROPBOX_APP_KEY;
            remote.fields['dropbox-expert-mode'] = appKey !== DEFAULT_DROPBOX_APP_KEY;
            if (appKey === DEFAULT_DROPBOX_APP_KEY) delete remote.fields['dropbox-client-id'];
            if (refreshToken !== null) remote.auth = { clientId: appKey, clientSecret: '', refreshToken: refreshToken };
        } else if (type === 'googledrive' && refreshToken !== null) {
            remote.auth = {
                clientId: remote.fields['gdrive-client-id'] || '',
                clientSecret: remote.fields['gdrive-client-secret'] || '', refreshToken: refreshToken
            };
        }
        if (oauth && !remote.auth)
            report.problems.push(name + ' has no refresh token: kept as it is, but log in again to change it.');
        if (label !== '' && !/^[A-Za-z0-9_-]+$/.test(label))
            report.problems.push(name + ': the label may only hold letters, digits, - and _; change it when you edit it.');
        return remote;
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
        let config = { remotes: [], folders: [], filter: '', extras: [] };
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
        onlyFolders: onlyFolders, withCarried: withCarried, toIni: toIni, parseIni: parseIni, fromIni: fromIni
    };
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
    else root.IniFile = api;
})(this);
