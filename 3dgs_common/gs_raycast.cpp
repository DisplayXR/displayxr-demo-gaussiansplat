// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CPU ray cast against a Gaussian-splat cloud. See gs_raycast.h.
 */

#include "gs_raycast.h"
#include "gs_scene_loader.h"   // GsVertex

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace {

constexpr uint32_t kLeafSize = 8;   // splats per leaf
constexpr uint32_t kFanout = 8;     // children per internal node

//! Spread the low 21 bits of v to every third bit (Morton interleave).
uint64_t Spread3(uint64_t v)
{
    v &= 0x1FFFFFull;
    v = (v | (v << 32)) & 0x1F00000000FFFFull;
    v = (v | (v << 16)) & 0x1F0000FF0000FFull;
    v = (v | (v << 8)) & 0x100F00F00F00F00Full;
    v = (v | (v << 4)) & 0x10C30C30C30C30C3ull;
    v = (v | (v << 2)) & 0x1249249249249249ull;
    return v;
}

//! Rotation matrix of a unit quaternion (w, x, y, z), the shaders' convention
//! (rotationFromQuaternion): covariance = R S^2 R^T.
void QuatToR(float qw, float qx, float qy, float qz, float R[3][3])
{
    R[0][0] = 1 - 2 * (qy * qy + qz * qz); R[0][1] = 2 * (qx * qy - qw * qz); R[0][2] = 2 * (qx * qz + qw * qy);
    R[1][0] = 2 * (qx * qy + qw * qz); R[1][1] = 1 - 2 * (qx * qx + qz * qz); R[1][2] = 2 * (qy * qz - qw * qx);
    R[2][0] = 2 * (qx * qz - qw * qy); R[2][1] = 2 * (qy * qz + qw * qx); R[2][2] = 1 - 2 * (qx * qx + qy * qy);
}

//! Ray / box slab test. [t0, t1] is the overlap with the ray's window.
inline bool RayBox(const float lo[3], const float hi[3], const float o[3], const float inv[3], float tMin,
                   float tMax, float& t0)
{
    float a = tMin, b = tMax;
    for (int k = 0; k < 3; k++) {
        float ta = (lo[k] - o[k]) * inv[k];
        float tb = (hi[k] - o[k]) * inv[k];
        if (ta > tb) std::swap(ta, tb);
        // NaN (0 * inf, origin on a face of a zero-direction slab) keeps the window.
        if (ta > a) a = ta;
        if (tb < b) b = tb;
        if (a > b) return false;
    }
    t0 = a;
    return true;
}

} // namespace

GsSplatRayCaster::~GsSplatRayCaster() { clear(); }

void GsSplatRayCaster::joinWorker()
{
    if (cancel_) cancel_->store(true);
    if (worker_.joinable()) worker_.join();
    cancel_.reset();
}

void GsSplatRayCaster::clear()
{
    joinWorker();
    std::lock_guard<std::mutex> lk(mu_);
    built_.reset();
}

bool GsSplatRayCaster::valid() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return built_ != nullptr;
}

GsSplatRayCaster::BuildStats GsSplatRayCaster::buildStats() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return built_ ? built_->stats : BuildStats();
}

