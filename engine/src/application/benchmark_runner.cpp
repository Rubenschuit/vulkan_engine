#include "pch.hpp"
#include "application/benchmark_runner.hpp"
#include "utils/ve_log.hpp"
#include "utils/ve_path.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>
#include <numeric>
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

// FNV-1a over the per-frame counter stream. Under frame-indexed camera motion
// the sequence is deterministic across runs, so this is the exact-match gate
// that replaces min==max once counters are no longer per-frame constant.
// (GPU-readback backends may lag counts a frame; gate their checksum with care.)
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
			config.fixed_dt = std::stof(value(i, "--bench-dt"));
			enabled = true;
		} else if (arg == "--bench-stats") {
			config.stats_path = value(i, "--bench-stats");
			enabled = true;
		} else if (arg == "--bench-screenshot") {
			config.screenshot_path = value(i, "--bench-screenshot");
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

BenchmarkRunner::Action BenchmarkRunner::onFrame(bool scene_idle, const FrameStats& stats) {
	switch (m_phase) {
	case Phase::WAIT_SCENE:
		if (scene_idle) {
			m_phase = Phase::WARMUP;
			m_phase_frames = 0;
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
	json += std::format("  \"schema\": 1,\n");
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

	// Counters are a deterministic per-frame sequence under a fixed workload +
	// frame-indexed camera. counter_checksum is the exact-match gate; min/max
	// are for human reading (min==max only when the camera is static).
	json += std::format("  \"counter_checksum\": \"{}\",\n", counterChecksum(m_samples));
	json += "  \"counters\": {\n";
	bool first = true;
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

	json += "  \"timings_ms\": {\n";
	first = true;
	Aggregate gpu_total{}, cpu_total{};
	std::vector<float> values(m_samples.size());
	for (const auto& metric : FLOAT_METRICS) {
		for (size_t i = 0; i < m_samples.size(); ++i)
			values[i] = m_samples[i].*metric.member;
		Aggregate a = aggregate(values);
		if (metric.member == &FrameStats::gpu_time)
			gpu_total = a;
		if (metric.member == &FrameStats::cpu_time)
			cpu_total = a;
		json += std::format(
			"{}    \"{}\": {{\"mean\": {:.4f}, \"median\": {:.4f}, \"p95\": {:.4f}, \"min\": {:.4f}, \"max\": {:.4f}}}",
			first ? "" : ",\n", metric.name, a.mean, a.median, a.p95, a.min, a.max);
		first = false;
	}
	json += "\n  }\n}\n";

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