$(function () {
    const DEFAULT_DROPBOX_CLIENT_ID = 'y88zxdudhuoo41q';

    let queryParams = new URLSearchParams(window.location.search);
    let authCode = queryParams.get('code');
    let authState = queryParams.get('state');
    let paths = [];

    let stepperInstace = new MStepper(document.querySelector('.stepper'), {
        firstActive: 0,
        autoFormCreation: false,
        stepTitleNavigation: false,
    });

    // --- OAuth redirect callback ---
    // Both providers use the PKCE authorization-code flow and come back with
    // ?code=&state=, so the stored state decides which one is answering.
    if (authCode !== null && authState !== null) {
        if (authState === localStorage.getItem('gdriveState')) {
            let codeVerifier = localStorage.getItem('gdriveCodeVerifier');
            localStorage.removeItem('gdriveState');
            localStorage.removeItem('gdriveCodeVerifier');
            // Clean the auth code from the URL to avoid reuse on refresh
            history.replaceState(null, '', window.location.pathname);
            exchangeGoogleCode(authCode, codeVerifier).then(function (success) {
                if (success) {
                    localStorage.setItem('provider', 'googledrive');
                    selectProvider('googledrive');
                    $('.auth-tile[data-provider="googledrive"] .auth-status').text('\u2713 Logged in');
                    stepperInstace.nextStep();
                } else {
                    alert('Google Drive authentication failed. Please try again.');
                }
            });
        } else if (authState === localStorage.getItem('dropboxState')) {
            let codeVerifier = localStorage.getItem('dropboxCodeVerifier');
            localStorage.removeItem('dropboxState');
            localStorage.removeItem('dropboxCodeVerifier');
            history.replaceState(null, '', window.location.pathname);
            exchangeDropboxCode(authCode, codeVerifier).then(function (success) {
                if (success) {
                    localStorage.setItem('provider', 'dropbox');
                    selectProvider('dropbox');
                    $('.auth-tile[data-provider="dropbox"] .auth-status').text('\u2713 Logged in');
                    stepperInstace.nextStep();
                } else {
                    alert('Dropbox authentication failed. Please try again.');
                }
            });
        }
    }

    // --- PKCE helpers ---
    function base64urlEncode(buffer) {
        return btoa(String.fromCharCode.apply(null, new Uint8Array(buffer)))
            .replace(/\+/g, '-')
            .replace(/\//g, '_')
            .replace(/=/g, '');
    }

    function generateCodeVerifier() {
        let array = new Uint8Array(32);
        crypto.getRandomValues(array);
        return base64urlEncode(array.buffer);
    }

    async function generateCodeChallenge(verifier) {
        let data = new TextEncoder().encode(verifier);
        let digest = await crypto.subtle.digest('SHA-256', data);
        return base64urlEncode(digest);
    }

    async function exchangeGoogleCode(code, codeVerifier) {
        let clientId = localStorage.getItem('gdriveClientId');
        let clientSecret = localStorage.getItem('gdriveClientSecret');
        let redirectUri = window.location.origin + window.location.pathname;
        try {
            let response = await fetch('https://oauth2.googleapis.com/token', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: new URLSearchParams({
                    code: code,
                    client_id: clientId,
                    client_secret: clientSecret,
                    redirect_uri: redirectUri,
                    code_verifier: codeVerifier,
                    grant_type: 'authorization_code'
                })
            });
            let data = await response.json();
            if (data.refresh_token) {
                localStorage.setItem('gdriveRefreshToken', data.refresh_token);
                return true;
            }
            console.error('No refresh_token in response:', data);
            return false;
        } catch (e) {
            console.error('Token exchange failed:', e);
            return false;
        }
    }

    async function exchangeDropboxCode(code, codeVerifier) {
        let clientId = localStorage.getItem('dropboxClientId');
        try {
            let response = await fetch('https://api.dropbox.com/oauth2/token', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: new URLSearchParams({
                    code: code,
                    grant_type: 'authorization_code',
                    client_id: clientId,
                    redirect_uri: dropboxRedirectUri(),
                    code_verifier: codeVerifier
                })
            });
            let data = await response.json();
            // token_access_type=offline is what makes Dropbox return this; a
            // bare access token would expire after about four hours.
            if (data.refresh_token) {
                localStorage.setItem('dropboxRefreshToken', data.refresh_token);
                localStorage.removeItem('dropboxToken');
                return true;
            }
            console.error('No refresh_token in response:', data);
            return false;
        } catch (e) {
            console.error('Dropbox token exchange failed:', e);
            return false;
        }
    }

    // --- Dropbox expert mode: bring your own app key ---
    // Same value the authorize request sends, so the hint can never drift from it
    function dropboxRedirectUri() {
        return window.location.origin + window.location.pathname;
    }

    $('#dropbox-redirect-uri').text(dropboxRedirectUri());

    $('#dropbox-copy-redirect-uri').on('click', function (e) {
        e.preventDefault();
        let $status = $('#dropbox-copy-status');
        navigator.clipboard.writeText(dropboxRedirectUri()).then(function () {
            $status.html('<span class="green-text"><i class="material-icons" style="font-size:1em;vertical-align:middle">check_circle</i></span>');
        }, function () {
            $status.html('<span class="red-text">Copy failed</span>');
        });
    });

    let storedDropboxClientId = localStorage.getItem('dropboxClientId');
    if (storedDropboxClientId && storedDropboxClientId !== DEFAULT_DROPBOX_CLIENT_ID) {
        $('#dropbox-client-id').val(storedDropboxClientId);
        $('#dropbox-expert-mode').prop('checked', true);
        $('#dropbox-expert-panel').show();
        M.updateTextFields();
    }

    $('#dropbox-expert-mode').on('change', function () {
        $('#dropbox-expert-panel').toggle(this.checked);
        M.updateTextFields();
    });

    // --- Login button handlers ---
    $('#dropbox-login').on('click', async function (e) {
        e.preventDefault();
        let clientId = DEFAULT_DROPBOX_CLIENT_ID;
        if ($('#dropbox-expert-mode').prop('checked')) {
            clientId = $('#dropbox-client-id').val().trim() || DEFAULT_DROPBOX_CLIENT_ID;
        }
        localStorage.setItem('dropboxClientId', clientId);

        let state = generateCodeVerifier();
        let codeVerifier = generateCodeVerifier();
        let codeChallenge = await generateCodeChallenge(codeVerifier);
        localStorage.setItem('dropboxState', state);
        localStorage.setItem('dropboxCodeVerifier', codeVerifier);

        // PKCE + token_access_type=offline yields a refresh token, so the 3DS
        // can sync without re-authenticating every four hours.
        window.location.href = 'https://www.dropbox.com/oauth2/authorize?client_id=' + encodeURIComponent(clientId)
            + '&response_type=code'
            + '&redirect_uri=' + encodeURIComponent(dropboxRedirectUri())
            + '&token_access_type=offline'
            + '&state=' + encodeURIComponent(state)
            + '&code_challenge=' + codeChallenge
            + '&code_challenge_method=S256';
    });

    // Auto-extract folder ID when user pastes a full Google Drive URL
    $('#gdrive-folder-id').on('input paste', function () {
        let val = $(this).val().trim();
        let match = val.match(/\/folders\/([a-zA-Z0-9_-]+)/);
        if (match) {
            $(this).val(match[1]);
            // Refresh Materialize label position
            $(this).trigger('change');
        }
    });

    // --- Google Drive: import client_secret.json ---
    $('#gdrive-json-import').on('change', function (e) {
        let file = e.target.files[0];
        if (!file) return;
        let reader = new FileReader();
        reader.onload = function (ev) {
            try {
                let json = JSON.parse(ev.target.result);
                // Support both "web" and "installed" application types
                let creds = json.web || json.installed;
                if (!creds || !creds.client_id || !creds.client_secret) {
                    $('#gdrive-json-status').html('<span class="red-text">Invalid file: missing client_id or client_secret</span>');
                    return;
                }
                $('#gdrive-client-id').val(creds.client_id);
                $('#gdrive-client-secret').val(creds.client_secret);
                M.updateTextFields();
                $('#gdrive-json-status').html('<span class="green-text"><i class="material-icons" style="font-size:1em;vertical-align:middle">check_circle</i> Imported</span>');
            } catch (err) {
                $('#gdrive-json-status').html('<span class="red-text">Could not parse JSON</span>');
            }
        };
        reader.readAsText(file);
        this.value = ''; // allow re-importing the same file
    });

    $('#gdrive-login').on('click', async function (e) {
        e.preventDefault();
        let clientId = $('#gdrive-client-id').val().trim();
        let clientSecret = $('#gdrive-client-secret').val().trim();
        if (!clientId) {
            $('#gdrive-client-id').addClass('invalid');
            return;
        }
        if (!clientSecret) {
            $('#gdrive-client-secret').addClass('invalid');
            return;
        }
        $('#gdrive-client-id').removeClass('invalid');
        $('#gdrive-client-secret').removeClass('invalid');

        let folderId = $('#gdrive-folder-id').val().trim();
        let state = [...Array(40)].map(i => (~~(Math.random() * 36)).toString(36)).join('');
        let codeVerifier = generateCodeVerifier();
        let codeChallenge = await generateCodeChallenge(codeVerifier);

        localStorage.setItem('gdriveState', state);
        localStorage.setItem('gdriveCodeVerifier', codeVerifier);
        localStorage.setItem('gdriveClientId', clientId);
        localStorage.setItem('gdriveClientSecret', clientSecret);
        localStorage.setItem('gdriveFolderId', folderId);

        let redirectUri = encodeURIComponent(window.location.origin + window.location.pathname);
        window.location.href = 'https://accounts.google.com/o/oauth2/v2/auth'
            + '?client_id=' + encodeURIComponent(clientId)
            + '&redirect_uri=' + redirectUri
            + '&response_type=code'
            + '&scope=' + encodeURIComponent('https://www.googleapis.com/auth/drive')
            + '&access_type=offline'
            + '&prompt=consent'
            + '&state=' + state
            + '&code_challenge=' + codeChallenge
            + '&code_challenge_method=S256';
    });

    // --- Auth method tiles ---
    // One remote per generated file, chosen by selecting its tile; the
    // settings for the selected one show below the grid, and clicking the
    // selected tile again clears the choice.  Dropbox and Drive log in
    // through OAuth and come back on a redirect, so their login survives in
    // localStorage; SMB, FTP and WebDAV are typed in and kept only in memory
    // (remoteSection), so their passwords never touch browser storage.
    let selectedProvider = null;
    let remoteSection = null; // { provider, section, entries: [[key, value], ...] }

    function oauthLoggedIn(provider) {
        if (provider === 'dropbox')
            return !!(localStorage.getItem('dropboxRefreshToken') || localStorage.getItem('dropboxToken'));
        if (provider === 'googledrive')
            return !!localStorage.getItem('gdriveRefreshToken');
        return false;
    }

    function selectProvider(provider) {
        selectedProvider = provider;
        $('.auth-tile').each(function () {
            let on = $(this).data('provider') === provider;
            $(this).toggleClass('selected', on).attr('aria-checked', on ? 'true' : 'false');
        });
        $('.auth-settings').removeClass('active');
        if (provider) $('#auth-settings-' + provider).addClass('active');
        $('#auth-empty').toggle(!provider);
        $('#auth-continue-hint').text('').removeClass('warn');
        M.updateTextFields();
    }

    function toggleTile($tile) {
        let provider = $tile.data('provider');
        selectProvider(selectedProvider === provider ? null : provider);
    }

    $('.auth-tile').on('click', function () {
        toggleTile($(this));
    }).on('keydown', function (e) {
        if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleTile($(this));
        }
    });

    ['dropbox', 'googledrive'].forEach(function (provider) {
        if (oauthLoggedIn(provider))
            $('.auth-tile[data-provider="' + provider + '"] .auth-status').text('\u2713 Logged in');
    });

    // Back from an earlier login: start on that backend.
    let storedProvider = localStorage.getItem('provider');
    selectProvider(storedProvider && oauthLoggedIn(storedProvider) ? storedProvider : null);

    // 3DSync reads the file with inih, which ends a value at a ';' that
    // follows whitespace (an inline comment) and trims spaces at both ends.
    // A value it would change is refused here rather than mangled on the 3DS,
    // where the only symptom would be a failed login.
    function iniValueProblem(value) {
        if (/[\r\n]/.test(value)) return 'Cannot contain a line break';
        if (/\s;/.test(value)) return 'A ";" after a space would cut the value short in 3DSync.ini';
        if (value !== value.trim()) return 'Leading or trailing spaces would be dropped';
        return null;
    }

    function setFieldError($input, message) {
        let $helper = $input.siblings('.helper-text');
        if (message) {
            $input.addClass('invalid').removeClass('valid');
            $helper.attr('data-error', message);
        } else {
            $input.removeClass('invalid');
            $helper.attr('data-error', '');
        }
    }

    // Validates every field of a form; returns false and marks the fields
    // when anything is wrong.  `required` lists ids that must not be empty.
    function checkFields(ids, required) {
        let ok = true;
        ids.forEach(function (id) {
            let $input = $('#' + id);
            let value = $input.val();
            let problem = iniValueProblem(value);
            if (!problem && required.indexOf(id) >= 0 && value === '') problem = 'Required';
            setFieldError($input, problem);
            if (problem) ok = false;
        });
        return ok;
    }

    function setVal(id, value) {
        $('#' + id).val(value);
        M.updateTextFields();
    }

    function trimField(id) {
        let $input = $('#' + id);
        // Passwords are left alone: a space in one is significant, and
        // iniValueProblem() reports it instead of silently changing it.
        if ($input.attr('type') !== 'password') $input.val($input.val().trim());
    }

    // "\\nas\saves\3ds", "smb://nas/saves/3ds" or "nas/saves" typed as the
    // server: split into server, share and folder, filling the fields that are
    // still empty.
    function normalizeSmb() {
        let server = $('#smb-server').val().replace(/^smb:\/\//i, '').replace(/\\/g, '/').replace(/^\/+/, '');
        let parts = server.split('/').filter(function (p) { return p !== ''; });
        if (parts.length > 1) {
            setVal('smb-server', parts[0]);
            if ($('#smb-share').val() === '') setVal('smb-share', parts[1]);
            if (parts.length > 2 && $('#smb-path').val() === '') setVal('smb-path', parts.slice(2).join('/'));
        } else {
            setVal('smb-server', parts[0] || '');
        }
        let share = $('#smb-share').val().replace(/\\/g, '/').replace(/^\/+|\/+$/g, '');
        let shareParts = share.split('/');
        setVal('smb-share', shareParts[0]);
        if (shareParts.length > 1 && $('#smb-path').val() === '') setVal('smb-path', shareParts.slice(1).join('/'));
        setVal('smb-path', $('#smb-path').val().replace(/\\/g, '/'));
    }

    // "ftp://user@host:2121/dir" or "host:2121" typed as the host.  ftps://
    // means implicit TLS, and is taken as such unless TLS was already chosen.
    function normalizeFtp() {
        let host = $('#ftp-host').val();
        let m = host.match(/^(ftps?):\/\/(?:([^@\/]*)@)?([^\/:]+)(?::(\d+))?(\/.*)?$/i);
        if (m) {
            if (m[1].toLowerCase() === 'ftps' && $('#ftp-tls').val() === 'try') $('#ftp-tls').val('implicit');
            if (m[2] && $('#ftp-user').val() === '') setVal('ftp-user', decodeURIComponent(m[2].split(':')[0]));
            if (m[4] && $('#ftp-port').val() === '') setVal('ftp-port', m[4]);
            if (m[5] && m[5] !== '/' && $('#ftp-path').val() === '') setVal('ftp-path', decodeURIComponent(m[5]));
            setVal('ftp-host', m[3]);
        } else {
            let hp = host.match(/^([^\/:]+):(\d+)$/);
            if (hp) {
                if ($('#ftp-port').val() === '') setVal('ftp-port', hp[2]);
                setVal('ftp-host', hp[1]);
            }
        }
        updateFtpNote();
    }

    function updateFtpNote() {
        let tls = $('#ftp-tls').val();
        let notes = {
            try: '',
            require: '',
            implicit: 'Implicit TLS is the older ftps:// scheme, normally on port 990. Most servers today use '
                + 'explicit TLS instead &mdash; pick that unless yours says otherwise.',
            none: 'Without TLS, the password and every file cross the network unencrypted. Only use this on a '
                + 'network you trust.'
        };
        $('#ftp-tls-note').html(notes[tls]).toggleClass('warn', tls === 'none');
    }

    $('#ftp-tls').on('change', updateFtpNote);

    // No scheme means https; the 3DS needs the trailing slash to treat the
    // URL as a folder, and adds one anyway, so it is shown here too.
    function normalizeWebdav() {
        let url = $('#webdav-url').val();
        if (url !== '' && !/^[a-z][a-z0-9+.-]*:\/\//i.test(url)) url = 'https://' + url;
        if (url !== '' && !url.endsWith('/')) url += '/';
        setVal('webdav-url', url);
        updateWebdavNote();
    }

    function updateWebdavNote() {
        let url = $('#webdav-url').val();
        let plain = /^http:\/\//i.test(url);
        $('#webdav-url-note')
            .html(plain ? 'This is plain http: the password and every file cross the network unencrypted. '
                + 'Use https if the server offers it.' : '')
            .toggleClass('warn', plain);
    }

    $('#smb-server, #smb-share').on('blur', normalizeSmb);
    $('#ftp-host').on('blur', normalizeFtp);
    $('#webdav-url').on('blur', normalizeWebdav);

    // Only the keys that have a value are written: an empty "User=" would
    // override the app's defaults (FTP falls back to anonymous only when the
    // key is missing altogether).
    function entriesFrom(pairs) {
        return pairs.filter(function (p) { return p[1] !== ''; });
    }

    const remoteForms = {
        smb: function () {
            ['smb-server', 'smb-share', 'smb-user', 'smb-domain', 'smb-path'].forEach(trimField);
            normalizeSmb();
            if (!checkFields(['smb-server', 'smb-share', 'smb-user', 'smb-password', 'smb-domain', 'smb-path'],
                ['smb-server', 'smb-share'])) return null;
            return {
                section: 'SMB',
                entries: entriesFrom([
                    ['Server', $('#smb-server').val()],
                    ['Share', $('#smb-share').val()],
                    ['User', $('#smb-user').val()],
                    ['Password', $('#smb-password').val()],
                    ['Domain', $('#smb-domain').val()],
                    ['Path', $('#smb-path').val()]
                ])
            };
        },
        ftp: function () {
            ['ftp-host', 'ftp-port', 'ftp-user', 'ftp-path'].forEach(trimField);
            normalizeFtp();
            let ok = checkFields(['ftp-host', 'ftp-port', 'ftp-user', 'ftp-password', 'ftp-path'], ['ftp-host']);
            let port = $('#ftp-port').val();
            if (port !== '' && !(/^\d+$/.test(port) && +port >= 1 && +port <= 65535)) {
                setFieldError($('#ftp-port'), 'A number from 1 to 65535');
                ok = false;
            }
            if (/:\/\//.test($('#ftp-host').val()) || /[\/\s]/.test($('#ftp-host').val())) {
                setFieldError($('#ftp-host'), 'Just the host name or address');
                ok = false;
            }
            if (!ok) return null;
            return {
                section: 'FTP',
                entries: entriesFrom([
                    ['Host', $('#ftp-host').val()],
                    ['Port', port],
                    ['User', $('#ftp-user').val()],
                    ['Password', $('#ftp-password').val()],
                    ['Path', $('#ftp-path').val()],
                    ['TLS', $('#ftp-tls').val()],
                    ['Mode', $('#ftp-mode').val()]
                ])
            };
        },
        webdav: function () {
            ['webdav-url', 'webdav-user'].forEach(trimField);
            normalizeWebdav();
            let ok = checkFields(['webdav-url', 'webdav-user', 'webdav-password'], ['webdav-url']);
            if (ok && !/^https?:\/\/[^\/\s]+\//i.test($('#webdav-url').val())) {
                setFieldError($('#webdav-url'), 'An http:// or https:// address');
                ok = false;
            }
            if (!ok) return null;
            return {
                section: 'WebDAV',
                entries: entriesFrom([
                    ['Url', $('#webdav-url').val()],
                    ['User', $('#webdav-user').val()],
                    ['Password', $('#webdav-password').val()]
                ])
            };
        }
    };

    function continueHint(text) {
        $('#auth-continue-hint').text(text).addClass('warn');
    }

    $('#auth-continue').on('click', function (e) {
        e.preventDefault();
        let provider = selectedProvider;
        if (!provider) {
            continueHint('Pick where to sync first.');
            return;
        }
        if (provider === 'dropbox' || provider === 'googledrive') {
            if (!oauthLoggedIn(provider)) {
                continueHint(provider === 'dropbox' ? 'Log in with Dropbox first.'
                    : 'Log in with Google Drive first.');
                return;
            }
            remoteSection = null;
            localStorage.setItem('provider', provider);
            stepperInstace.nextStep();
            return;
        }
        let section = remoteForms[provider]();
        if (section === null) {
            continueHint('Check the highlighted fields.');
            $('#auth-settings-' + provider + ' .invalid').first().focus();
            return;
        }
        section.provider = provider;
        remoteSection = section;
        stepperInstace.nextStep();
    });

    // --- Test connection ---
    // Only what a web page can reach: the Dropbox and Google APIs allow
    // cross-origin requests, and a WebDAV server may.  SMB and FTP are not
    // HTTP, so their settings say how to test from the 3DS instead.  Each
    // outcome says which of three things it is: the settings work, they are
    // wrong, or the browser could not tell (which says nothing about the 3DS).
    function showTest(provider, kind, text) {
        let icons = { busy: 'hourglass_empty', ok: 'check_circle', fail: 'error_outline', unknown: 'help_outline' };
        $('#test-result-' + provider)
            .removeClass('ok fail unknown')
            .addClass(kind === 'busy' ? '' : kind)
            .html('<i class="material-icons">' + icons[kind] + '</i>')
            .append(document.createTextNode(text));
    }

    function testError(kind, text) {
        let e = new Error(text);
        e.kind = kind;
        return e;
    }

    async function postForm(url, fields) {
        let response = await fetch(url, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: new URLSearchParams(fields)
        });
        let data = {};
        try { data = await response.json(); } catch (e) { /* not JSON */ }
        return { status: response.status, data: data };
    }

    async function testDropbox() {
        if (!oauthLoggedIn('dropbox')) throw testError('fail', 'Log in with Dropbox first.');
        let token = localStorage.getItem('dropboxToken');
        let refresh = localStorage.getItem('dropboxRefreshToken');
        if (refresh) {
            // What the 3DS does on every run: trade the refresh token for an
            // access token.
            let r = await postForm('https://api.dropbox.com/oauth2/token', {
                grant_type: 'refresh_token',
                refresh_token: refresh,
                client_id: localStorage.getItem('dropboxClientId') || DEFAULT_DROPBOX_CLIENT_ID
            });
            if (!r.data.access_token) {
                throw testError('fail', r.data.error === 'invalid_grant'
                    ? 'The login has expired or was revoked. Log in with Dropbox again.'
                    : 'Dropbox refused the login (' + (r.data.error_description || r.data.error || 'HTTP ' + r.status) + ').');
            }
            token = r.data.access_token;
        }
        let response = await fetch('https://api.dropboxapi.com/2/users/get_current_account', {
            method: 'POST',
            headers: { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json' },
            body: 'null'
        });
        if (response.status === 401) throw testError('fail', 'The login is no longer valid. Log in with Dropbox again.');
        if (!response.ok) throw testError('fail', 'Dropbox answered HTTP ' + response.status + '.');
        let account = await response.json();
        return 'Connected as ' + account.name.display_name + ' (' + account.email + ').';
    }

    async function testGoogleDrive() {
        if (!oauthLoggedIn('googledrive')) throw testError('fail', 'Log in with Google Drive first.');
        let r = await postForm('https://oauth2.googleapis.com/token', {
            grant_type: 'refresh_token',
            refresh_token: localStorage.getItem('gdriveRefreshToken'),
            client_id: localStorage.getItem('gdriveClientId'),
            client_secret: localStorage.getItem('gdriveClientSecret')
        });
        if (!r.data.access_token) {
            let reason = {
                invalid_grant: 'The login has expired or was revoked. Log in with Google Drive again.',
                invalid_client: 'Google does not accept the Client ID / Client Secret pair. Check them, then log in again.'
            }[r.data.error];
            throw testError('fail', reason || 'Google refused the login (' + (r.data.error_description || r.data.error || 'HTTP ' + r.status) + ').');
        }
        let auth = { headers: { 'Authorization': 'Bearer ' + r.data.access_token } };
        let about = await fetch('https://www.googleapis.com/drive/v3/about?fields=user(displayName,emailAddress)', auth);
        if (about.status === 403) throw testError('fail', 'Logged in, but the Drive API is not enabled for this Google Cloud project.');
        if (!about.ok) throw testError('fail', 'Google Drive answered HTTP ' + about.status + '.');
        let user = (await about.json()).user;
        let who = 'Connected as ' + user.displayName + ' (' + user.emailAddress + ')';

        let folderId = localStorage.getItem('gdriveFolderId');
        if (folderId) {
            let f = await fetch('https://www.googleapis.com/drive/v3/files/' + encodeURIComponent(folderId)
                + '?fields=name,mimeType,trashed&supportsAllDrives=true', auth);
            if (f.status === 404) throw testError('fail', who + ', but the Folder ID was not found in this account.');
            if (!f.ok) throw testError('fail', who + ', but the folder could not be read (HTTP ' + f.status + ').');
            let folder = await f.json();
            if (folder.mimeType !== 'application/vnd.google-apps.folder')
                throw testError('fail', who + ', but the Folder ID is a file, not a folder.');
            if (folder.trashed) throw testError('fail', who + ', but the folder "' + folder.name + '" is in the trash.');
            return who + '; syncing into "' + folder.name + '".';
        }
        return who + '.';
    }

    function base64Utf8(text) {
        let bytes = new TextEncoder().encode(text);
        let binary = '';
        bytes.forEach(function (b) { binary += String.fromCharCode(b); });
        return btoa(binary);
    }

    async function testWebdav() {
        let section = remoteForms.webdav();
        if (section === null) throw testError('fail', 'Check the highlighted fields first.');
        let url = $('#webdav-url').val();
        if (window.location.protocol === 'https:' && /^http:/i.test(url)) {
            throw testError('unknown', 'Browsers do not let an https page talk to a plain-http server, so this '
                + 'cannot be checked here. The 3DS is not affected; a debug sync (Y) will test it.');
        }
        let headers = { 'Depth': '0', 'Content-Type': 'application/xml; charset=utf-8' };
        if ($('#webdav-user').val() !== '' || $('#webdav-password').val() !== '')
            headers['Authorization'] = 'Basic ' + base64Utf8($('#webdav-user').val() + ':' + $('#webdav-password').val());
        let response;
        try {
            response = await fetch(url, {
                method: 'PROPFIND',
                headers: headers,
                body: '<?xml version="1.0" encoding="utf-8"?><d:propfind xmlns:d="DAV:"><d:prop><d:resourcetype/></d:prop></d:propfind>',
                credentials: 'omit',
                cache: 'no-store'
            });
        } catch (e) {
            // fetch() rejects the same way for "unreachable" and for "the
            // server does not allow web pages to ask" (CORS), and browsers
            // will not say which.
            throw testError('unknown', 'Could not get an answer from this browser. Most WebDAV servers do not '
                + 'accept requests from web pages (CORS), or the server may only be reachable from your home '
                + 'network. Neither means the 3DS will fail; a debug sync (Y) will test it.');
        }
        let reasons = {
            401: 'Login refused (401). Check user and password; with two-factor sign-in, use an app password.',
            403: 'Logged in, but not allowed to use this folder (403).',
            404: 'No folder at this URL (404). For Nextcloud the URL ends in /remote.php/dav/files/USERNAME/.',
            405: 'The server does not speak WebDAV at this URL (405).'
        };
        if (response.status !== 207) {
            throw testError('fail', reasons[response.status] || 'The server answered HTTP ' + response.status + '.');
        }
        let body = await response.text();
        if (!/collection/i.test(body)) throw testError('fail', 'Logged in, but this URL is a file, not a folder.');
        return 'Logged in, and the folder is there.';
    }

    const connectionTests = { dropbox: testDropbox, googledrive: testGoogleDrive, webdav: testWebdav };

    $('.test-connection').on('click', async function (e) {
        e.preventDefault();
        let $button = $(this);
        if ($button.hasClass('disabled')) return;
        let provider = $button.data('provider');
        $button.addClass('disabled');
        showTest(provider, 'busy', 'Testing…');
        try {
            showTest(provider, 'ok', await connectionTests[provider]());
        } catch (err) {
            showTest(provider, err.kind || 'unknown', err.kind ? err.message
                : 'Could not reach the service from this browser (' + err.message + ').');
        } finally {
            $button.removeClass('disabled');
        }
    });

    // --- Config generation ---
    function remoteSectionString(masked) {
        let secret = /^(password|clientsecret|appsecret|refreshtoken|token)$/i;
        let lines = ['[' + remoteSection.section + ']'];
        remoteSection.entries.forEach(function (kv) {
            lines.push(kv[0] + '=' + (masked && secret.test(kv[0]) ? '\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022' : kv[1]));
        });
        return lines.join('\n');
    }

    function getConfigString(masked) {
        let provider = selectedProvider || localStorage.getItem('provider');
        let strPaths = '';
        let strShallowPaths = '';
        let strUploadPaths = '';
        let strUploadShallowPaths = '';
        paths.forEach(function (path) {
            let name = path[0];
            let localPath = path[1];
            let recursive = path[2];
            let direction = path[3]; // 'both' or 'upload'
            if (direction === 'upload') {
                if (recursive) strUploadPaths += name + '=' + localPath + '\n';
                else strUploadShallowPaths += name + '=' + localPath + '\n';
            } else {
                if (recursive) strPaths += name + '=' + localPath + '\n';
                else strShallowPaths += name + '=' + localPath + '\n';
            }
        });
        let pathSections = '';
        if (strPaths) pathSections += '\n[Paths]\n' + strPaths;
        if (strShallowPaths) pathSections += '\n[ShallowPaths]\n' + strShallowPaths;
        if (strUploadPaths) pathSections += '\n[UploadPaths]\n' + strUploadPaths;
        if (strUploadShallowPaths) pathSections += '\n[UploadShallowPaths]\n' + strUploadShallowPaths;

        // The selected tile decides; a typed-in remote is only used while
        // its own tile is the selected one.
        if (remoteSection !== null && remoteSection.provider === selectedProvider) {
            return remoteSectionString(masked) + '\n' + pathSections;
        }

        let hide = function (value) {
            return masked ? '\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022' : value;
        };
        if (provider === 'googledrive') {
            let clientId = localStorage.getItem('gdriveClientId');
            let clientSecret = localStorage.getItem('gdriveClientSecret');
            let refreshToken = localStorage.getItem('gdriveRefreshToken');
            let folderId = localStorage.getItem('gdriveFolderId');
            let config = '[GoogleDrive]\nClientId=' + clientId
                + '\nClientSecret=' + hide(clientSecret)
                + '\nRefreshToken=' + hide(refreshToken);
            if (folderId) {
                config += '\nFolderId=' + folderId;
            }
            return config + '\n' + pathSections;
        } else {
            let refreshToken = localStorage.getItem('dropboxRefreshToken');
            let config;
            if (refreshToken) {
                config = '[Dropbox]\nAppKey=' + localStorage.getItem('dropboxClientId')
                    + '\nRefreshToken=' + hide(refreshToken);
            } else {
                // Only reachable for a session that authenticated before the
                // refresh-token flow; such a token expires in about 4 hours.
                config = '[Dropbox]\nToken=' + hide(localStorage.getItem('dropboxToken'));
            }
            return config + '\n' + pathSections;
        }
    }

    $('#download-config').on('click', function (e) {
        e.preventDefault();
        let blob = new Blob([getConfigString(false)], { type: "application/octet-stream;charset=utf-8" });
        const fileStream = streamSaver.createWriteStream('3DSync.ini', {
            size: blob.size
        });
        const readableStream = blob.stream();
        if (window.WritableStream && readableStream.pipeTo) {
            return readableStream.pipeTo(fileStream)
                .then(() => console.log('done writing'));
        }
        window.writer = fileStream.getWriter();
        const reader = readableStream.getReader();
        const pump = () => reader.read()
            .then(res => res.done
                ? writer.close()
                : writer.write(res.value).then(pump));
        pump();
    });

    $('#add-custom-path').on('click', function (e) {
        e.preventDefault();
        let id = Date.now();
        let $input = $('<div class="row">' +
            '<div class="input-field col s3"><input id="' + id + '-n" class="white-text" type="text"><label for="' + id + '-n" class="white-text">Name</label><span class="helper-text" data-error="Invalid name"></span></div>' +
            '<div class="input-field col s4"><input id="' + id + '" class="white-text path-custom" type="text"><label for="' + id + '" class="white-text">Path</label><span class="helper-text" data-error="Invalid path"></span></div>' +
            '<div class="col s1 valign-wrapper" style="padding-top:1.4rem"><label class="white-text"><input type="checkbox" class="filled-in path-recursive" checked><span class="white-text" style="font-size:0.82em;white-space:nowrap">Subdirs</span></label></div>' +
            '<div class="input-field col s2" style="padding-top:0.6rem"><select class="path-direction browser-default" style="color:#fff;background:transparent;border:1px solid rgba(255,255,255,0.5);padding:4px"><option value="both" selected>Both ways</option><option value="upload">Upload only</option></select></div>' +
            '<div class="col s2"><a href="#" class="btn-floating waves-effect waves-light red remove-custom-path"><i class="material-icons">remove</i></a></div></div>');
        $input.find('.remove-custom-path').on('click', function (e) {
            e.preventDefault();
            $(this).parent().parent().remove();
        });
        $(this).before($input);
    });

    const pathRegex = /^(\/?|)([\s\S]*?)((?:\.{1,2}|[^\/]+?|)(\.[^.\/]*|))(?:[\/]*)$/;

    function pathParse(path) {
        let parts = pathRegex.exec(path).slice(1);
        if (!parts || parts.length !== 4) {
            return false;
        }
        parts[1] = parts[1] || '';
        parts[2] = parts[2] || '';
        parts[3] = parts[3] || '';

        return {
            root: parts[0],
            dir: parts[0] + parts[1].slice(0, -1),
            base: parts[2],
            ext: parts[3],
            name: parts[2].slice(0, parts[2].length - parts[3].length)
        };
    }


    $('#folders-confirm').on('click', function (e) {
        e.preventDefault();
        paths = [];
        let error = false;
        $('#paths-presets input:checked, #paths-custom input.path-custom').each(function () {
            let $this = $(this);
            if ($this.hasClass('path-custom')) {
                let path = $this.val();
                let pathCheck = pathParse(path);
                if (pathCheck === false) {
                    error = true;
                    $this.addClass('invalid');
                } else {
                    let pathSync = '';
                    if (pathCheck['ext'] === '') {
                        pathSync += pathCheck['dir'];
                        if (pathCheck['dir'] !== '/') {
                            pathSync += '/';
                        }
                        pathSync += pathCheck['base'];
                    } else {
                        if (pathCheck['dir'] === '') {
                            error = true;
                        }
                        pathSync += pathCheck['dir'];
                    }
                    if (pathSync.startsWith('/') === false) pathSync = '/' + pathSync;
                    if (error === false) {
                        $this.removeClass('invalid');
                        let $name = $('#' + $this.attr('id') + '-n');
                        if ($name.val() === '') {
                            error = true;
                            $name.addClass('invalid');
                        } else {
                            $name.removeClass('invalid');
                            let isRecursive = $this.closest('.row').find('.path-recursive').prop('checked');
                            let direction = $this.closest('.row').find('.path-direction').val() || 'both';
                            paths.push([$name.val(), pathSync, isRecursive, direction]);
                        }
                    }
                }
            } else {
                paths.push([$this.next().text(), $this.data('path'), true, 'both']);
            }
        });
        if (error === false) {
            $('#config-preview').text(getConfigString(true));
            stepperInstace.nextStep();
        }
    });
});
