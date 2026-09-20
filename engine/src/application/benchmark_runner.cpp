#include "pch.hpp"
#include "application/benchmark_runner.hpp"
#include "rendering/render_settings.hpp"
#include "utils/ve_log.hpp"
#include "utils/ve_path.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <map>
#include <numeric>
#include <span>
#include <stdexcept>

namespace ve {

namespace {

struct FloatMetric {
	const char* name;
	float FrameStats::* member;
};

constexpr FloatMetric FLOAT_METRICS[] = {
	{"cpu_time", &FrameStats::cpu_time},
	{"fence_wait", &FrameStats::fence_wait},
	{"acquire_wait", &FrameStats::acquire_wait},
	{"gpu_time", &FrameStats::gpu_time},
	{"compute_gpu_time", &FrameStats::compute_gpu_time},
	{"gpu_culling", &FrameStats::gpu_culling},
	{"gpu_shadow_maps", &FrameStats::gpu_shadow_maps},
	{"gpu_geometry_prepass", &FrameStats::gpu_geometry_prepass},
	{"gpu_gtao", &FrameStats::gpu_gtao},
	{"gpu_scene_render", &FrameStats::gpu_scene_render},
	{"gpu_ssr", &FrameStats::gpu_ssr},
	{"gpu_rc", &FrameStats::gpu_rc},
	{"gpu_rc_build", &FrameStats::gpu_rc_build},
	{"gpu_rc_shade", &FrameStats::gpu_rc_shade},
	{"gpu_rc_map", &FrameStats::gpu_rc_map},
	{"gpu_rc_trace", &FrameStats::gpu_rc_trace},
	{"gpu_rc_resolve", &FrameStats::gpu_rc_resolve},
	{"gpu_rc_merge", &FrameStats::gpu_rc_merge},
	{"gpu_rc_irradiance", &FrameStats::gpu_rc_irradiance},
	{"gpu_rc_gather", &FrameStats::gpu_rc_gather},
	{"gpu_bloom", &FrameStats::gpu_bloom},
	{"gpu_post_process", &FrameStats::gpu_post_process},
	{"gpu_hiz", &FrameStats::gpu_hiz},
	{"gpu_shadow_mask", &FrameStats::gpu_shadow_mask},
	{"gpu_outline", &FrameStats::gpu_outline},
	{"gpu_skinning", &FrameStats::gpu_skinning},
	{"gpu_cluster_lights", &FrameStats::gpu_cluster_lights},
	{"gpu_particles", &FrameStats::gpu_particles},
	{"cpu_culling", &FrameStats::cpu_culling},
	{"cpu_shadow_maps", &FrameStats::cpu_shadow_maps},
	{"cpu_geometry_prepass", &FrameStats::cpu_geometry_prepass},
	{"cpu_gtao", &FrameStats::cpu_gtao},
	{"cpu_scene_render", &FrameStats::cpu_scene_render},
	{"cpu_ssr", &FrameStats::cpu_ssr},
	{"cpu_rc", &FrameStats::cpu_rc},
	{"cpu_bloom", &FrameStats::cpu_bloom},
	{"cpu_post_process", &FrameStats::cpu_post_process},
	{"cpu_hiz", &FrameStats::cpu_hiz},
	{"cpu_shadow_mask", &FrameStats::cpu_shadow_mask},
	{"cpu_outline", &FrameStats::cpu_outline},
	{"cpu_physics", &FrameStats::cpu_physics},
	{"cpu_ui", &FrameStats::cpu_ui},
	{"cpu_skinning", &FrameStats::cpu_skinning},
	{"cpu_cluster_lights", &FrameStats::cpu_cluster_lights},
	{"cpu_particles", &FrameStats::cpu_particles},
};

// Written as "rc_rays", outside timings_ms so no timing gate reads them
constexpr FloatMetric RAY_METRICS[] = {
	{"rc_rays_hit", &FrameStats::rc_rays_hit},
	{"rc_rays_clear", &FrameStats::rc_rays_clear},
	{"rc_rays_sky", &FrameStats::rc_rays_sky},
	{"rc_rays_sky_gated", &FrameStats::rc_rays_sky_gated},
	{"rc_rays_unk_occluded", &FrameStats::rc_rays_unk_occluded},
	{"rc_rays_unk_stepcap", &FrameStats::rc_rays_unk_stepcap},
	{"rc_rays_unk_edge", &FrameStats::rc_rays_unk_edge},
	{"rc_rays_unk_clip", &FrameStats::rc_rays_unk_clip},
};

struct CounterMetric {
	const char* name;
	uint32_t FrameStats::* member;
};

constexpr CounterMetric COUNTER_METRICS[] = {
	{"cull_total_objects", &FrameStats::cull_total_objects},
	{"cull_visible_objects", &FrameStats::cull_visible_objects},
	{"visible_triangles", &FrameStats::visible_triangles},
	{"visible_meshlets", &FrameStats::visible_meshlets},
	{"num_point_lights", &FrameStats::num_point_lights},
	{"num_directional_lights", &FrameStats::num_directional_lights},
	{"num_spot_lights", &FrameStats::num_spot_lights},
	{"num_area_lights", &FrameStats::num_area_lights},
	{"rc_probes", &FrameStats::rc_probes},
	{"rc_overflow", &FrameStats::rc_overflow},
	{"rc_probes_c0", &FrameStats::rc_probes_c0},
	{"rc_probes_c1", &FrameStats::rc_probes_c1},
	{"rc_probes_c2", &FrameStats::rc_probes_c2},
	{"rc_probes_c3", &FrameStats::rc_probes_c3},
	{"rc_probes_c4", &FrameStats::rc_probes_c4},
	{"rc_probes_c5", &FrameStats::rc_probes_c5},
};

struct Aggregate {
	float mean, median, p95, min, max;
};

Aggregate aggregate(std::vector<float>& values) {
	std::sort(values.begin(), values.end());
	auto at = [&](double p) {
		return values[static_cast<size_t>(p * static_cast<double>(values.size() - 1) + 0.5)];
	};
	double sum = std::accumulate(values.begin(), values.end(), 0.0);
	return {
		static_cast<float>(sum / static_cast<double>(values.size())),
		at(0.5), at(0.95), values.front(), values.back()
	};
}

std::string jsonEscape(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		if (c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	return out;
}

std::string utcTimestamp() {
	std::time_t now = std::time(nullptr);
	char buf[32];
	if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now)) == 0)
		return "unknown";
	return buf;
}