void GsSplatRayCaster::build(const GsVertex* verts, size_t count, const Options& opt)
{
    clear();
    std::shared_ptr<Built> b = doBuild(verts, count, opt, nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    built_ = std::move(b);
}

void GsSplatRayCaster::build(const GsVertex* verts, size_t count) { build(verts, count, Options()); }

void GsSplatRayCaster::buildAsync(std::vector<GsVertex>&& verts) { buildAsync(std::move(verts), Options()); }

void GsSplatRayCaster::buildAsync(std::vector<GsVertex>&& verts, const Options& opt)
{
    clear();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancel_ = cancel;
    worker_ = std::thread([this, v = std::move(verts), opt, cancel]() mutable {
        std::shared_ptr<Built> b = doBuild(v.data(), v.size(), opt, cancel.get());
        std::vector<GsVertex>().swap(v);   // the CPU vertex copy dies with the build
        if (b == nullptr || cancel->load()) return;
        std::lock_guard<std::mutex> lk(mu_);
        built_ = std::move(b);
    });
}

std::shared_ptr<GsSplatRayCaster::Built> GsSplatRayCaster::doBuild(const GsVertex* verts, size_t count,
                                                                   const Options& opt,
                                                                   const std::atomic<bool>* cancel)
{
    const auto clock0 = std::chrono::steady_clock::now();
    auto cancelled = [cancel]() { return cancel != nullptr && cancel->load(std::memory_order_relaxed); };
    auto b = std::make_shared<Built>();
    b->opt = opt;
    b->k2 = opt.sigmaCutoff * opt.sigmaCutoff;
    b->stats.input = count;
    if (verts == nullptr || count == 0) return nullptr;

    // ── 1. Opacity filter, compact splats ────────────────────────────────
    std::vector<Splat> sp;
    sp.reserve(count);
    for (size_t i = 0; i < count; i++) {
        const GsVertex& v = verts[i];
        const float op = v.scale_opacity[3];
        if (!(op >= opt.minOpacity)) continue;
        const float px = v.position[0], py = v.position[1], pz = v.position[2];
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;
        float s[3] = {v.scale_opacity[0], v.scale_opacity[1], v.scale_opacity[2]};
        const float smax = std::max({s[0], s[1], s[2]});
        if (!(smax > 0.0f) || !std::isfinite(smax)) continue;
        float qw = v.rotation[0], qx = v.rotation[1], qy = v.rotation[2], qz = v.rotation[3];
        const float ql = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
        if (ql > 0.0f) { qw /= ql; qx /= ql; qy /= ql; qz /= ql; } else { qw = 1.0f; qx = qy = qz = 0.0f; }
        Splat o;
        o.mu[0] = px; o.mu[1] = py; o.mu[2] = pz;
        o.opacity = std::min(op, 0.99f);   // the rasteriser's clamp
        // A flat surfel's thin axis can be ~0; floor it so S^-1 stays finite.
        for (int j = 0; j < 3; j++) o.invScale[j] = 1.0f / std::max(s[j], std::max(1e-4f * smax, 1e-9f));
        o.q[0] = (int16_t)std::lround(qw * 32767.0f);
        o.q[1] = (int16_t)std::lround(qx * 32767.0f);
        o.q[2] = (int16_t)std::lround(qy * 32767.0f);
        o.q[3] = (int16_t)std::lround(qz * 32767.0f);
        sp.push_back(o);
    }
    if (sp.empty() || cancelled()) return nullptr;

    // ── 2. Floater trim — the rule pickGaussian's set uses: drop centres
    //       beyond 3x the half-range of the per-axis [p2, p98] core. ──────
    float lo[3], hi[3];
    if (sp.size() >= 1024) {
        const size_t n = sp.size();
        std::vector<float> c(n);
        const size_t loIdx = (size_t)(0.02 * (double)(n - 1));
        const size_t hiIdx = std::max(loIdx + 1, (size_t)(0.98 * (double)(n - 1)));
        for (int a = 0; a < 3; a++) {
            for (size_t i = 0; i < n; i++) c[i] = sp[i].mu[a];
            std::nth_element(c.begin(), c.begin() + loIdx, c.end());
            const float l = c[loIdx];
            std::nth_element(c.begin() + loIdx + 1, c.begin() + hiIdx, c.end());
            const float h = c[hiIdx];
            lo[a] = 0.5f * (l + h) - 1.5f * (h - l);
            hi[a] = 0.5f * (l + h) + 1.5f * (h - l);
        }
        size_t kept = 0;
        for (size_t i = 0; i < n; i++) {
            const float* p = sp[i].mu;
            if (p[0] < lo[0] || p[0] > hi[0] || p[1] < lo[1] || p[1] > hi[1] || p[2] < lo[2] || p[2] > hi[2])
                continue;
            sp[kept++] = sp[i];
        }
        sp.resize(kept);
    }
    if (sp.empty() || cancelled()) return nullptr;
    const size_t n = sp.size();

    // ── 3. Morton sort (LSD radix, 4 x 16-bit digits over 63-bit keys) ────
    for (int a = 0; a < 3; a++) { lo[a] = std::numeric_limits<float>::max(); hi[a] = -lo[a]; }
    for (const Splat& s : sp)
        for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], s.mu[a]); hi[a] = std::max(hi[a], s.mu[a]); }
    float scale[3];
    for (int a = 0; a < 3; a++) scale[a] = (hi[a] > lo[a]) ? 2097151.0f / (hi[a] - lo[a]) : 0.0f;
    std::vector<uint64_t> key(n), key2(n);
    std::vector<uint32_t> idx(n), idx2(n);
    for (size_t i = 0; i < n; i++) {
        uint64_t m = 0;
        for (int a = 0; a < 3; a++) {
            const uint64_t qv = (uint64_t)std::min(2097151.0f, std::max(0.0f, (sp[i].mu[a] - lo[a]) * scale[a]));
            m |= Spread3(qv) << a;
        }
        key[i] = m;
        idx[i] = (uint32_t)i;
    }
    {
        std::vector<uint32_t> hist(65536);
        for (int pass = 0; pass < 4; pass++) {
            if (cancelled()) return nullptr;
            const int shift = pass * 16;
            std::fill(hist.begin(), hist.end(), 0u);
            for (size_t i = 0; i < n; i++) hist[(key[i] >> shift) & 0xFFFF]++;
            uint32_t sum = 0;
            for (uint32_t& h : hist) { const uint32_t c = h; h = sum; sum += c; }
            for (size_t i = 0; i < n; i++) {
                const uint32_t dst = hist[(key[i] >> shift) & 0xFFFF]++;
                key2[dst] = key[i];
                idx2[dst] = idx[i];
            }
            key.swap(key2);
            idx.swap(idx2);
        }
    }
    std::vector<uint64_t>().swap(key);
    std::vector<uint64_t>().swap(key2);
    std::vector<uint32_t>().swap(idx2);
    b->splats.resize(n);
    for (size_t i = 0; i < n; i++) b->splats[i] = sp[idx[i]];
    std::vector<Splat>().swap(sp);
    std::vector<uint32_t>().swap(idx);
    if (cancelled()) return nullptr;

    // ── 4. Boxes: leaves from the splats' k-sigma boxes, then upward ──────
    const size_t leaves = (n + kLeafSize - 1) / kLeafSize;
    size_t total = leaves, lv = leaves;
    while (lv > 1) { lv = (lv + kFanout - 1) / kFanout; total += lv; }
    b->nodes.resize(total);
    b->levelStart.push_back(0);
    for (size_t L = 0; L < leaves; L++) {
        Box bx;
        for (int a = 0; a < 3; a++) { bx.lo[a] = std::numeric_limits<float>::max(); bx.hi[a] = -bx.lo[a]; }
        const size_t e = std::min(n, (L + 1) * kLeafSize);
        for (size_t i = L * kLeafSize; i < e; i++) {
            const Splat& s = b->splats[i];
            float R[3][3];
            QuatToR(s.q[0] / 32767.0f, s.q[1] / 32767.0f, s.q[2] / 32767.0f, s.q[3] / 32767.0f, R);
            for (int a = 0; a < 3; a++) {
                // Exact AABB of the k-sigma ellipsoid: k * sqrt(diag(R S^2 R^T)),
                // padded by 0.1% so a hit on a box face is never lost to rounding.
                float var = 0.0f;
                for (int j = 0; j < 3; j++) {
                    const float sj = 1.0f / s.invScale[j];
                    var += R[a][j] * R[a][j] * sj * sj;
                }
                const float h = 1.001f * opt.sigmaCutoff * std::sqrt(var);
                bx.lo[a] = std::min(bx.lo[a], s.mu[a] - h);
                bx.hi[a] = std::max(bx.hi[a], s.mu[a] + h);
            }
        }
        b->nodes[L] = bx;
    }
    size_t prevStart = 0, prevCount = leaves, next = leaves;
    while (prevCount > 1) {
        b->levelStart.push_back((uint32_t)next);
        const size_t cnt = (prevCount + kFanout - 1) / kFanout;
        for (size_t j = 0; j < cnt; j++) {
            Box bx;
            for (int a = 0; a < 3; a++) { bx.lo[a] = std::numeric_limits<float>::max(); bx.hi[a] = -bx.lo[a]; }
            const size_t e = std::min(prevCount, (j + 1) * kFanout);
            for (size_t c = j * kFanout; c < e; c++) {
                const Box& cb = b->nodes[prevStart + c];
                for (int a = 0; a < 3; a++) {
                    bx.lo[a] = std::min(bx.lo[a], cb.lo[a]);
                    bx.hi[a] = std::max(bx.hi[a], cb.hi[a]);
                }
            }
            b->nodes[next + j] = bx;
        }
        prevStart = next;
        next += cnt;
        prevCount = cnt;
    }
    b->levelStart.push_back((uint32_t)next);

    b->stats.kept = n;
    b->stats.nodes = b->nodes.size();
    b->stats.levels = (uint32_t)b->levelStart.size() - 1;
    b->stats.bytes = n * sizeof(Splat) + b->nodes.size() * sizeof(Box);
    b->stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clock0).count();
    return b;
}

