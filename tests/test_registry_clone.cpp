#include <catch2/catch_test_macros.hpp>
#include <scene/ve_entity.hpp>
#include <scene/ve_registry.hpp>
#include <scene/ve_component.hpp>
#include <glm/gtc/matrix_transform.hpp>

using ve::Entity;
using ve::Registry;
using ve::TransformComponent;
using ve::AnimatorComponent;
using ve::SkinComponent;

// ── SkinComponent remap ─────────────────────────────────────────────────────

TEST_CASE("cloneEntityRecursive remaps SkinComponent joints and skeleton root",
          "[ecs][registry][clone][skin]") {
	Registry reg;

	Entity root = reg.createEntity("root");
	reg.addComponent<TransformComponent>(root);

	Entity joint_a = reg.createEntity("joint_a");
	reg.addComponent<TransformComponent>(joint_a);
	reg.setParent(joint_a, root);

	Entity joint_b = reg.createEntity("joint_b");
	reg.addComponent<TransformComponent>(joint_b);
	reg.setParent(joint_b, joint_a);

	Entity skinned = reg.createEntity("skinned");
	reg.addComponent<TransformComponent>(skinned);
	reg.setParent(skinned, root);
	auto& skin = reg.addComponent<SkinComponent>(skinned);
	skin.setJointEntities({joint_a, joint_b});
	skin.setSkeletonRoot(root);
	skin.setInverseBindMatrices({glm::mat4(1.0f), glm::mat4(1.0f)});
	skin.setPaletteOffset(42); // stale cache that must be cleared by remap

	Entity root_clone = reg.cloneEntityRecursive(root);
	REQUIRE(!root_clone.isNull());
	REQUIRE(root_clone != root);

	// Find cloned children by walking the cloned hierarchy
	Entity clone_skinned = Entity::null();
	Entity clone_joint_a = Entity::null();
	for (Entity c = reg.firstChild(root_clone); !c.isNull(); c = reg.nextSibling(c)) {
		if (reg.hasComponent<SkinComponent>(c))
			clone_skinned = c;
		else
			clone_joint_a = c;
	}
	REQUIRE(!clone_skinned.isNull());
	REQUIRE(!clone_joint_a.isNull());
	Entity clone_joint_b = reg.firstChild(clone_joint_a);
	REQUIRE(!clone_joint_b.isNull());

	const auto* cloned_skin = reg.getComponent<SkinComponent>(clone_skinned);
	REQUIRE(cloned_skin != nullptr);

	const auto& cloned_joints = cloned_skin->getJointEntities();
	REQUIRE(cloned_joints.size() == 2);
	REQUIRE(cloned_joints[0] == clone_joint_a);
	REQUIRE(cloned_joints[1] == clone_joint_b);
	REQUIRE(cloned_joints[0] != joint_a);
	REQUIRE(cloned_joints[1] != joint_b);

	REQUIRE(cloned_skin->getSkeletonRoot() == root_clone);
	REQUIRE(cloned_skin->getPaletteOffset() == 0);
}

// ── AnimatorComponent remap (root) ──────────────────────────────────────────

TEST_CASE("cloneEntityRecursive remaps AnimatorComponent on root clone",
          "[ecs][registry][clone][animator]") {
	Registry reg;

	Entity root = reg.createEntity("wrapper");
	reg.addComponent<TransformComponent>(root);

	Entity child0 = reg.createEntity("child0");
	reg.addComponent<TransformComponent>(child0);
	reg.setParent(child0, root);

	Entity child1 = reg.createEntity("child1");
	reg.addComponent<TransformComponent>(child1);
	reg.setParent(child1, root);

	auto& anim = reg.addComponent<AnimatorComponent>(root);
	anim.setNodeToEntityMap({root, child0, child1});

	Entity root_clone = reg.cloneEntityRecursive(root);
	REQUIRE(!root_clone.isNull());

	Entity clone_c0 = reg.firstChild(root_clone);
	REQUIRE(!clone_c0.isNull());
	Entity clone_c1 = reg.nextSibling(clone_c0);
	REQUIRE(!clone_c1.isNull());

	const auto* cloned_anim = reg.getComponent<AnimatorComponent>(root_clone);
	REQUIRE(cloned_anim != nullptr);

	const auto& map = cloned_anim->getNodeToEntityMap();
	REQUIRE(map.size() == 3);
	REQUIRE(map[0] == root_clone);
	// Children may be in any sibling order; check via set membership
	REQUIRE((map[1] == clone_c0 || map[1] == clone_c1));
	REQUIRE((map[2] == clone_c0 || map[2] == clone_c1));
	REQUIRE(map[1] != map[2]);
	// None of the cloned references should equal the source entities
	for (Entity e : map) {
		REQUIRE(e != root);
		REQUIRE(e != child0);
		REQUIRE(e != child1);
	}
}

