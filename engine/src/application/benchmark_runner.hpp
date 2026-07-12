/* Automated benchmark mode: runs a registered scene with a fixed timestep and
 * a fixed camera, waits for async loads to settle, discards warmup frames,
 * samples FrameStats over N measured frames, then writes an aggregate stats
 * JSON (median/p95 per pass) and optionally a screenshot PNG before exiting.
 *
 * Enabled from the command line (see BenchmarkConfig::parseArgs); the main
 * loop in VeApplication drives the runner once per rendered scene frame.
 */
#pragma once
#include "ve_export.hpp"
#include "rendering/frame_stats.hpp"

#include <filesystem>
#include <glm/vec3.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ve {

struct VENGINE_API BenchmarkConfig {
	std::string scene;                     // registered scene name; empty = app default scene
	uint32_t warmup_frames = 200;
	uint32_t measure_frames = 500;
	float fixed_dt = 1.0f / 60.0f;         // deterministic sim timestep, replaces wall-clock dt
	std::filesystem::path stats_path;      // aggregate JSON; empty = log summary only
	std::filesystem::path screenshot_path; // final-frame PNG; empty = no screenshot
	std::optional<glm::vec3> camera_pos;
	std::optional<glm::vec3> camera_look;

	// Recognized flags: --benchmark, --bench-scene <name>, --bench-frames <n>,
	// --bench-warmup <n>, --bench-dt <seconds>, --bench-stats <path>,
	// --bench-screenshot <path>, --bench-camera px,py,pz:lx,ly,lz.
	// Any of them enables benchmark mode; returns nullopt when none are present.
	// Throws std::runtime_error on a malformed flag.
	static std::optional<BenchmarkConfig> parseArgs(int argc, char** argv);
};

// Environment facts recorded into the stats JSON, gathered by the app at finish.
struct VENGINE_API BenchmarkRunInfo {
	std::string scene_name;
	std::string device_name;
	uint32_t width = 0;
	uint32_t height = 0;
	int msaa_samples = 1;
	bool hdr = false;
	bool validation_enabled = false;
	uint32_t validation_errors = 0;
	uint32_t validation_warnings = 0;
};

class VENGINE_API BenchmarkRunner {
public:
	enum class Action {
		CONTINUE,
		TAKE_SCREENSHOT, // request the renderer screenshot this frame
		FINISH           // call finish() and close the app
	};

	explicit BenchmarkRunner(BenchmarkConfig config) : m_config(std::move(config)) {}

	const BenchmarkConfig& config() const { return m_config; }

	// Once per rendered scene frame, before endFrame. scene_idle = active scene
	// present and no scene swap or model load pending or in flight.
	Action onFrame(bool scene_idle, const FrameStats& stats);

	// Aggregates samples and writes the stats JSON. Returns the process exit
	// code: 0 ok, 3 = validation errors occurred, 4 = scene never became idle.
	int finish(const BenchmarkRunInfo& info);

private:
	enum class Phase { WAIT_SCENE, WARMUP, MEASURE };

	// Frames to wait for scene load before giving up (safety net for CI hangs).
	static constexpr uint32_t MAX_WAIT_SCENE_FRAMES = 18000;

	BenchmarkConfig m_config;
	Phase m_phase = Phase::WAIT_SCENE;
	uint32_t m_phase_frames = 0;
	bool m_screenshot_requested = false;
	bool m_load_timed_out = false;
	std::vector<FrameStats> m_samples;
};

}
