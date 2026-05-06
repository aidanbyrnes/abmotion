// Adapted code by Jaryd from Core Electronics
// https://core-electronics.com.au/guides/sensors/diy-2d-and-3d-spatial-tracking-with-ultra-wideband-arduino-and-pico-guide/#XC4IICX

// filter raw data -> trilaterate -> Kalman filter

inlets  = 1;
outlets = 2;

// distance filter params
var OUTLIER_THRESH = 3;
var MIN_CLUSTER    = 2;

// trilateration params
var RMS_THRESH  = 0.4;
var DIST_THRESH = 5.0;
var anchors     = {};   // keyed 0-7, each {x, y}
var prev_point  = { x: -1, y: -1 };

// kalman filter params
var Q       = 0.01;
var R       = 3.0;
var dt      = 0.05;   // seconds (matches task interval below)
var damping = 0.5;    // velocity damping factor (0.0–1.0)

var kState = [0, 0, 0, 0];
var kP     = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]];
var kInit  = false;

var lastRMS = 0;

var outputTask = new Task(function() {
    kalmanPredict();
    if (!kInit) return;

    var norm = normalize(kState[0], kState[1]);
    if (norm) {
        outlet(0, [1 - norm.x, norm.y]);
        outlet(1, lastRMS);
    }
}, this);

outputTask.interval = 50;
outputTask.repeat();

// sensor input
function list() {
    var raw = arrayfromargs(arguments);

    if (raw.length !== 8) {
        error("tracker: expected 8 sensor values, got " + raw.length + "\n");
        return;
    }

    // 1 — cluster + median filter
    var filtered = applyClusterFilter(raw, MIN_CLUSTER);
    filtered     = applyMedianFilter(filtered, OUTLIER_THRESH);

    // 2 — trilateration
    var meas = [];
    for (var i = 0; i < filtered.length; i++) {
        if (filtered[i] > 0 && anchors[i] !== undefined) {
            meas.push({ ax: anchors[i].x, ay: anchors[i].y, d: filtered[i] });
        }
    }
    if (meas.length < 3) return;

    var result = trilaterate(meas);
    if (!result) { error("tracker: trilateration did not converge\n"); return; }

    lastRMS = result.err;
    if (result.err >= RMS_THRESH) return;

    if (prev_point.x !== -1 && prev_point.y !== -1
        && Math.sqrt(Math.pow(result.x - prev_point.x, 2) + Math.pow(result.y - prev_point.y, 2)) > DIST_THRESH) {
        return;
    }
    prev_point = { x: result.x, y: result.y };

    // 3 — Kalman filter
    kalmanUpdate(result.x, result.y);
}

function anything() {
    var addr = messagename;
    var args = arrayfromargs(arguments);

    // /base/<idx>/xy  x  y
    var baseMatch = addr.match(/^\/base\/(\d+)\/xy$/);
    if (baseMatch) {
        var idx = parseInt(baseMatch[1], 10);
        if (idx < 0 || idx > 7) { error("tracker: anchor index out of range: " + idx + "\n"); return; }
        var x = args[0], y = args[1];
        if (typeof x !== "number" || typeof y !== "number") { error("tracker: xy values must be numbers\n"); return; }
        anchors[idx] = { x: x, y: y };
        //post("anchor " + idx + " set to (" + x + ", " + y + ")\n");
        prev_point = { x: -1, y: -1 };
        return;
    }

    // /kalman/*
    if (addr === "/kalman/Q"       && args.length) { Q       = parseFloat(args[0]); return; }
    if (addr === "/kalman/R"       && args.length) { R       = parseFloat(args[0]); return; }
    if (addr === "/kalman/damping" && args.length) { damping = parseFloat(args[0]); return; }
    if (addr === "/kalman/reset")                  { kalmanReset();                 return; }

    // /filter/*
    if (addr === "/filter/threshold"  && args.length) { OUTLIER_THRESH = parseFloat(args[0]);              return; }
    if (addr === "/filter/mincluster" && args.length) { MIN_CLUSTER    = Math.max(2, Math.floor(args[0])); return; }

    // /trilat/*
    if (addr === "/trilat/rmsthresh"  && args.length) { RMS_THRESH  = parseFloat(args[0]); return; }
    if (addr === "/trilat/distthresh" && args.length) { DIST_THRESH = parseFloat(args[0]); return; }

    error("tracker: unrecognised message: " + addr + "\n");
}

// cluster filter
function applyClusterFilter(vals, minCluster) {
    var n    = vals.length;
    var keep = [];
    var i;

    for (i = 0; i < n; i++) keep[i] = (vals[i] > 0);

    i = 0;
    while (i < n) {
        if (!keep[i]) { i++; continue; }
        var j = i;
        while (j < n && keep[j]) j++;
        if (j - i < minCluster)
            for (var k = i; k < j; k++) keep[k] = false;
        i = j;
    }

    var head = 0;
    while (head < n && vals[head] > 0) head++;
    var tail = n - 1;
    while (tail >= 0 && vals[tail] > 0) tail--;

    if (head > 0 && tail < n - 1) {
        var runLen = head + (n - 1 - tail);
        if (runLen >= minCluster) {
            for (var a = 0; a < head; a++)     keep[a] = true;
            for (var b = tail + 1; b < n; b++) keep[b] = true;
        }
    }

    var out = [];
    for (i = 0; i < n; i++) out[i] = keep[i] ? vals[i] : -1;
    return out;
}