bool parseKeypoint(const char* s, CameraKeypoint& kp) {
	return std::sscanf(s, "%f,%f,%f:%f,%f,%f",
		&kp.pos.x, &kp.pos.y, &kp.pos.z, &kp.look.x, &kp.look.y, &kp.look.z) == 6;
}

float parseFloat(std::string_view text, const std::string& flag) {
	std::string s(text);
	char* end = nullptr;
	float v = std::strtof(s.c_str(), &end);
	if (s.empty() || end != s.c_str() + s.size() || !std::isfinite(v))
		throw std::runtime_error(flag + " expects a finite number, got '" + s + "'");
	return v;
}

uint32_t parseUnsigned(std::string_view text, const std::string& flag) {
	float v = parseFloat(text, flag);
	if (v < 0.0f || v != std::floor(v))
		throw std::runtime_error(flag + " expects a non-negative integer, got '" + std::string(text) + "'");
	return static_cast<uint32_t>(v);
}

using S = RenderSettings;
constexpr BenchSetKey BENCH_SET_KEYS[] = {
	{.name = "rc_enabled", .apply = [](S& s, float v) { s.rc.enabled = v != 0.0f; }},
	{.name = "rc_thickness", .apply = [](S& s, float v) { s.rc.thickness = v; }},
	{.name = "rc_intensity", .apply = [](S& s, float v) { s.rc.intensity = v; }},
	{.name = "rc_luma_clamp", .apply = [](S& s, float v) { s.rc.luma_clamp = v; }},
	{.name = "rc_resolution_div", .apply = [](S& s, float v) { s.rc.resolution_div = static_cast<int>(v); }},
	{.name = "rc_short_range_ao", .apply = [](S& s, float v) { s.rc.short_range_ao = v; }},
	{.name = "rc_spec_occlusion", .apply = [](S& s, float v) { s.rc.spec_occlusion = v; }},
	{.name = "ssr_max_roughness", .apply = [](S& s, float v) { s.ssr_max_roughness = v; }},
	{.name = "rc_rays_per_pixel", .apply = [](S& s, float v) { s.rc.rays_per_pixel = static_cast<int>(v); }},
	{.name = "rc_ds0", .apply = [](S& s, float v) { s.rc.ds0 = v; }},
	{.name = "rc_lod0_dist", .apply = [](S& s, float v) { s.rc.lod0_dist = v; }},
	{.name = "rc_sky_min_frac", .apply = [](S& s, float v) { s.rc.sky_min_frac = v; }},
	{.name = "rc_probe_ttl", .apply = [](S& s, float v) { s.rc.probe_ttl = static_cast<int>(v); }},
	{.name = "rc_sky_visibility", .apply = [](S& s, float v) { s.rc.sky_visibility = v; }},
	{.name = "rc_blend_zone", .apply = [](S& s, float v) { s.rc.blend_zone = v; }},
	{.name = "rc_cascades", .apply = [](S& s, float v) { s.rc.cascades = static_cast<int>(v); }},
	{.name = "rc_c0_polar_bins", .apply = [](S& s, float v) { s.rc.c0_polar_bins = static_cast<int>(v); }},
	{.name = "rc_c0_ray_length", .apply = [](S& s, float v) { s.rc.c0_ray_length = v; }},
	{.name = "rc_memory_mb", .apply = [](S& s, float v) { s.rc.memory_mb = static_cast<int>(v); }},
	{.name = "rc_hist_depth_check", .apply = [](S& s, float v) { s.rc.hist_depth_check = v != 0.0f; }},
	{.name = "rc_rough_specular", .apply = [](S& s, float v) { s.rc.rough_specular = v != 0.0f; }},
	{.name = "rc_spec_handoff_roughness", .apply = [](S& s, float v) { s.rc.spec_handoff_roughness = v; }},
	{.name = "rc_parent_mode", .apply = [](S& s, float v) { s.rc.parent_mode = static_cast<int>(v); }},
	{.name = "rc_deposit_mode", .apply = [](S& s, float v) { s.rc.deposit_mode = static_cast<int>(v); }},
	{.name = "rc_count_weighted_read", .apply = [](S& s, float v) { s.rc.count_weighted_read = v != 0.0f; }},
	{.name = "rc_freeze_probes", .apply = [](S& s, float v) { s.rc.freeze_probes = v != 0.0f; }},
	{.name = "rc_single_frame", .apply = [](S& s, float v) { s.rc.single_frame = v != 0.0f; }},
	{.name = "rc_probe_j_cascade", .apply = [](S& s, float v) { s.rc.probe_j_cascade = static_cast<int>(v); }},
	{.name = "ibl_enabled", .apply = [](S& s, float v) { s.ibl_enabled = v != 0.0f; }},
	{.name = "ibl_min_ambient", .apply = [](S& s, float v) { s.ibl_min_ambient = v; }},
	{.name = "ambient_light_intensity", .apply = [](S& s, float v) { s.ambient_light_intensity = v; }},
	{.name = "gtao_enabled", .apply = [](S& s, float v) { s.gtao_enabled = v != 0.0f; }},
	{.name = "exposure", .apply = [](S& s, float v) { s.exposure = v; }},
	{.name = "bloom_enabled", .apply = [](S& s, float v) { s.bloom_enabled = v != 0.0f; }},
	{.name = "ssr_enabled", .apply = [](S& s, float v) { s.ssr_enabled = v != 0.0f; }},
	{.name = "render_mode", .apply = [](S& s, float v) { s.render_mode = static_cast<RenderMode>(static_cast<uint32_t>(v)); },
		.unsigned_only = true},
	{.name = "tone_map_mode", .apply = [](S& s, float v) { s.tone_map_mode = static_cast<int>(v); }},
};

