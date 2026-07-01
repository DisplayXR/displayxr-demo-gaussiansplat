// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// gausssplat_vk_android entry point. Android leg of the DisplayXR
// Gaussian-splat demo: reuses cube_handle_vk_android's OpenXR-Android
// harness (loader → instance → Vulkan device → stereo swapchains →
// session) and the SHARED 3dgs_common GsRenderer (8 compute shaders,
// 32-bit packed sort keys — no shaderInt64, Adreno-compatible), rendering
// the bundled butterfly.spz per eye. The runtime's DP weaves the views.
//
// W7 (#396): chains XR_EXT_view_rig on xrLocateViews and consumes the
// runtime's render-ready XrView{pose, fov} — on the Android out-of-process
// path the server-side Kooima (runtime #510/#513) resolves the live window,
// orientation, and tracked eyes. The app keeps only the clip policy
// (near = ez - vH, far = ez + 1000·vH via rig_local_eye_z).

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <string>
#include <sys/system_properties.h>
#include <unistd.h>

#include "gs_adreno_renderer.h"

// XR_EXT_view_rig (#396 W7): vendored DisplayXR extension header.
#include <openxr/XR_EXT_view_rig.h>

#define LOG_TAG "gausssplat_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef XRT_DEBUG_ANDROID_VERBOSE
#define DXR_HW_DBG(...)      __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP: " __VA_ARGS__)
#define DXR_HW_DBG_ONCE(...) do {                                                                  \
		static bool _logged = false;                                                                \
		if (!_logged) { __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP[once]: " __VA_ARGS__); _logged = true; } \
	} while (0)
#else
#define DXR_HW_DBG(...)      ((void)0)
#define DXR_HW_DBG_ONCE(...) ((void)0)
#endif

namespace {

const char *
xr_result_str(XrResult r)
{
	switch (r) {
	case XR_SUCCESS:                       return "XR_SUCCESS";
	case XR_ERROR_RUNTIME_FAILURE:         return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_RUNTIME_UNAVAILABLE:     return "XR_ERROR_RUNTIME_UNAVAILABLE";
	case XR_ERROR_INSTANCE_LOST:           return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_INITIALIZATION_FAILED:   return "XR_ERROR_INITIALIZATION_FAILED";
	case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
	case XR_ERROR_EXTENSION_NOT_PRESENT:   return "XR_ERROR_EXTENSION_NOT_PRESENT";
	default:                               return nullptr;
	}
}

void
log_xr_result(const char *what, XrResult r)
{
	const char *name = xr_result_str(r);
	if (name != nullptr) {
		LOGI("%s -> %s", what, name);
	} else {
		LOGI("%s -> XrResult(%d)", what, (int)r);
	}
}

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system_id = XR_NULL_SYSTEM_ID;
XrVersion g_required_vk_version = XR_MAKE_VERSION(1, 1, 0);

VkInstance g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_phys_device = VK_NULL_HANDLE;
VkDevice g_vk_device = VK_NULL_HANDLE;
VkQueue g_vk_queue = VK_NULL_HANDLE;
uint32_t g_vk_queue_family = UINT32_MAX;

XrSession g_session = XR_NULL_HANDLE;
XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;
bool g_session_running = false;
bool g_exit_requested = false;
XrSpace g_app_space = XR_NULL_HANDLE;

constexpr uint32_t kViewCount = 2;

struct PerView
{
	XrSwapchain swapchain{XR_NULL_HANDLE};
	uint32_t width{0};
	uint32_t height{0};
	XrSwapchainImageVulkanKHR images[8]{};
	uint32_t image_count{0};
};
PerView g_views[kViewCount];

VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;

// The Adreno/TBDR-native splat renderer (graphics pipeline: preprocess →
// depth-sort N → instanced alpha-blended quads, ROP composite in tile memory).
// Renders butterfly.spz to each eye's swapchain image; the Leia DP weaves them.
GsAdrenoRenderer g_gs;
bool g_gs_ready = false;
std::atomic<bool> g_scene_loaded{false};

// Scene framing (W7): recenter the splat on its robust centroid so it
// straddles the world origin — which IS the virtual display plane (the
// display rig below has an identity pose). The runtime's render-ready views
// look at the origin from the tracked eye positions, so a recentered scene
// frames itself; size is governed by g_rig_vh (virtual display height in
// world units), not by pushing the model away.
float g_scene_center[3] = {0.0f, 0.0f, 0.0f};
// Slow turntable spin about Y (radians/frame). 0 = static. The angle is
// accumulated (g_spin_angle) and only advances after 10 s of no touch input
// (g_last_input_ms), so the scene is still at startup and while the user interacts.
float g_spin_speed = 0.01f;
float g_spin_angle = 0.0f;                 // render-thread accumulated auto-spin
std::atomic<int64_t> g_last_input_ms{0};   // last touch (ms); 0 = uninit → set frame 0

// Milliseconds on a monotonic clock (usable from the UI/JNI threads and render).
static int64_t now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}
// Any touch resets the auto-spin idle timer.
static void mark_user_input() { g_last_input_ms.store(now_ms(), std::memory_order_relaxed); }

// XR_EXT_view_rig state (#396 W7). g_has_view_rig latches at instance create
// when the runtime advertises the extension. The rig is a DISPLAY rig with
// an identity pose: the virtual display plane sits at the world origin, the
// recentered splat straddles it (ZDP), and the runtime returns render-ready
// per-eye view poses + off-axis FOVs around it. g_rig_vh is the virtual
// display height in world units, auto-derived from the scene extent at load
// (the screen roughly spans the scene — desktop auto-fit semantics).
bool g_has_view_rig = false;
std::atomic<float> g_rig_vh{1.0f};

// Tablet gesture state (fed via MainActivity.dispatchTouchEvent → nativeOnTouch +
// a GestureDetector, runtime#499). All applied to the DISPLAY rig pose / vH at
// xrLocateViews; the OOP runtime honors the app-supplied rig pose, so none of this
// needs a runtime change. Mirrors the macOS/Windows gauss controls:
//   1-finger drag  → orbit (rig orientation, yaw about up / pitch about right)
//   2-finger pinch → zoom  (scales the virtual display height)
//   double-tap     → focus: raycast the splat; the picked 3D point becomes the
//                    orbit/spin PIVOT (g_scene_center), so orbit AND the auto-spin
//                    rotate about the tapped feature, at its true depth.
//   long-press     → reset (orbit/zoom + pivot back to the framed centroid)
std::atomic<float> g_orbit_yaw{0.0f};   // 1-finger drag, 0.005 rad/px, -= convention
std::atomic<float> g_orbit_pitch{0.0f}; // clamped ±1.5 rad
std::atomic<float> g_zoom{1.0f};        // pinch: >1 zooms in (rig_vh = base/g_zoom)

// Double-tap focus / long-press reset: the UI thread sets a pending tap NDC (or a
// reset request); the render loop raycasts it (needs the located views) and
// smooth-transitions g_scene_center — the recenter pivot build_splat_model rotates
// about — toward the hit (focus) or the framed centroid (reset). 3D, so the tapped
// feature's true depth becomes the pivot (and lands at the comfortable ZDP).
std::atomic<bool> g_focus_pending{false};
std::atomic<float> g_focus_ndc_x{0.0f};
std::atomic<float> g_focus_ndc_y{0.0f};
std::atomic<bool> g_reset_pending{false};
// Pivot transition state — render-thread-only (g_scene_center is render-local).
bool g_pivot_transitioning = false;
float g_pivot_from[3] = {0, 0, 0};
float g_pivot_to[3] = {0, 0, 0};
float g_pivot_t = 0.0f;
float g_scene_center_orig[3] = {0, 0, 0}; // framed centroid (set at load) — reset target

// Build a quaternion from yaw (about +Y) then pitch (about +X), verbatim from
// common/view_rig_math.h quat_from_yaw_pitch (inlined to avoid an extra include
// path on the Android build). Same convention as the macOS/Windows gauss.
static inline void
orbit_quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf *out)
{
	float cy = cosf(yaw / 2.0f), sy = sinf(yaw / 2.0f);
	float cp = cosf(pitch / 2.0f), sp = sinf(pitch / 2.0f);
	// q = qYaw * qPitch (yaw applied in the world frame, pitch in the local frame).
	out->w = cy * cp;
	out->x = cy * sp;
	out->y = sy * cp;
	out->z = -sy * sp;
}

