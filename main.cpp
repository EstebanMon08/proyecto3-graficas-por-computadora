// Codigo generado con ayuda de un LLM (Claude, Anthropic).
#include "raylib.h"
#include "raymath.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

constexpr int W = 800, H = 450;
constexpr int WIN_W = 1280, WIN_H = 720;
constexpr float FOV_DEG = 70.f;
constexpr float NEAR_Z = 0.1f;
constexpr int SHIP_ORIENT = 1;
constexpr int SHIP_ROLL = 1;
constexpr float SHIP_SIZE = 1.0f;
constexpr float SHIP_DIST = 2.5f;
constexpr float SHIP_DOWN = 0.8f;
const Vector3 SUN_POS = {0, 0, 0};

static bool cullBack = true;
static std::vector<Color> fb(W * H);
static std::vector<float> zb(W * H);

static inline unsigned char clamp8(float x) { return (unsigned char)Clamp(x, 0, 255); }

static float hash2(int x, int y, int s) {
    unsigned h = x * 374761393u + y * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffff) / 65535.f;
}

struct Tex {
    int w = 0, h = 0;
    std::vector<Color> px;
    Color sample(float u, float v) const {
        u -= floorf(u);
        v -= floorf(v);
        int x = std::min(int(u * w), w - 1), y = std::min(int(v * h), h - 1);
        return px[y * w + x];
    }
};

static Tex loadTex(const std::string& path) {
    Tex t;
    if (!FileExists(path.c_str())) {
        TraceLog(LOG_WARNING, "Textura no encontrada: %s", path.c_str());
        return t;
    }
    Image im = LoadImage(path.c_str());
    if (im.data == nullptr) return t;
    t.w = im.width;
    t.h = im.height;
    t.px.resize(t.w * t.h);
    Color* c = LoadImageColors(im);
    for (int i = 0; i < t.w * t.h; i++) t.px[i] = c[i];
    UnloadImageColors(c);
    UnloadImage(im);
    return t;
}

struct Vert { Vector3 p, n; float u, v; };
struct Part {
    std::vector<Vert> v;
    std::vector<int> idx;
    Tex tex;
    Color kd{200, 200, 200, 255};
    float alpha = 1.f;
    bool transparent = false;
    Vector3 ke{0, 0, 0};
    Tex emitTex;
    bool hasEmit = false;
};
struct ModelData { std::vector<Part> parts; };

static ModelData makeSphere(int stacks, int slices) {
    ModelData m;
    Part p;
    for (int i = 0; i <= stacks; i++) {
        float phi = PI * i / stacks;
        for (int j = 0; j <= slices; j++) {
            float th = 2 * PI * j / slices;
            Vector3 n = {sinf(phi) * cosf(th), cosf(phi), sinf(phi) * sinf(th)};
            p.v.push_back({n, n, (float)j / slices, (float)i / stacks});
        }
    }
    for (int i = 0; i < stacks; i++)
        for (int j = 0; j < slices; j++) {
            int a = i * (slices + 1) + j, b = a + slices + 1, c = a + 1, d = b + 1;
            p.idx.insert(p.idx.end(), {a, c, b, b, c, d});
        }
    m.parts.push_back(std::move(p));
    return m;
}

struct MatInfo {
    Color kd{200, 200, 200, 255};
    std::string map;
    float d = 1.f;
    Vector3 ke{0, 0, 0};
    std::string mapKe;
};

static std::string dirOf(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? "" : p.substr(0, i + 1);
}