// One keypoint per line in --bench-camera syntax; blank lines and # comments ok.
std::vector<CameraKeypoint> loadCameraPath(const std::filesystem::path& file) {
	std::ifstream in(file);
	if (!in)
		throw std::runtime_error("--bench-path cannot open " + pathToUtf8(file));
	std::vector<CameraKeypoint> keypoints;
	std::string line;
	uint32_t lineno = 0;
	while (std::getline(in, line)) {
		++lineno;
		size_t start = line.find_first_not_of(" \t\r\n");
		if (start == std::string::npos || line[start] == '#')
			continue;
		CameraKeypoint kp;
		if (!parseKeypoint(line.c_str() + start, kp))
			throw std::runtime_error("--bench-path malformed line " + std::to_string(lineno)
				+ " (expects px,py,pz:lx,ly,lz)");
		keypoints.push_back(kp);
	}
	if (keypoints.empty())
		throw std::runtime_error("--bench-path has no keypoints: " + pathToUtf8(file));
	return keypoints;
}

// FNV-1a over the per-frame counter stream: the exact-match gate. Deterministic
// across runs under frame-indexed camera motion.
std::string counterChecksum(const std::vector<FrameStats>& samples) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](uint32_t v) {
		for (int b = 0; b < 4; ++b) {
			h ^= static_cast<uint8_t>(v >> (b * 8));
			h *= 1099511628211ull;
		}
	};
	for (const FrameStats& s : samples)
		for (const auto& metric : COUNTER_METRICS)
			mix(s.*metric.member);
	return std::format("{:#018x}", h);
}

} // namespace

