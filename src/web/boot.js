// Starts the application in a browser (kept apart from app.js, which is also loaded by the node tests).
(function () {
    // a File -> its bytes as base64 (the PFX upload); 1 MB at most is checked by the caller
    function readFile(f) {
        return new Promise(function (resolve, reject) {
            var r = new FileReader();
            r.onerror = function () { reject(new Error('cannot read the file')); };
            r.onload = function () { var u = new Uint8Array(r.result), s = ''; for (var i = 0; i < u.length; i += 8192) s += String.fromCharCode.apply(null, u.subarray(i, i + 8192)); resolve(btoa(s)); };
            r.readAsArrayBuffer(f);
        });
    }
    // a text -> a download of the browser (the HTML report of the history)
    function saveFile(name, mime, text) {
        var url = URL.createObjectURL(new Blob([text], { type: mime })), a = document.createElement('a');
        a.setAttribute('download', name); a.href = url; document.body.appendChild(a); a.click(); document.body.removeChild(a);
        setTimeout(function () { URL.revokeObjectURL(url); }, 10000);
    }
    // a language table is a script of its own (/lang/<code>.js), loaded when the language is needed; the choice is remembered in the browser
    function loadLang(code) {
        return new Promise(function (resolve) {
            var sc = document.createElement('script'); sc.src = '/lang/' + encodeURIComponent(code) + '.js';
            sc.onload = function () { resolve(); }; sc.onerror = function () { resolve(); };
            document.head.appendChild(sc);
        });
    }
    function savedLang() { try { return window.localStorage.getItem('vm2b.lang') || ''; } catch (e) { return ''; } }
    function saveLang(code) { try { window.localStorage.setItem('vm2b.lang', code); } catch (e) { /* private window */ } }
    var app = createApp({ document: document, fetch: window.fetch.bind(window), navigator: navigator, strings: window.VM2B_STRINGS, languages: window.VM2B_LANGS, loadLang: loadLang, savedLang: savedLang, saveLang: saveLang,
                          setInterval: window.setInterval.bind(window), clearInterval: window.clearInterval.bind(window), setTimeout: window.setTimeout.bind(window), confirm: window.confirm.bind(window), location: window.location, qr: window.VM2B_QR, readFile: readFile, saveFile: saveFile });
    app.start(document.getElementById('root'));
})();
