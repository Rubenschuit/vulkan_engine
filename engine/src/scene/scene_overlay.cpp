#include "pch.hpp"
#include "scene/scene_overlay.hpp"

#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "utils/ve_log.hpp"
#include "utils/ve_path.hpp"

#include <json.hpp>
#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace ve {

using nlohmann::json;

namespace {

std::optional<glm::vec3> parseVec3(const json& j) {
	if (!j.is_array() || j.size() != 3)
		return std::nullopt;
	return glm::vec3{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

bool selectorMatches(const SceneOverlay::LightOverride& lo, const std::string& name) {
	switch (lo.selector) {
	case SceneOverlay::SelectorKind::Name:
		return name == lo.selector_value;
	case SceneOverlay::SelectorKind::Group:
		return name.rfind(lo.selector_value, 0) == 0;
	case SceneOverlay::SelectorKind::Contains:
		return name.find(lo.selector_value) != std::string::npos;
	}
	return false;
}

void applyValues(auto* light, const SceneOverlay::LightOverride& lo) {
	if (!light)
		return;
	if (lo.color)
		light->setColor(*lo.color);
	if (lo.intensity)
		light->setIntensity(*lo.intensity);
	else if (lo.intensity_mul)
		light->setIntensity(light->getIntensity() * *lo.intensity_mul);
	if (lo.range)
		light->setRange(*lo.range);
}

void applyToLight(Registry& reg, Entity e, const SceneOverlay::LightOverride& lo) {
	if (lo.active)
		reg.setActive(e, *lo.active);

	if (lo.to_spot && reg.hasComponent<PointLightComponent>(e)) {
		const PointLightComponent* pl = reg.getComponent<PointLightComponent>(e);
		const glm::vec3 color = pl->getColor();
		const float intensity = pl->getIntensity();
		const float range = pl->getRange();
		reg.removeComponent<PointLightComponent>(e);
		SpotLightComponent& sl = reg.addComponent<SpotLightComponent>(e);
		sl.setColor(color);
		sl.setIntensity(intensity);
		sl.setRange(range);
		sl.setDirection(lo.spot_dir_local);
		sl.setInnerConeAngle(lo.spot_inner_rad);
		sl.setOuterConeAngle(lo.spot_outer_rad);
	}

	applyValues(reg.getComponent<PointLightComponent>(e), lo);
	applyValues(reg.getComponent<SpotLightComponent>(e), lo);
}

struct EmissiveLight {
	Entity e;
	std::string name;
	int occurrence;  // index among emissive lights that share this name
};

std::vector<EmissiveLight> collectEmissiveLights(Registry& reg) {
	std::vector<EmissiveLight> lights;
	for (auto [e, pl] : reg.view<PointLightComponent>().includeInactive()) {
		(void)pl;
		if (reg.getLightSource(e) == LightSource::Emissive)
			lights.push_back({e, reg.getName(e), 0});
	}
	for (auto [e, sl] : reg.view<SpotLightComponent>().includeInactive()) {
		(void)sl;
		if (reg.getLightSource(e) == LightSource::Emissive)
			lights.push_back({e, reg.getName(e), 0});
	}
	std::sort(lights.begin(), lights.end(),
		[](const EmissiveLight& a, const EmissiveLight& b) { return a.e.index() < b.e.index(); });
	std::unordered_map<std::string, int> counts;
	for (EmissiveLight& L : lights)
		L.occurrence = counts[L.name]++;
	return lights;
}

} // namespace

std::optional<SceneOverlay> SceneOverlay::loadFromFile(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		VE_LOGW("SceneOverlay: no overlay at " << pathToUtf8(path) << " (loading scene without it)");
		return std::nullopt;
	}

	json doc;
	try {
		doc = json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
	} catch (const std::exception& ex) {
		VE_LOGE("SceneOverlay: parse error in " << pathToUtf8(path) << ": " << ex.what());
		return std::nullopt;
	}

	SceneOverlay overlay;
	overlay.version = doc.value("version", 1);

	const auto overrides = doc.find("overrides");
	if (overrides == doc.end() || !overrides->is_array()) {
		VE_LOGW("SceneOverlay: " << pathToUtf8(path) << " has no 'overrides' array");
		return overlay;
	}

	for (const json& o : *overrides) {
		LightOverride lo;
		if (const auto sel = o.find("select"); sel != o.end() && sel->is_object()) {
			if (const auto v = sel->find("name"); v != sel->end()) {
				lo.selector = SelectorKind::Name;
				lo.selector_value = v->get<std::string>();
			} else if (const auto v = sel->find("group"); v != sel->end()) {
				lo.selector = SelectorKind::Group;
				lo.selector_value = v->get<std::string>();
			} else if (const auto v = sel->find("contains"); v != sel->end()) {
				lo.selector = SelectorKind::Contains;
				lo.selector_value = v->get<std::string>();
			}
			if (const auto v = sel->find("index"); v != sel->end())
				lo.occurrence = v->get<int>();
		}
		if (lo.selector_value.empty()) {
			VE_LOGW("SceneOverlay: override with missing/empty selector, skipping");
			continue;
		}

		if (const auto v = o.find("active"); v != o.end())
			lo.active = v->get<bool>();
		if (const auto v = o.find("color"); v != o.end())
			lo.color = parseVec3(*v);
		if (const auto v = o.find("intensity"); v != o.end())
			lo.intensity = v->get<float>();
		if (const auto v = o.find("intensity_mul"); v != o.end())
			lo.intensity_mul = v->get<float>();
		if (const auto v = o.find("range"); v != o.end())
			lo.range = v->get<float>();

		if (const auto s = o.find("as_spot"); s != o.end() && s->is_object()) {
			lo.to_spot = true;
			lo.spot_inner_rad = glm::radians(s->value("inner_deg", 25.0f));
			lo.spot_outer_rad = glm::radians(s->value("outer_deg", 35.0f));
			if (const auto d = s->find("dir"); d != s->end())
				if (const auto pv = parseVec3(*d))
					lo.spot_dir_local = *pv;
		}

		overlay.light_overrides.push_back(std::move(lo));
	}

	VE_LOGI("SceneOverlay: loaded " << overlay.light_overrides.size()
	        << " light override(s) from " << pathToUtf8(path));
	return overlay;
}

void SceneOverlay::apply(Registry& registry) const {
	if (light_overrides.empty())
		return;

	// Snapshot
	const std::vector<EmissiveLight> lights = collectEmissiveLights(registry);

	int applied = 0;
	for (const LightOverride& lo : light_overrides) {
		int matched = 0;
		if (lo.selector == SelectorKind::Name && lo.occurrence) {
			for (const EmissiveLight& L : lights) {
				if (L.name == lo.selector_value && L.occurrence == *lo.occurrence) {
					applyToLight(registry, L.e, lo);
					++matched;
					break;
				}
			}
		} else {
			for (const EmissiveLight& L : lights) {
				if (!selectorMatches(lo, L.name))
					continue;
				applyToLight(registry, L.e, lo);
				++matched;
			}
		}
		if (matched == 0)
			VE_LOGW("SceneOverlay: selector '" << lo.selector_value << "'"
				<< (lo.occurrence ? " #" + std::to_string(*lo.occurrence) : std::string{})
				<< " matched no emissive lights");
		applied += matched;
	}

	VE_LOGI("SceneOverlay: applied overrides to " << applied << " light(s)");
}

bool SceneOverlay::saveEmissiveState(Registry& registry, const std::filesystem::path& path) {
	const std::vector<EmissiveLight> lights = collectEmissiveLights(registry);
	std::unordered_map<std::string, int> totals;
	for (const EmissiveLight& L : lights)
		++totals[L.name];

	auto colorArray = [](const glm::vec3& c) {
		return json::array({c.r, c.g, c.b});
	};

	// One entry per active light
	struct Entry {
		std::string name;
		int occurrence;
		json obj;
	};
	std::vector<Entry> entries;
	for (const EmissiveLight& L : lights) {
		if (!registry.isActive(L.e))
			continue;
		json sel;
		sel["name"] = L.name;
		if (totals[L.name] > 1)
			sel["index"] = L.occurrence;

		json o;
		o["select"] = std::move(sel);
		o["active"] = true;
		if (const auto* pl = registry.getComponent<PointLightComponent>(L.e)) {
			o["color"] = colorArray(pl->getColor());
			o["intensity"] = pl->getIntensity();
			if (pl->getRange() > 0.0f)
				o["range"] = pl->getRange();
		} else if (const auto* sl = registry.getComponent<SpotLightComponent>(L.e)) {
			o["color"] = colorArray(sl->getColor());
			o["intensity"] = sl->getIntensity();
			if (sl->getRange() > 0.0f)
				o["range"] = sl->getRange();
			const glm::vec3 dir = sl->getDirection();
			o["as_spot"] = {
				{"inner_deg", glm::degrees(sl->getInnerConeAngle())},
				{"outer_deg", glm::degrees(sl->getOuterConeAngle())},
				{"dir", json::array({dir.x, dir.y, dir.z})},
			};
		} else {
			continue;
		}
		entries.push_back({L.name, L.occurrence, std::move(o)});
	}

	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
		return a.name != b.name ? a.name < b.name : a.occurrence < b.occurrence;
	});

	json overrides = json::array();
	for (Entry& en : entries)
		overrides.push_back(std::move(en.obj));

	json doc;
	doc["version"] = 1;
	doc["overrides"] = std::move(overrides);

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		VE_LOGE("SceneOverlay: cannot write overlay to " << pathToUtf8(path));
		return false;
	}
	out << doc.dump(1, '\t') << '\n';
	VE_LOGI("SceneOverlay: saved " << entries.size() << " light override(s) to " << pathToUtf8(path));
	return true;
}

} // namespace ve