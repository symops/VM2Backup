// A small QR code generator (ISO/IEC 18004): byte mode, error correction level M, versions 1..10 (up to 213 bytes), no libraries.
// The web UI draws the otpauth:// link of a two-factor enrolment with it, so the authenticator app can scan it from the screen.
//   VM2B_QR.encode(text)        -> { version, size, mask, modules: [[0|1, ...], ...] }   (modules[row][column], 1 = dark); throws when the text is too long
//   VM2B_QR.encode(text, mask)  -> the same with a fixed mask 0..7 (tests)
(function (root) {
    'use strict';
    // level M: [error correction codewords per block, [[number of blocks, data codewords per block], ...]]
    var ECC_M = [null, [10, [[1, 16]]], [16, [[1, 28]]], [26, [[1, 44]]], [18, [[2, 32]]], [24, [[2, 43]]], [16, [[4, 27]]], [18, [[4, 31]]], [22, [[2, 38], [2, 39]]], [22, [[3, 36], [2, 37]]], [26, [[4, 43], [1, 44]]]];
    var ALIGN = [null, [], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34], [6, 22, 38], [6, 24, 42], [6, 26, 46], [6, 28, 50]];
    var MAX_VERSION = 10;

    function dataCodewords(v) { var n = 0; ECC_M[v][1].forEach(function (g) { n += g[0] * g[1]; }); return n; }
    function capacityBytes(v) { return Math.floor((dataCodewords(v) * 8 - 4 - (v < 10 ? 8 : 16)) / 8); }

    // ---- Reed-Solomon over GF(256), polynomial 0x11D ----
    function gfMul(x, y) { var z = 0; for (var i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >>> 7) * 0x11D); z ^= ((y >>> i) & 1) * x; } return z; }
    function rsDivisor(degree) {
        var result = []; for (var i = 0; i < degree; i++) result.push(0);
        result[degree - 1] = 1; var rootv = 1;
        for (var k = 0; k < degree; k++) {
            for (var j = 0; j < degree; j++) { result[j] = gfMul(result[j], rootv); if (j + 1 < degree) result[j] ^= result[j + 1]; }
            rootv = gfMul(rootv, 2);
        }
        return result;
    }
    function rsRemainder(data, divisor) {
        var result = divisor.map(function () { return 0; });
        data.forEach(function (b) {
            var factor = b ^ result.shift(); result.push(0);
            divisor.forEach(function (coef, i) { result[i] ^= gfMul(coef, factor); });
        });
        return result;
    }

    function utf8(text) {
        var out = [];
        for (var i = 0; i < text.length; i++) {
            var c = text.charCodeAt(i);
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.length) { var d = text.charCodeAt(i + 1); if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; } }
            if (c < 0x80) out.push(c);
            else if (c < 0x800) out.push(0xC0 | (c >> 6), 0x80 | (c & 63));
            else if (c < 0x10000) out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
            else out.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        }
        return out;
    }

    function bit(x, i) { return ((x >>> i) & 1) !== 0; }

    function buildCodewords(bytes, version) {
        var bits = [];
        function put(val, len) { for (var i = len - 1; i >= 0; i--) bits.push((val >>> i) & 1); }
        put(4, 4); put(bytes.length, version < 10 ? 8 : 16);
        bytes.forEach(function (b) { put(b, 8); });
        var cap = dataCodewords(version) * 8;
        for (var t = 0; t < 4 && bits.length < cap; t++) bits.push(0);
        while (bits.length % 8) bits.push(0);
        var data = []; for (var i = 0; i < bits.length; i += 8) { var v = 0; for (var k = 0; k < 8; k++) v = (v << 1) | bits[i + k]; data.push(v); }
        for (var pad = 0xEC; data.length < dataCodewords(version); pad ^= 0xEC ^ 0x11) data.push(pad);
        // blocks, error correction, interleaving
        var eccLen = ECC_M[version][0], blocks = [], pos = 0;
        ECC_M[version][1].forEach(function (g) { for (var n = 0; n < g[0]; n++) { var d = data.slice(pos, pos + g[1]); pos += g[1]; blocks.push({ d: d, e: rsRemainder(d, rsDivisor(eccLen)) }); } });
        var out = [], maxLen = 0; blocks.forEach(function (b) { if (b.d.length > maxLen) maxLen = b.d.length; });
        for (var x = 0; x < maxLen; x++) blocks.forEach(function (b) { if (x < b.d.length) out.push(b.d[x]); });
        for (var y = 0; y < eccLen; y++) blocks.forEach(function (b) { out.push(b.e[y]); });
        return out;
    }

    function penalty(m, size) {
        var result = 0, x, y, run, i;
        function line(get) {
            var p = 0, run = 1, hist = [];
            for (var i = 1; i < size; i++) {
                if (get(i) === get(i - 1)) { run++; if (run === 5) p += 3; else if (run > 5) p++; } else run = 1;
            }
            var s = []; for (i = 0; i < size; i++) s.push(get(i) ? 1 : 0);
            var A = [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0], B = [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1];
            for (i = 0; i + 11 <= size; i++) {
                var a = true, b = true;
                for (var k = 0; k < 11; k++) { if (s[i + k] !== A[k]) a = false; if (s[i + k] !== B[k]) b = false; }
                if (a) p += 40; if (b) p += 40;
            }
            return p;
        }
        for (y = 0; y < size; y++) result += line(function (i) { return m[y][i]; });
        for (x = 0; x < size; x++) result += line(function (i) { return m[i][x]; });
        var dark = 0;
        for (y = 0; y < size; y++) for (x = 0; x < size; x++) {
            if (m[y][x]) dark++;
            if (x + 1 < size && y + 1 < size && m[y][x] === m[y][x + 1] && m[y][x] === m[y + 1][x] && m[y][x] === m[y + 1][x + 1]) result += 3;
        }
        var total = size * size;
        result += (Math.ceil(Math.abs(dark * 20 - total * 10) / total) - 1) * 10;
        return result;
    }

    function maskBit(mask, x, y) {
        switch (mask) {
            case 0: return (x + y) % 2 === 0;
            case 1: return y % 2 === 0;
            case 2: return x % 3 === 0;
            case 3: return (x + y) % 3 === 0;
            case 4: return (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0;
            case 5: return x * y % 2 + x * y % 3 === 0;
            case 6: return (x * y % 2 + x * y % 3) % 2 === 0;
            default: return ((x + y) % 2 + x * y % 3) % 2 === 0;
        }
    }

    function encode(text, forceMask) {
        var bytes = utf8(String(text)), version = 0;
        for (var v = 1; v <= MAX_VERSION; v++) if (bytes.length <= capacityBytes(v)) { version = v; break; }
        if (!version) throw new Error('QR: the text is too long (' + bytes.length + ' bytes, 213 at most)');
        var size = 17 + 4 * version, base = [], fn = [], x, y, i;
        for (y = 0; y < size; y++) { base.push([]); fn.push([]); for (x = 0; x < size; x++) { base[y].push(false); fn[y].push(false); } }
        function setFn(cx, cy, dark) { base[cy][cx] = !!dark; fn[cy][cx] = true; }
        function finder(cx, cy) {
            for (var dy = -4; dy <= 4; dy++) for (var dx = -4; dx <= 4; dx++) {
                var d = Math.max(Math.abs(dx), Math.abs(dy)), xx = cx + dx, yy = cy + dy;
                if (xx >= 0 && xx < size && yy >= 0 && yy < size) setFn(xx, yy, d !== 2 && d !== 4);
            }
        }
        for (i = 0; i < size; i++) { setFn(6, i, i % 2 === 0); setFn(i, 6, i % 2 === 0); }
        finder(3, 3); finder(size - 4, 3); finder(3, size - 4);
        var al = ALIGN[version], n = al.length;
        for (i = 0; i < n; i++) for (var j = 0; j < n; j++) {
            if ((i === 0 && j === 0) || (i === 0 && j === n - 1) || (i === n - 1 && j === 0)) continue;
            for (var dy = -2; dy <= 2; dy++) for (var dx = -2; dx <= 2; dx++) setFn(al[i] + dx, al[j] + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);
        }
        function formatBits(mask) {
            var data = 0 << 3 | mask, rem = data;                                  // level M = 00
            for (var k = 0; k < 10; k++) rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
            return (data << 10 | rem) ^ 0x5412;
        }
        function drawFormat(m, mask) {
            var b = formatBits(mask), k;
            function put(cx, cy, dark) { m[cy][cx] = !!dark; }
            for (k = 0; k <= 5; k++) put(8, k, bit(b, k));
            put(8, 7, bit(b, 6)); put(8, 8, bit(b, 7)); put(7, 8, bit(b, 8));
            for (k = 9; k < 15; k++) put(14 - k, 8, bit(b, k));
            for (k = 0; k < 8; k++) put(size - 1 - k, 8, bit(b, k));
            for (k = 8; k < 15; k++) put(8, size - 15 + k, bit(b, k));
            put(8, size - 8, true);
        }
        // reserve the format areas (drawn per mask) and the version blocks
        drawFormat(base, 0); for (y = 0; y < size; y++) for (x = 0; x < size; x++) { /* mark below */ }
        (function () { var k; for (k = 0; k <= 5; k++) fn[k][8] = true; fn[7][8] = true; fn[8][8] = true; fn[8][7] = true; for (k = 9; k < 15; k++) fn[8][14 - k] = true;
            for (k = 0; k < 8; k++) fn[8][size - 1 - k] = true; for (k = 8; k < 15; k++) fn[size - 15 + k][8] = true; fn[size - 8][8] = true; })();
        if (version >= 7) {
            var rem = version; for (i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >>> 11) * 0x1F25);
            var vb = version << 12 | rem;
            for (i = 0; i < 18; i++) { var dark = bit(vb, i), a = size - 11 + i % 3, b2 = Math.floor(i / 3); setFn(a, b2, dark); setFn(b2, a, dark); }
        }
        // the data
        var cw = buildCodewords(bytes, version), idx = 0;
        for (var right = size - 1; right >= 1; right -= 2) {
            if (right === 6) right = 5;
            for (var vert = 0; vert < size; vert++) for (var jj = 0; jj < 2; jj++) {
                var cx = right - jj, upward = ((right + 1) & 2) === 0, cy = upward ? size - 1 - vert : vert;
                if (!fn[cy][cx] && idx < cw.length * 8) { base[cy][cx] = bit(cw[idx >>> 3], 7 - (idx & 7)); idx++; }
            }
        }
        var best = null, bestMask = 0, bestPen = 1e9;
        for (var mask = 0; mask < 8; mask++) {
            if (forceMask !== undefined && mask !== forceMask) continue;
            var m = base.map(function (r) { return r.slice(); });
            for (y = 0; y < size; y++) for (x = 0; x < size; x++) if (!fn[y][x] && maskBit(mask, x, y)) m[y][x] = !m[y][x];
            drawFormat(m, mask);
            var p = penalty(m, size);
            if (p < bestPen) { bestPen = p; best = m; bestMask = mask; }
        }
        return { version: version, size: size, mask: bestMask, modules: best.map(function (r) { return r.map(function (b) { return b ? 1 : 0; }); }) };
    }

    var api = { encode: encode, capacity: function () { return capacityBytes(MAX_VERSION); } };
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
    if (root) root.VM2B_QR = api;
})(typeof window !== 'undefined' ? window : null);
