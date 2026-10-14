#pragma once

#include <array>
#include <cstdint>

namespace atb::hands_trace {

struct Matrix {
    std::array<float, 16> values{};
};

[[nodiscard]] std::array<float, 3> BasisLengths(
    const Matrix& matrix) noexcept;
[[nodiscard]] float DistanceFrom(const Matrix& matrix,
                                 const float position[3]) noexcept;
[[nodiscard]] bool HasUniformScale(const Matrix& matrix,
                                   float expectedScale,
                                   float tolerance = 0.025F) noexcept;
[[nodiscard]] bool IsFiniteAffine(const Matrix& matrix) noexcept;
[[nodiscard]] bool IsOrdinaryHandsRoot(const Matrix& matrix,
                                       const float cameraPosition[3]) noexcept;
[[nodiscard]] bool IsNearCamera(const Matrix& matrix,
                                const float cameraPosition[3],
                                float maximumDistance = 4.0F) noexcept;
[[nodiscard]] bool SameMatrix(const Matrix& left,
                              const Matrix& right) noexcept;
[[nodiscard]] float MaximumAbsoluteDifference(const Matrix& left,
                                              const Matrix& right) noexcept;
[[nodiscard]] Matrix Multiply(const Matrix& left,
                              const Matrix& right) noexcept;
[[nodiscard]] bool InvertAffine(const Matrix& matrix,
                                Matrix& inverse) noexcept;
[[nodiscard]] bool InterpolateRigid(const Matrix& older,
                                    const Matrix& newer,
                                    double alpha,
                                    Matrix& result) noexcept;
[[nodiscard]] bool ReadEntityMatrix(void* entity, Matrix& matrix) noexcept;
[[nodiscard]] void* GetMeshEntity(void* entity) noexcept;
[[nodiscard]] bool WriteEntityMatrix(void* entity,
                                     const Matrix& matrix) noexcept;

} // namespace atb::hands_trace