std::atomic<int> g_display_rotation{0};
std::atomic<bool> g_runtime_unavailable{false};
uint64_t g_frame_count = 0;

// ─── matrix helpers (column-major float[16]) ─────────────────────────────
struct Mat4 { float m[16]; };

Mat4
view_matrix_from_pose(const XrPosef &pose)
{
	const float x = pose.orientation.x, y = pose.orientation.y;
	const float z = pose.orientation.z, w = pose.orientation.w;
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;

	const float r00 = 1.0f - 2.0f * (yy + zz);
	const float r01 = 2.0f * (xy + wz);
	const float r02 = 2.0f * (xz - wy);
	const float r10 = 2.0f * (xy - wz);
	const float r11 = 1.0f - 2.0f * (xx + zz);
	const float r12 = 2.0f * (yz + wx);
	const float r20 = 2.0f * (xz + wy);
	const float r21 = 2.0f * (yz - wx);
	const float r22 = 1.0f - 2.0f * (xx + yy);

	const float tx = -(r00 * pose.position.x + r01 * pose.position.y + r02 * pose.position.z);
	const float ty = -(r10 * pose.position.x + r11 * pose.position.y + r12 * pose.position.z);
	const float tz = -(r20 * pose.position.x + r21 * pose.position.y + r22 * pose.position.z);

	Mat4 v{};
	v.m[0]  = r00; v.m[1]  = r10; v.m[2]  = r20; v.m[3]  = 0.0f;
	v.m[4]  = r01; v.m[5]  = r11; v.m[6]  = r21; v.m[7]  = 0.0f;
	v.m[8]  = r02; v.m[9]  = r12; v.m[10] = r22; v.m[11] = 0.0f;
	v.m[12] = tx;  v.m[13] = ty;  v.m[14] = tz;  v.m[15] = 1.0f;
	return v;
}

// Asymmetric perspective from the runtime's render-ready off-axis FOV
// (#396 W7). Clean +Y-up — GsRenderer owns the Vulkan Y-down handling at the
// RASTER stage (preprocess.comp), so neither the view nor the projection is
// reflected here. Mirrors mat4_from_xr_fov in macos/main.mm / windows
// (per-platform local helper duplication is the accepted pattern).
Mat4
mat4_from_xr_fov(const XrFovf &fov, float near_z, float far_z)
{
	const float tan_l = std::tan(fov.angleLeft);
	const float tan_r = std::tan(fov.angleRight);
	const float tan_d = std::tan(fov.angleDown);
	const float tan_u = std::tan(fov.angleUp);
	const float tan_w = tan_r - tan_l;
	const float tan_h = tan_u - tan_d;

	Mat4 p{};
	p.m[0]  = 2.0f / tan_w;
	p.m[5]  = 2.0f / tan_h;
	p.m[8]  = (tan_r + tan_l) / tan_w;
	p.m[9]  = (tan_u + tan_d) / tan_h;
	p.m[10] = -(far_z + near_z) / (far_z - near_z);
	p.m[11] = -1.0f;
	p.m[14] = -(2.0f * far_z * near_z) / (far_z - near_z);
	return p;
}

// Display-local eye distance for the ZDP-anchored clip: z of
// (rigPose^-1 * eyeWorld). Degenerates to eye_world.z at identity rig pose.
// Mirrors rig_local_eye_z in cube_handle_vk_android / RigLocalEyeZ on the
// desktop legs.
float
rig_local_eye_z(const XrPosef &rig, const XrVector3f &eye_world)
{
	const float dx = eye_world.x - rig.position.x;
	const float dy = eye_world.y - rig.position.y;
	const float dz = eye_world.z - rig.position.z;
	const float qx = -rig.orientation.x, qy = -rig.orientation.y;
	const float qz = -rig.orientation.z, qw = rig.orientation.w;
	const float cx = qy * dz - qz * dy + qw * dx;
	const float cy = qz * dx - qx * dz + qw * dy;
	return dz + 2.0f * (qx * cy - qy * cx);
}

Mat4
mat4_identity()
{
	Mat4 r{};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

// Column-major a*b.
Mat4
mat4_mul(const Mat4 &a, const Mat4 &b)
{
	Mat4 r{};
	for (int c = 0; c < 4; ++c) {
		for (int row = 0; row < 4; ++row) {
			float s = 0.0f;
			for (int k = 0; k < 4; ++k) {
				s += a.m[k * 4 + row] * b.m[c * 4 + k];
			}
			r.m[c * 4 + row] = s;
		}
	}
	return r;
}

// Splat model transform (same for both eyes): recenter on the scene centroid
// so the splat straddles the display plane (world origin), then a slow
// turntable spin about Y. NO Y-flip and NO push-back (W7): the renderer's
// raster-stage flip owns Vulkan Y-down, and framing comes from g_rig_vh.
// M = RotY(angle) * Translate(-center)
Mat4
build_splat_model(float angle)
{
	Mat4 recenter = mat4_identity();
	recenter.m[12] = -g_scene_center[0];
	recenter.m[13] = -g_scene_center[1];
	recenter.m[14] = -g_scene_center[2];

	const float c = std::cos(angle), s = std::sin(angle);
	Mat4 roty = mat4_identity();
	roty.m[0] = c;  roty.m[2] = -s;
	roty.m[8] = s;  roty.m[10] = c;

	return mat4_mul(roty, recenter);
}

// ─── OpenXR-Android bring-up (reused verbatim from cube_handle_vk_android) ─
bool
initialize_loader(struct android_app *app)
{
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xrInitializeLoaderKHR));
	if (res != XR_SUCCESS || xrInitializeLoaderKHR == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed (%d)", (int)res);
		return false;
	}
	XrLoaderInitInfoAndroidKHR loader_init = {};
	loader_init.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
	loader_init.applicationVM = app->activity->vm;
	loader_init.applicationContext = app->activity->clazz;
	res = xrInitializeLoaderKHR(
	    reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_init));
	log_xr_result("xrInitializeLoaderKHR", res);
	return res == XR_SUCCESS;
}

