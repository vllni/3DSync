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

    // --- Config generation ---
    function getConfigString() {
        let provider = localStorage.getItem('provider');
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
        if (provider === 'googledrive') {
            let clientId = localStorage.getItem('gdriveClientId');
            let clientSecret = localStorage.getItem('gdriveClientSecret');
            let refreshToken = localStorage.getItem('gdriveRefreshToken');
            let folderId = localStorage.getItem('gdriveFolderId');
            let config = '[GoogleDrive]\nClientId=' + clientId
                + '\nClientSecret=' + clientSecret
                + '\nRefreshToken=' + refreshToken;
            if (folderId) {
                config += '\nFolderId=' + folderId;
            }
            if (strPaths) config += '\n[Paths]\n' + strPaths;
            if (strShallowPaths) config += '\n[ShallowPaths]\n' + strShallowPaths;
            if (strUploadPaths) config += '\n[UploadPaths]\n' + strUploadPaths;
            if (strUploadShallowPaths) config += '\n[UploadShallowPaths]\n' + strUploadShallowPaths;
            return config;
        } else {
            let refreshToken = localStorage.getItem('dropboxRefreshToken');
            let config;
            if (refreshToken) {
                config = '[Dropbox]\nAppKey=' + localStorage.getItem('dropboxClientId')
                    + '\nRefreshToken=' + refreshToken;
            } else {
                // Only reachable for a session that authenticated before the
                // refresh-token flow; such a token expires in about 4 hours.
                config = '[Dropbox]\nToken=' + localStorage.getItem('dropboxToken');
            }
            if (strPaths) config += '\n[Paths]\n' + strPaths;
            if (strShallowPaths) config += '\n[ShallowPaths]\n' + strShallowPaths;
            if (strUploadPaths) config += '\n[UploadPaths]\n' + strUploadPaths;
            if (strUploadShallowPaths) config += '\n[UploadShallowPaths]\n' + strUploadShallowPaths;
            return config;
        }
    }

    $('#download-config').on('click', function (e) {
        e.preventDefault();
        let blob = new Blob([getConfigString()], { type: "application/octet-stream;charset=utf-8" });
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
            stepperInstace.nextStep();
        }
    });
});