static std::string restOfLine(std::istringstream& ss) {
    std::string s;
    std::getline(ss >> std::ws, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

static std::string baseName(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

static std::map<std::string, MatInfo> loadMtl(const std::string& path) {
    std::map<std::string, MatInfo> mats;
    std::ifstream f(path);
    if (!f) {
        TraceLog(LOG_WARNING, "No se encontro el .mtl: %s (sin el, no hay texturas ni transparencia)", path.c_str());
        return mats;
    }
    std::string line, cur;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "newmtl") {
            cur = restOfLine(ss);
            mats[cur] = MatInfo();
        } else if (tag == "Kd" && !cur.empty()) {
            float r = 0.8f, g = 0.8f, b = 0.8f;
            ss >> r >> g >> b;
            mats[cur].kd = {clamp8(r * 255), clamp8(g * 255), clamp8(b * 255), 255};
        } else if (tag == "Ke" && !cur.empty()) {
            Vector3 k{0, 0, 0};
            ss >> k.x >> k.y >> k.z;
            mats[cur].ke = k;
        } else if (tag == "map_Ke" && !cur.empty()) {
            std::string tok, last;
            while (ss >> tok) last = tok;
            mats[cur].mapKe = last;
        } else if (tag == "d" && !cur.empty()) {
            float d = 1.f;
            if (ss >> d) mats[cur].d = Clamp(d, 0, 1);
        } else if (tag == "Tr" && !cur.empty()) {
            float tr = 0.f;
            if (ss >> tr) mats[cur].d = Clamp(1 - tr, 0, 1);
        } else if (tag == "map_Kd" && !cur.empty()) {
            std::string tok, last;
            while (ss >> tok) last = tok;
            mats[cur].map = last;
        }
    }
    return mats;
}

static bool texHasAlpha(const Tex& t) {
    for (const Color& c : t.px)
        if (c.a < 255) return true;
    return false;
}

static ModelData loadObj(const std::string& path) {
    ModelData model;
    std::ifstream f(path);
    if (!f) {
        TraceLog(LOG_WARNING, "No se pudo abrir %s", path.c_str());
        return model;
    }
    std::string dir = dirOf(path);
    std::vector<Vector3> P, N;
    std::vector<Vector2> T;
    std::map<std::string, MatInfo> mats;
    std::map<std::string, int> partOf;
    std::map<std::array<int, 4>, int> cache;
    int cur = -1;

    auto getPart = [&](const std::string& name) -> int {
        auto it = partOf.find(name);
        if (it != partOf.end()) return it->second;
        Part p;
        auto mi = mats.find(name);
        if (mi != mats.end()) {
            p.kd = mi->second.kd;
            if (!mi->second.map.empty()) p.tex = loadTex(dir + baseName(mi->second.map));
            p.alpha = mi->second.d;
            p.ke = mi->second.ke;
            if (!mi->second.mapKe.empty()) p.emitTex = loadTex(dir + baseName(mi->second.mapKe));
            p.hasEmit = fmaxf(p.ke.x, fmaxf(p.ke.y, p.ke.z)) > 0.001f;
        }
        p.transparent = p.alpha < 0.999f || texHasAlpha(p.tex);
        model.parts.push_back(std::move(p));
        int id = (int)model.parts.size() - 1;
        partOf[name] = id;
        return id;
    };

    struct Corner { int v, t, n; };
    auto fix = [](int i, size_t cnt) { return i > 0 ? i - 1 : (i < 0 ? (int)cnt + i : -1); };

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "mtllib") {
            std::string n = restOfLine(ss);
            auto m = loadMtl(dir + n);
            mats.insert(m.begin(), m.end());
        } else if (tag == "v") {
            Vector3 v{0, 0, 0};
            ss >> v.x >> v.y >> v.z;
            P.push_back(v);
        } else if (tag == "vt") {
            Vector2 t{0, 0};
            ss >> t.x >> t.y;
            T.push_back(t);
        } else if (tag == "vn") {
            Vector3 v{0, 0, 0};
            ss >> v.x >> v.y >> v.z;
            N.push_back(v);
        } else if (tag == "usemtl") {
            cur = getPart(restOfLine(ss));
        } else if (tag == "f") {
            if (cur < 0) cur = getPart("default");
            std::vector<Corner> cs;
            std::string tok;
            while (ss >> tok) {
                int vals[3] = {0, 0, 0};
                int k = 0;
                size_t pos = 0;
                while (k < 3) {
                    size_t s = tok.find('/', pos);
                    std::string part = tok.substr(pos, s == std::string::npos ? std::string::npos : s - pos);
                    if (!part.empty()) vals[k] = std::atoi(part.c_str());
                    k++;
                    if (s == std::string::npos) break;
                    pos = s + 1;
                }
                cs.push_back({fix(vals[0], P.size()), fix(vals[1], T.size()), fix(vals[2], N.size())});
            }
            Part& pt = model.parts[cur];
            for (size_t i = 1; i + 1 < cs.size(); i++) {
                Corner tri[3] = {cs[0], cs[i], cs[i + 1]};
                if (tri[0].v < 0 || tri[1].v < 0 || tri[2].v < 0) continue;
                Vector3 fn = Vector3Normalize(Vector3CrossProduct(
                    Vector3Subtract(P[tri[1].v], P[tri[0].v]), Vector3Subtract(P[tri[2].v], P[tri[0].v])));
                for (int k = 0; k < 3; k++) {
                    const Corner& c = tri[k];
                    std::array<int, 4> key = {cur, c.v, c.t, c.n};
                    if (c.n >= 0) {
                        auto it = cache.find(key);
                        if (it != cache.end()) {
                            pt.idx.push_back(it->second);
                            continue;
                        }
                    }
                    Vert vt;
                    vt.p = P[c.v];
                    vt.n = c.n >= 0 ? Vector3Normalize(N[c.n]) : fn;
                    vt.u = c.t >= 0 ? T[c.t].x : 0;
                    vt.v = c.t >= 0 ? 1 - T[c.t].y : 0;
                    pt.v.push_back(vt);
                    int id = (int)pt.v.size() - 1;
                    if (c.n >= 0) cache[key] = id;
                    pt.idx.push_back(id);
                }
            }
        }
    }

    model.parts.erase(std::remove_if(model.parts.begin(), model.parts.end(),
                                     [](const Part& p) { return p.idx.empty(); }),
                      model.parts.end());
    if (model.parts.empty()) return model;

    Vector3 mn = {1e30f, 1e30f, 1e30f}, mx = {-1e30f, -1e30f, -1e30f};
    for (const Part& p : model.parts)
        for (const Vert& v : p.v) {
            mn = Vector3Min(mn, v.p);
            mx = Vector3Max(mx, v.p);
        }
    Vector3 c = Vector3Scale(Vector3Add(mn, mx), 0.5f);
    float rad = 1e-6f;
    for (const Part& p : model.parts)
        for (const Vert& v : p.v) rad = fmaxf(rad, Vector3Distance(v.p, c));
    for (Part& p : model.parts)
        for (Vert& v : p.v) v.p = Vector3Scale(Vector3Subtract(v.p, c), 1.0f / rad);
    return model;
}