// median outlier filter
function applyMedianFilter(vals, thresh) {
    var validVals = [];
    var i;

    for (i = 0; i < vals.length; i++)
        if (vals[i] > 0) validVals.push(vals[i]);

    if (validVals.length < 2) return vals;

    var med = median(validVals);
    var out = [];

    for (i = 0; i < vals.length; i++) {
        out[i] = (vals[i] > 0 && Math.abs(vals[i] - med) > thresh) ? -1 : vals[i];
    }
    return out;
}

function median(arr) {
    var s = arr.slice().sort(function(a, b) { return a - b; });
    var m = Math.floor(s.length / 2);
    return (s.length % 2) ? s[m] : (s[m - 1] + s[m]) / 2.0;
}

// trilateration using iterative non-linear least squares (Gauss-Newton)
function trilaterate(meas) {
    var wx = 0, wy = 0, wt = 0;
    for (var i = 0; i < meas.length; i++) {
        var w = 1.0 / meas[i].d;
        wx += meas[i].ax * w;
        wy += meas[i].ay * w;
        wt += w;
    }
    var px = wx / wt, py = wy / wt;

    var MAX_ITER = 50, TOL = 1e-6;

    for (var iter = 0; iter < MAX_ITER; iter++) {
        var JTJ00 = 0, JTJ01 = 0, JTJ11 = 0, JTr0 = 0, JTr1 = 0;

        for (var m = 0; m < meas.length; m++) {
            var dx   = px - meas[m].ax;
            var dy   = py - meas[m].ay;
            var dist = Math.sqrt(dx * dx + dy * dy);
            if (dist < 1e-9) continue;

            var jx  = dx / dist, jy = dy / dist;
            var res = dist - meas[m].d;

            JTJ00 += jx * jx;  JTJ01 += jx * jy;  JTJ11 += jy * jy;
            JTr0  += jx * res; JTr1  += jy * res;
        }

        var det = JTJ00 * JTJ11 - JTJ01 * JTJ01;
        if (Math.abs(det) < 1e-12) break;

        var stepX = (-JTr0 * JTJ11 + JTr1 * JTJ01) / det;
        var stepY = (-JTr1 * JTJ00 + JTr0 * JTJ01) / det;

        px += stepX;
        py += stepY;

        if (Math.sqrt(stepX * stepX + stepY * stepY) < TOL) break;
    }

    var rms = 0;
    for (var n = 0; n < meas.length; n++) {
        var ex = px - meas[n].ax, ey = py - meas[n].ay;
        var r  = Math.sqrt(ex * ex + ey * ey) - meas[n].d;
        rms += r * r;
    }
    rms = Math.sqrt(rms / meas.length);

    return { x: px, y: py, err: rms };
}

// kalman filter
function kalmanReset() {
    kState = [0, 0, 0, 0];
    kP     = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]];
    kInit  = false;
}

function kalmanPredict() {
    if (!kInit) return;
    kState[0] += kState[2] * dt;
    kState[1] += kState[3] * dt;
    for (var i = 0; i < 4; i++)
        for (var j = 0; j < 4; j++)
            kP[i][j] += Q;
}

function kalmanUpdate(mx, my) {
    if (!kInit) {
        kState = [mx, my, 0, 0];
        kInit  = true;
        return;
    }
    var Kx = kP[0][0] / (kP[0][0] + R);
    var Ky = kP[1][1] / (kP[1][1] + R);
    var px = kState[0], py = kState[1];
    kState[0] += Kx * (mx - kState[0]);
    kState[1] += Ky * (my - kState[1]);
    if (dt > 0) {
        kState[2] = (kState[0] - px) / dt * damping;
        kState[3] = (kState[1] - py) / dt * damping;
    }
    kP[0][0] = (1 - Kx) * kP[0][0];
    kP[1][1] = (1 - Ky) * kP[1][1];
}

// normalize based on anchor positions
function normalize(x, y) {
    var keys = [];
    for (var k in anchors) keys.push(k);
    if (keys.length < 2) return null;

    var minX = Infinity, maxX = -Infinity;
    var minY = Infinity, maxY = -Infinity;

    for (var i = 0; i < keys.length; i++) {
        var a = anchors[keys[i]];
        if (a.x < minX) minX = a.x;
        if (a.x > maxX) maxX = a.x;
        if (a.y < minY) minY = a.y;
        if (a.y > maxY) maxY = a.y;
    }

    var rangeX = maxX - minX;
    var rangeY = maxY - minY;
    if (rangeX < 1e-9 || rangeY < 1e-9) return null;   // degenerate / collinear

    return {
        x: (x - minX) / rangeX,
        y: (y - minY) / rangeY
    };
}