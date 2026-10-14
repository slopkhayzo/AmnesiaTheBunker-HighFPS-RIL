#include "world_runtime.h"

#include "hands_pose_interpolation.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using atb::hands_trace::Matrix;

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

Matrix At(const float x, const float scale = 1.0F) {
    Matrix matrix{};
    matrix.values[0] = scale;
    matrix.values[5] = scale;
    matrix.values[10] = scale;
    matrix.values[3] = x;
    matrix.values[15] = 1.0F;
    return matrix;
}

atb::clock::Sample Pair(const std::uint64_t older, const double alpha) {
    atb::clock::Sample sample{};
    sample.kind = atb::clock::SampleKind::interpolated;
    sample.generation = 1;
    sample.olderSerial = older;
    sample.newerSerial = older + 1;
    sample.alpha = alpha;
    return sample;
}

const Matrix* Render(const void* const key,
                     const Matrix& source,
                     const std::uint64_t serial,
                     const double alpha,
                     const bool substitute = true) {
    const atb::clock::Sample sample = Pair(serial - 1, alpha);
    atb::world::BeginRender(serial, &sample, substitute);
    const Matrix* const result = atb::world::Present(key, &source);
    atb::world::EndRender();
    return result;
}

} // namespace

int main() {
    int keyStorage[8] = {};
    const void* const key = &keyStorage[0];

    // Outside a render nothing is tracked or substituted.
    Matrix first = At(0.0F);
    Require(atb::world::Present(key, &first) == &first,
            "a call outside a render was not passed through");

    atb::world::Reset();
    Require(Render(key, first, 10, 0.5) == &first,
            "a submesh with no history was substituted");
    Matrix second = At(2.0F);
    const Matrix* presented = Render(key, second, 11, 0.5);
    Require(presented != &second &&
                std::abs(presented->values[3] - 1.0F) < 1.0e-5F,
            "a moving submesh was not interpolated at the midpoint");
    // Second render of the same tick reuses the pair with the new alpha.
    presented = Render(key, second, 11, 0.75);
    Require(presented != &second &&
                std::abs(presented->values[3] - 1.5F) < 1.0e-5F,
            "the stored pair was not reused within one tick");
    // The object comes to rest: the engine pointer is returned again.
    Require(Render(key, second, 12, 0.5) == &second,
            "a resting submesh was still substituted");
    atb::world::Counters counters = atb::world::TakeCounters();
    Require(counters.tracked == 4 && counters.moving == 2 &&
                counters.applied == 2 && counters.rejected == 0,
            "counters did not describe the simple moving case");

    // Update counts are perturbed only for a renderable that is currently
    // presented at an interpolated position, and differ on every render.
    {
        const void* const mover = &keyStorage[5];
        Matrix m0 = At(0.0F);
        Matrix m1 = At(1.0F);
        Require(Render(mover, m0, 60, 0.5) == &m0, "refresh setup failed");
        std::array<int, 3> seen{};
        for (std::size_t frame = 0; frame < seen.size(); ++frame) {
            const atb::clock::Sample sample = Pair(60, 0.25 * (frame + 1.0));
            atb::world::BeginRender(61, &sample, true);
            Require(atb::world::Present(mover, &m1) != &m1,
                    "refresh fixture was not interpolated");
            seen[frame] = atb::world::RefreshUpdateCount(mover, 7);
            Require(atb::world::RefreshUpdateCount(&keyStorage[6], 7) == 7,
                    "an untracked renderable had its update count changed");
            atb::world::EndRender();
        }
        Require(seen[0] != 7 && seen[0] != seen[1] && seen[1] != seen[2],
                "an interpolated renderable kept a stable update count");
        Require(atb::world::RefreshUpdateCount(mover, 7) == 7,
                "an update count was changed outside a render");
        // Two renders at rest: the count returns to the engine's own value.
        Require(Render(mover, m1, 62, 0.5) == &m1 &&
                    Render(mover, m1, 63, 0.5) == &m1,
                "refresh fixture did not come to rest");
        const atb::clock::Sample sample = Pair(63, 0.5);
        atb::world::BeginRender(64, &sample, true);
        Require(atb::world::RefreshUpdateCount(mover, 7) == 7,
                "a resting renderable still had its update count changed");
        atb::world::EndRender();
        // A renderable presented by another layer is refreshed the same way.
        const void* const external = &keyStorage[4];
        atb::world::BeginRender(65, &sample, true);
        Require(atb::world::RefreshUpdateCount(external, 9) == 9,
                "an unmarked renderable had its update count changed");
        atb::world::MarkPresented(external);
        Require(atb::world::RefreshUpdateCount(external, 9) != 9,
                "a marked renderable kept its update count");
        atb::world::EndRender();
        (void)atb::world::TakeCounters();
    }

    // A look lead turns moving objects near the view about the camera and
    // leaves everything farther away alone.
    {
        atb::world::Reset();
        atb::world::LeadTransform lead{};
        // Quarter turn about +Y: x -> -z, z -> x.
        lead.rotation[0] = 0.0F;
        lead.rotation[2] = 1.0F;
        lead.rotation[6] = -1.0F;
        lead.rotation[8] = 0.0F;
        lead.center[0] = 1.0F;
        lead.radius = 1.0F;
        const void* const nearKey = &keyStorage[3];
        const void* const farKey = &keyStorage[2];
        Matrix near0 = At(1.2F);
        Matrix near1 = At(1.6F);
        Matrix far0 = At(8.0F);
        Matrix far1 = At(9.0F);
        const atb::clock::Sample sample = Pair(49, 0.5);
        atb::world::BeginRender(49, &sample, true, &lead);
        (void)atb::world::Present(nearKey, &near0);
        (void)atb::world::Present(farKey, &far0);
        atb::world::EndRender();
        atb::world::BeginRender(50, &sample, true, &lead);
        const Matrix* const nearResult = atb::world::Present(nearKey, &near1);
        const Matrix* const farResult = atb::world::Present(farKey, &far1);
        atb::world::EndRender();
        // Interpolated x = 1.4, i.e. 0.4 from the centre along +x, which the
        // quarter turn carries to -z.
        Require(nearResult != &near1 &&
                    std::abs(nearResult->values[3] - 1.0F) < 1.0e-5F &&
                    std::abs(nearResult->values[11] + 0.4F) < 1.0e-5F &&
                    std::abs(nearResult->values[8] + 1.0F) < 1.0e-5F,
                "a near object did not follow the look lead");
        Require(farResult != &far1 &&
                    std::abs(farResult->values[3] - 8.5F) < 1.0e-5F &&
                    std::abs(farResult->values[11]) < 1.0e-5F,
                "a distant object was turned by the look lead");
        Require(atb::world::TakeCounters().led == 1,
                "the look lead was not counted once");
    }

    // A skipped tick has no adjacent pair.
    Matrix third = At(4.0F);
    Require(Render(key, third, 14, 0.5) == &third,
            "non-adjacent ticks were blended");

    // Trace mode builds history but never substitutes.
    Matrix fourth = At(5.0F);
    Require(Render(key, fourth, 15, 0.5, false) == &fourth,
            "trace mode substituted a matrix");
    counters = atb::world::TakeCounters();
    Require(counters.eligible == 1 && counters.applied == 0,
            "trace mode did not report the eligible interpolant");

    // A teleport-sized step fails closed.
    Matrix far = At(500.0F);
    Require(Render(key, far, 16, 0.5) == &far,
            "a discontinuous step was blended");
    Require(atb::world::TakeCounters().rejected == 1,
            "the discontinuity was not counted as rejected");

    // A matrix that changes without a fixed tick is render-driven: it is
    // quarantined even once it later moves across ticks.
    const void* const animated = &keyStorage[1];
    Matrix a0 = At(0.0F);
    Matrix a1 = At(0.1F);
    Matrix a2 = At(0.2F);
    Require(Render(animated, a0, 20, 0.2) == &a0 &&
                Render(animated, a1, 20, 0.7) == &a1 &&
                Render(animated, a2, 21, 0.2) == &a2,
            "a render-rate transform was substituted");
    counters = atb::world::TakeCounters();
    Require(counters.sameTickChanges == 1 && counters.applied == 0,
            "the same-tick change was not detected");

    // Differing matrices for one submesh within a render stop substitution.
    const void* const perView = &keyStorage[2];
    Matrix v0 = At(0.0F);
    Matrix v1 = At(1.0F);
    Matrix v1b = At(1.25F);
    Require(Render(perView, v0, 30, 0.5) == &v0, "per-view setup failed");
    {
        const atb::clock::Sample sample = Pair(30, 0.5);
        atb::world::BeginRender(31, &sample, true);
        Require(atb::world::Present(perView, &v1) != &v1,
                "per-view first call was not interpolated");
        Require(atb::world::Present(perView, &v1b) == &v1b,
                "a per-view variant kept the stored interpolant");
        atb::world::EndRender();
    }
    Require(atb::world::TakeCounters().frameVariants == 1,
            "the per-view variant was not counted");

    // Uniformly scaled objects interpolate and keep their scale.
    Matrix scaledResult{};
    Require(atb::world::InterpolateScaledRigid(At(0.0F, 2.5F), At(2.0F, 2.5F),
                                               0.5, scaledResult) &&
                std::abs(scaledResult.values[0] - 2.5F) < 1.0e-4F &&
                std::abs(scaledResult.values[3] - 1.0F) < 1.0e-5F,
            "a scaled rigid transform was not interpolated");
    Require(!atb::world::InterpolateScaledRigid(At(0.0F, 1.0F), At(0.0F, 1.5F),
                                                0.5, scaledResult),
            "a changing scale was treated as rigid motion");
    Matrix nonFinite = At(0.0F);
    nonFinite.values[7] = std::nanf("");
    Require(!atb::world::InterpolateScaledRigid(At(0.0F), nonFinite, 0.5,
                                                scaledResult),
            "a non-finite endpoint was accepted");

    // Skinned world meshes: palettes interpolate across adjacent ticks.
    {
        using atb::hands_pose::DualQuaternion;
        atb::world::Reset();
        atb::world::ConfigurePose(true);
        const void* const mesh = &keyStorage[7];
        std::vector<DualQuaternion> palette(208);
        const auto set = [&](const float translation) {
            for (auto& bone : palette) {
                bone.values = {0.0F, 0.0F, 0.0F, 1.0F,
                               translation * 0.5F, 0.0F, 0.0F, 0.0F};
            }
        };
        const auto present = [&](const std::uint64_t serial,
                                 const double alpha) {
            const atb::clock::Sample sample = Pair(serial - 1, alpha);
            atb::world::BeginRender(serial, &sample, true);
            const void* const result = atb::world::PresentPalette(
                mesh, palette.data(), palette.size());
            Require(atb::world::PresentPalette(mesh, palette.data(),
                                               palette.size()) == result,
                    "a second call in one render returned another palette");
            atb::world::EndRender();
            return static_cast<const DualQuaternion*>(result);
        };
        set(0.0F);
        Require(present(80, 0.5) == palette.data(),
                "a palette with no history was substituted");
        set(2.0F);
        const DualQuaternion* blended = present(81, 0.5);
        Require(blended != palette.data() &&
                    std::abs(blended[207].values[4] - 0.5F) < 1.0e-5F &&
                    std::abs(blended[0].values[3] - 1.0F) < 1.0e-5F,
                "an animated palette was not interpolated at the midpoint");
        blended = present(81, 0.75);
        Require(blended != palette.data() &&
                    std::abs(blended[0].values[4] - 0.75F) < 1.0e-5F,
                "the palette pair was not reused within one tick");
        Require(present(82, 0.5) == palette.data(),
                "a resting palette was still substituted");
        set(3.0F);
        Require(present(84, 0.5) == palette.data(),
                "palettes from non-adjacent ticks were blended");
        // A pose that jumps is a teleport or an animation cut.
        set(500.0F);
        Require(present(85, 0.5) == palette.data(),
                "a discontinuous palette was blended");
        atb::world::PoseCounters pose = atb::world::TakePoseCounters();
        Require(pose.applied == 2 && pose.changed == 3 &&
                    pose.rejected == 1 && pose.changedMaximumBones == 208 &&
                    pose.oversize == 0,
                "pose counters did not describe the animated case");

        // Real skeletons: unused zero bones and non-unit bones must not
        // sink the whole palette, and a single bone that cuts only snaps.
        atb::world::Reset();
        atb::world::ConfigurePose(true);
        set(0.0F);
        palette[5].values = {};
        for (float& value : palette[6].values) {
            value *= 3.0F;
        }
        Require(present(86, 0.5) == palette.data(), "mixed setup failed");
        set(2.0F);
        palette[5].values = {};
        for (float& value : palette[6].values) {
            value *= 3.0F;
        }
        palette[7].values = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
        blended = present(87, 0.5);
        Require(blended != palette.data(),
                "a palette with unused and scaled bones was rejected");
        Require(std::abs(blended[0].values[4] - 0.5F) < 1.0e-5F,
                "ordinary bones were not blended beside special ones");
        Require(blended[5].values[3] == 0.0F && blended[5].values[4] == 0.0F,
                "an unused bone was altered");
        Require(std::abs(blended[6].values[3] - 3.0F) < 1.0e-4F &&
                    std::abs(blended[6].values[4] - 1.5F) < 1.0e-4F,
                "a non-unit bone lost its magnitude");
        Require(blended[7].values[0] == 1.0F && blended[7].values[3] == 0.0F,
                "a bone that cut was blended");
        pose = atb::world::TakePoseCounters();
        Require(pose.applied == 1 && pose.rejected == 0 &&
                    pose.snappedBones == 1,
                "per-bone handling was not counted correctly");

        // A palette that changes without a tick is render-driven.
        set(0.0F);
        Require(present(90, 0.2) == palette.data(), "pose setup failed");
        set(0.1F);
        Require(present(90, 0.7) == palette.data(),
                "a same-tick palette change was substituted");
        set(0.2F);
        Require(present(91, 0.2) == palette.data(),
                "a render-driven palette was substituted");
        pose = atb::world::TakePoseCounters();
        Require(pose.sameTickChanges == 1 && pose.quarantined == 1,
                "the render-driven palette was not quarantined");

        // Trace mode and oversize palettes always pass through.
        atb::world::Reset();
        atb::world::ConfigurePose(false);
        set(0.0F);
        Require(present(100, 0.5) == palette.data(), "trace setup failed");
        set(1.0F);
        Require(present(101, 0.5) == palette.data(),
                "trace mode substituted a palette");
        std::vector<DualQuaternion> huge(300, palette[0]);
        const atb::clock::Sample sample = Pair(101, 0.5);
        atb::world::BeginRender(102, &sample, true);
        Require(atb::world::PresentPalette(&keyStorage[6], huge.data(),
                                           huge.size()) == huge.data(),
                "an oversize palette was substituted");
        atb::world::EndRender();
        pose = atb::world::TakePoseCounters();
        Require(pose.changed == 1 && pose.applied == 0 && pose.oversize == 1 &&
                    pose.maximumBones == 300,
                "trace-mode pose counters were wrong");
        Require(atb::world::PresentPalette(mesh, palette.data(),
                                           palette.size()) == palette.data(),
                "a palette was substituted outside a render");
    }

    // Far more live submeshes than the table can hold must degrade to
    // pass-through, never to a wrong slot.
    atb::world::Reset();
    std::vector<Matrix> sources(60000, At(1.0F));
    {
        const atb::clock::Sample sample = Pair(39, 0.5);
        atb::world::BeginRender(40, &sample, true);
        for (const Matrix& source : sources) {
            Require(atb::world::Present(&source, &source) == &source,
                    "a first-sight submesh was substituted under pressure");
        }
        atb::world::EndRender();
    }
    counters = atb::world::TakeCounters();
    Require(counters.tableFull != 0 &&
                counters.tracked + counters.tableFull == sources.size(),
            "table pressure was not reported as pass-through");

    std::cout << "world runtime tests passed\n";
    return EXIT_SUCCESS;
}
