/* Automated benchmark mode: runs a registered scene with a fixed timestep and
 * a fixed or scripted camera, waits for async loads to settle, discards warmup
 * frames, samples FrameStats over N measured frames, then writes an aggregate
 * stats JSON (median/p95 per pass) and optionally a screenshot PNG before
 * exiting.
 *
 * The camera is either a single static pose (--bench-camera) or a keypoint
 * path (--bench-path) interpolated by measured-frame index, so the trajectory
 * is bit-identical every run against the fixed dt. A moving camera is what
 * actually exercises culling / Hi-Z occlusion / temporal reprojection.
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

// Camera pose in the same px,py,pz:lx,ly,lz form the Debug panel copies.
struct CameraKeypoint {
	glm::vec3 pos{0.0f};
	glm::vec3 look{0.0f};
};

// --bench-culling selector; DEFAULT leaves RenderSettings untouched. The app
// maps GPU/MESHLET to their CullingBackendMode and forces Hi-Z occlusion on.
enum class BenchCulling { DEFAULT, CPU, GPU, MESHLET };

struct VENGINE_API BenchmarkConfig {
	std::string scene;                     // registered scene name; empty = app default scene
	uint32_t warmup_frames = 200;
	uint32_t measure_frames = 500;
	float fixed_dt = 1.0f / 60.0f;         // deterministic sim timestep, replaces wall-clock dt
	std::filesystem::path stats_path;      // aggregate JSON; empty = log summary only
	std::filesystem::path screenshot_path; // final-frame PNG; empty = no screenshot

	// Empty = scene/editor default camera. One keypoint = static pose. Two or
	// more = path interpolated across the measure window (warmup holds at [0]).
	std::vector<CameraKeypoint> keypoints;
	BenchCulling culling = BenchCulling::DEFAULT;

	// Skybox display-name override; empty = scene/app default.
	std::string skybox;

	// Window size override
	uint32_t width = 0;
	uint32_t height = 0;

	// Recognized flags: --benchmark, --bench-scene <name>, --bench-frames <n>,
	// --bench-warmup <n>, --bench-dt <seconds>, --bench-stats <path>,
	// --bench-screenshot <path>, --bench-camera px,py,pz:lx,ly,lz,
	// --bench-path <file> (one keypoint per line, same syntax; # comments ok),
	// --bench-culling cpu|gpu|meshlet, --bench-res WxH, --bench-skybox <name>.
	// Any of them enables benchmark mode; returns nullopt when none are present.
	// Throws std::runtime_error on a malformed flag or unreadable path file.
	static std::optional<BenchmarkConfig> parseArgs(int argc, char** argv);
};

// Environment facts recorded into the stats JSON, gathered by the app at finish.
struct VENGINE_API BenchmarkRunInfo {
	std::string scene_name;
	std::string device_name;
	std::string driver_name;
	uint32_t width = 0;
	uint32_t height = 0;
	int msaa_samples = 1;
	bool hdr = false;
	bool validation_enabled = false;
	uint32_t validation_errors = 0;
	uint32_t validation_warnings = 0;
	std::string culling_backend = "cpu"; // resolved backend actually rendered
	bool hiz_occlusion = false;
	bool draw_indirect_count = false;
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

	// Camera pose to apply for the frame about to render, or nullopt to leave
	// the camera alone. Warmup/wait hold at keypoint 0; measure interpolates by
	// sample index. Called by the app before building the camera view.
	std::optional<CameraKeypoint> cameraPose() const;

	// Once per rendered scene frame, before endFrame. scene_idle = active scene
	// present and no scene swap or model load pending or in flight.
	Action onFrame(bool scene_idle, const FrameStats& stats);

	// Aggregates samples and writes the stats JSON. Returns the process exit
	// code: 0 ok, 3 = validation errors occurred, 4 = scene never became idle.
	int finish(const BenchmarkRunInfo& info);

private:
	enum class Phase { WAIT_SCENE, WARMUP, MEASURE };

	static constexpr uint32_t MAX_WAIT_SCENE_FRAMES = 18000;

	CameraKeypoint poseAtFrame(uint32_t measure_index) const;

	BenchmarkConfig m_config;
	Phase m_phase = Phase::WAIT_SCENE;
	uint32_t m_phase_frames = 0;
	bool m_screenshot_requested = false;
	bool m_load_timed_out = false;
	std::vector<FrameStats> m_samples;
};

}
