// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the camera-rig rest anchor math (gs_rest_anchor.h,
 *         #162): the rest eye's offset from the CURRENT canvas centre, the
 *         rig gains measured from an untracked locate, and the cancellation.
 *         No GPU, no OpenXR. Exit code 0 = all passed.
 */

#include "gs_rest_anchor.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            g_failures++;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                      \
    do {                                                                           \
        const double a_ = (a), b_ = (b);                                           \
        if (!(std::fabs(a_ - b_) <= (tol))) {                                      \
            std::printf("FAIL %s:%d: %s = %.6f, expected %.6f (tol %.1e)\n",       \
                        __FILE__, __LINE__, #a, a_, b_, (double)(tol));            \
            g_failures++;                                                          \
        }                                                                          \
    } while (0)

// The 8K Leia panel PR #163 measured on: 7680x4320 px; the metre size is what
// its log implies (canvas centre (-0.2892, 0.1559) m for 1280x720 at 18,85).
constexpr uint32_t kPxW = 7680, kPxH = 4320;
constexpr float kWM = 0.6980f, kHM = 0.3927f;

// A synthetic camera rig: moves the eye `poseGain` m per metre of offset and
// shears the frustum `tanGain` tan per metre. What the runtime would hand back.
struct FakeRig {
    float poseGain, tanGain;
    void locate(float offX, float offY, float eyeLocal[2], float tan[2]) const
    {
        eyeLocal[0] = poseGain * offX;
        eyeLocal[1] = poseGain * offY;
        tan[0] = tanGain * offX;
        tan[1] = tanGain * offY;
    }
};

void TestCornerWindowMatchesWindowsLog()
{
    float offX = 0, offY = 0, cx = 0, cy = 0;
    CHECK(GsRestOffsetFromCanvas(0.0f, 0.10f, 18, 85, 1280, 720, kPxW, kPxH, kWM, kHM,
                                 &offX, &offY, &cx, &cy));
    CHECK_NEAR(cx, -0.2892, 5e-4);
    CHECK_NEAR(cy, 0.1559, 5e-4);
    CHECK_NEAR(offX, 0.2892, 5e-4);
    CHECK_NEAR(offY, -0.0559, 5e-4);
}

void TestCentredFullscreenIsTheOldAnchor()
{
    // A canvas centred on the panel: the canvas centre is the panel centre, so
    // the offset is the rest eye's display-space position — exactly what the
    // old one-shot anchor cancelled. (ds1-linux log: eye=(0.0000, 0.1000).)
    float offX = 1, offY = 1, cx = 1, cy = 1;
    CHECK(GsRestOffsetFromCanvas(0.0f, 0.10f, 0, 0, 3840, 2160, 3840, 2160, 0.3455f, 0.1943f,
                                 &offX, &offY, &cx, &cy));
    CHECK_NEAR(cx, 0.0, 1e-6);
    CHECK_NEAR(cy, 0.0, 1e-6);
    CHECK_NEAR(offX, 0.0, 1e-6);
    CHECK_NEAR(offY, 0.10, 1e-6);
}

void TestMoveTracksTheWindow()
{
    // 900 px right moves the canvas centre 900/7680 * W right, so the offset
    // shrinks by exactly that in x and is unchanged in y.
    float ax, ay, bx, by;
    CHECK(GsRestOffsetFromCanvas(0.0f, 0.10f, 18, 85, 1280, 720, kPxW, kPxH, kWM, kHM, &ax, &ay));
    CHECK(GsRestOffsetFromCanvas(0.0f, 0.10f, 918, 85, 1280, 720, kPxW, kPxH, kWM, kHM, &bx, &by));
    CHECK_NEAR(ax - bx, 900.0 / kPxW * kWM, 1e-5);
    CHECK_NEAR(ay, by, 1e-6);
    // Pixels run down, metres run up: a canvas lower on the panel has a lower
    // centre, so the (above-centre) eye's offset grows in +y.
    float dx, dy;
    CHECK(GsRestOffsetFromCanvas(0.0f, 0.10f, 18, 1085, 1280, 720, kPxW, kPxH, kWM, kHM, &dx, &dy));
    CHECK_NEAR(dy - ay, 1000.0 / kPxH * kHM, 1e-5);
}

void TestUnknownGeometryRefuses()
{
    float offX = 42, offY = 42;
    CHECK(!GsRestOffsetFromCanvas(0, 0.1f, 0, 0, 0, 720, kPxW, kPxH, kWM, kHM, &offX, &offY));
    CHECK(!GsRestOffsetFromCanvas(0, 0.1f, 0, 0, 1280, 0, kPxW, kPxH, kWM, kHM, &offX, &offY));
    CHECK(!GsRestOffsetFromCanvas(0, 0.1f, 0, 0, 1280, 720, 0, kPxH, kWM, kHM, &offX, &offY));
    CHECK(!GsRestOffsetFromCanvas(0, 0.1f, 0, 0, 1280, 720, kPxW, 0, kWM, kHM, &offX, &offY));
    CHECK(!GsRestOffsetFromCanvas(0, 0.1f, 0, 0, 1280, 720, kPxW, kPxH, 0.0f, kHM, &offX, &offY));
    CHECK(!GsRestOffsetFromCanvas(0, 0.1f, 0, 0, 1280, 720, kPxW, kPxH, kWM, NAN, &offX, &offY));
    CHECK(offX == 42 && offY == 42);
}

void TestGainsRecoverTheRig()
{
    // PR #163's measured gains: 1.0000 m/m and -1/pivot with pivot 1.849 m.
    const FakeRig rig{1.0f, -1.0f / 1.849f};
    const float offX = 0.2892f, offY = -0.0559f;
    float eye[2], tan[2];
    rig.locate(offX, offY, eye, tan);
    float pg = 0, tg = 0;
    CHECK(GsRestGainsFromLocate(offX, offY, eye[0], eye[1], tan[0], tan[1], &pg, &tg));
    CHECK_NEAR(pg, 1.0, 1e-5);
    CHECK_NEAR(tg, -0.54083, 1e-4);

    // Stereo terms are symmetric about the head and drop out of the mean.
    const float halfB = 0.0315f, halfTan = 0.0315f / 1.849f;
    const float meanEyeX = 0.5f * ((eye[0] - halfB) + (eye[0] + halfB));
    const float meanTanU = 0.5f * ((tan[0] + halfTan) + (tan[0] - halfTan));
    CHECK(GsRestGainsFromLocate(offX, offY, meanEyeX, eye[1], meanTanU, tan[1], &pg, &tg));
    CHECK_NEAR(pg, 1.0, 1e-5);
    CHECK_NEAR(tg, -0.54083, 1e-4);
}

void TestTinyOffsetKeepsGain()
{
    float pg = 7, tg = 7;
    CHECK(!GsRestGainsFromLocate(0.005f, 0.005f, 0.005f, 0.005f, -0.003f, -0.003f, &pg, &tg));
    CHECK(pg == 7 && tg == 7);
}

void TestCancellationNullsTheRig()
{
    // Whatever the window does, declaring (camera + poseDelta) and adding
    // tanDelta must land the rest eye on the capture camera with a centred
    // frustum: rig displacement + correction == 0.
    const FakeRig rig{1.0f, -0.5409f};
    const int rects[][4] = {{18, 85, 1280, 720}, {918, 85, 1280, 720}, {0, 0, 7680, 4320},
                            {6000, 3000, 1600, 1200}};
    for (const auto& r : rects) {
        float offX, offY;
        CHECK(GsRestOffsetFromCanvas(0.0f, 0.10f, r[0], r[1], r[2], r[3], kPxW, kPxH, kWM, kHM,
                                     &offX, &offY));
        float eye[2], tan[2];
        rig.locate(offX, offY, eye, tan);
        float pose[2], tanD[2];
        GsRestCancellation(offX, offY, true, rig.poseGain, rig.tanGain, 0.0f, pose, tanD);
        CHECK_NEAR(eye[0] + pose[0], 0.0, 1e-6);
        CHECK_NEAR(eye[1] + pose[1], 0.0, 1e-6);
        CHECK_NEAR(tan[0] + tanD[0], 0.0, 1e-6);
        CHECK_NEAR(tan[1] + tanD[1], 0.0, 1e-6);
    }
}

void TestNominalFallback()
{
    // Before a gain was measured: pose gain 1, tan gain -convergenceDiopters.
    float pose[2], tanD[2];
    GsRestCancellation(0.2f, -0.05f, false, 99.0f, 99.0f, 1.0f / 1.849f, pose, tanD);
    CHECK_NEAR(pose[0], -0.2, 1e-6);
    CHECK_NEAR(pose[1], 0.05, 1e-6);
    CHECK_NEAR(tanD[0], 0.2 / 1.849, 1e-6);
    CHECK_NEAR(tanD[1], -0.05 / 1.849, 1e-6);
}

} // namespace

int main()
{
    TestCornerWindowMatchesWindowsLog();
    TestCentredFullscreenIsTheOldAnchor();
    TestMoveTracksTheWindow();
    TestUnknownGeometryRefuses();
    TestGainsRecoverTheRig();
    TestTinyOffsetKeepsGain();
    TestCancellationNullsTheRig();
    TestNominalFallback();
    if (g_failures) {
        std::printf("gs_rest_anchor_test: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("gs_rest_anchor_test: all checks passed\n");
    return 0;
}