bool
create_instance(struct android_app *app)
{
	g_runtime_unavailable.store(false, std::memory_order_relaxed);

	// Detect XR_EXT_view_rig (#396 W7) before instance creation; enable it
	// when advertised. Without it the plain locate path still renders but
	// with the device's fixed FOV (no server-side Kooima) — log loudly.
	g_has_view_rig = false;
	{
		uint32_t n = 0;
		if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr) ==
		        XR_SUCCESS &&
		    n > 0 && n < 256) {
			XrExtensionProperties props[256];
			for (uint32_t i = 0; i < n; ++i) {
				props[i] = {};
				props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
			}
			if (xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props) ==
			    XR_SUCCESS) {
				for (uint32_t i = 0; i < n; ++i) {
					if (std::strcmp(props[i].extensionName,
					                XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0) {
						g_has_view_rig = true;
						break;
					}
				}
			}
		}
	}
	LOGI("XR_EXT_view_rig: %s", g_has_view_rig ? "AVAILABLE" : "NOT FOUND");
	if (!g_has_view_rig) {
		LOGW("XR_EXT_view_rig missing — falling back to plain xrLocateViews "
		     "(fixed device FOV, no server-side Kooima; expect degraded 3D)");
	}

	const char *extensions[4] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	};
	uint32_t extension_count = 2;
	if (g_has_view_rig) {
		extensions[extension_count++] = XR_EXT_VIEW_RIG_EXTENSION_NAME;
	}
	XrInstanceCreateInfoAndroidKHR android_info = {};
	android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android_info.applicationVM = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo create_info = {};
	create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
	create_info.next = &android_info;
	std::strncpy(create_info.applicationInfo.applicationName,
	             "gausssplat_vk_android", XR_MAX_APPLICATION_NAME_SIZE - 1);
	create_info.applicationInfo.applicationVersion = 1;
	std::strncpy(create_info.applicationInfo.engineName, "displayxr",
	             XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.engineVersion = 1;
	create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	create_info.enabledExtensionCount = extension_count;
	create_info.enabledExtensionNames = extensions;

	XrResult res = XR_ERROR_RUNTIME_UNAVAILABLE;
	for (int attempt = 0; attempt < 5; ++attempt) {
		res = xrCreateInstance(&create_info, &g_instance);
		if (res != XR_ERROR_RUNTIME_UNAVAILABLE) {
			break;
		}
		LOGW("xrCreateInstance: runtime unavailable (attempt %d/5); launch the "
		     "DisplayXR app once if this persists…", attempt + 1);
		usleep(400 * 1000);
	}
	log_xr_result("xrCreateInstance", res);
	if (res != XR_SUCCESS) {
		if (res == XR_ERROR_RUNTIME_UNAVAILABLE) {
			g_runtime_unavailable.store(true, std::memory_order_relaxed);
		}
		return false;
	}
	LOGI("ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS");
	return true;
}

bool
query_system_and_graphics_reqs()
{
	XrSystemGetInfo sys_info = {};
	sys_info.type = XR_TYPE_SYSTEM_GET_INFO;
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(g_instance, &sys_info, &g_system_id);
	log_xr_result("xrGetSystem(HMD)", res);
	if (res != XR_SUCCESS) {
		sys_info.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
		res = xrGetSystem(g_instance, &sys_info, &g_system_id);
		log_xr_result("xrGetSystem(HANDHELD)", res);
		if (res != XR_SUCCESS) {
			return false;
		}
	}

	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
	res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsRequirements2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs));
	if (res != XR_SUCCESS || get_reqs == nullptr) {
		LOGE("xrGetInstanceProcAddr(GraphicsRequirements2) failed (%d)", (int)res);
		return false;
	}
	XrGraphicsRequirementsVulkanKHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	res = get_reqs(g_instance, g_system_id, &reqs);
	log_xr_result("xrGetVulkanGraphicsRequirements2KHR", res);
	if (res != XR_SUCCESS) {
		return false;
	}
	g_required_vk_version = reqs.minApiVersionSupported;
	return true;
}

bool
create_vulkan_instance()
{
	PFN_xrCreateVulkanInstanceKHR xr_create_vk_instance = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanInstanceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_instance));
	if (res != XR_SUCCESS || xr_create_vk_instance == nullptr) {
		LOGE("xrGetInstanceProcAddr(CreateVulkanInstance) failed (%d)", (int)res);
		return false;
	}
	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "gausssplat_vk_android";
	app_info.applicationVersion = 1;
	app_info.pEngineName = "displayxr";
	app_info.engineVersion = 1;
	app_info.apiVersion = VK_MAKE_VERSION(
	    XR_VERSION_MAJOR(g_required_vk_version),
	    XR_VERSION_MINOR(g_required_vk_version), 0);

	VkInstanceCreateInfo vk_ci = {};
	vk_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	vk_ci.pApplicationInfo = &app_info;

	XrVulkanInstanceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanCreateInfo = &vk_ci;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_instance(g_instance, &xr_ci, &g_vk_instance, &vk_result);
	log_xr_result("xrCreateVulkanInstanceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanInstanceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	return true;
}

bool
pick_physical_device()
{
	PFN_xrGetVulkanGraphicsDevice2KHR xr_get_phys = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsDevice2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_get_phys));
	if (res != XR_SUCCESS || xr_get_phys == nullptr) {
		LOGE("xrGetInstanceProcAddr(GraphicsDevice2) failed (%d)", (int)res);
		return false;
	}
	XrVulkanGraphicsDeviceGetInfoKHR info = {};
	info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	info.systemId = g_system_id;
	info.vulkanInstance = g_vk_instance;
	res = xr_get_phys(g_instance, &info, &g_vk_phys_device);
	log_xr_result("xrGetVulkanGraphicsDevice2KHR", res);
	return res == XR_SUCCESS;
}

