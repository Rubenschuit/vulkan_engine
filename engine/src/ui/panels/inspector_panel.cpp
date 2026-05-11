#include "pch.hpp"
#include "ui/panels/inspector_panel.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/texture_inspector.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_texture.hpp"
#include "resources/ve_material_properties.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>

namespace ve {

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
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
	if (ImGui::Button("X", btn_size))
		{ values.x = reset_value; changed = true; }
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(drag_width);
	if (ImGui::DragFloat("##X", &values.x, speed, 0.0f, 0.0f, fmt))
		changed = true;
	ImGui::SameLine();

	// Y
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.6f, 0.15f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.75f, 0.2f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.6f, 0.15f, 1.0f));
	if (ImGui::Button("Y", btn_size))
		{ values.y = reset_value; changed = true; }
	ImGui::PopStyleColor(3);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(drag_width);
	if (ImGui::DragFloat("##Y", &values.y, speed, 0.0f, 0.0f, fmt))
		changed = true;
	ImGui::SameLine();

	// Z
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.7f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.85f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.7f, 1.0f));
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

// Two-column labeled widget with optional reset button (variadic resetFn)
template <typename WidgetFn, typename... ResetFn>
static void labeledWidget(float label_w, const char* text, WidgetFn widgetFn, ResetFn... resetFn) {
	ImGui::Columns(2, nullptr, false);
	ImGui::SetColumnWidth(0, label_w);
	ImGui::AlignTextToFramePadding();
	ImGui::Text("%s", text);
	if constexpr (sizeof...(resetFn) > 0) {
		ImGui::SameLine();
		ImGui::PushID(text);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 0));
		if (ImGui::SmallButton("*"))
			(resetFn(), ...);
		ImGui::PopStyleVar();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Reset to default");
		ImGui::PopID();
	}
	ImGui::NextColumn();
	ImGui::SetNextItemWidth(-FLT_MIN);
	widgetFn();
	ImGui::Columns(1);
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

	if (state.selected_entity.isNull() || !registry || !registry->isAlive(state.selected_entity)) {
		ImGui::TextDisabled("No entity selected");
		ImGui::End();
		return;
	}

	Entity entity = state.selected_entity;

	renderEntityHeader(*registry, entity, state);
	ImGui::Separator();

	// Transform (no remove button)
	auto* transform = registry->getComponent<TransformComponent>(entity);
	if (transform) {
		bool open = ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
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
		bool open = ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
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
		bool open = ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
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
		bool open = ImGui::CollapsingHeader("Spot Light", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
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

	// Directional Light
	if (registry->hasComponent<DirectionalLightComponent>(entity)) {
		bool open = ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
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
		bool open = ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
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
		bool open = ImGui::CollapsingHeader("Animator", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (open && registry->hasComponent<AnimatorComponent>(entity))
			renderAnimator(*registry->getComponent<AnimatorComponent>(entity));
	}

	// Skin
	if (registry->hasComponent<SkinComponent>(entity)) {
		auto* sc = registry->getComponent<SkinComponent>(entity);
		char header[64];
		snprintf(header, sizeof(header), "Skin (%zu joints)", sc->jointCount());
		bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (open && registry->hasComponent<SkinComponent>(entity))
			renderSkin(*registry, *registry->getComponent<SkinComponent>(entity), state);
	}

	// Camera
	if (registry->hasComponent<CameraComponent>(entity)) {
		bool open = ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
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

	// Add Component
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	float button_width = ImGui::GetContentRegionAvail().x;
	if (ImGui::Button("Add Component", ImVec2(button_width, 0)))
		ImGui::OpenPopup("AddComponentPopup");

	if (ImGui::BeginPopup("AddComponentPopup")) {
		if (!registry->hasComponent<PointLightComponent>(entity))
			if (ImGui::MenuItem("Point Light"))
				registry->addComponent<PointLightComponent>(entity);

		if (!registry->hasComponent<SpotLightComponent>(entity))
			if (ImGui::MenuItem("Spot Light"))
				registry->addComponent<SpotLightComponent>(entity);

		if (!registry->hasComponent<DirectionalLightComponent>(entity))
			if (ImGui::MenuItem("Directional Light"))
				registry->addComponent<DirectionalLightComponent>(entity);

		if (!registry->hasComponent<RigidbodyComponent>(entity))
			if (ImGui::MenuItem("Rigidbody"))
				registry->addComponent<RigidbodyComponent>(entity);

		if (!registry->hasComponent<CameraComponent>(entity))
			if (ImGui::MenuItem("Camera"))
				registry->addComponent<CameraComponent>(entity);

		if (!registry->hasComponent<MeshComponent>(entity)) {
			ImGui::BeginDisabled(true);
			ImGui::MenuItem("Mesh (load model first)");
			ImGui::EndDisabled();
		}

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
	if (ImGui::Button("^##select_parent", ImVec2(btn_width, 0))) {
		state.selected_entity = current_parent;
		state.selection_changed = true;
	}
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

	// Shadow toggle
	if (ImGui::Checkbox("Cast Shadow", &mesh.has_shadow))
		if (mesh.getRegistry())
			mesh.getRegistry()->events().emit(MeshDataChangedEvent{mesh.getEntity()});

	// Material editing
	VeMaterial* mat = mesh.getMaterial();
	if (!mat) {
		ImGui::TextDisabled("No material");
		return;
	}

	if (!ImGui::TreeNode("Material"))
		return;

	ImGui::TextDisabled("%s", mat->getId().c_str());

	constexpr float mat_label_w = 85.0f;

	auto factors = mat->getMaterialFactors();
	MaterialFactors defaults;
	bool changed = false;

	labeledWidget(mat_label_w, "Base Color", [&]() {
		if (ImGui::ColorEdit4("##BaseColor", glm::value_ptr(factors.base_color_factor)))
			changed = true;
	}, [&]() { factors.base_color_factor = defaults.base_color_factor; changed = true; });

	labeledWidget(mat_label_w, "Metallic", [&]() {
		if (ImGui::SliderFloat("##Metallic", &factors.metallic_factor, 0.0f, 1.0f))
			changed = true;
	}, [&]() { factors.metallic_factor = defaults.metallic_factor; changed = true; });

	labeledWidget(mat_label_w, "Roughness", [&]() {
		if (ImGui::SliderFloat("##Roughness", &factors.roughness_factor, 0.0f, 1.0f))
			changed = true;
	}, [&]() { factors.roughness_factor = defaults.roughness_factor; changed = true; });

	labeledWidget(mat_label_w, "Emissive", [&]() {
		if (ImGui::ColorEdit3("##Emissive", glm::value_ptr(factors.emissive_factor)))
			changed = true;
	}, [&]() { factors.emissive_factor = defaults.emissive_factor; changed = true; });

	labeledWidget(mat_label_w, "Emissive Str", [&]() {
		if (ImGui::DragFloat("##EmissiveStr", &factors.emissive_strength, 0.001f, 0.0f, 100.0f))
			changed = true;
	}, [&]() { factors.emissive_strength = defaults.emissive_strength; changed = true; });

	labeledWidget(mat_label_w, "Transmission", [&]() {
		if (ImGui::SliderFloat("##Transmission", &factors.transmission_factor, 0.0f, 1.0f))
			changed = true;
	}, [&]() { factors.transmission_factor = defaults.transmission_factor; changed = true; });

	labeledWidget(mat_label_w, "IOR", [&]() {
		if (ImGui::DragFloat("##IOR", &factors.ior, 0.01f, 1.0f, 3.0f, "%.2f"))
			changed = true;
	}, [&]() { factors.ior = defaults.ior; changed = true; });

	labeledWidget(mat_label_w, "Specular F0", [&]() {
		if (ImGui::ColorEdit3("##SpecularF0", glm::value_ptr(factors.specular_factor),
				ImGuiColorEditFlags_Float))
			changed = true;
	}, [&]() { factors.specular_factor = defaults.specular_factor; changed = true; });

	if (changed) {
		mat->setMaterialFactors(factors);
		if (mesh.getRegistry())
			mesh.getRegistry()->events().emit(MeshDataChangedEvent{mesh.getEntity()});
	}

	// Alpha properties
	auto alpha = mat->getAlphaProps();
	bool alpha_changed = false;

	const char* alpha_modes[] = { "Opaque", "Mask", "Blend" };
	int alpha_idx = static_cast<int>(alpha.alpha_mode);
	labeledWidget(mat_label_w, "Alpha Mode", [&]() {
		if (ImGui::Combo("##AlphaMode", &alpha_idx, alpha_modes, 3)) {
			alpha.alpha_mode = static_cast<AlphaMode>(alpha_idx);
			alpha_changed = true;
		}
	});

	if (alpha.alpha_mode == AlphaMode::MASK) {
		labeledWidget(mat_label_w, "Alpha Cutoff", [&]() {
			if (ImGui::SliderFloat("##AlphaCutoff", &alpha.alpha_cutoff, 0.0f, 1.0f))
				alpha_changed = true;
		});
	}

	if (ImGui::Checkbox("Double Sided", &alpha.double_sided))
		alpha_changed = true;

	if (alpha_changed) {
		mat->setAlphaProps(alpha);
		if (mesh.getRegistry())
			mesh.getRegistry()->events().emit(MeshDataChangedEvent{mesh.getEntity()});
	}

	// Texture thumbnails
	if (ImGui::TreeNode("Textures")) {
		if (ImGui::BeginTable("##TexTable", 3, ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 52.0f);
			ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
			renderTextureSlot("Albedo",    mat->getAlbedoTexture().getId(), mat->getAlbedoTexture().get());
			renderTextureSlot("Normal",    mat->getNormalTexture().getId(), mat->getNormalTexture().get());
			renderTextureSlot("Metal/Rgh", mat->getMetallicRoughnessTexture().getId(), mat->getMetallicRoughnessTexture().get());
			renderTextureSlot("Occlusion", mat->getOcclusionTexture().getId(), mat->getOcclusionTexture().get());
			renderTextureSlot("Emissive",  mat->getEmissiveTexture().getId(), mat->getEmissiveTexture().get());
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
	ImGui::Text("Clips: %zu", clips.size());

	for (uint32_t i = 0; i < static_cast<uint32_t>(clips.size()); i++) {
		const auto& binding = clips[i];
		if (!binding.clip)
			continue;

		ImGui::PushID(static_cast<int>(i));

		const char* clip_name = binding.clip->name.empty() ? "Unnamed" : binding.clip->name.c_str();
		if (ImGui::TreeNodeEx(clip_name, ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::TextDisabled("Duration: %.2fs  Channels: %zu", binding.clip->duration, binding.clip->channels.size());

			float time = binding.current_time;
			if (ImGui::SliderFloat("Time", &time, 0.0f, binding.clip->duration, "%.2fs"))
				animator.setTime(i, time);

			float speed = binding.speed;
			if (ImGui::DragFloat("Speed", &speed, 0.01f, -10.0f, 10.0f, "%.2f"))
				animator.setSpeed(i, speed);

			bool playing = binding.playing;
			if (ImGui::Checkbox("Playing", &playing)) {
				if (playing)
					animator.play(i);
				else
					animator.pause(i);
			}

			ImGui::SameLine();
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
			if (ImGui::Selectable(label) && !je.isNull() && registry.isAlive(je)) {
				state.selected_entity = je;
				state.selection_changed = true;
			}
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

} // namespace ve