const BenchSetKey* findBenchSetKey(std::string_view name) {
	for (const BenchSetKey& k : BENCH_SET_KEYS)
		if (name == k.name)
			return &k;
	return nullptr;
}

std::optional<BenchmarkConfig> BenchmarkConfig::parseArgs(int argc, char** argv) {
	BenchmarkConfig config;
	bool enabled = false;
	std::optional<CameraKeypoint> single_pose;
	std::filesystem::path path_file;

	auto value = [&](int& i, const char* flag) -> const char* {
		if (i + 1 >= argc)
			throw std::runtime_error(std::string(flag) + " requires a value");
		return argv[++i];
	};

	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--benchmark") {
			enabled = true;
		} else if (arg == "--bench-scene") {
			config.scene = value(i, "--bench-scene");
			enabled = true;
		} else if (arg == "--bench-frames") {
			config.measure_frames = static_cast<uint32_t>(std::stoul(value(i, "--bench-frames")));
			enabled = true;
		} else if (arg == "--bench-warmup") {
			config.warmup_frames = static_cast<uint32_t>(std::stoul(value(i, "--bench-warmup")));
			enabled = true;
		} else if (arg == "--bench-dt") {
			config.fixed_dt = parseFloat(value(i, "--bench-dt"), "--bench-dt");
			enabled = true;
		} else if (arg == "--bench-stats") {
			config.stats_path = value(i, "--bench-stats");
			enabled = true;
		} else if (arg == "--bench-screenshot") {
			config.screenshot_path = value(i, "--bench-screenshot");
			enabled = true;
		} else if (arg == "--bench-rc-cold") {
			config.rc_cold_start = true;
			enabled = true;
		} else if (arg == "--bench-rc-warm") {
			config.rc_cold_start = false;
			enabled = true;
		} else if (arg == "--bench-camera") {
			CameraKeypoint kp;
			if (!parseKeypoint(value(i, "--bench-camera"), kp))
				throw std::runtime_error("--bench-camera expects px,py,pz:lx,ly,lz");
			single_pose = kp;
			enabled = true;
		} else if (arg == "--bench-path") {
			path_file = value(i, "--bench-path");
			enabled = true;
		} else if (arg == "--bench-culling") {
			std::string_view v = value(i, "--bench-culling");
			if (v == "cpu")
				config.culling = BenchCulling::CPU;
			else if (v == "gpu")
				config.culling = BenchCulling::GPU;
			else if (v == "meshlet")
				config.culling = BenchCulling::MESHLET;
			else
				throw std::runtime_error("--bench-culling expects cpu|gpu|meshlet");
			enabled = true;
		} else if (arg == "--bench-skybox") {
			config.skybox = value(i, "--bench-skybox");
			enabled = true;
		} else if (arg == "--bench-skybox-exposure") {
			config.skybox_exposure = parseFloat(value(i, "--bench-skybox-exposure"), "--bench-skybox-exposure");
			enabled = true;
		} else if (arg == "--bench-debug-view") {
			config.debug_render_mode = static_cast<int>(parseUnsigned(value(i, "--bench-debug-view"), "--bench-debug-view"));
			enabled = true;
		} else if (arg == "--bench-rc-view") {
			uint32_t id = parseUnsigned(value(i, "--bench-rc-view"), "--bench-rc-view");
			if (!isRcDebugView(id))
				throw std::runtime_error("--bench-rc-view: no RcDebugView with id " + std::to_string(id));
			config.rc_view = static_cast<int>(id);
			enabled = true;
		} else if (arg == "--bench-set") {
			std::string_view kv = value(i, "--bench-set");
			size_t eq = kv.find('=');
			if (eq == std::string_view::npos || eq == 0 || eq + 1 >= kv.size())
				throw std::runtime_error("--bench-set expects key=value");
			std::string key(kv.substr(0, eq));
			const BenchSetKey* entry = findBenchSetKey(key);
			if (!entry)
				throw std::runtime_error("--bench-set unknown key: " + key);
			float v = parseFloat(kv.substr(eq + 1), "--bench-set " + key);
			if (entry->unsigned_only && v < 0.0f)
				throw std::runtime_error("--bench-set " + key + " must not be negative");
			config.set_values.emplace_back(std::move(key), v);
			enabled = true;
		} else if (arg == "--bench-res") {
			unsigned w = 0, h = 0;
			if (std::sscanf(value(i, "--bench-res"), "%ux%u", &w, &h) != 2 || w == 0 || h == 0)
				throw std::runtime_error("--bench-res expects WxH (e.g. 1920x1080)");
			config.width = w;
			config.height = h;
			enabled = true;
		} else if (arg.starts_with("--bench")) {
			throw std::runtime_error("unknown benchmark flag: " + std::string(arg));
		}
	}

	if (!enabled)
		return std::nullopt;
	if (config.measure_frames == 0)
		throw std::runtime_error("--bench-frames must be > 0");
	if (config.fixed_dt <= 0.0f)
		throw std::runtime_error("--bench-dt must be > 0");

	// A path (>= 2 keypoints) wins over a single --bench-camera pose.
	if (!path_file.empty())
		config.keypoints = loadCameraPath(path_file);
	else if (single_pose)
		config.keypoints = {*single_pose};
	return config;
}