bool
create_vulkan_device()
{
	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, nullptr);
	if (qf_count == 0) {
		LOGE("No Vulkan queue families");
		return false;
	}
	VkQueueFamilyProperties qf_props[16] = {};
	const uint32_t qf_cap = sizeof(qf_props) / sizeof(qf_props[0]);
	if (qf_count > qf_cap) {
		qf_count = qf_cap;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, qf_props);

	g_vk_queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count; ++i) {
		if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g_vk_queue_family = i;
			break;
		}
	}
	if (g_vk_queue_family == UINT32_MAX) {
		LOGE("No graphics-capable queue family");
		return false;
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = g_vk_queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;

	// shaderInt64 is NO LONGER needed: the 3dgs sort uses 32-bit packed keys
	// (the whole point of the Adreno port — Adreno has no shaderInt64).
	// render.comp writes a writeonly image2D without a format qualifier, so
	// request shaderStorageImageWriteWithoutFormat when available. The
	// runtime's xrCreateVulkanDeviceKHR copies pEnabledFeatures through
	// verbatim (oxr_vk_create_vulkan_device).
	VkPhysicalDeviceFeatures supported = {};
	vkGetPhysicalDeviceFeatures(g_vk_phys_device, &supported);

	VkPhysicalDeviceFeatures features = {};
	features.shaderStorageImageWriteWithoutFormat =
	    supported.shaderStorageImageWriteWithoutFormat;

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.pEnabledFeatures = &features;

	PFN_xrCreateVulkanDeviceKHR xr_create_vk_device = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanDeviceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_device));
	if (res != XR_SUCCESS || xr_create_vk_device == nullptr) {
		LOGE("xrGetInstanceProcAddr(CreateVulkanDevice) failed (%d)", (int)res);
		return false;
	}
	XrVulkanDeviceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanPhysicalDevice = g_vk_phys_device;
	xr_ci.vulkanCreateInfo = &dci;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_device(g_instance, &xr_ci, &g_vk_device, &vk_result);
	log_xr_result("xrCreateVulkanDeviceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanDeviceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);
	LOGI("Vulkan device ready: queue_family=%u", g_vk_queue_family);
	return true;
}

bool
create_session()
{
	XrGraphicsBindingVulkanKHR binding = {};
	binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
	binding.instance = g_vk_instance;
	binding.physicalDevice = g_vk_phys_device;
	binding.device = g_vk_device;
	binding.queueFamilyIndex = g_vk_queue_family;
	binding.queueIndex = 0;

	XrSessionCreateInfo ci = {};
	ci.type = XR_TYPE_SESSION_CREATE_INFO;
	ci.next = &binding;
	ci.systemId = g_system_id;
	XrResult res = xrCreateSession(g_instance, &ci, &g_session);
	log_xr_result("xrCreateSession", res);
	return res == XR_SUCCESS;
}

bool
create_swapchains()
{
	uint32_t expected_view_count = 0;
	XrResult res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    0, &expected_view_count, nullptr);
	if (res != XR_SUCCESS || expected_view_count != kViewCount) {
		LOGE("Expected %u views, runtime reports %u", kViewCount, expected_view_count);
		return false;
	}
	XrViewConfigurationView view_configs[kViewCount] = {};
	for (uint32_t i = 0; i < kViewCount; ++i) {
		view_configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}
	res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    kViewCount, &expected_view_count, view_configs);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateViewConfigurationViews", res);
		return false;
	}

	uint32_t format_count = 0;
	res = xrEnumerateSwapchainFormats(g_session, 0, &format_count, nullptr);
	if (res != XR_SUCCESS || format_count == 0) {
		log_xr_result("xrEnumerateSwapchainFormats(count)", res);
		return false;
	}
	int64_t formats[64] = {};
	if (format_count > 64) {
		format_count = 64;
	}
	res = xrEnumerateSwapchainFormats(g_session, format_count, &format_count, formats);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainFormats(fill)", res);
		return false;
	}
	const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
	for (int64_t pref : preferred) {
		for (uint32_t i = 0; i < format_count && g_swapchain_format == VK_FORMAT_UNDEFINED; ++i) {
			if (formats[i] == pref) {
				g_swapchain_format = (VkFormat)pref;
			}
		}
		if (g_swapchain_format != VK_FORMAT_UNDEFINED) {
			break;
		}
	}
	if (g_swapchain_format == VK_FORMAT_UNDEFINED) {
		g_swapchain_format = (VkFormat)formats[0];
	}
	LOGI("Chose swapchain format: 0x%x", (uint32_t)g_swapchain_format);

	for (uint32_t i = 0; i < kViewCount; ++i) {
		g_views[i].width = view_configs[i].recommendedImageRectWidth;
		g_views[i].height = view_configs[i].recommendedImageRectHeight;

		XrSwapchainCreateInfo ci = {};
		ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
		ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
		                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		ci.format = g_swapchain_format;
		ci.sampleCount = view_configs[i].recommendedSwapchainSampleCount;
		ci.width = g_views[i].width;
		ci.height = g_views[i].height;
		ci.faceCount = 1;
		ci.arraySize = 1;
		ci.mipCount = 1;
		res = xrCreateSwapchain(g_session, &ci, &g_views[i].swapchain);
		if (res != XR_SUCCESS) {
			log_xr_result("xrCreateSwapchain", res);
			return false;
		}
		uint32_t img_count = 0;
		res = xrEnumerateSwapchainImages(g_views[i].swapchain, 0, &img_count, nullptr);
		if (res != XR_SUCCESS) {
			log_xr_result("xrEnumerateSwapchainImages(count)", res);
			return false;
		}
		if (img_count > 8) {
			img_count = 8;
		}
		for (uint32_t j = 0; j < img_count; ++j) {
			g_views[i].images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
		}
		res = xrEnumerateSwapchainImages(
		    g_views[i].swapchain, img_count, &img_count,
		    reinterpret_cast<XrSwapchainImageBaseHeader *>(g_views[i].images));
		if (res != XR_SUCCESS) {
			log_xr_result("xrEnumerateSwapchainImages(fill)", res);
			return false;
		}
		g_views[i].image_count = img_count;
		LOGI("View %u swapchain: %ux%u, %u images", i, g_views[i].width,
		     g_views[i].height, img_count);
	}
	return true;
}

bool
create_reference_space()
{
	XrReferenceSpaceCreateInfo ci = {};
	ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	ci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
	ci.poseInReferenceSpace.position = {0, 0, 0};
	XrResult res = xrCreateReferenceSpace(g_session, &ci, &g_app_space);
	log_xr_result("xrCreateReferenceSpace(LOCAL)", res);
	return res == XR_SUCCESS;
}

// Initialize the 3DGS renderer against the runtime's Vulkan resources.
bool
gs_init()
{
	if (!g_gs.init(g_vk_instance, g_vk_phys_device, g_vk_device, g_vk_queue,
	               g_vk_queue_family, g_views[0].width, g_views[0].height)) {
		LOGE("GsAdrenoRenderer::init failed");
		return false;
	}
	// The Adreno/TBDR-native renderer self-tunes: it reads debug.dxr.gs.scale /
	// .keep / .minalpha internally in init() (PR #44), so the old compute-era
	// setRenderScale / setKeepFraction / setCullMinOpacity calls are gone — those
	// methods don't exist on this renderer.
	g_gs_ready = true;
	LOGI("GsAdrenoRenderer initialized (%ux%u/eye)", g_views[0].width, g_views[0].height);
	return true;
}

