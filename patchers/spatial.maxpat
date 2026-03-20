{
    "patcher": {
        "fileversion": 1,
        "appversion": {
            "major": 9,
            "minor": 1,
            "revision": 2,
            "architecture": "x64",
            "modernui": 1
        },
        "classnamespace": "box",
        "rect": [ 59.0, 106.0, 1000.0, 780.0 ],
        "boxes": [
            {
                "box": {
                    "filename": "none",
                    "id": "obj-3",
                    "maxclass": "newobj",
                    "numinlets": 1,
                    "numoutlets": 1,
                    "outlettype": [ "" ],
                    "patching_rect": [ 15.0, 60.0, 21.0, 22.0 ],
                    "saved_object_attributes": {
                        "parameter_enable": 0
                    },
                    "text": "v8",
                    "textfile": {
                        "text": "// Port of code by Jaryd from Core Electronics\n// https://core-electronics.com.au/guides/sensors/diy-2d-and-3d-spatial-tracking-with-ultra-wideband-arduino-and-pico-guide/#XC4IICX\ninlets  = 1;\noutlets = 1;\nvar baseStations = new Array(8);\nvar distOffsets  = new Array(8);\nfor (var i = 0; i < 8; i++) { baseStations[i] = null; distOffsets[i] = 0.0; }\nbaseStations[0] = {x: 0, y: 0};\nbaseStations[1] = {x: 1, y: 0};\nbaseStations[2] = {x: 1, y: 1};\nbaseStations[3] = {x: 0, y: 1};\nvar Q = 0.12, R = 1.1, dt = 0.10;\nvar kState = [0, 0, 0, 0];\nvar kP     = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]];\nvar kInit  = false;\nfunction kalmanReset() {\n    kState = [0, 0, 0, 0];\n    kP     = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]];\n    kInit  = false;\n}\nfunction kalmanPredict() {\n    if (!kInit) return;\n    kState[0] += kState[2] * dt;\n    kState[1] += kState[3] * dt;\n    for (var i = 0; i < 4; i++)\n        for (var j = 0; j < 4; j++)\n            kP[i][j] += Q;\n}\nfunction kalmanUpdate(mx, my) {\n    if (!kInit) {\n        kState = [mx, my, 0, 0];\n        kInit  = true;\n        return [mx, my];\n    }\n    var Kx = kP[0][0] / (kP[0][0] + R);\n    var Ky = kP[1][1] / (kP[1][1] + R);\n    var px = kState[0], py = kState[1];\n    kState[0] += Kx * (mx - kState[0]);\n    kState[1] += Ky * (my - kState[1]);\n    if (dt > 0) {\n        kState[2] = (kState[0] - px) / dt * 0.1;\n        kState[3] = (kState[1] - py) / dt * 0.1;\n    }\n    kP[0][0] = (1 - Kx) * kP[0][0];\n    kP[1][1] = (1 - Ky) * kP[1][1];\n    return [kState[0], kState[1]];\n}\nfunction trilaterate2d(distances) {\n    var valid = [];\n    for (var i = 0; i < 8; i++) {\n        if (baseStations[i] !== null && distances[i] > 0)\n            valid.push({x: baseStations[i].x, y: baseStations[i].y,\n                        dist: distances[i] + distOffsets[i]});\n    }\n    if (valid.length < 3) return null;\n    var x1 = valid[0].x, y1 = valid[0].y, r1 = valid[0].dist;\n    var A = [], b = [];\n    for (var k = 1; k < valid.length && A.length < 7; k++) {\n        var xi = valid[k].x, yi = valid[k].y, ri = valid[k].dist;\n        A.push([2*(xi-x1), 2*(yi-y1)]);\n        b.push(ri*ri - r1*r1 - xi*xi + x1*x1 - yi*yi + y1*y1);\n    }\n    if (A.length < 2) return null;\n    var det = A[0][0]*A[1][1] - A[0][1]*A[1][0];\n    if (Math.abs(det) < 1e-6) return null;\n    return {\n        x: -(b[0]*A[1][1] - b[1]*A[0][1]) / det,\n        y: -(A[0][0]*b[1] - A[1][0]*b[0]) / det\n    };\n}\nfunction list() {\n    var args = arrayfromargs(arguments);\n    var distances = [];\n    for (var i = 0; i < 8; i++) distances.push(i < args.length ? parseFloat(args[i]) : 0);\n    kalmanPredict();\n    var pos      = trilaterate2d(distances);\n    var filtered = pos ? kalmanUpdate(pos.x, pos.y) : [kState[0], kState[1]];\n    outlet(0, filtered);\n}\nfunction anything() {\n    var sel  = messagename;\n    var args = arrayfromargs(arguments);\n    var m = sel.match(/^\\/base\\/(\\d)\\/xy$/);\n    if (m) {\n        var idx = parseInt(m[1]);\n        if (idx < 0 || idx > 7 || args.length < 2) return;\n        baseStations[idx] = {x: parseFloat(args[0]), y: parseFloat(args[1])};\n        return;\n    }\n    m = sel.match(/^\\/offset\\/(\\d)$/);\n    if (m) {\n        var idx = parseInt(m[1]);\n        if (idx < 0 || idx > 7 || args.length < 1) return;\n        distOffsets[idx] = parseFloat(args[0]);\n        return;\n    }\n    if (sel === \"/kalman/Q\")  { if (args.length) Q  = parseFloat(args[0]); return; }\n    if (sel === \"/kalman/R\")  { if (args.length) R  = parseFloat(args[0]); return; }\n    if (sel === \"/kalman/dt\") { if (args.length) dt = parseFloat(args[0]); return; }\n    if (sel === \"reset\") { kalmanReset(); return; }\n}\nfunction reset() { kalmanReset(); }",
                        "filename": "none",
                        "flags": 0,
                        "embed": 1,
                        "autowatch": 1
                    }
                }
            },
            {
                "box": {
                    "comment": "",
                    "id": "obj-2",
                    "index": 0,
                    "maxclass": "outlet",
                    "numinlets": 1,
                    "numoutlets": 0,
                    "patching_rect": [ 15.0, 90.0, 30.0, 30.0 ]
                }
            },
            {
                "box": {
                    "comment": "",
                    "id": "obj-1",
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
                    "destination": [ "obj-3", 0 ],
                    "source": [ "obj-1", 0 ]
                }
            },
            {
                "patchline": {
                    "destination": [ "obj-2", 0 ],
                    "source": [ "obj-3", 0 ]
                }
            }
        ],
        "autosave": 0
    }
}