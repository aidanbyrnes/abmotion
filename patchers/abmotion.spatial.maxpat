{
    "patcher": {
        "fileversion": 1,
        "appversion": {
            "major": 9,
            "minor": 1,
            "revision": 4,
            "architecture": "x64",
            "modernui": 1
        },
        "classnamespace": "box",
        "rect": [ 34.0, 87.0, 1372.0, 779.0 ],
        "boxes": [
            {
                "box": {
                    "id": "obj-15",
                    "maxclass": "newobj",
                    "numinlets": 2,
                    "numoutlets": 2,
                    "outlettype": [ "", "" ],
                    "patching_rect": [ 15.0, 60.0, 60.0, 22.0 ],
                    "text": "zl.change"
                }
            },
            {
                "box": {
                    "filename": "none",
                    "id": "obj-14",
                    "maxclass": "newobj",
                    "numinlets": 1,
                    "numoutlets": 2,
                    "outlettype": [ "", "" ],
                    "patching_rect": [ 15.0, 90.0, 83.0, 22.0 ],
                    "saved_object_attributes": {
                        "parameter_enable": 0
                    },
                    "text": "v8 @embed 1",
                    "textfile": {
                        "text": "// Adapted code by Jaryd from Core Electronics\n// https://core-electronics.com.au/guides/sensors/diy-2d-and-3d-spatial-tracking-with-ultra-wideband-arduino-and-pico-guide/#XC4IICX\n\n// filter raw data -> trilaterate -> Kalman filter\n\ninlets  = 1;\noutlets = 2;\n\n// distance filter params\nvar OUTLIER_THRESH = 3;\nvar MIN_CLUSTER    = 2;\n\n// trilateration params\nvar RMS_THRESH  = 0.4;\nvar DIST_THRESH = 5.0;\nvar anchors     = {};   // keyed 0-7, each {x, y}\nvar prev_point  = { x: -1, y: -1 };\n\n// kalman filter params\nvar Q       = 0.01;\nvar R       = 3.0;\nvar dt      = 0.05;   // seconds (matches task interval below)\nvar damping = 0.5;    // velocity damping factor (0.0–1.0)\n\nvar kState = [0, 0, 0, 0];\nvar kP     = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]];\nvar kInit  = false;\n\nvar lastRMS = 0;\n\nvar outputTask = new Task(function() {\n    kalmanPredict();\n    if (!kInit) return;\n\n    var norm = normalize(kState[0], kState[1]);\n    if (norm) {\n        outlet(0, [1 - norm.x, norm.y]);\n        outlet(1, lastRMS);\n    }\n}, this);\n\noutputTask.interval = 50;\noutputTask.repeat();\n\n// sensor input\nfunction list() {\n    var raw = arrayfromargs(arguments);\n\n    if (raw.length !== 8) {\n        error(\"abmotion.spatial: expected 8 sensor values, got \" + raw.length + \"\\n\");\n        return;\n    }\n\n    // 1 — cluster + median filter\n    var filtered = applyClusterFilter(raw, MIN_CLUSTER);\n    filtered     = applyMedianFilter(filtered, OUTLIER_THRESH);\n\n    // 2 — trilateration\n    var meas = [];\n    for (var i = 0; i < filtered.length; i++) {\n        if (filtered[i] > 0 && anchors[i] !== undefined) {\n            meas.push({ ax: anchors[i].x, ay: anchors[i].y, d: filtered[i] });\n        }\n    }\n    if (meas.length < 3) return;\n\n    var result = trilaterate(meas);\n    if (!result) { error(\"abmotion.spatial: trilateration did not converge\\n\"); return; }\n\n    lastRMS = result.err;\n    if (result.err >= RMS_THRESH) return;\n\n    if (prev_point.x !== -1 && prev_point.y !== -1\n        && Math.sqrt(Math.pow(result.x - prev_point.x, 2) + Math.pow(result.y - prev_point.y, 2)) > DIST_THRESH) {\n        return;\n    }\n    prev_point = { x: result.x, y: result.y };\n\n    // 3 — Kalman filter\n    kalmanUpdate(result.x, result.y);\n}\n\nfunction anything() {\n    var addr = messagename;\n    var args = arrayfromargs(arguments);\n\n    // /base/<idx>/xy  x  y\n    var baseMatch = addr.match(/^\\/base\\/(\\d+)\\/xy$/);\n    if (baseMatch) {\n        var idx = parseInt(baseMatch[1], 10);\n        if (idx < 0 || idx > 7) { error(\"abmotion.spatial: anchor index out of range: \" + idx + \"\\n\"); return; }\n        var x = args[0], y = args[1];\n        if (typeof x !== \"number\" || typeof y !== \"number\") { error(\"abmotion.spatial: xy values must be numbers\\n\"); return; }\n        anchors[idx] = { x: x, y: y };\n        //post(\"anchor \" + idx + \" set to (\" + x + \", \" + y + \")\\n\");\n        prev_point = { x: -1, y: -1 };\n        return;\n    }\n\n    // /kalman/*\n    if (addr === \"/kalman/Q\"       && args.length) { Q       = parseFloat(args[0]); return; }\n    if (addr === \"/kalman/R\"       && args.length) { R       = parseFloat(args[0]); return; }\n    if (addr === \"/kalman/damping\" && args.length) { damping = parseFloat(args[0]); return; }\n    if (addr === \"/kalman/reset\")                  { kalmanReset();                 return; }\n\n    // /filter/*\n    if (addr === \"/filter/threshold\"  && args.length) { OUTLIER_THRESH = parseFloat(args[0]);              return; }\n    if (addr === \"/filter/mincluster\" && args.length) { MIN_CLUSTER    = Math.max(2, Math.floor(args[0])); return; }\n\n    // /trilat/*\n    if (addr === \"/trilat/rmsthresh\"  && args.length) { RMS_THRESH  = parseFloat(args[0]); return; }\n    if (addr === \"/trilat/distthresh\" && args.length) { DIST_THRESH = parseFloat(args[0]); return; }\n\n    error(\"abmotion.spatial: unrecognised message: \" + addr + \"\\n\");\n}\n\n// cluster filter\nfunction applyClusterFilter(vals, minCluster) {\n    var n    = vals.length;\n    var keep = [];\n    var i;\n\n    for (i = 0; i < n; i++) keep[i] = (vals[i] > 0);\n\n    i = 0;\n    while (i < n) {\n        if (!keep[i]) { i++; continue; }\n        var j = i;\n        while (j < n && keep[j]) j++;\n        if (j - i < minCluster)\n            for (var k = i; k < j; k++) keep[k] = false;\n        i = j;\n    }\n\n    var head = 0;\n    while (head < n && vals[head] > 0) head++;\n    var tail = n - 1;\n    while (tail >= 0 && vals[tail] > 0) tail--;\n\n    if (head > 0 && tail < n - 1) {\n        var runLen = head + (n - 1 - tail);\n        if (runLen >= minCluster) {\n            for (var a = 0; a < head; a++)     keep[a] = true;\n            for (var b = tail + 1; b < n; b++) keep[b] = true;\n        }\n    }\n\n    var out = [];\n    for (i = 0; i < n; i++) out[i] = keep[i] ? vals[i] : -1;\n    return out;\n}\n\n// median outlier filter\nfunction applyMedianFilter(vals, thresh) {\n    var validVals = [];\n    var i;\n\n    for (i = 0; i < vals.length; i++)\n        if (vals[i] > 0) validVals.push(vals[i]);\n\n    if (validVals.length < 2) return vals;\n\n    var med = median(validVals);\n    var out = [];\n\n    for (i = 0; i < vals.length; i++) {\n        out[i] = (vals[i] > 0 && Math.abs(vals[i] - med) > thresh) ? -1 : vals[i];\n    }\n    return out;\n}\n\nfunction median(arr) {\n    var s = arr.slice().sort(function(a, b) { return a - b; });\n    var m = Math.floor(s.length / 2);\n    return (s.length % 2) ? s[m] : (s[m - 1] + s[m]) / 2.0;\n}\n\n// trilateration using iterative non-linear least squares (Gauss-Newton)\nfunction trilaterate(meas) {\n    var wx = 0, wy = 0, wt = 0;\n    for (var i = 0; i < meas.length; i++) {\n        var w = 1.0 / meas[i].d;\n        wx += meas[i].ax * w;\n        wy += meas[i].ay * w;\n        wt += w;\n    }\n    var px = wx / wt, py = wy / wt;\n\n    var MAX_ITER = 50, TOL = 1e-6;\n\n    for (var iter = 0; iter < MAX_ITER; iter++) {\n        var JTJ00 = 0, JTJ01 = 0, JTJ11 = 0, JTr0 = 0, JTr1 = 0;\n\n        for (var m = 0; m < meas.length; m++) {\n            var dx   = px - meas[m].ax;\n            var dy   = py - meas[m].ay;\n            var dist = Math.sqrt(dx * dx + dy * dy);\n            if (dist < 1e-9) continue;\n\n            var jx  = dx / dist, jy = dy / dist;\n            var res = dist - meas[m].d;\n\n            JTJ00 += jx * jx;  JTJ01 += jx * jy;  JTJ11 += jy * jy;\n            JTr0  += jx * res; JTr1  += jy * res;\n        }\n\n        var det = JTJ00 * JTJ11 - JTJ01 * JTJ01;\n        if (Math.abs(det) < 1e-12) break;\n\n        var stepX = (-JTr0 * JTJ11 + JTr1 * JTJ01) / det;\n        var stepY = (-JTr1 * JTJ00 + JTr0 * JTJ01) / det;\n\n        px += stepX;\n        py += stepY;\n\n        if (Math.sqrt(stepX * stepX + stepY * stepY) < TOL) break;\n    }\n\n    var rms = 0;\n    for (var n = 0; n < meas.length; n++) {\n        var ex = px - meas[n].ax, ey = py - meas[n].ay;\n        var r  = Math.sqrt(ex * ex + ey * ey) - meas[n].d;\n        rms += r * r;\n    }\n    rms = Math.sqrt(rms / meas.length);\n\n    return { x: px, y: py, err: rms };\n}\n\n// kalman filter\nfunction kalmanReset() {\n    kState = [0, 0, 0, 0];\n    kP     = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]];\n    kInit  = false;\n}\n\nfunction kalmanPredict() {\n    if (!kInit) return;\n    kState[0] += kState[2] * dt;\n    kState[1] += kState[3] * dt;\n    for (var i = 0; i < 4; i++)\n        for (var j = 0; j < 4; j++)\n            kP[i][j] += Q;\n}\n\nfunction kalmanUpdate(mx, my) {\n    if (!kInit) {\n        kState = [mx, my, 0, 0];\n        kInit  = true;\n        return;\n    }\n    var Kx = kP[0][0] / (kP[0][0] + R);\n    var Ky = kP[1][1] / (kP[1][1] + R);\n    var px = kState[0], py = kState[1];\n    kState[0] += Kx * (mx - kState[0]);\n    kState[1] += Ky * (my - kState[1]);\n    if (dt > 0) {\n        kState[2] = (kState[0] - px) / dt * damping;\n        kState[3] = (kState[1] - py) / dt * damping;\n    }\n    kP[0][0] = (1 - Kx) * kP[0][0];\n    kP[1][1] = (1 - Ky) * kP[1][1];\n}\n\n// normalize based on anchor positions\nfunction normalize(x, y) {\n    var keys = [];\n    for (var k in anchors) keys.push(k);\n    if (keys.length < 2) return null;\n\n    var minX = Infinity, maxX = -Infinity;\n    var minY = Infinity, maxY = -Infinity;\n\n    for (var i = 0; i < keys.length; i++) {\n        var a = anchors[keys[i]];\n        if (a.x < minX) minX = a.x;\n        if (a.x > maxX) maxX = a.x;\n        if (a.y < minY) minY = a.y;\n        if (a.y > maxY) maxY = a.y;\n    }\n\n    var rangeX = maxX - minX;\n    var rangeY = maxY - minY;\n    if (rangeX < 1e-9 || rangeY < 1e-9) return null;   // degenerate / collinear\n\n    return {\n        x: (x - minX) / rangeX,\n        y: (y - minY) / rangeY\n    };\n}",
                        "filename": "none",
                        "flags": 1,
                        "embed": 1,
                        "autowatch": 1
                    }
                }
            },
            {
                "box": {
                    "comment": "",
                    "id": "obj-13",
                    "index": 0,
                    "maxclass": "outlet",
                    "numinlets": 1,
                    "numoutlets": 0,
                    "patching_rect": [ 15.0, 120.0, 30.0, 30.0 ]
                }
            },
            {
                "box": {
                    "comment": "",
                    "id": "obj-12",
                    "index": 0,
                    "maxclass": "inlet",
                    "numinlets": 0,
                    "numoutlets": 1,
                    "outlettype": [ "" ],
                    "patching_rect": [ 15.0, 15.0, 30.0, 30.0 ]
                }
            }
        ],
        "lines": [
            {
                "patchline": {
                    "destination": [ "obj-15", 0 ],
                    "source": [ "obj-12", 0 ]
                }
            },
            {
                "patchline": {
                    "destination": [ "obj-13", 0 ],
                    "source": [ "obj-14", 0 ]
                }
            },
            {
                "patchline": {
                    "destination": [ "obj-14", 0 ],
                    "source": [ "obj-15", 0 ]
                }
            }
        ],
        "autosave": 0
    }
}