// Copy butterfly.spz out of the APK assets into app-private storage (the
// SPZ loader takes a filesystem path), then load it into the renderer.
bool
load_butterfly(struct android_app *app)
{
	if (!g_gs_ready) {
		return false;
	}
	AAssetManager *mgr = app->activity->assetManager;
	AAsset *asset = AAssetManager_open(mgr, "butterfly.spz", AASSET_MODE_BUFFER);
	if (asset == nullptr) {
		LOGE("butterfly.spz not found in assets");
		return false;
	}
	const void *buf = AAsset_getBuffer(asset);
	const off_t len = AAsset_getLength(asset);
	std::string path = std::string(app->activity->internalDataPath) + "/butterfly.spz";
	bool ok = false;
	if (buf != nullptr && len > 0) {
		FILE *f = std::fopen(path.c_str(), "wb");
		if (f != nullptr) {
			ok = std::fwrite(buf, 1, (size_t)len, f) == (size_t)len;
			std::fclose(f);
		}
	}
	AAsset_close(asset);
	if (!ok) {
		LOGE("failed to stage butterfly.spz to %s", path.c_str());
		return false;
	}
	if (!g_gs.loadScene(path.c_str())) {
		LOGE("GsRenderer::loadScene failed for %s", path.c_str());
		return false;
	}
	LOGI("Loaded butterfly.spz: %u gaussians", g_gs.gaussianCount());

	// Auto-frame (W7): recenter on the robust scene centroid (so the splat
	// straddles the display plane at the origin) and derive the virtual
	// display height from the scene's screen-facing extent — the screen
	// roughly spans the scene with a small margin, mirroring the desktop
	// auto-fit semantics (vHeight = scene height).
	float ext[3] = {1.0f, 1.0f, 1.0f};
	if (g_gs.getRobustSceneBounds(0.05f, 0.95f, g_scene_center, ext)) {
		// Remember the framed centroid as the long-press reset target.
		g_scene_center_orig[0] = g_scene_center[0];
		g_scene_center_orig[1] = g_scene_center[1];
		g_scene_center_orig[2] = g_scene_center[2];
		float face = ext[0] > ext[1] ? ext[0] : ext[1];  // width/height extent
		g_rig_vh.store(face * 1.1f, std::memory_order_relaxed);
		LOGI("scene center=(%.2f,%.2f,%.2f) extent=(%.2f,%.2f,%.2f) rig_vh=%.2f",
		     g_scene_center[0], g_scene_center[1], g_scene_center[2],
		     ext[0], ext[1], ext[2], g_rig_vh.load(std::memory_order_relaxed));
	}
	g_scene_loaded.store(true, std::memory_order_relaxed);
	return true;
}

void
handle_session_state(XrSessionState new_state)
{
	g_session_state = new_state;
	switch (new_state) {
	case XR_SESSION_STATE_READY: {
		XrSessionBeginInfo begin = {};
		begin.type = XR_TYPE_SESSION_BEGIN_INFO;
		begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		XrResult res = xrBeginSession(g_session, &begin);
		log_xr_result("xrBeginSession", res);
		if (res == XR_SUCCESS) {
			g_session_running = true;
		}
		break;
	}
	case XR_SESSION_STATE_STOPPING: {
		XrResult res = xrEndSession(g_session);
		log_xr_result("xrEndSession", res);
		g_session_running = false;
		break;
	}
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		g_exit_requested = true;
		break;
	default:
		break;
	}
}

void
poll_xr_events()
{
	for (;;) {
		XrEventDataBuffer ev = {};
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		XrResult res = xrPollEvent(g_instance, &ev);
		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}
		if (res != XR_SUCCESS) {
			log_xr_result("xrPollEvent", res);
			break;
		}
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto *e = reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
			if (e->session == g_session) {
				LOGI("session state -> %d", (int)e->state);
				handle_session_state(e->state);
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g_exit_requested = true;
		}
	}
}