bool GsSplatRayCaster::evalSplat(const Built& b, const Splat& s, const float o[3], const float d[3], Hit& h)
{
    // Re-origin the ray at its Euclidean closest approach to the centre, t0,
    // so the vector fed through S^-1 is the short perpendicular one, not the
    // whole eye distance: a flat surfel's S^-1 is ~1e4 along its normal, and
    // c - b^2/a on the long vector cancels catastrophically (a splat 7 sigma
    // off the ray then reads as a hit). Double for the same reason.
    const double dd = (double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2];
    if (!(dd > 0.0)) return false;
    const double t0 = ((double)(s.mu[0] - o[0]) * d[0] + (double)(s.mu[1] - o[1]) * d[1] +
                       (double)(s.mu[2] - o[2]) * d[2]) / dd;
    const double px = (double)o[0] + t0 * d[0] - s.mu[0];
    const double py = (double)o[1] + t0 * d[1] - s.mu[1];
    const double pz = (double)o[2] + t0 * d[2] - s.mu[2];
    // Cheap cull before the rotation: the k-sigma ellipsoid lies inside the
    // sphere of radius k * (largest scale), so a ray passing outside it (most
    // of a box's neighbours) cannot reach D^2 <= k^2.
    {
        const float minInv = std::min({s.invScale[0], s.invScale[1], s.invScale[2]});
        const double r = 1.0 / (double)minInv;
        // p = o + t0 d - mu is the perpendicular from the centre to the ray
        // line, in scene units whatever |d| is.
        if ((px * px + py * py + pz * pz) > (double)b.k2 * r * r) return false;
    }
    float R[3][3];
    QuatToR(s.q[0] / 32767.0f, s.q[1] / 32767.0f, s.q[2] / 32767.0f, s.q[3] / 32767.0f, R);
    // Into the unit-sphere frame: x' = S^-1 R^T x.
    double ov[3], vv[3];
    for (int j = 0; j < 3; j++) {
        ov[j] = (R[0][j] * px + R[1][j] * py + R[2][j] * pz) * s.invScale[j];
        vv[j] = ((double)R[0][j] * d[0] + (double)R[1][j] * d[1] + (double)R[2][j] * d[2]) * s.invScale[j];
    }
    const double a = vv[0] * vv[0] + vv[1] * vv[1] + vv[2] * vv[2];
    if (!(a > 0.0)) return false;
    const double ts = -(ov[0] * vv[0] + ov[1] * vv[1] + ov[2] * vv[2]) / a;   // relative to t0
    const double wx = ov[0] + ts * vv[0], wy = ov[1] + ts * vv[1], wz = ov[2] + ts * vv[2];
    const double d2 = wx * wx + wy * wy + wz * wz;   // min Mahalanobis^2 along the ray
    if (d2 > (double)b.k2) return false;
    const float alpha = s.opacity * std::exp(-0.5f * (float)d2);
    if (alpha < (1.0f / 255.0f)) return false;   // the rasteriser's floor
    h.t = (float)(t0 + ts);
    h.alpha = alpha;
    return true;
}