// ── AnimatorComponent remap (non-root descendant) ───────────────────────────

TEST_CASE("cloneEntityRecursive remaps AnimatorComponent on non-root descendant",
          "[ecs][registry][clone][animator]") {
	Registry reg;

	Entity container = reg.createEntity("container");
	reg.addComponent<TransformComponent>(container);

	Entity wrapper = reg.createEntity("wrapper");
	reg.addComponent<TransformComponent>(wrapper);
	reg.setParent(wrapper, container);

	Entity bone = reg.createEntity("bone");
	reg.addComponent<TransformComponent>(bone);
	reg.setParent(bone, wrapper);

	auto& anim = reg.addComponent<AnimatorComponent>(wrapper);
	anim.setNodeToEntityMap({wrapper, bone});

	Entity container_clone = reg.cloneEntityRecursive(container);
	REQUIRE(!container_clone.isNull());

	Entity clone_wrapper = reg.firstChild(container_clone);
	REQUIRE(!clone_wrapper.isNull());
	Entity clone_bone = reg.firstChild(clone_wrapper);
	REQUIRE(!clone_bone.isNull());

	const auto* cloned_anim = reg.getComponent<AnimatorComponent>(clone_wrapper);
	REQUIRE(cloned_anim != nullptr);

	const auto& map = cloned_anim->getNodeToEntityMap();
	REQUIRE(map.size() == 2);
	REQUIRE(map[0] == clone_wrapper);
	REQUIRE(map[1] == clone_bone);
	REQUIRE(map[0] != wrapper);
	REQUIRE(map[1] != bone);
}

// ── Independence after clone ────────────────────────────────────────────────

TEST_CASE("cloneEntityRecursive yields a hierarchy independent of the source",
          "[ecs][registry][clone]") {
	Registry reg;

	Entity src_root = reg.createEntity("src");
	auto& src_tc = reg.addComponent<TransformComponent>(src_root);
	src_tc.setTranslation(glm::vec3(1.0f, 2.0f, 3.0f));

	Entity src_child = reg.createEntity("src_child");
	auto& src_child_tc = reg.addComponent<TransformComponent>(src_child);
	src_child_tc.setTranslation(glm::vec3(0.5f, 0.0f, 0.0f));
	reg.setParent(src_child, src_root);

	Entity clone_root = reg.cloneEntityRecursive(src_root);
	Entity clone_child = reg.firstChild(clone_root);
	REQUIRE(!clone_child.isNull());

	// Mutate source after the clone
	reg.getComponent<TransformComponent>(src_root)->setTranslation(glm::vec3(100.0f));
	reg.getComponent<TransformComponent>(src_child)->setTranslation(glm::vec3(50.0f));

	const auto* clone_root_tc = reg.getComponent<TransformComponent>(clone_root);
	const auto* clone_child_tc = reg.getComponent<TransformComponent>(clone_child);
	REQUIRE(clone_root_tc->getTranslation() == glm::vec3(1.0f, 2.0f, 3.0f));
	REQUIRE(clone_child_tc->getTranslation() == glm::vec3(0.5f, 0.0f, 0.0f));

	// World transform of clone child should reflect clone root's untouched translation
	glm::mat4 world = reg.getWorldTransform(clone_child);
	REQUIRE(world[3][0] == 1.5f);
	REQUIRE(world[3][1] == 2.0f);
	REQUIRE(world[3][2] == 3.0f);

	// Destroying source must not affect clone
	reg.destroyEntityRecursive(src_root);
	REQUIRE(!reg.isAlive(src_root));
	REQUIRE(reg.isAlive(clone_root));
	REQUIRE(reg.isAlive(clone_child));
}

// ── Out-of-subtree references are preserved ─────────────────────────────────

TEST_CASE("cloneEntityRecursive leaves out-of-subtree entity references untouched",
          "[ecs][registry][clone][animator]") {
	Registry reg;

	// External entity, not part of the cloned subtree
	Entity external = reg.createEntity("external");
	reg.addComponent<TransformComponent>(external);

	Entity root = reg.createEntity("root");
	reg.addComponent<TransformComponent>(root);

	Entity child = reg.createEntity("child");
	reg.addComponent<TransformComponent>(child);
	reg.setParent(child, root);

	auto& anim = reg.addComponent<AnimatorComponent>(root);
	anim.setNodeToEntityMap({root, child, external});

	Entity clone_root = reg.cloneEntityRecursive(root);
	Entity clone_child = reg.firstChild(clone_root);
	REQUIRE(!clone_child.isNull());

	const auto& map = reg.getComponent<AnimatorComponent>(clone_root)->getNodeToEntityMap();
	REQUIRE(map.size() == 3);
	REQUIRE(map[0] == clone_root);
	REQUIRE(map[1] == clone_child);
	REQUIRE(map[2] == external); // unchanged, since `external` was outside the cloned subtree
}