bool
render_frame()
{
	// Frame-phase profiler: wall-clock wait / render(2 eyes) / endFrame to see
	// whether the wall is app compute (renderEye) or runtime pacing+weave
	// (xrWaitFrame / xrEndFrame). Logged every 120 frames next to the fps line.
	auto pf_now = []{ return std::chrono::steady_clock::now(); };
	auto pf_ms = [](auto a, auto b){
		return std::chrono::duration<double, std::milli>(b - a).count(); };
	static double pf_wait = 0, pf_render = 0, pf_end = 0, pf_setup = 0;
	auto pf_t0 = pf_now();

	XrFrameWaitInfo wait_info = {};
	wait_info.type = XR_TYPE_FRAME_WAIT_INFO;
	XrFrameState frame_state = {};
	frame_state.type = XR_TYPE_FRAME_STATE;
	XrResult res = xrWaitFrame(g_session, &wait_info, &frame_state);
	auto pf_t1 = pf_now();
	pf_wait = pf_ms(pf_t0, pf_t1);
	if (res != XR_SUCCESS) {
		log_xr_result("xrWaitFrame", res);
		return false;
	}
	XrFrameBeginInfo begin_info = {};
	begin_info.type = XR_TYPE_FRAME_BEGIN_INFO;
	res = xrBeginFrame(g_session, &begin_info);
	if (res != XR_SUCCESS) {
		log_xr_result("xrBeginFrame", res);
		return false;
	}

	XrCompositionLayerProjectionView projection_views[kViewCount] = {};
	bool rendered = false;
	if (frame_state.shouldRender && g_scene_loaded.load(std::memory_order_relaxed)) {
		XrViewState view_state = {};
		view_state.type = XR_TYPE_VIEW_STATE;
		XrViewLocateInfo locate_info = {};
		locate_info.type = XR_TYPE_VIEW_LOCATE_INFO;
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = g_app_space;

		// XR_EXT_view_rig (#396 W7): chain the DISPLAY rig so the runtime owns
		// the live-window resolve + off-axis Kooima (server-side over IPC on
		// the OOP path, runtime #510/#513) and returns render-ready
		// XrView{pose, fov}. Identity pose = display plane at the world
		// origin; the recentered splat straddles it. The raw channel reports
		// the DP's display-space eyes (verification/logging only — no HUD).
		// Advance the pivot transition (double-tap focus / long-press reset), easing
		// g_scene_center toward the target over ~0.4 s. build_splat_model recenters +
		// spins about g_scene_center, so orbit AND the auto-spin pivot around it.
		if (g_pivot_transitioning) {
			g_pivot_t += 0.06f; // ~16 frames at 60 fps
			float u = g_pivot_t > 1.0f ? 1.0f : g_pivot_t;
			const float s = u * u * (3.0f - 2.0f * u); // smoothstep
			for (int a = 0; a < 3; a++)
				g_scene_center[a] = g_pivot_from[a] + (g_pivot_to[a] - g_pivot_from[a]) * s;
			if (u >= 1.0f) g_pivot_transitioning = false;
		}

		// Tablet gestures applied to the rig: pinch → zoom (smaller vH = zoomed in),
		// 2-finger drag → pan (rig position in its view plane), 1-finger drag →
		// orbit (rig orientation). The runtime resolves the off-axis Kooima around
		// this pose (server-side over IPC on the OOP path).
		const float rig_vh = g_rig_vh.load(std::memory_order_relaxed) /
		                     g_zoom.load(std::memory_order_relaxed);
		XrPosef rig_pose = {};
		orbit_quat_from_yaw_pitch(g_orbit_yaw.load(std::memory_order_relaxed),
		                          g_orbit_pitch.load(std::memory_order_relaxed),
		                          &rig_pose.orientation);
		rig_pose.position = {0.0f, 0.0f, 0.0f};
		XrDisplayRigEXT display_rig = {XR_TYPE_DISPLAY_RIG_EXT};
		XrViewDisplayRawEXT view_raw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
		if (g_has_view_rig) {
			display_rig.pose = rig_pose;
			display_rig.virtualDisplayHeight = rig_vh;
			display_rig.ipdFactor = 1.0f;
			display_rig.parallaxFactor = 1.0f;
			display_rig.perspectiveFactor = 1.0f;
			locate_info.next = &display_rig;
			view_state.next = &view_raw;
		}

		XrView views[kViewCount] = {};
		for (uint32_t i = 0; i < kViewCount; ++i) {
			views[i].type = XR_TYPE_VIEW;
		}
		uint32_t located = 0;
		res = xrLocateViews(g_session, &locate_info, &view_state, kViewCount, &located, views);
		if (res == XR_SUCCESS && located == kViewCount) {
			DXR_HW_DBG_ONCE("first xrLocateViews success");
			// One-shot W7 verification log: raw display-space eyes + tracking.
			static bool logged_rig = false;
			if (!logged_rig && g_has_view_rig) {
				LOGI("W7: rig chained vh=%.2f raw_eyes=%u tracking=%d "
				     "eye0=(%.3f,%.3f,%.3f) view0=(%.2f,%.2f,%.2f) "
				     "fov0=(%.3f,%.3f,%.3f,%.3f)",
				     rig_vh, view_raw.eyeCountOutput, (int)view_raw.isTracking,
				     view_raw.rawEyes[0].x, view_raw.rawEyes[0].y,
				     view_raw.rawEyes[0].z, views[0].pose.position.x,
				     views[0].pose.position.y, views[0].pose.position.z,
				     views[0].fov.angleLeft, views[0].fov.angleRight,
				     views[0].fov.angleUp, views[0].fov.angleDown);
				logged_rig = true;
			}

			// Auto-spin idle gate: only advance the turntable after 10 s of no touch
			// (including from startup), so the scene is still until the user has been
			// idle. Seed the timer on the first frame so startup counts as "fresh".
			{
				int64_t now = now_ms();
				int64_t last = g_last_input_ms.load(std::memory_order_relaxed);
				if (last == 0) { last = now; g_last_input_ms.store(now, std::memory_order_relaxed); }
				if (now - last > 10000) g_spin_angle += g_spin_speed;
			}

			// Long-press reset: ease the pivot back to the framed centroid (orbit/zoom
			// were already reset on the UI thread).
			if (g_reset_pending.exchange(false, std::memory_order_acquire)) {
				for (int a = 0; a < 3; a++) {
					g_pivot_from[a] = g_scene_center[a];
					g_pivot_to[a] = g_scene_center_orig[a];
				}
				g_pivot_t = 0.0f;
				g_pivot_transitioning = true;
			}

			// Double-tap focus: raycast the tapped point against the splat; the hit
			// (in 3D) becomes the new pivot (g_scene_center), so orbit AND the auto-
			// spin rotate about the tapped feature at its true depth — the Android
			// analogue of the desktop double-click focus. Shared CPU picker.
			if (g_focus_pending.exchange(false, std::memory_order_acquire)) {
				// Center eye = average of the located views' poses + off-axis fovs.
				XrVector3f cpos = {0, 0, 0};
				XrFovf cfov = {0, 0, 0, 0};
				for (uint32_t e = 0; e < kViewCount; ++e) {
					cpos.x += views[e].pose.position.x;
					cpos.y += views[e].pose.position.y;
					cpos.z += views[e].pose.position.z;
					cfov.angleLeft += views[e].fov.angleLeft;
					cfov.angleRight += views[e].fov.angleRight;
					cfov.angleUp += views[e].fov.angleUp;
					cfov.angleDown += views[e].fov.angleDown;
				}
				const float inv = 1.0f / (float)kViewCount;
				cpos.x *= inv; cpos.y *= inv; cpos.z *= inv;
				cfov.angleLeft *= inv; cfov.angleRight *= inv;
				cfov.angleUp *= inv; cfov.angleDown *= inv;
				const XrQuaternionf q = views[0].pose.orientation; // eyes share orientation

				const float ndcX = g_focus_ndc_x.load(std::memory_order_relaxed);
				const float ndcY = g_focus_ndc_y.load(std::memory_order_relaxed);
				// View-space ray dir through the tap (off-axis fov); camera looks −Z.
				const float tl = tanf(cfov.angleLeft), tr = tanf(cfov.angleRight);
				const float tdn = tanf(cfov.angleDown), tup = tanf(cfov.angleUp);
				float vx = tl + (tr - tl) * (ndcX + 1.0f) * 0.5f;
				float vy = tdn + (tup - tdn) * (ndcY + 1.0f) * 0.5f;
				float vz = -1.0f;
				const float vl = sqrtf(vx * vx + vy * vy + vz * vz);
				vx /= vl; vy /= vl; vz /= vl;
				// Rotate the view-space dir into world: d' = v + 2qw(q×v) + 2q×(q×v).
				const float qx = q.x, qy = q.y, qz = q.z, qw = q.w;
				const float tX = 2.0f * (qy * vz - qz * vy);
				const float tY = 2.0f * (qz * vx - qx * vz);
				const float tZ = 2.0f * (qx * vy - qy * vx);
				const float wx = vx + qw * tX + (qy * tZ - qz * tY);
				const float wy = vy + qw * tY + (qz * tX - qx * tZ);
				const float wz = vz + qw * tZ + (qx * tY - qy * tX);

				// Splats render as splat_model * raw (recenter + auto-spin about Y); the
				// picker tests RAW positions, so inverse-transform the ray (undo the spin,
				// add back the recenter pivot) into raw space before picking.
				const float pang = g_spin_angle;
				const float pc = cosf(pang), ps = sinf(pang);
				float rayOrigin[3] = {pc * cpos.x - ps * cpos.z + g_scene_center[0],
				                      cpos.y + g_scene_center[1],
				                      ps * cpos.x + pc * cpos.z + g_scene_center[2]};
				float rayDir[3] = {pc * wx - ps * wz, wy, ps * wx + pc * wz};
				{ float l = sqrtf(rayDir[0]*rayDir[0] + rayDir[1]*rayDir[1] + rayDir[2]*rayDir[2]); if (l > 1e-6f) { rayDir[0] /= l; rayDir[1] /= l; rayDir[2] /= l; } }
				float hit[3];
				if (g_gs.pickGaussian(rayOrigin, rayDir, hit, 100.0f)) {
					g_pivot_from[0] = g_scene_center[0];
					g_pivot_from[1] = g_scene_center[1];
					g_pivot_from[2] = g_scene_center[2]; g_pivot_to[0] = hit[0];
					g_pivot_to[1] = hit[1]; g_pivot_to[2] = hit[2];
					g_pivot_t = 0.0f;
					g_pivot_transitioning = true;
					LOGI("focus: hit (%.3f,%.3f,%.3f) → recenter", hit[0], hit[1], hit[2]);
				} else {
					LOGI("focus: ray missed the splat (ndc %.2f,%.2f)", ndcX, ndcY);
				}
			}

			// Splat model (recenter + spin) — same for both eyes.
			const Mat4 splat_model = build_splat_model(g_spin_angle);
			auto pf_r0 = pf_now();
			pf_setup = pf_ms(pf_t1, pf_r0);  // xrBeginFrame + xrLocateViews (IPC for view_rig)
			for (uint32_t i = 0; i < kViewCount; ++i) {
				XrSwapchainImageAcquireInfo acq = {};
				acq.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
				uint32_t img_idx = 0;
				res = xrAcquireSwapchainImage(g_views[i].swapchain, &acq, &img_idx);
				if (res != XR_SUCCESS) {
					log_xr_result("xrAcquireSwapchainImage", res);
					break;
				}
				XrSwapchainImageWaitInfo wait_img = {};
				wait_img.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
				wait_img.timeout = XR_INFINITE_DURATION;
				res = xrWaitSwapchainImage(g_views[i].swapchain, &wait_img);
				if (res != XR_SUCCESS) {
					log_xr_result("xrWaitSwapchainImage", res);
					break;
				}

				// W7 clip policy: near = ez - vH, far = ez + 1000·vH (opaque),
				// where ez = rig-local eye Z. The splat shader culls
				// geometrically on p_view.z (it ignores ndc.z), so these flow
				// into renderEye's explicit view-space culls.
				const float ez = rig_local_eye_z(rig_pose, views[i].pose.position);
				float near_z = (ez - rig_vh > 1.0e-4f) ? (ez - rig_vh) : 1.0e-4f;
				float far_z = ez + 1000.0f * rig_vh;
				if (far_z < near_z + 1.0e-4f) {
					far_z = near_z + 1.0e-4f;
				}
				Mat4 viewM = view_matrix_from_pose(views[i].pose);
				Mat4 evM = mat4_mul(viewM, splat_model);  // apply splat model
				Mat4 projM = mat4_from_xr_fov(views[i].fov, near_z, far_z);
				g_gs.renderEye(
				    g_views[i].images[img_idx].image, g_swapchain_format,
				    g_views[i].width, g_views[i].height,
				    0, 0, g_views[i].width, g_views[i].height,
				    evM.m, projM.m,
				    /*transparentBg=*/false, near_z, far_z,
				    /*clipFadeFrac=*/0.15f);

				XrSwapchainImageReleaseInfo rel = {};
				rel.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
				res = xrReleaseSwapchainImage(g_views[i].swapchain, &rel);
				if (res != XR_SUCCESS) {
					log_xr_result("xrReleaseSwapchainImage", res);
					break;
				}

				projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				projection_views[i].pose = views[i].pose;
				projection_views[i].fov = views[i].fov;
				projection_views[i].subImage.swapchain = g_views[i].swapchain;
				projection_views[i].subImage.imageRect.offset = {0, 0};
				projection_views[i].subImage.imageRect.extent = {
				    (int32_t)g_views[i].width, (int32_t)g_views[i].height};
				projection_views[i].subImage.imageArrayIndex = 0;
			}
			rendered = (res == XR_SUCCESS);
			pf_render = pf_ms(pf_r0, pf_now());
		} else {
			log_xr_result("xrLocateViews", res);
		}
	}

	XrCompositionLayerProjection projection_layer = {};
	projection_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	projection_layer.space = g_app_space;
	projection_layer.viewCount = kViewCount;
	projection_layer.views = projection_views;
	const XrCompositionLayerBaseHeader *layers[1] = {
	    reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection_layer)};

	XrFrameEndInfo end_info = {};
	end_info.type = XR_TYPE_FRAME_END_INFO;
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = rendered ? 1 : 0;
	end_info.layers = rendered ? layers : nullptr;
	auto pf_e0 = pf_now();
	res = xrEndFrame(g_session, &end_info);
	pf_end = pf_ms(pf_e0, pf_now());
	if (res != XR_SUCCESS) {
		log_xr_result("xrEndFrame", res);
		return false;
	}

	g_frame_count++;
	if ((g_frame_count % 120) == 0) {
		static auto last = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		double ms = std::chrono::duration<double, std::milli>(now - last).count() / 120.0;
		last = now;
		LOGI("frame %llu  ~%.1f ms/frame (%.1f fps) | PHASE wait=%.1f setup=%.1f render=%.1f end=%.1f ms | "
		     "predDisplayPeriod=%.1f ms",
		     (unsigned long long)g_frame_count,
		     ms, ms > 0.0 ? 1000.0 / ms : 0.0, pf_wait, pf_setup, pf_render, pf_end,
		     (double)frame_state.predictedDisplayPeriod / 1.0e6);
	}
	return true;
}

