/* Entity editing shared by the viewport and the hierarchy: the context menu, its
 * keyboard shortcuts, and applying the EntityRequests they queue on EditorState.
 */
#pragma once
#include "ui/editor_state.hpp"

namespace ve {

class Registry;

// Items of the entity context menu, for use inside an open popup. Acts on the whole
// selection when `clicked` is part of it, on `clicked` alone otherwise.
void entityContextMenuItems(Registry& registry, EditorState& state, Entity clicked);

// Delete / Backspace, X (asks first), Ctrl+D, Ctrl+G and F on the selection, unless a text
// field has the keyboard
void queueEntityShortcuts(EditorState& state);

// The confirm X opens. Call every frame at the same ID stack level as queueEntityShortcuts.
void entityDeleteConfirm(Registry& registry, EditorState& state);

// Every action but Focus, which needs the camera and stays with the caller
void applyEntityRequest(Registry& registry, EditorState& state, const EntityRequest& request);

// Possession, with the follow camera taken over when state.follow_possessed_camera is set
void possessEntity(Registry& registry, EditorState& state, Entity entity);
void unpossessEntity(Registry& registry, EditorState& state);
// The follow camera targeting `target`, created when none exists, snapped behind it
Entity acquireFollowCamera(Registry& registry, Entity target);
bool isFollowCameraOf(Registry& registry, Entity camera, Entity target);

} // namespace ve
