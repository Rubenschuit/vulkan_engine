#include "pch.hpp"
#include "ui/panels/inspector_panel.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/editor_icons.hpp"
#include "ui/imgui_helpers.hpp"
#include "ui/texture_inspector.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_texture.hpp"
#include "resources/ve_material_properties.hpp"
#include "utils/ve_string.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>

namespace ve {

using namespace ve::ui;

static const char* textureFormatName(vk::Format fmt) {
	switch (fmt) {
		case vk::Format::eR8G8B8A8Srgb:       return "RGBA8 sRGB";
		case vk::Format::eR8G8B8A8Unorm:       return "RGBA8";
		case vk::Format::eR8G8B8Srgb:          return "RGB8 sRGB";
		case vk::Format::eR8G8B8Unorm:         return "RGB8";
		case vk::Format::eBc1RgbSrgbBlock:     return "BC1 sRGB";
		case vk::Format::eBc1RgbUnormBlock:    return "BC1";
		case vk::Format::eBc1RgbaSrgbBlock:    return "BC1a sRGB";
		case vk::Format::eBc1RgbaUnormBlock:   return "BC1a";
		case vk::Format::eBc3SrgbBlock:        return "BC3 sRGB";
		case vk::Format::eBc3UnormBlock:       return "BC3";
		case vk::Format::eBc4UnormBlock:       return "BC4";
		case vk::Format::eBc5UnormBlock:       return "BC5";
		case vk::Format::eBc7SrgbBlock:        return "BC7 sRGB";
		case vk::Format::eBc7UnormBlock:       return "BC7";
		case vk::Format::eAstc4x4SrgbBlock:   return "ASTC 4x4 sRGB";
		case vk::Format::eAstc4x4UnormBlock:  return "ASTC 4x4";
		case vk::Format::eAstc6x6SrgbBlock:   return "ASTC 6x6 sRGB";
		case vk::Format::eAstc6x6UnormBlock:  return "ASTC 6x6";
		case vk::Format::eAstc8x8SrgbBlock:   return "ASTC 8x8 sRGB";
		case vk::Format::eAstc8x8UnormBlock:  return "ASTC 8x8";
		default:                                return "Other";
	}
}

InspectorPanel::~InspectorPanel() {
	clearTextureCache();
}

VkDescriptorSet InspectorPanel::getOrCreateTextureDescriptor(const std::string& id, const VeTexture* texture) {
	if (!texture || id.empty())
		return VK_NULL_HANDLE;

	auto it = m_texture_cache.find(id);
	if (it != m_texture_cache.end()) {
		it->second.last_used_frame = m_frame_counter;
		return it->second.descriptor_set;
	}

	if (m_texture_cache.size() >= MAX_CACHED_TEXTURES)
		evictStaleTextures();

	VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(
		static_cast<VkSampler>(*texture->getSampler()),
		static_cast<VkImageView>(*texture->getImageView()),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_texture_cache[id] = {ds, m_frame_counter};
	return ds;
}

void InspectorPanel::evictStaleTextures() {
	for (auto it = m_texture_cache.begin(); it != m_texture_cache.end(); ) {
		if (m_frame_counter - it->second.last_used_frame > TEXTURE_CACHE_TTL) {
			ImGui_ImplVulkan_RemoveTexture(it->second.descriptor_set);
			it = m_texture_cache.erase(it);
		} else {
			++it;
		}
	}
}

void InspectorPanel::clearTextureCache() {
	for (auto& [tex, entry] : m_texture_cache)
		if (entry.descriptor_set != VK_NULL_HANDLE)
			ImGui_ImplVulkan_RemoveTexture(entry.descriptor_set);
	m_texture_cache.clear();
}

void InspectorPanel::renderTextureSlot(const char* label, const std::string& id, const VeTexture* texture, float thumb_size) {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	ImGui::Text("%s", label);
	ImGui::TableNextColumn();
	if (texture) {
		VkDescriptorSet ds = getOrCreateTextureDescriptor(id, texture);
		if (ds != VK_NULL_HANDLE) {
			ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(ds)),
			             ImVec2(thumb_size, thumb_size));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Click to inspect\n%s", id.c_str());
			if (ImGui::IsItemClicked() && m_texture_inspector)
				m_texture_inspector->open(texture, label);
		} else {
			ImGui::TextDisabled("[failed]");
		}
		ImGui::TableNextColumn();
		uint32_t w = texture->getWidth();
		uint32_t h = texture->getHeight();
		uint32_t mips = texture->getMipLevels();
		if (w > 0 && h > 0) {
			ImGui::Text("%ux%u", w, h);
			ImGui::Text("%u %s", mips, mips == 1 ? "mip" : "mips");
			ImGui::TextDisabled("%s", textureFormatName(texture->getFormat()));
		}
	} else {
		ImGui::TextDisabled("[none]");
		ImGui::TableNextColumn();
	}
}

// Draws a vec3 editor with colored X/Y/Z labels
// TODO: make fit better
static bool drawVec3Control(const char* label, glm::vec3& values, float speed = 0.1f, float reset_value = 0.0f, int decimals = 2) {
	bool changed = false;
	ImGui::PushID(label);

	char fmt[8];
	snprintf(fmt, sizeof(fmt), "%%.%df", decimals);

	float line_height = ImGui::GetFrameHeight();
	ImVec2 btn_size = {line_height, line_height};

	ImGui::Columns(2, nullptr, false);
	ImGui::SetColumnWidth(0, 85.0f);
	ImGui::Text("%s", label);
	ImGui::NextColumn();

	float total_width = ImGui::GetContentRegionAvail().x;
	float spacing = 1.0f;
	float drag_width = (total_width - btn_size.x * 3 - spacing * 5) / 3.0f;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));

	// X
	ImGui::PushStyleColor(ImGuiCol_Button, COL_AXIS_X);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_AXIS_X_HOVER);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL_AXIS_X);
	if (ImGui::Button("X", btn_size))
		{ values.x = reset_value; changed = true; }
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(drag_width);
	if (ImGui::DragFloat("##X", &values.x, speed, 0.0f, 0.0f, fmt))
		changed = true;
	ImGui::SameLine();

	// Y
	ImGui::PushStyleColor(ImGuiCol_Button, COL_AXIS_Y);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_AXIS_Y_HOVER);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL_AXIS_Y);
	if (ImGui::Button("Y", btn_size))
		{ values.y = reset_value; changed = true; }
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(drag_width);
	if (ImGui::DragFloat("##Y", &values.y, speed, 0.0f, 0.0f, fmt))
		changed = true;
	ImGui::SameLine();

	// Z
	ImGui::PushStyleColor(ImGuiCol_Button, COL_AXIS_Z);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_AXIS_Z_HOVER);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL_AXIS_Z);
	if (ImGui::Button("Z", btn_size))
		{ values.z = reset_value; changed = true; }
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(drag_width);
	if (ImGui::DragFloat("##Z", &values.z, speed, 0.0f, 0.0f, fmt))
		changed = true;

	ImGui::PopStyleVar();
	ImGui::Columns(1);
	ImGui::PopID();
	return changed;
}