void
destroy_all()
{
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(g_vk_device);
	}
	if (g_gs_ready) {
		g_gs.cleanup();
		g_gs_ready = false;
	}
	if (g_session != XR_NULL_HANDLE) {
		xrDestroySession(g_session);
		g_session = XR_NULL_HANDLE;
	}
	for (uint32_t i = 0; i < kViewCount; ++i) {
		if (g_views[i].swapchain != XR_NULL_HANDLE) {
			xrDestroySwapchain(g_views[i].swapchain);
			g_views[i].swapchain = XR_NULL_HANDLE;
		}
	}
	if (g_app_space != XR_NULL_HANDLE) {
		xrDestroySpace(g_app_space);
		g_app_space = XR_NULL_HANDLE;
	}
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDestroyDevice(g_vk_device, nullptr);
		g_vk_device = VK_NULL_HANDLE;
	}
	if (g_vk_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(g_vk_instance, nullptr);
		g_vk_instance = VK_NULL_HANDLE;
	}
	if (g_instance != XR_NULL_HANDLE) {
		xrDestroyInstance(g_instance);
		g_instance = XR_NULL_HANDLE;
	}
}

void
handle_cmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		LOGI("APP_CMD_INIT_WINDOW (window=%p)", app->window);
		if (g_instance == XR_NULL_HANDLE) {
			bool ok =
			    create_instance(app) &&
			    query_system_and_graphics_reqs() &&
			    create_vulkan_instance() &&
			    pick_physical_device() &&
			    create_vulkan_device() &&
			    create_session() &&
			    create_swapchains() &&
			    create_reference_space() &&
			    gs_init() &&
			    load_butterfly(app);
			LOGI(ok ? "Bring-up complete." : "Bring-up failed; see logs.");
		}
		break;
	case APP_CMD_DESTROY:
		LOGI("APP_CMD_DESTROY");
		destroy_all();
		break;
	default:
		break;
	}
}