bool GsSplatRayCaster::raycast(const float o[3], const float d[3], float tMin, float tMax, float& tHit,
                               RayStats* rs) const
{
    std::shared_ptr<const Built> bp;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bp = built_;
    }
    if (rs) *rs = RayStats();
    if (!bp || !(tMax > tMin)) return false;
    const Built& b = *bp;

    struct NodeRef {
        float t;
        uint32_t level;
        uint32_t index;
    };
    thread_local std::vector<NodeRef> heap;
    thread_local std::vector<Hit> hits;
    heap.clear();
    hits.clear();
    auto nodeGreater = [](const NodeRef& x, const NodeRef& y) { return x.t > y.t; };
    auto hitGreater = [](const Hit& x, const Hit& y) { return x.t > y.t; };

    uint32_t work = 0, nodes = 0, tests = 0;
    float T = 1.0f;
    const float thr = b.opt.transmittanceThreshold;
    auto finish = [&](bool hit) {
        if (rs) { rs->nodes = nodes; rs->tests = tests; rs->transmittance = T; }
        return hit;
    };
    // Composite pending hits in order up to `frontier`; true once opaque.
    auto drain = [&](float frontier) -> bool {
        while (!hits.empty() && hits.front().t < frontier) {
            std::pop_heap(hits.begin(), hits.end(), hitGreater);
            const Hit h = hits.back();
            hits.pop_back();
            T *= (1.0f - h.alpha);
            if (T <= thr) { tHit = h.t; return true; }
        }
        return false;
    };
    auto addHit = [&](const Splat& s) {
        Hit h;
        tests++;
        work++;
        if (evalSplat(b, s, o, d, h) && h.t >= tMin && h.t <= tMax) {
            hits.push_back(h);
            std::push_heap(hits.begin(), hits.end(), hitGreater);
        }
    };

    if (b.opt.bruteForce) {
        for (const Splat& s : b.splats) addHit(s);
        return finish(drain(std::numeric_limits<float>::infinity()));
    }

    float inv[3];
    for (int k = 0; k < 3; k++) inv[k] = 1.0f / d[k];   // +-inf on an axis-parallel ray: the slab test copes
    const uint32_t root = (uint32_t)b.levelStart.size() - 2;   // top level, one node
    float t0;
    nodes++;
    if (RayBox(b.nodes[b.levelStart[root]].lo, b.nodes[b.levelStart[root]].hi, o, inv, tMin, tMax, t0))
        heap.push_back({t0, root, 0});

    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), nodeGreater);
        const NodeRef nr = heap.back();
        heap.pop_back();
        // Nothing still queued can produce a hit before nr.t: hits before it are final.
        if (drain(nr.t)) return finish(true);
        if (work > b.opt.maxWorkPerRay) return finish(false);

        if (nr.level == 0) {
            const size_t s0 = (size_t)nr.index * kLeafSize;
            const size_t s1 = std::min(b.splats.size(), s0 + kLeafSize);
            for (size_t i = s0; i < s1; i++) addHit(b.splats[i]);
            continue;
        }
        const uint32_t childLevel = nr.level - 1;
        const uint32_t cBase = b.levelStart[childLevel];
        const uint32_t cCount = b.levelStart[childLevel + 1] - cBase;
        const uint32_t c0 = nr.index * kFanout;
        const uint32_t c1 = std::min(cCount, c0 + kFanout);
        for (uint32_t c = c0; c < c1; c++) {
            nodes++;
            work++;
            const Box& bx = b.nodes[cBase + c];
            if (RayBox(bx.lo, bx.hi, o, inv, tMin, tMax, t0)) {
                heap.push_back({t0, childLevel, c});
                std::push_heap(heap.begin(), heap.end(), nodeGreater);
            }
        }
    }
    return finish(drain(std::numeric_limits<float>::infinity()));
}