std::optional<CameraKeypoint> BenchmarkRunner::cameraPose() const {
	if (m_config.keypoints.empty())
		return std::nullopt;
	// Static pose, or holding at the start during wait/warmup.
	if (m_config.keypoints.size() == 1 || m_phase != Phase::MEASURE)
		return m_config.keypoints.front();
	return poseAtFrame(static_cast<uint32_t>(m_samples.size()));
}

// Interpolate the keypoint path linearly by measured-frame index. Keypoints are
// evenly spaced across the measure window; frame 0 is keypoint 0, the final
// measured frame is the last keypoint.
CameraKeypoint BenchmarkRunner::poseAtFrame(uint32_t measure_index) const {
	const std::vector<CameraKeypoint>& kps = m_config.keypoints;
	const uint32_t n = static_cast<uint32_t>(kps.size());
	const float span = static_cast<float>(m_config.measure_frames > 1 ? m_config.measure_frames - 1 : 1);
	float t = (static_cast<float>(measure_index) / span) * static_cast<float>(n - 1);
	t = std::clamp(t, 0.0f, static_cast<float>(n - 1));
	uint32_t k = static_cast<uint32_t>(t);
	if (k > n - 2)
		k = n - 2;
	const float f = t - static_cast<float>(k);
	CameraKeypoint out;
	out.pos = kps[k].pos + (kps[k + 1].pos - kps[k].pos) * f;
	out.look = kps[k].look + (kps[k + 1].look - kps[k].look) * f;
	return out;
}

