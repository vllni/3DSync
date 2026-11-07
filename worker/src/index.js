// The Worker in front of the site's files.
//
// Cloudflare serves every file in the assets as it is (html_handling = "none"
// in wrangler.toml, so /configurator.html is never redirected).  This code
// runs for /api/ (see google.js), and for "/" and an address that matches no
// file, where it does what GitHub Pages did: "/" is index.html, and
// "/privacy-policy" is privacy-policy.html, served without a redirect.

import { handleGoogle } from './google.js';

export default {
    async fetch(request, env) {
        if (new URL(request.url).pathname.startsWith('/api/')) {
            const answer = await handleGoogle(request, env);
            return answer || new Response('Not found', { status: 404 });
        }
        return serveSite(request, env.ASSETS);
    },
};

export async function serveSite(request, assets) {
    const url = new URL(request.url);
    if (url.pathname.endsWith('/'))
        return assets.fetch(withPath(request, url, url.pathname + 'index.html'));

    const response = await assets.fetch(request);
    // An address without an extension may name an .html page.
    if (response.status === 404 && !/\.[^/]*$/.test(url.pathname)) {
        const page = await assets.fetch(withPath(request, url, url.pathname + '.html'));
        if (page.status !== 404)
            return page;
    }
    return response;
}

function withPath(request, url, pathname) {
    const next = new URL(url);
    next.pathname = pathname;
    return new Request(next, request);
}