enum ShaderKind { SH_LIT, SH_SUN, SH_RIM, SH_SHIP };

struct Body {
    const char* name;
    int parent;
    float radius, orbitR, orbitSpeed, spinSpeed;
    Color color;
    ShaderKind shader;
    const ModelData* model;
    float orbitAng = 0, spinAng = 0;
    Vector3 pos = {0, 0, 0};
};

struct Xform {
    Vector3 ax, ay, az, pos;
    float s;
};

static Vector3 camPos = {0, 40, 150}, camF, camR, camU;
static float yaw = 0, pitch = -0.25f;

static void updateCamBasis() {
    camF = {cosf(pitch) * sinf(yaw), sinf(pitch), -cosf(pitch) * cosf(yaw)};
    camR = Vector3Normalize(Vector3CrossProduct(camF, {0, 1, 0}));
    camU = Vector3CrossProduct(camR, camF);
}

static Vector3 rotate(const Xform& X, Vector3 q) {
    return Vector3Add(Vector3Add(Vector3Scale(X.ax, q.x), Vector3Scale(X.ay, q.y)), Vector3Scale(X.az, q.z));
}

static Color shade(ShaderKind k, Color c, float u, Vector3 n, Vector3 wp, float t, Vector3 em) {
    if (k == SH_SUN) {
        float m = 0.9f + 0.1f * sinf(t * 2 + u * 20);
        return {clamp8(c.r * m + em.x), clamp8(c.g * m + em.y), clamp8(c.b * m + em.z), 255};
    }
    n = Vector3Normalize(n);
    Vector3 L = Vector3Normalize(Vector3Subtract(SUN_POS, wp));
    float d = fmaxf(0, Vector3DotProduct(n, L));
    float amb = k == SH_SHIP ? 0.35f : 0.15f;
    float li = amb + (1 - amb) * d;
    float r = c.r * li + em.x, g = c.g * li + em.y, bl = c.b * li + em.z;
    if (k == SH_RIM) {
        Vector3 V = Vector3Normalize(Vector3Subtract(camPos, wp));
        float rim = powf(1 - fmaxf(0, Vector3DotProduct(n, V)), 3) * d;
        r += 60 * rim;
        g += 120 * rim;
        bl += 255 * rim;
    }
    return {clamp8(r), clamp8(g), clamp8(bl), 255};
}