bool BenchmarkRunner::consumeRcReset() {
	bool pending = m_rc_reset_pending;
	m_rc_reset_pending = false;
	return pending;
}

BenchmarkRunner::Action BenchmarkRunner::onFrame(bool scene_idle, const FrameStats& stats) {
	switch (m_phase) {
	case Phase::WAIT_SCENE:
		if (scene_idle) {
			m_phase = Phase::WARMUP;
			m_phase_frames = 0;
			m_rc_reset_pending = m_config.rc_cold_start;
			VE_LOGI("[bench] scene ready, warming up for " << m_config.warmup_frames << " frames");
		} else if (++m_phase_frames > MAX_WAIT_SCENE_FRAMES) {
			VE_LOGE("[bench] scene never finished loading, aborting");
			m_load_timed_out = true;
			return Action::FINISH;
		}
		return Action::CONTINUE;

	case Phase::WARMUP:
		if (++m_phase_frames >= m_config.warmup_frames) {
			m_phase = Phase::MEASURE;
			m_samples.reserve(m_config.measure_frames);
			VE_LOGI("[bench] measuring " << m_config.measure_frames << " frames");
		}
		return Action::CONTINUE;

	case Phase::MEASURE:
		if (m_samples.size() < m_config.measure_frames) {
			m_samples.push_back(stats);
			if (m_samples.size() == m_config.measure_frames) {
				if (!m_config.screenshot_path.empty() && !m_screenshot_requested) {
					m_screenshot_requested = true;
					return Action::TAKE_SCREENSHOT;
				}
				return Action::FINISH;
			}
			return Action::CONTINUE;
		}
		return Action::FINISH; // the extra frame that carried the screenshot copy
	}
	return Action::CONTINUE;
}