void InspectorPanel::render(Registry* registry, EditorState& state, UIContext& /*context*/) {
	m_frame_counter++;

	// Flush texture cache on scene switch
	if (registry != m_last_registry) {
		clearTextureCache();
		m_last_registry = registry;
	}

	if (m_frame_counter % 60 == 0)
		evictStaleTextures();

	if (!ImGui::Begin("Entity Inspector", &state.show_inspector, ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::End();
		return;
	}

	if (state.selectedEntity().isNull() || !registry || !registry->isAlive(state.selectedEntity())) {
		ImGui::TextDisabled("No entity selected");
		ImGui::End();
		return;
	}

	Entity entity = state.selectedEntity();

	if (state.selected_entities.size() > 1) {
		const std::string& primary_name = registry->getName(entity);
		if (primary_name.empty())
			ImGui::TextDisabled("%zu selected, editing Entity %u", state.selected_entities.size(), entity.index());
		else
			ImGui::TextDisabled("%zu selected, editing \"%s\"", state.selected_entities.size(), primary_name.c_str());
		ImGui::Separator();
	}

	renderEntityHeader(*registry, entity, state);
	ImGui::Separator();

	// Transform (no remove button)
	auto* transform = registry->getComponent<TransformComponent>(entity);
	if (transform) {
		bool open = ImGui::CollapsingHeader(ICON_TRANSFORM "  Transform", ImGuiTreeNodeFlags_DefaultOpen);
		if (ImGui::BeginPopupContextItem("transform_ctx")) {
			if (ImGui::MenuItem("Copy")) {
				CopiedTransform ct;
				ct.translation = transform->getTranslation();
				ct.rotation = transform->getRotation();
				ct.scale = transform->getScale();
				state.component_clipboard = ct;
			}
			if (state.component_clipboard && std::holds_alternative<CopiedTransform>(*state.component_clipboard))
				if (ImGui::MenuItem("Paste")) {
					auto& ct = std::get<CopiedTransform>(*state.component_clipboard);
					transform->setTranslation(ct.translation);
					transform->setRotation(ct.rotation);
					transform->setScale(ct.scale);
				}
			ImGui::EndPopup();
		}
		if (open)
			renderTransform(*transform);
	}

	// Mesh
	if (registry->hasComponent<MeshComponent>(entity)) {
		bool open = ImGui::CollapsingHeader(ICON_MESH "  Mesh", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8.0f);
		ImGui::PushID("remove_mesh");
		if (ImGui::SmallButton("X"))
			registry->queueComponentRemoval<MeshComponent>(entity);
		ImGui::PopID();
		if (open && registry->hasComponent<MeshComponent>(entity))
			renderMesh(*registry->getComponent<MeshComponent>(entity));
	}

	// Point Light
	if (registry->hasComponent<PointLightComponent>(entity)) {
		bool open = ImGui::CollapsingHeader(ICON_POINT_LIGHT "  Point Light", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (ImGui::BeginPopupContextItem("pl_ctx")) {
			if (ImGui::MenuItem("Copy")) {
				auto* pl = registry->getComponent<PointLightComponent>(entity);
				CopiedPointLight cpl;
				cpl.intensity = pl->getIntensity();
				cpl.color = pl->getColor();
				cpl.range = pl->getRange();
				cpl.rotates = pl->getRotates();
				cpl.casts_shadow = pl->getCastsShadow();
				state.component_clipboard = cpl;
			}
			if (state.component_clipboard && std::holds_alternative<CopiedPointLight>(*state.component_clipboard))
				if (ImGui::MenuItem("Paste")) {
					auto* pl = registry->getComponent<PointLightComponent>(entity);
					auto& cpl = std::get<CopiedPointLight>(*state.component_clipboard);
					pl->setIntensity(cpl.intensity);
					pl->setColor(cpl.color);
					pl->setRange(cpl.range);
					pl->setRotates(cpl.rotates);
					pl->setCastsShadow(cpl.casts_shadow);
				}
			ImGui::EndPopup();
		}
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8.0f);
		ImGui::PushID("remove_pl");
		if (ImGui::SmallButton("X"))
			registry->queueComponentRemoval<PointLightComponent>(entity);
		ImGui::PopID();
		if (open && registry->hasComponent<PointLightComponent>(entity))
			renderPointLight(*registry->getComponent<PointLightComponent>(entity));
	}

	// Spot Light
	if (registry->hasComponent<SpotLightComponent>(entity)) {
		bool open = ImGui::CollapsingHeader(ICON_SPOT_LIGHT "  Spot Light", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (ImGui::BeginPopupContextItem("sl_ctx")) {
			if (ImGui::MenuItem("Copy")) {
				auto* sl = registry->getComponent<SpotLightComponent>(entity);
				CopiedSpotLight csl;
				csl.intensity = sl->getIntensity();
				csl.color = sl->getColor();
				csl.range = sl->getRange();
				csl.direction = sl->getDirection();
				csl.inner_cone_angle = sl->getInnerConeAngle();
				csl.outer_cone_angle = sl->getOuterConeAngle();
				csl.casts_shadow = sl->getCastsShadow();
				state.component_clipboard = csl;
			}
			if (state.component_clipboard && std::holds_alternative<CopiedSpotLight>(*state.component_clipboard))
				if (ImGui::MenuItem("Paste")) {
					auto* sl = registry->getComponent<SpotLightComponent>(entity);
					auto& csl = std::get<CopiedSpotLight>(*state.component_clipboard);
					sl->setIntensity(csl.intensity);
					sl->setColor(csl.color);
					sl->setRange(csl.range);
					sl->setDirection(csl.direction);
					sl->setInnerConeAngle(csl.inner_cone_angle);
					sl->setOuterConeAngle(csl.outer_cone_angle);
					sl->setCastsShadow(csl.casts_shadow);
				}
			ImGui::EndPopup();
		}
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8.0f);
		ImGui::PushID("remove_sl");
		if (ImGui::SmallButton("X"))
			registry->queueComponentRemoval<SpotLightComponent>(entity);
		ImGui::PopID();
		if (open && registry->hasComponent<SpotLightComponent>(entity))
			renderSpotLight(*registry->getComponent<SpotLightComponent>(entity));
	}

	// Area Light
	if (registry->hasComponent<AreaLightComponent>(entity)) {
		bool open = ImGui::CollapsingHeader(ICON_AREA_LIGHT "  Area Light", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (ImGui::BeginPopupContextItem("al_ctx")) {
			if (ImGui::MenuItem("Copy")) {
				auto* al = registry->getComponent<AreaLightComponent>(entity);
				CopiedAreaLight cal;
				cal.intensity = al->getIntensity();
				cal.color = al->getColor();
				cal.two_sided = al->getTwoSided();
				cal.range = al->getRange();
				state.component_clipboard = cal;
			}
			if (state.component_clipboard && std::holds_alternative<CopiedAreaLight>(*state.component_clipboard))
				if (ImGui::MenuItem("Paste")) {
					auto* al = registry->getComponent<AreaLightComponent>(entity);
					auto& cal = std::get<CopiedAreaLight>(*state.component_clipboard);
					al->setIntensity(cal.intensity);
					al->setColor(cal.color);
					al->setTwoSided(cal.two_sided);
					al->setRange(cal.range);
				}
			ImGui::EndPopup();
		}
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8.0f);
		ImGui::PushID("remove_al");
		if (ImGui::SmallButton("X"))
			registry->queueComponentRemoval<AreaLightComponent>(entity);
		ImGui::PopID();
		if (open && registry->hasComponent<AreaLightComponent>(entity))
			renderAreaLight(*registry->getComponent<AreaLightComponent>(entity),
				*registry->getComponent<TransformComponent>(entity));
	}

	// Directional Light
	if (registry->hasComponent<DirectionalLightComponent>(entity)) {
		bool open = ImGui::CollapsingHeader(ICON_DIR_LIGHT "  Directional Light", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (ImGui::BeginPopupContextItem("dl_ctx")) {
			if (ImGui::MenuItem("Copy")) {
				auto* dl = registry->getComponent<DirectionalLightComponent>(entity);
				CopiedDirectionalLight cdl;
				cdl.direction = dl->getDirection();
				cdl.color = dl->getColor();
				cdl.intensity = dl->getIntensity();
				cdl.casts_shadow = dl->getCastsShadow();
				cdl.celestial_type = static_cast<uint8_t>(dl->getCelestialType());
				state.component_clipboard = cdl;
			}
			if (state.component_clipboard && std::holds_alternative<CopiedDirectionalLight>(*state.component_clipboard))
				if (ImGui::MenuItem("Paste")) {
					auto* dl = registry->getComponent<DirectionalLightComponent>(entity);
					auto& cdl = std::get<CopiedDirectionalLight>(*state.component_clipboard);
					dl->setDirection(cdl.direction);
					dl->setColor(cdl.color);
					dl->setIntensity(cdl.intensity);
					dl->setCastsShadow(cdl.casts_shadow);
					dl->setCelestialType(static_cast<CelestialType>(cdl.celestial_type));
				}
			ImGui::EndPopup();
		}
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8.0f);
		ImGui::PushID("remove_dl");
		if (ImGui::SmallButton("X"))
			registry->queueComponentRemoval<DirectionalLightComponent>(entity);
		ImGui::PopID();
		if (open && registry->hasComponent<DirectionalLightComponent>(entity))
			renderDirectionalLight(*registry->getComponent<DirectionalLightComponent>(entity));
	}

	// Rigidbody
	if (registry->hasComponent<RigidbodyComponent>(entity)) {
		bool open = ImGui::CollapsingHeader(ICON_RIGIDBODY "  Rigidbody", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (ImGui::BeginPopupContextItem("rb_ctx")) {
			if (ImGui::MenuItem("Copy")) {
				auto* rb = registry->getComponent<RigidbodyComponent>(entity);
				CopiedRigidbody crb;
				crb.motion_type = static_cast<uint8_t>(rb->getMotionType());
				crb.shape_type = static_cast<uint8_t>(rb->getShapeDesc().type);
				crb.mass = rb->getMass();
				crb.friction = rb->getFriction();
				crb.restitution = rb->getRestitution();
				crb.hull_tolerance = rb->getHullTolerance();
				state.component_clipboard = crb;
			}
			if (state.component_clipboard && std::holds_alternative<CopiedRigidbody>(*state.component_clipboard))
				if (ImGui::MenuItem("Paste")) {
					auto* rb = registry->getComponent<RigidbodyComponent>(entity);
					auto& crb = std::get<CopiedRigidbody>(*state.component_clipboard);
					rb->setMotionType(static_cast<PhysicsMotionType>(crb.motion_type));
					PhysicsShapeDesc desc;
					desc.type = static_cast<PhysicsShapeType>(crb.shape_type);
					rb->setShapeDesc(desc);
					rb->setMass(crb.mass);
					rb->setFriction(crb.friction);
					rb->setRestitution(crb.restitution);
					rb->setHullTolerance(crb.hull_tolerance);
				}
			ImGui::EndPopup();
		}
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8.0f);
		ImGui::PushID("remove_rb");
		if (ImGui::SmallButton("X"))
			registry->queueComponentRemoval<RigidbodyComponent>(entity);
		ImGui::PopID();
		if (open && registry->hasComponent<RigidbodyComponent>(entity))
			renderRigidbody(*registry->getComponent<RigidbodyComponent>(entity), state);
	}

	// Animator
	if (registry->hasComponent<AnimatorComponent>(entity)) {
		bool open = ImGui::CollapsingHeader(ICON_ANIMATOR "  Animator", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (open && registry->hasComponent<AnimatorComponent>(entity))
			renderAnimator(*registry->getComponent<AnimatorComponent>(entity));
	}

	// Skin
	if (registry->hasComponent<SkinComponent>(entity)) {
		auto* sc = registry->getComponent<SkinComponent>(entity);
		char header[64];
		snprintf(header, sizeof(header), ICON_SKIN "  Skin (%zu joints)", sc->jointCount());
		bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (open && registry->hasComponent<SkinComponent>(entity))
			renderSkin(*registry, *registry->getComponent<SkinComponent>(entity), state);
	}

	// Morph
	if (registry->hasComponent<MorphComponent>(entity)) {
		auto* morph = registry->getComponent<MorphComponent>(entity);
		char header[64];
		snprintf(header, sizeof(header), "Morph (%zu targets)", morph->targetCount());
		bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (open) {
			ImGui::PushID("morph");
			auto& weights = morph->weights();
			for (size_t i = 0; i < weights.size(); i++) {
				char label[32];
				snprintf(label, sizeof(label), "Weight %zu", i);
				ImGui::DragFloat(label, &weights[i], 0.01f);
			}
			ImGui::PopID();
		}
	}

	// Camera
	if (registry->hasComponent<CameraComponent>(entity)) {
		bool open = ImGui::CollapsingHeader(ICON_CAMERA "  Camera", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (ImGui::BeginPopupContextItem("cam_ctx")) {
			if (ImGui::MenuItem("Copy")) {
				auto* cc = registry->getComponent<CameraComponent>(entity);
				CopiedCamera ccp;
				ccp.projection = static_cast<uint8_t>(cc->getProjection());
				ccp.fov_y_radians = cc->getFovY();
				ccp.ortho_size = cc->getOrthoSize();
				ccp.near_plane = cc->getNear();
				ccp.far_plane = cc->getFar();
				ccp.active = cc->isActive();
				ccp.priority = cc->getPriority();
				state.component_clipboard = ccp;
			}
			if (state.component_clipboard && std::holds_alternative<CopiedCamera>(*state.component_clipboard))
				if (ImGui::MenuItem("Paste")) {
					auto* cc = registry->getComponent<CameraComponent>(entity);
					auto& ccp = std::get<CopiedCamera>(*state.component_clipboard);
					cc->setProjection(static_cast<CameraComponent::ProjectionType>(ccp.projection));
					cc->setFovY(ccp.fov_y_radians);
					cc->setOrthoSize(ccp.ortho_size);
					cc->setNear(ccp.near_plane);
					cc->setFar(ccp.far_plane);
					cc->setActive(ccp.active);
					cc->setPriority(ccp.priority);
				}
			ImGui::EndPopup();
		}
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8.0f);
		ImGui::PushID("remove_cam");
		if (ImGui::SmallButton("X"))
			registry->queueComponentRemoval<CameraComponent>(entity);
		ImGui::PopID();
		if (open && registry->hasComponent<CameraComponent>(entity))
			renderCamera(*registry->getComponent<CameraComponent>(entity));
	}

	// Particle Emitter
	if (registry->hasComponent<ParticleEmitterComponent>(entity)) {
		bool open = ImGui::CollapsingHeader(ICON_PARTICLE "  Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 8.0f);
		ImGui::PushID("remove_emitter");
		if (ImGui::SmallButton("X"))
			registry->queueComponentRemoval<ParticleEmitterComponent>(entity);
		ImGui::PopID();
		if (open && registry->hasComponent<ParticleEmitterComponent>(entity))
			renderParticleEmitter(*registry->getComponent<ParticleEmitterComponent>(entity));
	}

	// Add Component
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	float button_width = ImGui::GetContentRegionAvail().x;
	if (ImGui::Button("Add Component", ImVec2(button_width, 0)))
		ImGui::OpenPopup("AddComponentPopup");

	if (ImGui::BeginPopup("AddComponentPopup")) {
		bool no_pl = !registry->hasComponent<PointLightComponent>(entity);
		bool no_sl = !registry->hasComponent<SpotLightComponent>(entity);
		bool no_dl = !registry->hasComponent<DirectionalLightComponent>(entity);
		bool no_al = !registry->hasComponent<AreaLightComponent>(entity);
		bool no_cam = !registry->hasComponent<CameraComponent>(entity);
		bool no_emitter = !registry->hasComponent<ParticleEmitterComponent>(entity);
		bool no_rb = !registry->hasComponent<RigidbodyComponent>(entity);

		if (no_pl || no_sl || no_dl || no_al) {
			ImGui::SeparatorText("Lights");
			if (no_pl && ImGui::MenuItem(ICON_POINT_LIGHT "  Point Light"))
				registry->addComponent<PointLightComponent>(entity);
			if (no_sl && ImGui::MenuItem(ICON_SPOT_LIGHT "  Spot Light"))
				registry->addComponent<SpotLightComponent>(entity);
			if (no_dl && ImGui::MenuItem(ICON_DIR_LIGHT "  Directional Light"))
				registry->addComponent<DirectionalLightComponent>(entity);
			if (no_al && ImGui::MenuItem(ICON_AREA_LIGHT "  Area Light"))
				registry->addComponent<AreaLightComponent>(entity);
		}

		if (no_cam || no_emitter) {
			ImGui::SeparatorText("Rendering");
			if (no_cam && ImGui::MenuItem(ICON_CAMERA "  Camera"))
				registry->addComponent<CameraComponent>(entity);
			if (no_emitter && ImGui::MenuItem(ICON_PARTICLE "  Particle Emitter"))
				registry->addComponent<ParticleEmitterComponent>(entity);
		}

		if (no_rb) {
			ImGui::SeparatorText("Physics");
			if (ImGui::MenuItem(ICON_RIGIDBODY "  Rigidbody"))
				registry->addComponent<RigidbodyComponent>(entity);
		}

		if (!no_pl && !no_sl && !no_dl && !no_al && !no_cam && !no_emitter && !no_rb)
			ImGui::TextDisabled("No components to add");

		ImGui::EndPopup();
	}

	ImGui::End();
}

void InspectorPanel::renderEntityHeader(Registry& registry, Entity entity, EditorState& state) {
	// Name
	const std::string& name = registry.getName(entity);
	char buf[256]{};
	std::snprintf(buf, sizeof(buf), "%s", name.c_str());
	if (ImGui::InputText("Name", buf, sizeof(buf)))
		registry.setName(entity, buf);

	// Active
	bool active = registry.isActive(entity);
	if (ImGui::Checkbox("Active", &active))
		registry.setActive(entity, active);

	// Entity ID
	ImGui::SameLine();
	ImGui::TextDisabled("(ID: %u)", entity.id());

	// Parent
	Entity current_parent = registry.getParent(entity);
	const char* parent_label;
	char parent_buf[64];
	if (current_parent.isNull()) {
		parent_label = "(No parent)";
	} else {
		const std::string& pname = registry.getName(current_parent);
		if (pname.empty()) {
			snprintf(parent_buf, sizeof(parent_buf), "Entity %u", current_parent.index());
			parent_label = parent_buf;
		} else {
			parent_label = pname.c_str();
		}
	}

	ImGui::Columns(2, nullptr, false);
	ImGui::SetColumnWidth(0, 85.0f);
	ImGui::Text("Parent");
	ImGui::NextColumn();

	// "Select Parent" button
	bool has_parent = !current_parent.isNull();
	if (!has_parent)
		ImGui::BeginDisabled();
	float btn_width = ImGui::GetFrameHeight();
	if (ImGui::Button("^##select_parent", ImVec2(btn_width, 0)))
		state.selectSingle(current_parent);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Select parent");
	if (!has_parent)
		ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##Parent", parent_label)) {
		if (ImGui::Selectable("(No parent)", current_parent.isNull()))
			registry.reparent(entity, Entity::null());

		uint32_t max_idx = registry.maxEntityIndex();
		for (uint32_t i = 0; i < max_idx; ++i) {
			if (!registry.isAliveAtIndex(i))
				continue;
			Entity candidate = registry.entityFromIndex(i);
			if (candidate == entity)
				continue;

			// Prevent cycles: check if entity is an ancestor of candidate
			bool is_descendant = false;
			Entity walk = candidate;
			while (!walk.isNull()) {
				if (walk == entity) { is_descendant = true; break; }
				walk = registry.getParent(walk);
			}
			if (is_descendant)
				continue;

			const std::string& cname = registry.getName(candidate);
			char entry[256];
			if (cname.empty())
				snprintf(entry, sizeof(entry), "Entity %u", i);
			else
				snprintf(entry, sizeof(entry), "%s (%u)", cname.c_str(), i);

			if (ImGui::Selectable(entry, current_parent == candidate))
				registry.reparent(entity, candidate);
		}
		ImGui::EndCombo();
	}
	ImGui::Columns(1);
}

void InspectorPanel::renderTransform(TransformComponent& transform) {
	glm::vec3 pos = transform.getTranslation();
	if (drawVec3Control("Translation", pos, 0.1f))
		transform.setTranslation(pos);

	glm::vec3 euler = glm::degrees(glm::eulerAngles(transform.getRotation()));
	if (drawVec3Control("Rotation", euler, 0.5f))
		transform.setRotationEuler(glm::radians(euler));

	glm::vec3 scl = transform.getScale();
	if (drawVec3Control("Scale", scl, 0.01f, 1.0f))
		transform.setScale(scl);
}

void InspectorPanel::renderMesh(MeshComponent& mesh) {
	// Mesh info (read-only)
	VeMesh* m = mesh.getMesh();
	if (m) {
		ImGui::TextDisabled("%s", m->getId().c_str());
		ImGui::Text("Vertices: %u  Indices: %u  LODs: %u",
			m->getVertexCount(), m->getIndexCount(), m->getLodCount());
	} else {
		ImGui::TextDisabled("No mesh data");
	}

	constexpr float label_w = 110.0f;

	labeledWidget(label_w, "Cast Shadow", [&]() {
		if (ImGui::Checkbox("##CastShadow", &mesh.has_shadow))
			if (mesh.getRegistry())
				mesh.getRegistry()->events().emit(MeshDataChangedEvent{mesh.getEntity()});
	});

	VeMaterial* mat = mesh.getMaterial();
	if (!mat) {
		ImGui::TextDisabled("No material");
		return;
	}

	if (!ImGui::TreeNode(ICON_MATERIAL "  Material"))
		return;

	ImGui::TextDisabled("%s", mat->getId().c_str());
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", mat->getId().c_str());

	auto factors = mat->getMaterialFactors();
	auto alpha = mat->getAlphaProps();
	MaterialFactors defaults;
	bool changed = false;
	bool alpha_changed = false;

	ImGui::SeparatorText("Base");
	labeledWidget(label_w, "Base Color", [&]() {
		if (ImGui::ColorEdit4("##BaseColor", glm::value_ptr(factors.base_color_factor)))
			changed = true;
	}, [&]() { factors.base_color_factor = defaults.base_color_factor; changed = true; });
	sliderRow(label_w, "Metallic", "##Metallic", &factors.metallic_factor, 0.0f, 1.0f, defaults.metallic_factor, changed);
	sliderRow(label_w, "Roughness", "##Roughness", &factors.roughness_factor, 0.0f, 1.0f, defaults.roughness_factor, changed);
	sliderRow(label_w, "Occlusion Str", "##Occlusion", &factors.occlusion_strength, 0.0f, 1.0f, defaults.occlusion_strength, changed);

	ImGui::SeparatorText("Emission");
	labeledWidget(label_w, "Emissive", [&]() {
		if (ImGui::ColorEdit3("##Emissive", glm::value_ptr(factors.emissive_factor)))
			changed = true;
	}, [&]() { factors.emissive_factor = defaults.emissive_factor; changed = true; });
	dragRow(label_w, "Emissive Str", "##EmissiveStr", &factors.emissive_strength, 0.001f, 0.0f, 100.0f, "%.4f", defaults.emissive_strength, changed);

	ImGui::SeparatorText("Specular");
	labeledWidget(label_w, "Specular Color", [&]() {
		if (ImGui::ColorEdit3("##SpecularColor", glm::value_ptr(factors.specular_factor),
				ImGuiColorEditFlags_Float))
			changed = true;
	}, [&]() { factors.specular_factor = defaults.specular_factor; changed = true; });
	sliderRow(label_w, "Specular Str", "##SpecularStr", &factors.specular_strength, 0.0f, 1.0f, defaults.specular_strength, changed);

	ImGui::SeparatorText("Transparency & Refraction");
	sliderRow(label_w, "Transmission", "##Transmission", &factors.transmission_factor, 0.0f, 1.0f, defaults.transmission_factor, changed);
	dragRow(label_w, "IOR", "##IOR", &factors.ior, 0.01f, 1.0f, 3.0f, "%.2f", defaults.ior, changed);

	ImGui::SeparatorText("Alpha & Render");
	const char* alpha_modes[] = { "Opaque", "Mask", "Blend" };
	int alpha_idx = static_cast<int>(alpha.alpha_mode);
	labeledWidget(label_w, "Alpha Mode", [&]() {
		if (ImGui::Combo("##AlphaMode", &alpha_idx, alpha_modes, 3)) {
			alpha.alpha_mode = static_cast<AlphaMode>(alpha_idx);
			alpha_changed = true;
		}
	});
	if (alpha.alpha_mode == AlphaMode::MASK)
		labeledWidget(label_w, "Alpha Cutoff", [&]() {
			if (ImGui::SliderFloat("##AlphaCutoff", &alpha.alpha_cutoff, 0.0f, 1.0f))
				alpha_changed = true;
		});
	labeledWidget(label_w, "Double Sided", [&]() {
		if (ImGui::Checkbox("##DoubleSided", &alpha.double_sided))
			alpha_changed = true;
	});

	if (changed)
		mat->setMaterialFactors(factors);
	if (alpha_changed)
		mat->setAlphaProps(alpha);
	if ((changed || alpha_changed) && mesh.getRegistry())
		mesh.getRegistry()->events().emit(MeshDataChangedEvent{mesh.getEntity()});

	// Texture thumbnails
	if (ImGui::TreeNode(ICON_TEXTURE "  Textures")) {
		if (ImGui::BeginTable("##TexTable", 3, ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 52.0f);
			ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
			renderTextureSlot("Albedo",    mat->getAlbedoTexture().getId(), mat->getAlbedoTexture().get());
			renderTextureSlot("Normal",    mat->getNormalTexture().getId(), mat->getNormalTexture().get());
			renderTextureSlot("Metal/Rgh", mat->getMetallicRoughnessTexture().getId(), mat->getMetallicRoughnessTexture().get());
			renderTextureSlot("Occlusion", mat->getOcclusionTexture().getId(), mat->getOcclusionTexture().get());
			renderTextureSlot("Emissive",  mat->getEmissiveTexture().getId(), mat->getEmissiveTexture().get());
			renderTextureSlot("Specular",  mat->getSpecularTexture().getId(), mat->getSpecularTexture().get());
			renderTextureSlot("Spec Color", mat->getSpecularColorTexture().getId(), mat->getSpecularColorTexture().get());
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}

	ImGui::TreePop();
}

void InspectorPanel::renderPointLight(PointLightComponent& light) {
	constexpr float light_label_w = 110.0f;

	glm::vec3 color = light.getColor();
	labeledWidget(light_label_w, "Color", [&]() {
		if (ImGui::ColorEdit3("##Color", glm::value_ptr(color)))
			light.setColor(color);
	}, [&]() { light.setColor(glm::vec3(1.0f)); });

	float intensity = light.getIntensity();
	labeledWidget(light_label_w, "Intensity", [&]() {
		if (ImGui::DragFloat("##Intensity", &intensity, 0.1f, 0.0f, 10000.0f))
			light.setIntensity(intensity);
	}, [&]() { light.setIntensity(1.0f); });

	float range = light.getRange();
	labeledWidget(light_label_w, "Range", [&]() {
		if (ImGui::DragFloat("##Range", &range, 0.1f, 0.0f, 1000.0f, "%.1f"))
			light.setRange(range);
	}, [&]() { light.setRange(0.0f); });

	ImGui::Text("Effective Range: %.1f", light.getEffectiveRange());

	bool casts_shadow = light.getCastsShadow();
	if (ImGui::Checkbox("Casts Shadow", &casts_shadow))
		light.setCastsShadow(casts_shadow);

	bool rotates = light.getRotates();
	if (ImGui::Checkbox("Rotates", &rotates))
		light.setRotates(rotates);

	bool show_billboard = light.getShowBillboard();
	if (ImGui::Checkbox("Show Billboard", &show_billboard))
		light.setShowBillboard(show_billboard);
}

void InspectorPanel::renderSpotLight(SpotLightComponent& light) {
	constexpr float light_label_w = 110.0f;

	glm::vec3 color = light.getColor();
	labeledWidget(light_label_w, "Color", [&]() {
		if (ImGui::ColorEdit3("##Color", glm::value_ptr(color)))
			light.setColor(color);
	}, [&]() { light.setColor(glm::vec3(1.0f)); });

	float intensity = light.getIntensity();
	labeledWidget(light_label_w, "Intensity", [&]() {
		if (ImGui::DragFloat("##Intensity", &intensity, 0.1f, 0.0f, 10000.0f))
			light.setIntensity(intensity);
	}, [&]() { light.setIntensity(1.0f); });

	float range = light.getRange();
	labeledWidget(light_label_w, "Range", [&]() {
		if (ImGui::DragFloat("##Range", &range, 0.1f, 0.0f, 1000.0f, "%.1f"))
			light.setRange(range);
	}, [&]() { light.setRange(0.0f); });

	ImGui::Text("Effective Range: %.1f", light.getEffectiveRange());

	glm::vec3 dir = light.getDirection();
	if (drawVec3Control("Direction", dir, 0.01f)) {
		float len = glm::length(dir);
		if (len > 0.001f)
			light.setDirection(dir / len);
	}

	float inner_deg = glm::degrees(light.getInnerConeAngle());
	labeledWidget(light_label_w, "Inner Cone", [&]() {
		if (ImGui::SliderFloat("##InnerCone", &inner_deg, 0.0f, 90.0f, "%.1f deg"))
			light.setInnerConeAngle(glm::radians(inner_deg));
	}, [&]() { light.setInnerConeAngle(glm::radians(25.0f)); });

	float outer_deg = glm::degrees(light.getOuterConeAngle());
	labeledWidget(light_label_w, "Outer Cone", [&]() {
		if (ImGui::SliderFloat("##OuterCone", &outer_deg, 0.0f, 90.0f, "%.1f deg"))
			light.setOuterConeAngle(glm::radians(outer_deg));
	}, [&]() { light.setOuterConeAngle(glm::radians(35.0f)); });

	bool casts_shadow = light.getCastsShadow();
	if (ImGui::Checkbox("Casts Shadow", &casts_shadow))
		light.setCastsShadow(casts_shadow);

	bool show_billboard = light.getShowBillboard();
	if (ImGui::Checkbox("Show Billboard", &show_billboard))
		light.setShowBillboard(show_billboard);
}

void InspectorPanel::renderAreaLight(AreaLightComponent& light, TransformComponent& transform) {
	constexpr float light_label_w = 110.0f;

	glm::vec3 color = light.getColor();
	labeledWidget(light_label_w, "Color", [&]() {
		if (ImGui::ColorEdit3("##Color", glm::value_ptr(color)))
			light.setColor(color);
	}, [&]() { light.setColor(glm::vec3(1.0f)); });

	float intensity = light.getIntensity();
	labeledWidget(light_label_w, "Intensity", [&]() {
		if (ImGui::DragFloat("##Intensity", &intensity, 0.1f, 0.0f, 10000.0f))
			light.setIntensity(intensity);
	}, [&]() { light.setIntensity(1.0f); });

	glm::vec3 scale = transform.getScale();
	labeledWidget(light_label_w, "Width", [&]() {
		if (ImGui::DragFloat("##Width", &scale.x, 0.05f, 0.01f, 1000.0f, "%.2f"))
			transform.setScale(scale);
	}, [&]() { scale.x = 1.0f; transform.setScale(scale); });

	labeledWidget(light_label_w, "Height", [&]() {
		if (ImGui::DragFloat("##Height", &scale.z, 0.05f, 0.01f, 1000.0f, "%.2f"))
			transform.setScale(scale);
	}, [&]() { scale.z = 1.0f; transform.setScale(scale); });

	float range = light.getRange();
	labeledWidget(light_label_w, "Range", [&]() {
		if (ImGui::DragFloat("##Range", &range, 0.1f, 0.0f, 1000.0f, light.getRange() <= 0.0f ? "auto" : "%.1f"))
			light.setRange(range);
	}, [&]() { light.setRange(0.0f); });

	bool two_sided = light.getTwoSided();
	if (ImGui::Checkbox("Two-Sided", &two_sided))
		light.setTwoSided(two_sided);

	bool show_gizmo = light.getShowGizmo();
	if (ImGui::Checkbox("Show Gizmo", &show_gizmo))
		light.setShowGizmo(show_gizmo);
}

void InspectorPanel::renderDirectionalLight(DirectionalLightComponent& light) {
	glm::vec3 dir = light.getDirection();
	if (drawVec3Control("Direction", dir, 0.01f)) {
		float len = glm::length(dir);
		if (len > 0.001f)
			light.setDirection(dir / len);
	}

	constexpr float light_label_w = 110.0f;

	glm::vec3 color = light.getColor();
	labeledWidget(light_label_w, "Color", [&]() {
		if (ImGui::ColorEdit3("##Color", glm::value_ptr(color)))
			light.setColor(color);
	}, [&]() { light.setColor(glm::vec3(1.f)); });

	float intensity = light.getIntensity();
	labeledWidget(light_label_w, "Intensity", [&]() {
		if (ImGui::DragFloat("##Intensity", &intensity, 0.1f, 0.0f, 100.0f))
			light.setIntensity(intensity);
	}, [&]() { light.setIntensity(1.f); });

	bool casts_shadow = light.getCastsShadow();
	if (ImGui::Checkbox("Casts Shadow", &casts_shadow))
		light.setCastsShadow(casts_shadow);

	bool show_billboard = light.getShowBillboard();
	if (ImGui::Checkbox("Show Billboard", &show_billboard))
		light.setShowBillboard(show_billboard);

	const char* celestial_types[] = { "Moon", "Sun" };
	int ct = static_cast<int>(light.getCelestialType());
	if (ImGui::Combo("Celestial Type", &ct, celestial_types, 2))
		light.setCelestialType(static_cast<CelestialType>(ct));
}

void InspectorPanel::renderRigidbody(RigidbodyComponent& rb, EditorState& state) {
	constexpr float label_w = 110.0f;

	const char* motion_types[] = {"Static", "Kinematic", "Dynamic"};
	int mt = static_cast<int>(rb.getMotionType());
	labeledWidget(label_w, "Motion Type", [&]() {
		if (ImGui::Combo("##MotionType", &mt, motion_types, 3)) {
			rb.setMotionType(static_cast<PhysicsMotionType>(mt));
			if (mt != 0 && rb.getShapeDesc().type == PhysicsShapeType::MeshStatic) {
				PhysicsShapeDesc d = rb.getShapeDesc();
				d.type = PhysicsShapeType::Box;
				rb.setShapeDesc(d);
			}
		}
	});

	const char* shape_types[] = {"Box", "Sphere", "Capsule", "Convex Hull", "Mesh (Static)"};
	PhysicsShapeDesc desc = rb.getShapeDesc();
	int st = static_cast<int>(desc.type);
	labeledWidget(label_w, "Shape Type", [&]() {
		if (ImGui::Combo("##ShapeType", &st, shape_types, 5)) {
			desc.type = static_cast<PhysicsShapeType>(st);
			rb.setShapeDesc(desc);
		}
	});

	if (desc.type == PhysicsShapeType::MeshStatic)
		ImGui::TextDisabled("Uses mesh geometry");

	if (desc.type == PhysicsShapeType::ConvexHull) {
		float hull_tol = rb.getHullTolerance();
		labeledWidget(label_w, "Hull Tolerance", [&]() {
			if (ImGui::SliderFloat("##HullTolerance", &hull_tol, 0.0f, 0.5f, "%.3f"))
				rb.setHullTolerance(hull_tol);
		});
	}

	if (rb.getMotionType() == PhysicsMotionType::Dynamic) {
		float mass = rb.getMass();
		labeledWidget(label_w, "Mass", [&]() {
			if (ImGui::DragFloat("##Mass", &mass, 0.1f, 0.001f, 100000.0f, "%.2f"))
				rb.setMass(std::max(mass, 0.001f));
		});
	}

	float friction = rb.getFriction();
	labeledWidget(label_w, "Friction", [&]() {
		if (ImGui::SliderFloat("##Friction", &friction, 0.0f, 1.0f, "%.2f"))
			rb.setFriction(friction);
	});

	float restitution = rb.getRestitution();
	labeledWidget(label_w, "Restitution", [&]() {
		if (ImGui::SliderFloat("##Restitution", &restitution, 0.0f, 1.0f, "%.2f"))
			rb.setRestitution(restitution);
	});

	if (rb.hasBody()) {
		ImGui::TextDisabled("Body ID: %u", rb.getBodyId());
		ImGui::Checkbox("Show Collision Shape", &state.show_collision_shape);
	} else {
		ImGui::TextDisabled("No physics body");
	}
}

void InspectorPanel::renderAnimator(AnimatorComponent& animator) {
	const auto& clips = animator.getClipBindings();
	const size_t total = clips.size();

	size_t playing_count = 0;
	for (const auto& b : clips)
		if (b.clip && b.playing)
			playing_count++;

	ImGui::Text("Clips: %zu  Playing: %zu", total, playing_count);

	if (ImGui::Button("Play All"))
		animator.playAll();
	ImGui::SameLine();
	if (ImGui::Button("Pause All"))
		animator.pauseAll();
	ImGui::SameLine();
	if (ImGui::Button("Stop All"))
		animator.stopAll();

	char filter_buf[128];
	std::snprintf(filter_buf, sizeof(filter_buf), "%s", m_animation_filter.c_str());
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::InputTextWithHint("##anim_filter", "Filter by name...", filter_buf, sizeof(filter_buf)))
		m_animation_filter = filter_buf;

	ImGui::Checkbox("Playing only", &m_animation_playing_only);

	std::string filter_lower = ve::toLower(m_animation_filter);

	auto matches_filter = [&](const VeAnimationClip& clip) {
		if (filter_lower.empty())
			return true;
		return ve::toLower(clip.name).find(filter_lower) != std::string::npos;
	};

	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, 320.0f));
	if (ImGui::BeginChild("anim_clip_list", ImVec2(0, 0),
	                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
		size_t visible_count = 0;
		for (uint32_t i = 0; i < static_cast<uint32_t>(clips.size()); i++) {
			const auto& binding = clips[i];
			if (!binding.clip)
				continue;
			if (m_animation_playing_only && !binding.playing)
				continue;
			if (!matches_filter(*binding.clip))
				continue;

			visible_count++;
			ImGui::PushID(static_cast<int>(i));

			bool playing = binding.playing;
			if (ImGui::Checkbox("##playing", &playing)) {
				if (playing)
					animator.play(i);
				else
					animator.pause(i);
			}
			ImGui::SameLine();

			const char* clip_name = binding.clip->name.empty() ? "(unnamed)" : binding.clip->name.c_str();
			char header_label[160];
			std::snprintf(header_label, sizeof(header_label), "[%u] %s", i, clip_name);
			bool open = ImGui::TreeNodeEx(header_label, ImGuiTreeNodeFlags_SpanAvailWidth);
			ImGui::SameLine();
			ImGui::TextDisabled("%.2fs", binding.clip->duration);

			if (open) {
				ImGui::TextDisabled("Channels: %zu", binding.clip->channels.size());

				float time = binding.current_time;
				if (ImGui::SliderFloat("Time", &time, 0.0f, binding.clip->duration, "%.2fs"))
					animator.setTime(i, time);

				float speed = binding.speed;
				if (ImGui::DragFloat("Speed", &speed, 0.01f, -10.0f, 10.0f, "%.2f"))
					animator.setSpeed(i, speed);

				bool loop = binding.loop;
				if (ImGui::Checkbox("Loop", &loop))
					animator.setLoop(i, loop);

				ImGui::SameLine();
				if (ImGui::Button("Reset"))
					animator.stop(i);

				ImGui::TreePop();
			}

			ImGui::PopID();
		}

		if (visible_count == 0)
			ImGui::TextDisabled("(no clips match)");
	}
	ImGui::EndChild();
}

void InspectorPanel::renderSkin(Registry& registry, SkinComponent& skin, EditorState& state) {
	const auto& joints = skin.getJointEntities();
	const auto& ibms = skin.getInverseBindMatrices();
	ImGui::Text("Joints: %zu  IBMs: %zu", joints.size(), ibms.size());

	Entity skel_root = skin.getSkeletonRoot();
	if (!skel_root.isNull() && registry.isAlive(skel_root))
		ImGui::TextDisabled("Skeleton root: %s", registry.getName(skel_root).c_str());
	else
		ImGui::TextDisabled("Skeleton root: (unset)");

	if (ImGui::TreeNodeEx("Joint list", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (size_t j = 0; j < joints.size(); j++) {
			Entity je = joints[j];
			char label[128];
			if (!je.isNull() && registry.isAlive(je)) {
				const std::string& nm = registry.getName(je);
				snprintf(label, sizeof(label), "[%zu] %s", j, nm.empty() ? "(unnamed)" : nm.c_str());
			} else {
				snprintf(label, sizeof(label), "[%zu] (missing)", j);
			}
			ImGui::PushID(static_cast<int>(j));
			if (ImGui::Selectable(label) && !je.isNull() && registry.isAlive(je))
				state.selectSingle(je);
			ImGui::PopID();
		}
		ImGui::TreePop();
	}
}

void InspectorPanel::renderCamera(CameraComponent& camera) {
	constexpr float label_w = 110.0f;

	int proj = static_cast<int>(camera.getProjection());
	labeledWidget(label_w, "Projection", [&]() {
		const char* items[] = {"Perspective", "Orthographic"};
		if (ImGui::Combo("##Projection", &proj, items, 2))
			camera.setProjection(static_cast<CameraComponent::ProjectionType>(proj));
	});

	if (camera.getProjection() == CameraComponent::ProjectionType::Perspective) {
		float fov_deg = glm::degrees(camera.getFovY());
		labeledWidget(label_w, "FOV (deg)", [&]() {
			if (ImGui::SliderFloat("##FOV", &fov_deg, 1.0f, 170.0f, "%.1f"))
				camera.setFovY(glm::radians(fov_deg));
		}, [&]() { camera.setFovY(glm::radians(55.0f)); });
	} else {
		float ortho = camera.getOrthoSize();
		labeledWidget(label_w, "Ortho Size", [&]() {
			if (ImGui::DragFloat("##Ortho", &ortho, 0.1f, 0.01f, 10000.0f, "%.2f"))
				camera.setOrthoSize(ortho);
		}, [&]() { camera.setOrthoSize(10.0f); });
	}

	float near_p = camera.getNear();
	labeledWidget(label_w, "Near", [&]() {
		if (ImGui::DragFloat("##Near", &near_p, 0.01f, 0.001f, 100.0f, "%.3f"))
			camera.setNear(near_p);
	}, [&]() { camera.setNear(0.1f); });

	float far_p = camera.getFar();
	labeledWidget(label_w, "Far", [&]() {
		if (ImGui::DragFloat("##Far", &far_p, 1.0f, 0.1f, 1000000.0f, "%.1f"))
			camera.setFar(far_p);
	}, [&]() { camera.setFar(1000.0f); });

	bool active = camera.isActive();
	if (ImGui::Checkbox("Active##Camera", &active))
		camera.setActive(active);

	int priority = camera.getPriority();
	labeledWidget(label_w, "Priority", [&]() {
		if (ImGui::DragInt("##Priority", &priority, 1.0f, -1000, 1000))
			camera.setPriority(priority);
	}, [&]() { camera.setPriority(0); });
}

void InspectorPanel::renderParticleEmitter(ParticleEmitterComponent& emitter) {
	EmitterParams& p = emitter.params;
	constexpr float label_w = 110.0f;

	bool active = emitter.isActive();
	if (ImGui::Checkbox("Active##ParticleEmitter", &active))
		emitter.setActive(active);

	if (ImGui::TreeNodeEx(ICON_EMISSION "  Emission", ImGuiTreeNodeFlags_DefaultOpen)) {
		labeledWidget(label_w, "Rate (p/s)", [&]() {
			ImGui::DragFloat("##Rate", &emitter.rate, 1.0f, 0.0f, 10000.0f, "%.1f",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { emitter.rate = 0.0f; });

		int bc = static_cast<int>(emitter.burst_count);
		labeledWidget(label_w, "Burst size", [&]() {
			if (ImGui::DragInt("##BurstCount", &bc, 1.0f, 0, 10000, "%d", ImGuiSliderFlags_AlwaysClamp))
				emitter.burst_count = static_cast<uint32_t>(std::max(0, bc));
		}, [&]() { emitter.burst_count = 0u; });

		labeledWidget(label_w, "Burst interval", [&]() {
			ImGui::DragFloat("##BurstPeriod", &emitter.burst_period, 0.01f, 0.0f, 60.0f, "%.2f s",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { emitter.burst_period = 0.0f; });

		if (emitter.burst_count == 0u || emitter.burst_period <= 0.0f)
			ImGui::TextDisabled("Bursts disabled (set size and interval > 0).");
		else
			ImGui::TextDisabled("Fires %u particle%s every %.2fs.",
				emitter.burst_count, emitter.burst_count == 1u ? "" : "s", emitter.burst_period);
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Particle", ImGuiTreeNodeFlags_DefaultOpen)) {
		labeledWidget(label_w, "Size", [&]() {
			ImGui::DragFloat("##Size", &emitter.scale, 0.01f, 0.0f, 1000.0f, "%.3f",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { emitter.scale = 1.0f; });

		labeledWidget(label_w, "Color start", [&]() {
			ImGui::ColorEdit4("##ColorStart", &p.color_start.r);
		}, [&]() { p.color_start = glm::vec4(1.0f); });

		labeledWidget(label_w, "Color end", [&]() {
			ImGui::ColorEdit4("##ColorEnd", &p.color_end.r);
		}, [&]() { p.color_end = glm::vec4(1.0f); });

		labeledWidget(label_w, "Brightness", [&]() {
			ImGui::DragFloat("##Brightness", &p.brightness, 0.05f, 0.0f, 100.0f, "%.2fx",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { p.brightness = 1.0f; });

		ImGui::TextDisabled("Lerps start (life=max) -> end (life=0); brightness scales rgb.");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Lifetime", ImGuiTreeNodeFlags_DefaultOpen)) {
		labeledWidget(label_w, "Life range (s)", [&]() {
			ImGui::DragFloatRange2("##LifeRange", &p.min_life, &p.max_life, 0.1f, 0.01f, 1000.0f,
				"%.2f", nullptr, ImGuiSliderFlags_AlwaysClamp);
		});
		labeledWidget(label_w, "Velocity mean", [&]() {
			ImGui::DragFloat("##VelMean", &p.mean, 0.1f, -100.0f, 100.0f, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { p.mean = 0.0f; });
		labeledWidget(label_w, "Velocity stddev", [&]() {
			ImGui::DragFloat("##VelStddev", &p.stddev, 0.1f, 0.0f, 100.0f, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { p.stddev = 5.0f; });
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Forces", ImGuiTreeNodeFlags_DefaultOpen)) {
		labeledWidget(label_w, "Gravity", [&]() {
			ImGui::DragFloat("##Gravity", &p.gravity, 0.1f, -100.0f, 100.0f, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { p.gravity = 9.81f; });
		labeledWidget(label_w, "Drag", [&]() {
			ImGui::DragFloat("##Drag", &p.drag, 0.01f, 0.0f, 10.0f, "%.3f",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { p.drag = 0.0f; });
		labeledWidget(label_w, "Central", [&]() {
			ImGui::DragFloat("##Central", &p.central_attractor, 0.1f, -100.0f, 100.0f, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { p.central_attractor = 0.0f; });
		labeledWidget(label_w, "Tangential", [&]() {
			ImGui::DragFloat("##Tangential", &p.tangential_strength, 0.1f, -100.0f, 100.0f, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { p.tangential_strength = 0.0f; });

		bool floor_enabled = p.floor_z > FLOOR_DISABLED_THRESHOLD;
		labeledWidget(label_w, "Floor Z", [&]() {
			ImGui::PushID("FloorEnable");
			if (ImGui::Checkbox("##floor_en", &floor_enabled))
				p.floor_z = floor_enabled ? 0.0f : FLOOR_DISABLED;
			ImGui::PopID();
			ImGui::SameLine();
			if (floor_enabled) {
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::DragFloat("##FloorZ", &p.floor_z, 0.1f, -1000.0f, 1000.0f, "%.2f",
					ImGuiSliderFlags_AlwaysClamp);
			} else {
				ImGui::TextDisabled("(disabled)");
			}
		});

		labeledWidget(label_w, "Wind dir", [&]() {
			if (ImGui::DragFloat3("##WindDir", &p.wind.x, 0.05f, -1.0f, 1.0f, "%.2f")) {
				float len = glm::length(glm::vec3(p.wind));
				if (len > 1e-4f) {
					glm::vec3 n = glm::vec3(p.wind) / len;
					p.wind.x = n.x; p.wind.y = n.y; p.wind.z = n.z;
				}
			}
		});
		labeledWidget(label_w, "Wind strength", [&]() {
			ImGui::DragFloat("##WindStr", &p.wind.w, 0.1f, 0.0f, 100.0f, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
		}, [&]() { p.wind.w = 0.0f; });
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Atlas (sprite sheet)")) {
		if (emitter.texture.isValid() && emitter.texture.get()) {
			if (ImGui::BeginTable("##EmitterTex", 3, ImGuiTableFlags_SizingFixedFit)) {
				ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 52.0f);
				ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
				renderTextureSlot("Sprite", emitter.texture.getId(), emitter.texture.get());
				ImGui::EndTable();
			}
		} else {
			ImGui::TextDisabled("No atlas bound (procedural round mask).");
		}

		ImGui::TextDisabled("Bindless slot: %u", p.atlas_index);

		int rows = static_cast<int>(p.row_count);
		labeledWidget(label_w, "Rows", [&]() {
			if (ImGui::DragInt("##Rows", &rows, 1.0f, 1, 32, "%d", ImGuiSliderFlags_AlwaysClamp))
				p.row_count = static_cast<uint32_t>(std::max(1, rows));
		}, [&]() { p.row_count = 8u; });

		bool one_shot = p.atlas_one_shot != 0u;
		labeledWidget(label_w, "One-shot", [&]() {
			if (ImGui::Checkbox("##OneShot", &one_shot))
				p.atlas_one_shot = one_shot ? 1u : 0u;
		});
		ImGui::TreePop();
	}
}

} // namespace ve