struct TV { Vector3 w, n; float sx, sy, iz, u, v; bool ok; };

static bool hasTransparent(const ModelData& m) {
    for (const Part& p : m.parts)
        if (p.transparent) return true;
    return false;
}

static void drawModel(const ModelData& m, const Xform& X, ShaderKind kind, const Color* flat, float t, int pass) {
    static std::vector<TV> tv;
    const float f = (H / 2.f) / tanf(FOV_DEG * DEG2RAD / 2);

    for (const Part& part : m.parts) {
        if ((pass == 1) != part.transparent) continue;
        tv.resize(part.v.size());
        for (size_t i = 0; i < part.v.size(); i++) {
            const Vert& s = part.v[i];
            Vector3 w = Vector3Add(X.pos, Vector3Scale(rotate(X, s.p), X.s));
            Vector3 d = Vector3Subtract(w, camPos);
            float x = Vector3DotProduct(d, camR), y = Vector3DotProduct(d, camU),
                  z = Vector3DotProduct(d, camF);
            TV& o = tv[i];
            o.w = w;
            o.n = rotate(X, s.n);
            o.u = s.u;
            o.v = s.v;
            o.ok = z > NEAR_Z;
            if (o.ok) {
                o.iz = 1 / z;
                o.sx = W / 2.f + x * o.iz * f;
                o.sy = H / 2.f - y * o.iz * f;
            }
        }

        for (int face = pass == 1 ? 0 : 1; face < 2; face++) {
            for (size_t k = 0; k + 2 < part.idx.size(); k += 3) {
                const TV& A = tv[part.idx[k]];
                const TV& B = tv[part.idx[k + 1]];
                const TV& C = tv[part.idx[k + 2]];
                if (!A.ok || !B.ok || !C.ok) continue;

                Vector3 fn = Vector3CrossProduct(Vector3Subtract(B.w, A.w), Vector3Subtract(C.w, A.w));
                bool front = Vector3DotProduct(fn, Vector3Subtract(A.w, camPos)) < 0;
                if (pass == 1) {
                    if ((face == 1) != front) continue;
                } else if (cullBack && !front) {
                    continue;
                }

                float area = (B.sx - A.sx) * (C.sy - A.sy) - (B.sy - A.sy) * (C.sx - A.sx);
                if (fabsf(area) < 1e-6f) continue;
                int x0 = std::max(0, (int)floorf(fminf(A.sx, fminf(B.sx, C.sx))));
                int x1 = std::min(W - 1, (int)ceilf(fmaxf(A.sx, fmaxf(B.sx, C.sx))));
                int y0 = std::max(0, (int)floorf(fminf(A.sy, fminf(B.sy, C.sy))));
                int y1 = std::min(H - 1, (int)ceilf(fmaxf(A.sy, fmaxf(B.sy, C.sy))));

                for (int py = y0; py <= y1; py++)
                    for (int px = x0; px <= x1; px++) {
                        float X0 = px + 0.5f, Y0 = py + 0.5f;
                        float w0 = ((B.sx - X0) * (C.sy - Y0) - (B.sy - Y0) * (C.sx - X0)) / area;
                        float w1 = ((C.sx - X0) * (A.sy - Y0) - (C.sy - Y0) * (A.sx - X0)) / area;
                        float w2 = 1 - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float iz = w0 * A.iz + w1 * B.iz + w2 * C.iz;
                        int id = py * W + px;
                        if (iz <= zb[id]) continue;
                        float q0 = w0 * A.iz / iz, q1 = w1 * B.iz / iz, q2 = w2 * C.iz / iz;
                        float u = q0 * A.u + q1 * B.u + q2 * C.u;
                        float v = q0 * A.v + q1 * B.v + q2 * C.v;
                        Color base = flat ? *flat : (part.tex.w > 0 ? part.tex.sample(u, v) : part.kd);
                        float al = pass == 1 ? (base.a / 255.f) * part.alpha : 1.f;
                        if (al <= 0.02f) continue;
                        Vector3 n = Vector3Add(Vector3Add(Vector3Scale(A.n, q0), Vector3Scale(B.n, q1)),
                                               Vector3Scale(C.n, q2));
                        if (pass == 1 && face == 0) n = Vector3Negate(n);
                        Vector3 wp = Vector3Add(Vector3Add(Vector3Scale(A.w, q0), Vector3Scale(B.w, q1)),
                                                Vector3Scale(C.w, q2));
                        Vector3 em = {0, 0, 0};
                        if (part.hasEmit) {
                            if (part.emitTex.w > 0) {
                                Color e = part.emitTex.sample(u, v);
                                float k = fmaxf(part.ke.x, fmaxf(part.ke.y, part.ke.z));
                                em = {e.r * k, e.g * k, e.b * k};
                            } else {
                                em = {part.ke.x * 255, part.ke.y * 255, part.ke.z * 255};
                            }
                        }
                        Color c = shade(kind, base, u, n, wp, t, em);
                        if (pass == 0) {
                            zb[id] = iz;
                            fb[id] = c;
                        } else {
                            Color d = fb[id];
                            fb[id] = {clamp8(d.r + (c.r - d.r) * al), clamp8(d.g + (c.g - d.g) * al),
                                      clamp8(d.b + (c.b - d.b) * al), 255};
                        }
                    }
            }
        }
    }
}