// Swipe-orbit input. gauss owns its NativeActivity window, but the runtime client's
// MonadoView display surface sits above it, so the native InputQueue
// (app->onInputEvent) is NOT fed (runtime#499) — touch arrives via
// MainActivity.dispatchTouchEvent → nativeOnTouch JNI instead (the same path the
// cube + model-viewer demos use). A one-finger drag rotates the virtual display
// (yaw/pitch), a double-tap re-centers; the values feed the DISPLAY rig pose at
// xrLocateViews. action uses MotionEvent.ACTION_* (0=DOWN, 1=UP, 2=MOVE), time_ms is
// the event time (for double-tap timing). Single-pointer (pointer 0).
void
process_touch_event(int32_t action, int32_t count, float x0, float y0, float x1, float y1)
{
	mark_user_input(); // any touch holds the auto-spin idle timer
	static float last_x = 0.0f, last_y = 0.0f;            // 1-finger orbit anchor
	static float pinch_last = 0.0f;                        // last 2-finger distance
	static bool two_finger = false;                        // a 2-finger gesture is active
	static bool drag_valid = false;                        // a clean 1-finger drag

	if (count >= 2) {
		// ── two fingers: pinch-to-zoom ── (suppresses orbit). Two-finger pan was
		// intentionally dropped — it competed with the pinch; lateral navigation is
		// via double-tap focus instead.
		drag_valid = false;
		const float ex = x0 - x1, ey = y0 - y1;
		const float dist = sqrtf(ex * ex + ey * ey);
		if (two_finger && pinch_last > 1.0f) {
			// Pinch: scale zoom by the finger-distance ratio (clamped 0.2×–8×).
			float z = g_zoom.load(std::memory_order_relaxed) * (dist / pinch_last);
			if (z < 0.2f) z = 0.2f;
			if (z > 8.0f) z = 8.0f;
			g_zoom.store(z, std::memory_order_relaxed);
		}
		pinch_last = dist;
		two_finger = true;
		return;
	}

	// ── fewer than two fingers ──
	pinch_last = 0.0f;
	switch (action) {
	case AMOTION_EVENT_ACTION_DOWN:
		last_x = x0;
		last_y = y0;
		drag_valid = true;
		two_finger = false;
		break;
	case AMOTION_EVENT_ACTION_MOVE:
		// Suppress orbit while/after a 2-finger gesture (until all fingers lift) so
		// lifting one finger of a pinch doesn't snap the orbit.
		if (drag_valid && !two_finger) {
			const float dx = x0 - last_x;
			const float dy = y0 - last_y;
			last_x = x0;
			last_y = y0;
			// macOS/Windows gauss: 0.005 rad/px, sign-flipped so the display tracks
			// the finger; pitch clamped ±1.5.
			float yaw = g_orbit_yaw.load(std::memory_order_relaxed) - dx * 0.005f;
			float pitch = g_orbit_pitch.load(std::memory_order_relaxed) - dy * 0.005f;
			if (pitch > 1.5f) pitch = 1.5f;
			if (pitch < -1.5f) pitch = -1.5f;
			g_orbit_yaw.store(yaw, std::memory_order_relaxed);
			g_orbit_pitch.store(pitch, std::memory_order_relaxed);
		}
		break;
	case AMOTION_EVENT_ACTION_UP:
	case AMOTION_EVENT_ACTION_CANCEL:
		drag_valid = false;
		two_finger = false;
		break;
	default:
		break;
	}
}

// Double-tap focus: store the tap NDC; the render loop raycasts it against the
// splat once it has the located views, and flies the rig to the hit (Chunk 2).
void
request_focus(float ndc_x, float ndc_y)
{
	mark_user_input();
	g_focus_ndc_x.store(ndc_x, std::memory_order_relaxed);
	g_focus_ndc_y.store(ndc_y, std::memory_order_relaxed);
	g_focus_pending.store(true, std::memory_order_release);
}

// Long-press / reset: orbit + zoom here; the pivot eases back to the framed centroid
// on the render thread (g_reset_pending).
void
reset_view(void)
{
	mark_user_input();
	g_orbit_yaw.store(0.0f, std::memory_order_relaxed);
	g_orbit_pitch.store(0.0f, std::memory_order_relaxed);
	g_zoom.store(1.0f, std::memory_order_relaxed);
	g_reset_pending.store(true, std::memory_order_release);
	/* pivot reset runs on the render thread */
	LOGI("gesture: reset view");
}

} // namespace

// ─── JNI bridge to MainActivity ──────────────────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_gausssplat_1vk_1android_MainActivity_nativeSetRotation(
    JNIEnv * /*env*/, jobject /*thiz*/, jint rotation)
{
	g_display_rotation.store(rotation & 3, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_gausssplat_1vk_1android_MainActivity_nativeRuntimeUnavailable(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return g_runtime_unavailable.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_gausssplat_1vk_1android_MainActivity_nativeXrReady(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return (g_instance != XR_NULL_HANDLE) ? JNI_TRUE : JNI_FALSE;
}

// Gesture bridge: MainActivity.dispatchTouchEvent forwards multitouch here (the
// native InputQueue is not fed under the runtime overlay, runtime#499). count is
// the pointer count; (x1,y1) are valid only when count>=2.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_gausssplat_1vk_1android_MainActivity_nativeOnTouch(
    JNIEnv * /*env*/, jobject /*thiz*/, jint action, jint count, jfloat x0, jfloat y0, jfloat x1,
    jfloat y1)
{
	process_touch_event(action, count, x0, y0, x1, y1);
}

// Double-tap focus (from the Java GestureDetector). ndc_x/ndc_y are the tap in
// normalized device coords (+Y up); resolved against the splat in the render loop.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_gausssplat_1vk_1android_MainActivity_nativeFocusAt(
    JNIEnv * /*env*/, jobject /*thiz*/, jfloat ndc_x, jfloat ndc_y)
{
	request_focus(ndc_x, ndc_y);
}

// Long-press reset (from the Java GestureDetector).
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_gausssplat_1vk_1android_MainActivity_nativeResetView(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	reset_view();
}

extern "C" void
android_main(struct android_app *app)
{
	LOGI("gausssplat_vk_android: android_main entered");
	app->onAppCmd = handle_cmd;
	// Swipe-orbit touch arrives via MainActivity.dispatchTouchEvent → nativeOnTouch
	// (runtime#499: the native InputQueue is not fed under the runtime's overlay),
	// not via app->onInputEvent.

	if (!initialize_loader(app)) {
		LOGE("OpenXR loader init failed");
	}

	while (true) {
		const int poll_timeout_ms = g_session_running ? 0 : 250;
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(poll_timeout_ms, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) {
				source->process(app, source);
			}
			if (app->destroyRequested != 0) {
				destroy_all();
				return;
			}
		}
		if (g_instance != XR_NULL_HANDLE) {
			poll_xr_events();
			if (g_exit_requested) {
				destroy_all();
				return;
			}
			// Drive frames from READY (not SYNCHRONIZED+): a CTS-compliant
			// runtime only advances READY->SYNCHRONIZED on the first
			// xrBeginFrame, so gating on SYNCHRONIZED+ deadlocks at READY ->
			// black (David's #507). render_frame honors shouldRender.
			if (app->window != nullptr && g_session_running) {
				render_frame();
			}
		}
	}
}
