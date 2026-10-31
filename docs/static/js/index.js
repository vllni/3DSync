$(function () {
    const DEFAULT_DROPBOX_CLIENT_ID = 'y88zxdudhuoo41q';
    const LOGIN_CHANNEL = '3dsync-login';

    // --- Nothing secret in browser storage ---
    // Earlier versions of this page carried the Google client secret, the
    // OAuth tokens and the PKCE verifiers across a full-page redirect in
    // localStorage.  Logins now happen in a popup (below) and everything stays
    // in this tab's memory, so whatever such a version left behind is deleted
    // unread.  Only the Dropbox expert app key is remembered: it is not secret.
    ['gdriveClientSecret', 'gdriveRefreshToken', 'dropboxRefreshToken', 'dropboxToken', 'gdriveState',
        'gdriveCodeVerifier', 'dropboxState', 'dropboxCodeVerifier', 'gdriveClientId', 'gdriveFolderId',
        'provider'].forEach(function (k) {
        localStorage.removeItem(k);
    });

    // --- Back from a login: this page is the popup ---
    // Both providers redirect to this page with ?code=&state= (or ?error=).
    // Loaded like that, the page is the login popup: it hands the answer to
    // the tab that opened it over a BroadcastChannel and closes.
    // window.opener would be the obvious route, but the providers' login
    // pages send Cross-Origin-Opener-Policy headers that may cut the popup
    // off from its opener for good; a same-origin channel does not depend on
    // it.  The opening tab alone holds the PKCE verifier, so a code is
    // worthless to anything else listening.
    let queryParams = new URLSearchParams(window.location.search);
    if (queryParams.has('state') && (queryParams.has('code') || queryParams.has('error'))) {
        history.replaceState(null, '', window.location.pathname);
        runAsLoginPopup(queryParams);
        return;
    }

    function runAsLoginPopup(params) {
        let result = { type: 'result', state: params.get('state'), code: params.get('code'), error: params.get('error') };
        let $note = $('<div class="card-panel blue darken-2 white-text"></div>')
            .text('Passing the login back to the configurator…');
        $('main > .container').empty().append($('<div class="row"></div>').append($note));
        let answered = false;
        let channel = new BroadcastChannel(LOGIN_CHANNEL);
        channel.onmessage = function (e) {
            if (!e.data || e.data.type !== 'ack' || e.data.state !== result.state) return;
            answered = true;
            $note.text('Done — this window can be closed.');
            window.close();
        };
        channel.postMessage(result);
        setTimeout(function () {
            if (!answered) {
                $note.text('No configurator tab took this login. Go back to the configurator and press its '
                    + 'login button again; this window can be closed.');
            }
        }, 3000);
    }

    let paths = [];

    let stepperInstace = new MStepper(document.querySelector('.stepper'), {
        firstActive: 0,
        autoFormCreation: false,
        // Its own title navigation would also jump *forward*, past the checks
        // behind Continue and Confirm; going back is handled below instead.
        stepTitleNavigation: false,
    });

    // --- Step titles: reopen a step already passed ---
    // Every step before the open one is done, so its title reopens it; the
    // open step and those after it can only be reached with the buttons.
    const $steps = $('.stepper > li.step');

    function activeStepIndex() {
        return $steps.index($steps.filter('.active'));
    }

    function markReachableSteps() {
        let active = activeStepIndex();
        $steps.each(function (i) {
            let reachable = active >= 0 && i < active;
            $(this).toggleClass('step-reachable', reachable);
            let $title = $(this).children('.step-title');
            if (reachable) $title.attr({ role: 'button', tabindex: '0', title: 'Back to this step' });
            else $title.removeAttr('role tabindex title');
        });
    }

    $steps.children('.step-title').on('click keydown', function (e) {
        if (e.type === 'keydown' && e.key !== 'Enter' && e.key !== ' ') return;
        let index = $steps.index($(this).parent());
        if (index < activeStepIndex()) {
            e.preventDefault();
            stepperInstace.openStep(index);
        }
    });

    // The stepper marks the open step with "active"; following that class
    // covers every way of moving — buttons and titles.
    new MutationObserver(markReachableSteps).observe(document.querySelector('.stepper'), {
        subtree: true, attributes: true, attributeFilter: ['class']
    });
    markReachableSteps();

    // --- Remotes ---
    // remotes holds every saved remote, in the order they were added; draft
    // is the one open in the editor, a copy until it is saved.  All of it,
    // passwords and tokens included, lives in this tab only, and a reload
    // forgets it.
    const TYPES = {
        dropbox: { section: 'Dropbox', name: 'Dropbox', icon: '<i class="fa fa-dropbox"></i>' },
        googledrive: { section: 'GoogleDrive', name: 'Drive', icon: '<i class="material-icons">add_to_drive</i>' },
        smb: { section: 'SMB', name: 'SMB', icon: '<i class="material-icons">storage</i>' },
        ftp: { section: 'FTP', name: 'FTP', icon: '<i class="material-icons">folder_shared</i>' },
        webdav: { section: 'WebDAV', name: 'WebDAV', icon: '<i class="material-icons">cloud_queue</i>' }
    };

    // The inputs of each form, saved with the remote so it can be edited again.
    const FORM_FIELDS = {
        dropbox: ['dropbox-expert-mode', 'dropbox-client-id'],
        googledrive: ['gdrive-client-id', 'gdrive-client-secret', 'gdrive-folder-id'],
        smb: ['smb-server', 'smb-share', 'smb-user', 'smb-password', 'smb-domain', 'smb-path'],
        ftp: ['ftp-host', 'ftp-port', 'ftp-user', 'ftp-password', 'ftp-path', 'ftp-tls', 'ftp-mode'],
        webdav: ['webdav-url', 'webdav-user', 'webdav-password']
    };

    let remotes = []; // { uid, type, label, role, fields, auth, entries }
    let excluded = {}; // uid -> { lowercased folder name: true } it does not sync
    let draft = null;
    let draftSnapshot = '';
    let nextUid = 1;

    // What the 3DS calls it: "SMB", or "SMB (backup)" for a labelled one.
    function displayName(remote) {
        let name = TYPES[remote.type].name;
        return remote.label ? name + ' (' + remote.label + ')' : name;
    }

    function sectionName(remote) {
        return TYPES[remote.type].section + (remote.label ? ':' + remote.label : '');
    }

    function remoteDetail(remote) {
        let f = remote.fields;
        switch (remote.type) {
            case 'dropbox': return 'Logged in';
            case 'googledrive': return f['gdrive-folder-id'] ? 'Folder ' + f['gdrive-folder-id'] : 'My Drive';
            case 'smb': return '\\\\' + f['smb-server'] + '\\' + f['smb-share']
                + (f['smb-path'] ? '\\' + f['smb-path'].replace(/\//g, '\\') : '');
            case 'ftp': return f['ftp-host'] + (f['ftp-port'] ? ':' + f['ftp-port'] : '')
                + (f['ftp-path'] ? ' — ' + f['ftp-path'] : '');
            case 'webdav': return f['webdav-url'];
        }
        return '';
    }

    function renderRemotes() {
        let $list = $('#remote-list').empty();
        remotes.forEach(function (remote) {
            let $row = $('<div class="remote-row"></div>').toggleClass('editing', !!draft && draft.uid === remote.uid);
            $row.append($('<span class="remote-row-icon" aria-hidden="true"></span>').html(TYPES[remote.type].icon));
            let $text = $('<span class="remote-row-text"></span>');
            let $name = $('<span class="remote-row-name"></span>').text(displayName(remote));
            if (remote.role !== 'sync') $name.append($('<span class="role-badge"></span>').text(remote.role));
            $text.append($name, $('<span class="remote-row-detail"></span>').text(remoteDetail(remote)));
            $row.append($text);
            let $edit = $('<a href="#" class="btn-flat white-text remote-row-button" title="Edit"><i class="material-icons">edit</i></a>')
                .attr('aria-label', 'Edit ' + displayName(remote));
            let $remove = $('<a href="#" class="btn-flat white-text remote-row-button" title="Remove"><i class="material-icons">delete</i></a>')
                .attr('aria-label', 'Remove ' + displayName(remote));
            $edit.on('click', function (e) {
                e.preventDefault();
                openEditor(remote.type, remote);
            });
            $remove.on('click', function (e) {
                e.preventDefault();
                if (draft && draft.uid === remote.uid) closeEditor();
                remotes = remotes.filter(function (r) { return r.uid !== remote.uid; });
                delete excluded[remote.uid];
                renderRemotes();
            });
            $row.append($edit, $remove);
            $list.append($row);
        });
        $('#remote-list-empty').toggle(remotes.length === 0);
    }

    function editorState() {
        return JSON.stringify({
            fields: draft ? captureFields(draft.type) : null,
            label: $('#remote-label').val(),
            role: $('#remote-role').val(),
            auth: draft ? draft.auth : null
        });
    }

    function editorDirty() {
        return draft !== null && editorState() !== draftSnapshot;
    }

    function captureFields(type) {
        let fields = {};
        FORM_FIELDS[type].forEach(function (id) {
            let $input = $('#' + id);
            fields[id] = $input.is(':checkbox') ? $input.prop('checked') : $input.val();
        });
        return fields;
    }

    function resetForm(type) {
        FORM_FIELDS[type].forEach(function (id) {
            let $input = $('#' + id);
            if ($input.is(':checkbox')) $input.prop('checked', false);
            else if ($input.is('select')) $input.val($input.find('option[selected]').val());
            else $input.val('');
            setFieldError($input, null);
        });
        $('#test-result-' + type).removeClass('ok fail unknown').empty();
        $('#login-status-' + type).removeClass('ok fail unknown').empty();
        $('#gdrive-json-status').empty();
    }

    function loadFields(type, fields) {
        FORM_FIELDS[type].forEach(function (id) {
            if (!(id in fields)) return;
            let $input = $('#' + id);
            if ($input.is(':checkbox')) $input.prop('checked', fields[id]);
            else $input.val(fields[id]);
        });
    }

    function openEditor(type, remote) {
        if (editorDirty() && !confirm('Discard the changes to ' + $('#remote-editor-title').text() + '?')) return;
        draft = remote ? $.extend(true, {}, remote)
            : { uid: nextUid++, type: type, label: '', role: 'sync', fields: {}, auth: null, isNew: true };
        resetForm(type);
        if (remote) {
            loadFields(type, remote.fields);
        } else if (type === 'dropbox') {
            // Expert mode is remembered for the next Dropbox remote.
            let storedClientId = localStorage.getItem('dropboxClientId');
            if (storedClientId && storedClientId !== DEFAULT_DROPBOX_CLIENT_ID) {
                $('#dropbox-client-id').val(storedClientId);
                $('#dropbox-expert-mode').prop('checked', true);
            }
        }
        $('#dropbox-expert-panel').toggle($('#dropbox-expert-mode').prop('checked'));
        $('#remote-label').val(draft.label);
        setFieldError($('#remote-label'), null);
        $('#remote-role').val(draft.role);
        updateRoleNote();
        updateLabelHint();
        updateFtpNote();
        updateWebdavNote();
        renderLoginStatus();

        $('#remote-editor-title').text(remote ? 'Edit ' + displayName(remote) : 'New ' + TYPES[type].name + ' remote');
        $('.auth-settings[data-provider]').removeClass('active');
        $('#auth-settings-' + type).addClass('active');
        $('.auth-tile').removeClass('selected');
        $('.auth-tile[data-provider="' + type + '"]').addClass('selected');
        $('#remote-editor').addClass('open');
        $('#remote-editor-hint, #auth-continue-hint').text('').removeClass('warn');
        M.updateTextFields();
        draftSnapshot = editorState();
        renderRemotes();
    }

    function closeEditor() {
        draft = null;
        draftSnapshot = '';
        $('#remote-editor').removeClass('open');
        $('.auth-tile').removeClass('selected');
        renderRemotes();
    }

    function addTile($tile) {
        openEditor($tile.data('provider'), null);
        document.getElementById('remote-editor').scrollIntoView({ behavior: 'smooth', block: 'start' });
    }

    $('.auth-tile').on('click', function () {
        addTile($(this));
    }).on('keydown', function (e) {
        if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            addTile($(this));
        }
    });

    function sameType() {
        return remotes.filter(function (r) { return r.type === draft.type && r.uid !== draft.uid; });
    }

    // Two remotes of one type are told apart by the label; the 3DS lowercases
    // section names, so labels differing only in case would be the same one.
    function labelProblem(label) {
        if (label !== '' && !/^[A-Za-z0-9_-]+$/.test(label)) return 'Letters, digits, - and _ only';
        let name = TYPES[draft.type].name;
        let taken = sameType().some(function (r) { return r.label.toLowerCase() === label.toLowerCase(); });
        if (taken && label === '') return 'Your other ' + name + ' remote has no label, so this one needs one, e.g. backup';
        if (taken) return 'Your other ' + name + ' remote already has this label';
        return null;
    }

    function updateLabelHint() {
        let name = TYPES[draft.type].name;
        $('#remote-label-hint').html(sameType().length
            ? 'Tells it apart from your other ' + name + ' remote, e.g. <strong>backup</strong>'
            : 'Optional. Leave blank for your first ' + name + ' remote: a 3DS that already syncs with one '
            + 'then picks up where it left off.');
    }

    function updateRoleNote() {
        let notes = {
            backup: 'The 3DS only ever sends to it: nothing is downloaded from it, a change made there is never '
                + 'asked about, and files only it has are left alone.',
            source: 'The 3DS only ever takes from it: nothing is written there, and a change made there reaches '
                + 'the 3DS and your other remotes. It takes no part in upload-only folders.'
        };
        $('#remote-role-note').text(notes[$('#remote-role').val()] || '');
    }

    $('#remote-role').on('change', updateRoleNote);
    $('#remote-label').on('blur', function () {
        $(this).val($(this).val().trim());
    });

    function editorHint(text) {
        $('#remote-editor-hint').text(text).addClass('warn');
    }

    // Validates the editor and stores the remote; false when something has
    // to be fixed first, with the fields marked and a hint beside Save.
    function saveDraft() {
        let entries = remoteForms[draft.type]();
        let label = $('#remote-label').val().trim();
        $('#remote-label').val(label);
        let problem = labelProblem(label);
        setFieldError($('#remote-label'), problem);
        if (typeof entries === 'string') {
            editorHint(entries);
            return false;
        }
        if (entries === null || problem) {
            editorHint('Check the highlighted fields.');
            $('#remote-editor .invalid').first().focus();
            return false;
        }
        draft.label = label;
        draft.role = $('#remote-role').val();
        draft.fields = captureFields(draft.type);
        draft.entries = entries;
        delete draft.isNew;
        let saved = draft;
        let at = remotes.findIndex(function (r) { return r.uid === saved.uid; });
        if (at >= 0) remotes[at] = saved;
        else remotes.push(saved);
        closeEditor();
        return true;
    }

    $('#remote-save').on('click', function (e) {
        e.preventDefault();
        saveDraft();
    });

    $('#remote-cancel').on('click', function (e) {
        e.preventDefault();
        closeEditor();
    });

    $('#auth-continue').on('click', function (e) {
        e.preventDefault();
        // An editor left open with nothing changed is simply closed; one with
        // changes is saved, as if Save had been pressed.
        if (draft !== null) {
            if (!editorDirty()) closeEditor();
            else if (!saveDraft()) {
                continueHint('Save or cancel the remote you are editing first.');
                return;
            }
        }
        if (remotes.length === 0) {
            continueHint('Add a remote first.');
            return;
        }
        $('#auth-continue-hint').text('').removeClass('warn');
        stepperInstace.nextStep();
        renderRouting();
    });

    function continueHint(text) {
        $('#auth-continue-hint').text(text).addClass('warn');
    }

    renderRemotes();

    // --- Logins (Dropbox, Google Drive) ---
    // In a popup, so the remotes already added survive: a full-page redirect
    // would reload this page and forget them.  The popup comes back to this
    // page, which passes the answer on (runAsLoginPopup() above).
    const pendingLogins = {}; // state -> { provider, uid, verifier, clientId, clientSecret }
    const loginChannel = new BroadcastChannel(LOGIN_CHANNEL);
    loginChannel.onmessage = function (e) {
        let msg = e.data;
        if (!msg || msg.type !== 'result' || !Object.prototype.hasOwnProperty.call(pendingLogins, msg.state)) return;
        let login = pendingLogins[msg.state];
        delete pendingLogins[msg.state];
        loginChannel.postMessage({ type: 'ack', state: msg.state });
        finishLogin(login, msg);
    };

    function showLogin(provider, kind, text) {
        let icons = { busy: 'hourglass_empty', ok: 'check_circle', fail: 'error_outline' };
        $('#login-status-' + provider)
            .removeClass('ok fail unknown')
            .addClass(kind === 'busy' ? '' : kind)
            .html('<i class="material-icons">' + icons[kind] + '</i>')
            .append(document.createTextNode(text));
    }

    function renderLoginStatus() {
        if (draft && draft.auth) showLogin(draft.type, 'ok', 'Logged in');
    }

    // Opened straight from the click, before anything is awaited: browsers
    // block a popup opened later.  Its address is set once the PKCE challenge
    // is ready.
    function openLoginPopup(provider) {
        let w = 520, h = 680;
        let popup = window.open('about:blank', '3dsync-login', 'popup,width=' + w + ',height=' + h
            + ',left=' + Math.max(0, window.screenX + (window.outerWidth - w) / 2)
            + ',top=' + Math.max(0, window.screenY + (window.outerHeight - h) / 2));
        if (!popup) {
            showLogin(provider, 'fail', 'The browser blocked the login window. Allow pop-ups for this site, '
                + 'then press the button again.');
        }
        return popup;
    }

    async function finishLogin(login, msg) {
        let stillOpen = function () { return draft !== null && draft.uid === login.uid; };
        if (msg.error || !msg.code) {
            if (stillOpen()) showLogin(login.provider, 'fail', msg.error === 'access_denied'
                ? 'Login cancelled.' : 'Login failed (' + (msg.error || 'no code') + ').');
            return;
        }
        if (stillOpen()) showLogin(login.provider, 'busy', 'Finishing the login…');
        let result = login.provider === 'dropbox'
            ? await exchangeDropboxCode(msg.code, login) : await exchangeGoogleCode(msg.code, login);
        // The editor may have moved on to another remote meanwhile; the token
        // is then dropped with the draft it belonged to.
        if (!stillOpen()) return;
        if (result.error) {
            showLogin(login.provider, 'fail', result.error);
            return;
        }
        draft.auth = { clientId: login.clientId, clientSecret: login.clientSecret || '', refreshToken: result.token };
        renderLoginStatus();
        $('#remote-editor-hint').text('').removeClass('warn');
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

    // Both providers send the login back to this very page.
    function redirectUri() {
        return window.location.origin + window.location.pathname;
    }

    async function exchangeGoogleCode(code, login) {
        try {
            let response = await fetch('https://oauth2.googleapis.com/token', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: new URLSearchParams({
                    code: code,
                    client_id: login.clientId,
                    client_secret: login.clientSecret,
                    redirect_uri: redirectUri(),
                    code_verifier: login.verifier,
                    grant_type: 'authorization_code'
                })
            });
            let data = await response.json();
            if (data.refresh_token) return { token: data.refresh_token };
            console.error('No refresh_token in response:', data);
            if (data.error === 'invalid_client') {
                return { error: 'Google does not accept this Client Secret for this Client ID. Check that it is '
                    + 'the current secret (not one you have disabled), then log in again.' };
            }
            return { error: 'Google refused the login (' + (data.error_description || data.error) + ').' };
        } catch (e) {
            console.error('Token exchange failed:', e);
            return { error: 'Could not reach Google to finish the login. Please try again.' };
        }
    }

    async function exchangeDropboxCode(code, login) {
        try {
            let response = await fetch('https://api.dropbox.com/oauth2/token', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: new URLSearchParams({
                    code: code,
                    grant_type: 'authorization_code',
                    client_id: login.clientId,
                    redirect_uri: redirectUri(),
                    code_verifier: login.verifier
                })
            });
            let data = await response.json();
            // token_access_type=offline is what makes Dropbox return this; a
            // bare access token would expire after about four hours.
            if (data.refresh_token) return { token: data.refresh_token };
            console.error('No refresh_token in response:', data);
            return { error: 'Dropbox refused the login (' + (data.error_description || data.error) + ').' };
        } catch (e) {
            console.error('Dropbox token exchange failed:', e);
            return { error: 'Could not reach Dropbox to finish the login. Please try again.' };
        }
    }

    // --- Dropbox expert mode: bring your own app key ---
    $('#dropbox-redirect-uri').text(redirectUri());

    $('#dropbox-copy-redirect-uri').on('click', function (e) {
        e.preventDefault();
        let $status = $('#dropbox-copy-status');
        navigator.clipboard.writeText(redirectUri()).then(function () {
            $status.html('<span class="green-text"><i class="material-icons" style="font-size:1em;vertical-align:middle">check_circle</i></span>');
        }, function () {
            $status.html('<span class="red-text">Copy failed</span>');
        });
    });

    $('#dropbox-expert-mode').on('change', function () {
        $('#dropbox-expert-panel').toggle(this.checked);
        M.updateTextFields();
    });

    // The app key a login now would use.
    function dropboxClientId() {
        if (!$('#dropbox-expert-mode').prop('checked')) return DEFAULT_DROPBOX_CLIENT_ID;
        return $('#dropbox-client-id').val().trim() || DEFAULT_DROPBOX_CLIENT_ID;
    }

    $('#dropbox-login').on('click', async function (e) {
        e.preventDefault();
        if (!draft) return;
        let popup = openLoginPopup('dropbox');
        if (!popup) return;
        let clientId = dropboxClientId();
        localStorage.setItem('dropboxClientId', clientId);

        let state = generateCodeVerifier();
        let codeVerifier = generateCodeVerifier();
        pendingLogins[state] = { provider: 'dropbox', uid: draft.uid, verifier: codeVerifier, clientId: clientId };
        // A second Dropbox remote is normally a second account; without this
        // Dropbox logs straight in to the one the browser is signed in to.
        let another = sameType().length > 0;
        let codeChallenge = await generateCodeChallenge(codeVerifier);
        showLogin('dropbox', 'busy', 'Log in in the window that opened…');

        // PKCE + token_access_type=offline yields a refresh token, so the 3DS
        // can sync without re-authenticating every four hours.
        popup.location.href = 'https://www.dropbox.com/oauth2/authorize?client_id=' + encodeURIComponent(clientId)
            + '&response_type=code'
            + '&redirect_uri=' + encodeURIComponent(redirectUri())
            + '&token_access_type=offline'
            + '&state=' + encodeURIComponent(state)
            + '&code_challenge=' + codeChallenge
            + '&code_challenge_method=S256'
            + (another ? '&force_reauthentication=true' : '');
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
        if (!draft) return;
        let clientId = $('#gdrive-client-id').val().trim();
        let clientSecret = $('#gdrive-client-secret').val().trim();
        setFieldError($('#gdrive-client-id'), clientId ? null : 'Required');
        setFieldError($('#gdrive-client-secret'), clientSecret ? null : 'Required');
        if (!clientId || !clientSecret) return;

        let popup = openLoginPopup('googledrive');
        if (!popup) return;
        let state = generateCodeVerifier();
        let codeVerifier = generateCodeVerifier();
        pendingLogins[state] = {
            provider: 'googledrive', uid: draft.uid, verifier: codeVerifier,
            clientId: clientId, clientSecret: clientSecret
        };
        let codeChallenge = await generateCodeChallenge(codeVerifier);
        showLogin('googledrive', 'busy', 'Log in in the window that opened…');

        // select_account: a second Drive remote is normally a second account.
        popup.location.href = 'https://accounts.google.com/o/oauth2/v2/auth'
            + '?client_id=' + encodeURIComponent(clientId)
            + '&redirect_uri=' + encodeURIComponent(redirectUri())
            + '&response_type=code'
            + '&scope=' + encodeURIComponent('https://www.googleapis.com/auth/drive')
            + '&access_type=offline'
            + '&prompt=' + encodeURIComponent('select_account consent')
            + '&state=' + encodeURIComponent(state)
            + '&code_challenge=' + codeChallenge
            + '&code_challenge_method=S256';
    });

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

    // One per type: validates the open form and returns the section's
    // entries, null when a field is marked, or a string saying what to do.
    const remoteForms = {
        dropbox: function () {
            let name = TYPES.dropbox.name;
            if (!draft.auth) return 'Log in with ' + name + ' first.';
            if (draft.auth.clientId !== dropboxClientId())
                return 'The app key changed since the login. Log in with ' + name + ' again.';
            return [['AppKey', draft.auth.clientId], ['RefreshToken', draft.auth.refreshToken]];
        },
        googledrive: function () {
            ['gdrive-client-id', 'gdrive-client-secret', 'gdrive-folder-id'].forEach(trimField);
            if (!checkFields(['gdrive-client-id', 'gdrive-client-secret', 'gdrive-folder-id'], [])) return null;
            if (!draft.auth) return 'Log in with Google Drive first.';
            if (draft.auth.clientId !== $('#gdrive-client-id').val() ||
                draft.auth.clientSecret !== $('#gdrive-client-secret').val())
                return 'The Client ID or Secret changed since the login. Log in with Google Drive again.';
            return entriesFrom([
                ['ClientId', draft.auth.clientId],
                ['ClientSecret', draft.auth.clientSecret],
                ['RefreshToken', draft.auth.refreshToken],
                ['FolderId', $('#gdrive-folder-id').val()]
            ]);
        },
        smb: function () {
            ['smb-server', 'smb-share', 'smb-user', 'smb-domain', 'smb-path'].forEach(trimField);
            normalizeSmb();
            if (!checkFields(['smb-server', 'smb-share', 'smb-user', 'smb-password', 'smb-domain', 'smb-path'],
                ['smb-server', 'smb-share'])) return null;
            return entriesFrom([
                ['Server', $('#smb-server').val()],
                ['Share', $('#smb-share').val()],
                ['User', $('#smb-user').val()],
                ['Password', $('#smb-password').val()],
                ['Domain', $('#smb-domain').val()],
                ['Path', $('#smb-path').val()]
            ]);
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
            // An anonymous login (no User, or the names servers take as one)
            // can rarely write to the directory it starts in: stock servers
            // allow uploads in one folder such as pub/, and without Path= the
            // first sync fails on the 3DS when it cannot create its folder.
            let anonymous = /^(anonymous|ftp)?$/i.test($('#ftp-user').val());
            if (anonymous && $('#ftp-path').val().replace(/^\/+|\/+$/g, '') === '' &&
                !iniValueProblem($('#ftp-path').val())) {
                setFieldError($('#ftp-path'), 'Required for anonymous FTP: the folder uploads are '
                    + 'allowed in, e.g. pub');
                ok = false;
            }
            if (!ok) return null;
            return entriesFrom([
                ['Host', $('#ftp-host').val()],
                ['Port', port],
                ['User', $('#ftp-user').val()],
                ['Password', $('#ftp-password').val()],
                ['Path', $('#ftp-path').val()],
                ['TLS', $('#ftp-tls').val()],
                ['Mode', $('#ftp-mode').val()]
            ]);
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
            return entriesFrom([
                ['Url', $('#webdav-url').val()],
                ['User', $('#webdav-user').val()],
                ['Password', $('#webdav-password').val()]
            ]);
        }
    };

    // --- Test connection ---
    // Only what a web page can reach: the Dropbox and Google APIs allow
    // cross-origin requests, and a WebDAV server may.  SMB and FTP are not
    // HTTP, so their settings say how to test from the 3DS instead.  Each
    // outcome says which of three things it is: the settings work, they are
    // wrong, or the browser could not tell (which says nothing about the 3DS).
    // A test always tries the remote open in the editor.
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
        if (!draft.auth) throw testError('fail', 'Log in with Dropbox first.');
        // What the 3DS does on every run: trade the refresh token for an
        // access token.
        let r = await postForm('https://api.dropbox.com/oauth2/token', {
            grant_type: 'refresh_token',
            refresh_token: draft.auth.refreshToken,
            client_id: draft.auth.clientId
        });
        if (!r.data.access_token) {
            throw testError('fail', r.data.error === 'invalid_grant'
                ? 'The login has expired or was revoked. Log in with Dropbox again.'
                : 'Dropbox refused the login (' + (r.data.error_description || r.data.error || 'HTTP ' + r.status) + ').');
        }
        let response = await fetch('https://api.dropboxapi.com/2/users/get_current_account', {
            method: 'POST',
            headers: { 'Authorization': 'Bearer ' + r.data.access_token, 'Content-Type': 'application/json' },
            body: 'null'
        });
        if (response.status === 401) throw testError('fail', 'The login is no longer valid. Log in with Dropbox again.');
        if (!response.ok) throw testError('fail', 'Dropbox answered HTTP ' + response.status + '.');
        let account = await response.json();
        return 'Connected as ' + account.name.display_name + ' (' + account.email + ').';
    }

    async function testGoogleDrive() {
        if (!draft.auth) throw testError('fail', 'Log in with Google Drive first.');
        let r = await postForm('https://oauth2.googleapis.com/token', {
            grant_type: 'refresh_token',
            refresh_token: draft.auth.refreshToken,
            client_id: draft.auth.clientId,
            client_secret: draft.auth.clientSecret
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

        let folderId = $('#gdrive-folder-id').val().trim();
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
        if (remoteForms.webdav() === null) throw testError('fail', 'Check the highlighted fields first.');
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
        if ($button.hasClass('disabled') || !draft) return;
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

    // --- Remotes per folder ---
    // With two remotes or more, a table of folder by remote, all ticked to
    // begin with.  What is stored is what was unticked, by folder name, so a
    // folder added later is synced everywhere until told otherwise.  It
    // becomes Only= on the remotes that leave a folder out.

    // The names of the folders chosen in this step right now, as typed.
    function currentFolderNames() {
        let names = [];
        $('#paths-presets input:checked').each(function () {
            names.push($(this).next().text());
        });
        $('#paths-custom input.path-custom').each(function () {
            let name = $('#' + $(this).attr('id') + '-n').val().trim();
            if (name !== '') names.push(name);
        });
        return names.filter(function (name, i) {
            return names.findIndex(function (n) { return n.toLowerCase() === name.toLowerCase(); }) === i;
        });
    }

    function syncsFolder(remote, name) {
        return remotes.length < 2 || !(excluded[remote.uid] || {})[name.toLowerCase()];
    }

    function renderRouting() {
        let names = currentFolderNames();
        $('#routing-section').toggle(remotes.length > 1);
        let $table = $('<table></table>');
        let $head = $('<tr></tr>').append($('<th>Folder</th>'));
        remotes.forEach(function (remote) {
            $head.append($('<th></th>').text(displayName(remote)));
        });
        $table.append($('<thead></thead>').append($head));
        let $body = $('<tbody></tbody>');
        names.forEach(function (name) {
            let $row = $('<tr></tr>').append($('<td></td>').text(name));
            remotes.forEach(function (remote) {
                let $box = $('<input type="checkbox" class="filled-in">')
                    .prop('checked', syncsFolder(remote, name))
                    .attr('aria-label', name + ' on ' + displayName(remote))
                    .on('change', function () {
                        let set = excluded[remote.uid] = excluded[remote.uid] || {};
                        if (this.checked) delete set[name.toLowerCase()];
                        else set[name.toLowerCase()] = true;
                        $('#routing-note').text('').removeClass('warn');
                    });
                $row.append($('<td></td>').append($('<label></label>').append($box, '<span></span>')));
            });
            $body.append($row);
        });
        if (names.length === 0)
            $body.append($('<tr><td class="routing-empty">Choose a folder above first.</td></tr>'));
        $('#routing-table').empty().append($table.append($body));
    }

    $('#paths-presets, #paths-custom').on('input change', 'input', renderRouting);

    // The folders a remote is limited to, or null when it syncs all of them.
    function onlyFolders(remote) {
        let names = paths.map(function (p) { return p[0]; });
        let kept = names.filter(function (name) { return syncsFolder(remote, name); });
        return kept.length === names.length ? null : kept;
    }

    // What keeps the routing from being written, or null.
    function routingProblem() {
        // A source is never written to, so it takes no part in one.
        for (let i = 0; i < paths.length; i++) {
            let name = paths[i][0];
            if (paths[i][3] === 'upload' &&
                !remotes.some(function (r) { return r.role !== 'source' && syncsFolder(r, name); }))
                return '"' + name + '" is upload-only, and a Source remote is never written to. '
                    + 'Sync it both ways, or send it to another remote.';
        }
        if (remotes.length < 2) return null;
        for (let i = 0; i < paths.length; i++) {
            let name = paths[i][0];
            if (!remotes.some(function (r) { return syncsFolder(r, name); }))
                return 'No remote syncs "' + name + '". Tick at least one.';
        }
        for (let i = 0; i < remotes.length; i++) {
            let only = onlyFolders(remotes[i]);
            if (only === null) continue;
            // A remote with nothing ticked would get an empty Only=, which the
            // 3DS reads as "every folder".
            if (only.length === 0)
                return displayName(remotes[i]) + ' syncs no folder. Tick one, or remove it in step 1.';
            // Only= is a list split on commas and spaces.
            let bad = only.find(function (name) { return /[\s,;]/.test(name); });
            if (bad !== undefined)
                return '"' + bad + '" cannot be listed for ' + displayName(remotes[i]) + ': rename the folder '
                    + 'without spaces or commas, or tick it for every remote.';
        }
        return null;
    }

    // --- Config generation ---
    function remoteSectionString(remote, masked) {
        let secret = /^(password|clientsecret|appsecret|refreshtoken|token)$/i;
        let lines = ['[' + sectionName(remote) + ']'];
        remote.entries.forEach(function (kv) {
            lines.push(kv[0] + '=' + (masked && secret.test(kv[0]) ? '••••••••' : kv[1]));
        });
        if (remote.role !== 'sync') lines.push('Role=' + remote.role);
        let only = onlyFolders(remote);
        if (only !== null) lines.push('Only=' + only.join(', '));
        return lines.join('\n') + '\n';
    }

    function getConfigString(masked) {
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
        // Written only when filled: no [Filter] section means all files.
        let fileFilter = $('#file-filter').val().trim();
        if (fileFilter) pathSections += '\n[Filter]\nExtensions=' + fileFilter + '\n';

        return remotes.map(function (remote) { return remoteSectionString(remote, masked); }).join('\n')
            + pathSections;
    }

    // A few hundred bytes: a Blob and a link with the download attribute is all
    // it takes, in every current browser.  The file holds passwords and tokens,
    // so it is built and saved entirely inside this page — never handed to a
    // third-party helper page or service worker to stream.
    $('#download-config').on('click', function (e) {
        e.preventDefault();
        let blob = new Blob([getConfigString(false)], { type: 'application/octet-stream' });
        let url = URL.createObjectURL(blob);
        let link = document.createElement('a');
        link.href = url;
        link.download = '3DSync.ini';
        document.body.appendChild(link);
        link.click();
        link.remove();
        // Not revoked at once: Firefox may still be reading the URL when
        // click() returns, and a revoked one fails the download.
        setTimeout(function () { URL.revokeObjectURL(url); }, 40000);
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
            renderRouting();
        });
        $(this).before($input);
        renderRouting();
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
        // The 3DS splits the list on commas and spaces only, so "sav;bin"
        // would be read as one extension that never matches.
        let $filter = $('#file-filter');
        $filter.val($filter.val().trim());
        let filterProblem = $filter.val().indexOf(';') >= 0
            ? 'Separate extensions with commas or spaces' : iniValueProblem($filter.val());
        setFieldError($filter, filterProblem);
        if (filterProblem) error = true;
        renderRouting();
        let routing = error ? null : routingProblem();
        $('#routing-note').text(routing || '').toggleClass('warn', !!routing);
        if (routing) {
            $('#routing-section').show(); // hidden for a single remote
            error = true;
        }
        if (error === false) {
            $('#config-preview').text(getConfigString(true));
            stepperInstace.nextStep();
        }
    });
});