static int countTris(const ModelData& m) {
    int n = 0;
    for (const Part& p : m.parts) n += (int)p.idx.size() / 3;
    return n;
}

static std::vector<Vector3> stars;

static void drawStars() {
    const float f = (H / 2.f) / tanf(FOV_DEG * DEG2RAD / 2);
    for (const Vector3& s : stars) {
        float z = Vector3DotProduct(s, camF);
        if (z < 0.01f) continue;
        int px = (int)(W / 2.f + Vector3DotProduct(s, camR) / z * f);
        int py = (int)(H / 2.f - Vector3DotProduct(s, camU) / z * f);
        if (px < 0 || px >= W || py < 0 || py >= H) continue;
        unsigned char c = 120 + (unsigned)(hash2(px, py, 7) * 135);
        fb[py * W + px] = {c, c, c, 255};
    }
}

int main() {
    InitWindow(WIN_W, WIN_H, "Sistema solar (software renderer)");
    SetTargetFPS(60);
    DisableCursor();

    Image img = GenImageColor(W, H, BLACK);
    Texture2D screen = LoadTextureFromImage(img);
    UnloadImage(img);

    ModelData sphereLod[3] = {makeSphere(8, 16), makeSphere(16, 32), makeSphere(40, 80)};
    ModelData ship = loadObj("assets/Spaceship.obj");
    const char* files[7] = {"AgujeroNegro", "PuzzlePlank", "PopStar", "ShiverStar",
                            "Moon", "CosmicCove", "RollingMasterpiece"};
    ModelData models[7];
    for (int i = 0; i < 7; i++) models[i] = loadObj(std::string("assets/") + files[i] + ".obj");
    auto mp = [&](int i) -> const ModelData* { return models[i].parts.empty() ? nullptr : &models[i]; };
    TraceLog(LOG_INFO, "Carpeta de trabajo: %s", GetWorkingDirectory());
    TraceLog(LOG_INFO, "Nave: %d partes, %d triangulos", (int)ship.parts.size(), countTris(ship));
    for (int i = 0; i < 7; i++) TraceLog(LOG_INFO, "%s: %d partes, %d triangulos", files[i], (int)models[i].parts.size(), countTris(models[i]));

    std::vector<Body> bodies = {
        {"AgujeroNegro", -1, 24, 0, 0, 0.15f, {255, 200, 60, 255}, SH_SUN, mp(0)},
        {"PuzzlePlank", -1, 6, 65, 0.55f, 0.6f, {200, 160, 90, 255}, SH_LIT, mp(1)},
        {"PopStar", -1, 8, 100, 0.4f, 0.8f, {255, 150, 200, 255}, SH_LIT, mp(2)},
        {"ShiverStar", -1, 8, 140, 0.3f, 0.5f, {150, 210, 255, 255}, SH_LIT, mp(3)},
        {"Moon", 3, 2, 16, 1.2f, 0.3f, {200, 200, 200, 255}, SH_LIT, mp(4)},
        {"CosmicCove", -1, 9, 185, 0.22f, 0.7f, {60, 120, 200, 255}, SH_LIT, mp(5)},
        {"RollingMasterpiece", -1, 7, 230, 0.15f, 1.0f, {220, 120, 60, 255}, SH_LIT, mp(6)},
    };

    for (int i = 0; i < 1500; i++) {
        Vector3 d = {(float)GetRandomValue(-1000, 1000), (float)GetRandomValue(-1000, 1000),
                     (float)GetRandomValue(-1000, 1000)};
        if (Vector3Length(d) > 10) stars.push_back(Vector3Normalize(d));
    }

    float t = 0;
    int target = -1;
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        t += dt;

        if (IsKeyPressed(KEY_C)) cullBack = !cullBack;
        if (IsKeyPressed(KEY_P)) {
            target = (target + 1) % (int)bodies.size();
            const Body& pb = bodies[target];
            Vector3 toSun = Vector3Subtract(SUN_POS, pb.pos);
            toSun = Vector3Length(toSun) < 1e-3f ? Vector3{0, 0, 1} : Vector3Normalize(toSun);
            camPos = Vector3Add(pb.pos, Vector3Scale(toSun, pb.radius * 3.5f + SHIP_DIST));
            Vector3 dir = Vector3Normalize(Vector3Subtract(pb.pos, camPos));
            yaw = atan2f(dir.x, -dir.z);
            pitch = asinf(dir.y);
        }
        Vector2 md = GetMouseDelta();
        yaw += md.x * 0.0025f;
        pitch = Clamp(pitch - md.y * 0.0025f, -1.5f, 1.5f);
        updateCamBasis();
        float sp = (IsKeyDown(KEY_LEFT_SHIFT) ? 80.f : 20.f) * dt;
        if (IsKeyDown(KEY_W)) camPos = Vector3Add(camPos, Vector3Scale(camF, sp));
        if (IsKeyDown(KEY_S)) camPos = Vector3Subtract(camPos, Vector3Scale(camF, sp));
        if (IsKeyDown(KEY_D)) camPos = Vector3Add(camPos, Vector3Scale(camR, sp));
        if (IsKeyDown(KEY_A)) camPos = Vector3Subtract(camPos, Vector3Scale(camR, sp));
        if (IsKeyDown(KEY_SPACE)) camPos.y += sp;
        if (IsKeyDown(KEY_LEFT_CONTROL)) camPos.y -= sp;

        for (Body& b : bodies) {
            b.orbitAng += b.orbitSpeed * dt;
            b.spinAng += b.spinSpeed * dt;
            Vector3 base = b.parent >= 0 ? bodies[b.parent].pos : Vector3{0, 0, 0};
            b.pos = {base.x + cosf(b.orbitAng) * b.orbitR, base.y, base.z + sinf(b.orbitAng) * b.orbitR};
        }

        for (const Body& b : bodies) {
            Vector3 d = Vector3Subtract(camPos, b.pos);
            float len = Vector3Length(d), minD = b.radius * 1.1f + 0.5f + SHIP_DIST;
            if (len < minD && len > 1e-4f) camPos = Vector3Add(b.pos, Vector3Scale(d, minD / len));
        }

        std::fill(fb.begin(), fb.end(), Color{0, 0, 0, 255});
        std::fill(zb.begin(), zb.end(), 0.f);
        drawStars();

        auto bodyXform = [](const Body& b) {
            float cs = cosf(b.spinAng), sn = sinf(b.spinAng);
            return Xform{{cs, 0, -sn}, {0, 1, 0}, {sn, 0, cs}, b.pos, b.radius};
        };

        std::vector<const Body*> translucent;
        for (const Body& b : bodies) {
            Xform X = bodyXform(b);
            if (b.model) {
                drawModel(*b.model, X, b.shader, nullptr, t, 0);
                if (hasTransparent(*b.model)) translucent.push_back(&b);
            } else {
                float ratio = Vector3Distance(b.pos, camPos) / b.radius;
                int L = ratio > 40 ? 0 : (ratio > 12 ? 1 : 2);
                drawModel(sphereLod[L], X, b.shader, &b.color, t, 0);
            }
        }
        std::sort(translucent.begin(), translucent.end(), [](const Body* a, const Body* b) {
            return Vector3DistanceSqr(a->pos, camPos) > Vector3DistanceSqr(b->pos, camPos);
        });
        for (const Body* b : translucent) drawModel(*b->model, bodyXform(*b), b->shader, nullptr, t, 1);

        if (!ship.parts.empty()) {
            std::fill(zb.begin(), zb.end(), 0.f);
            Vector3 sp3 = Vector3Add(camPos, Vector3Add(Vector3Scale(camF, SHIP_DIST), Vector3Scale(camU, -SHIP_DOWN)));
            Xform S = SHIP_ORIENT == 0 ? Xform{camR, camU, Vector3Negate(camF), sp3, SHIP_SIZE}
                    : SHIP_ORIENT == 1 ? Xform{camR, camF, camU, sp3, SHIP_SIZE}
                    : SHIP_ORIENT == 2 ? Xform{camR, Vector3Negate(camU), camF, sp3, SHIP_SIZE}
                                       : Xform{camR, Vector3Negate(camF), Vector3Negate(camU), sp3, SHIP_SIZE};
            if (SHIP_ROLL != 0) {
                S.ax = Vector3RotateByAxisAngle(S.ax, S.ay, SHIP_ROLL * PI / 2);
                S.az = Vector3RotateByAxisAngle(S.az, S.ay, SHIP_ROLL * PI / 2);
            }
            drawModel(ship, S, SH_SHIP, nullptr, t, 0);
            if (hasTransparent(ship)) drawModel(ship, S, SH_SHIP, nullptr, t, 1);
        }

        UpdateTexture(screen, fb.data());
        BeginDrawing();
        DrawTexturePro(screen, {0, 0, (float)W, (float)H}, {0, 0, (float)WIN_W, (float)WIN_H}, {0, 0}, 0, WHITE);
        EndDrawing();
        SetWindowTitle(TextFormat("Sistema solar | %d FPS | culling %s", GetFPS(), cullBack ? "ON" : "OFF"));
    }
    CloseWindow();
    return 0;
}