int BenchmarkRunner::finish(const BenchmarkRunInfo& info) {
	if (m_load_timed_out || m_samples.empty()) {
		VE_LOGE("[bench] no samples collected, nothing written");
		return 4;
	}

	std::string json;
	json += "{\n";
	json += std::format("  \"schema\": 2,\n");
	json += std::format("  \"timestamp\": \"{}\",\n", utcTimestamp());
	json += std::format("  \"scene\": \"{}\",\n", jsonEscape(info.scene_name));
	json += std::format("  \"device\": \"{}\",\n", jsonEscape(info.device_name));
	json += std::format("  \"driver\": \"{}\",\n", jsonEscape(info.driver_name));
	json += std::format("  \"resolution\": {{\"width\": {}, \"height\": {}}},\n", info.width, info.height);
	json += std::format("  \"msaa_samples\": {},\n", info.msaa_samples);
	json += std::format("  \"hdr\": {},\n", info.hdr);
	json += std::format("  \"fixed_dt\": {},\n", m_config.fixed_dt);
	json += std::format("  \"warmup_frames\": {},\n", m_config.warmup_frames);
	json += std::format("  \"measured_frames\": {},\n", m_samples.size());
	json += std::format("  \"validation\": {{\"enabled\": {}, \"errors\": {}, \"warnings\": {}}},\n",
		info.validation_enabled, info.validation_errors, info.validation_warnings);
	json += std::format("  \"culling\": {{\"backend\": \"{}\", \"hiz_occlusion\": {}, \"draw_indirect_count\": {}}},\n",
		info.culling_backend, info.hiz_occlusion, info.draw_indirect_count);
	json += std::format("  \"camera\": {{\"keypoints\": {}, \"moving\": {}}},\n",
		m_config.keypoints.size(), m_config.keypoints.size() > 1);
	json += std::format("  \"rc\": {{\"enabled\": {}, \"cold_start\": {}}},\n",
		info.rc_enabled, m_config.rc_cold_start);

	std::map<std::string, float> overrides;
	for (const auto& [key, v] : m_config.set_values)
		overrides[key] = v;
	json += "  \"overrides\": {";
	bool first = true;
	for (const auto& [key, v] : overrides) {
		json += std::format("{}\"{}\": {}", first ? "" : ", ", key, v);
		first = false;
	}
	json += "},\n";

	// Counters are a deterministic per-frame sequence under a fixed workload +
	// frame-indexed camera. counter_checksum is the exact-match gate; min/max
	// are for human reading (min==max only when the camera is static).
	json += std::format("  \"counter_checksum\": \"{}\",\n", counterChecksum(m_samples));
	json += "  \"counters\": {\n";
	first = true;
	for (const auto& metric : COUNTER_METRICS) {
		uint32_t min_v = m_samples.front().*metric.member;
		uint32_t max_v = min_v;
		for (const FrameStats& s : m_samples) {
			min_v = std::min(min_v, s.*metric.member);
			max_v = std::max(max_v, s.*metric.member);
		}
		json += std::format("{}    \"{}\": {{\"min\": {}, \"max\": {}}}",
			first ? "" : ",\n", metric.name, min_v, max_v);
		first = false;
	}
	json += "\n  },\n";

	Aggregate gpu_total{}, cpu_total{};
	std::vector<float> values(m_samples.size());
	auto write_aggregates = [&](const char* section, std::span<const FloatMetric> metrics) {
		json += std::format("  \"{}\": {{\n", section);
		bool first_metric = true;
		for (const auto& metric : metrics) {
			for (size_t i = 0; i < m_samples.size(); ++i)
				values[i] = m_samples[i].*metric.member;
			Aggregate a = aggregate(values);
			if (metric.member == &FrameStats::gpu_time)
				gpu_total = a;
			if (metric.member == &FrameStats::cpu_time)
				cpu_total = a;
			json += std::format(
				"{}    \"{}\": {{\"mean\": {:.4f}, \"median\": {:.4f}, \"p95\": {:.4f}, \"min\": {:.4f}, \"max\": {:.4f}}}",
				first_metric ? "" : ",\n", metric.name, a.mean, a.median, a.p95, a.min, a.max);
			first_metric = false;
		}
		json += "\n  }";
	};
	write_aggregates("timings_ms", FLOAT_METRICS);
	json += ",\n";
	write_aggregates("rc_rays", RAY_METRICS);
	json += "\n}\n";

	if (!m_config.stats_path.empty()) {
		std::error_code ec;
		if (m_config.stats_path.has_parent_path())
			std::filesystem::create_directories(m_config.stats_path.parent_path(), ec);
		std::ofstream out(m_config.stats_path);
		if (!out) {
			VE_LOGE("[bench] failed to open stats file " << m_config.stats_path);
			return 4;
		}
		out << json;
		VE_LOGI("[bench] stats written to " << m_config.stats_path);
	}

	VE_LOGI("[bench] " << info.scene_name << " @ " << info.width << "x" << info.height
		<< " | cpu median " << cpu_total.median << " ms, p95 " << cpu_total.p95
		<< " ms | gpu median " << gpu_total.median << " ms, p95 " << gpu_total.p95 << " ms");

	if (info.validation_errors > 0) {
		VE_LOGE("[bench] " << info.validation_errors << " validation errors during run");
		return 3;
	}
	return 0;
}

}