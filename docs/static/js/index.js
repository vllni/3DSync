$(function () {
    let hashParams = new URLSearchParams(window.location.hash.substr(1));
    let queryParams = new URLSearchParams(window.location.search);
    let dropboxToken = hashParams.get('access_token');
    let dropboxState = hashParams.get('state');
    let googleCode = queryParams.get('code');
    let googleState = queryParams.get('state');
    let paths = [];

    let stepperInstace = new MStepper(document.querySelector('.stepper'), {
        firstActive: 0,
        autoFormCreation: false,
        stepTitleNavigation: false,
    });

    // --- Dropbox redirect callback ---
    if (dropboxToken !== null && dropboxState !== null) {
        if (dropboxState === localStorage.getItem('dropboxStateToken')) {
            localStorage.setItem('dropboxToken', dropboxToken);
            localStorage.setItem('provider', 'dropbox');
            stepperInstace.nextStep();
        }
    }
    localStorage.removeItem('dropboxStateToken');

    // --- Google Drive redirect callback ---
    if (googleCode !== null && googleState !== null) {
        if (googleState === localStorage.getItem('gdriveState')) {
            let codeVerifier = localStorage.getItem('gdriveCodeVerifier');
            localStorage.removeItem('gdriveState');
            localStorage.removeItem('gdriveCodeVerifier');
            // Clean the auth code from the URL to avoid reuse on refresh
            history.replaceState(null, '', window.location.pathname);
            exchangeGoogleCode(googleCode, codeVerifier).then(function (success) {
                if (success) {
                    localStorage.setItem('provider', 'googledrive');
                    stepperInstace.nextStep();
                } else {
                    alert('Google Drive authentication failed. Please try again.');
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

    // --- Login button handlers ---
    $('#dropbox-login').on('click', function (e) {
        e.preventDefault();
        let token = [...Array(100)].map(i => (~~(Math.random() * 36)).toString(36)).join('');
        localStorage.setItem('dropboxStateToken', token);
        window.location.href = "https://www.dropbox.com/oauth2/authorize?client_id=3x8ipjhtplvcoba&response_type=token&redirect_uri=https://vllni.github.io/3DSync/&state=" + token;
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
            let config = '[Dropbox]\nToken=' + localStorage.getItem('dropboxToken');
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
