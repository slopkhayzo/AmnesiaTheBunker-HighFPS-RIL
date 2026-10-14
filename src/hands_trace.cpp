#include "hands_trace.h"

#include "guarded_memory.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace atb::hands_trace {
namespace {

constexpr std::size_t kGetMatrixVtableOffset = 0xA8;
constexpr std::size_t kGetMeshEntityVtableOffset = 0x100;
constexpr std::size_t kSetMatrixVtableOffset = 0x170;

// Resolves one virtual method of a live engine object. The object and its
// vtable are read through the fault guard, and the target must lie inside the
// host executable image, so no memory-protection query is needed.
void* ResolveVirtual(void* const object,
                     const std::size_t vtableOffset) noexcept {
    const std::uint8_t* vtable = nullptr;
    void* function = nullptr;
    if (!memory::Read(object, vtable) || vtable == nullptr ||
        !memory::Read(vtable + vtableOffset, function) ||
        !memory::IsMainImageAddress(function)) {
        return nullptr;
    }
    return function;
}

float RowLength(const Matrix& matrix, const std::size_t row) noexcept {
    const std::size_t offset = row * 4;
    const float x = matrix.values[offset];
    const float y = matrix.values[offset + 1];
    const float z = matrix.values[offset + 2];
    return std::sqrt(x * x + y * y + z * z);
}

struct Quaternion {
    float w = 1.0F;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

float Dot(const Quaternion& left, const Quaternion& right) noexcept {
    return left.w * right.w + left.x * right.x + left.y * right.y +
           left.z * right.z;
}

Quaternion Normalize(const Quaternion& value) noexcept {
    const float lengthSquared = Dot(value, value);
    if (!(lengthSquared > 0.0F) || !std::isfinite(lengthSquared)) {
        return {};
    }
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {value.w * inverseLength, value.x * inverseLength,
            value.y * inverseLength, value.z * inverseLength};
}

bool ToQuaternion(const Matrix& matrix, Quaternion& result) noexcept {
    if (!IsFiniteAffine(matrix)) {
        return false;
    }
    const float r0x = matrix.values[0];
    const float r0y = matrix.values[1];
    const float r0z = matrix.values[2];
    const float r1x = matrix.values[4];
    const float r1y = matrix.values[5];
    const float r1z = matrix.values[6];
    const float r2x = matrix.values[8];
    const float r2y = matrix.values[9];
    const float r2z = matrix.values[10];
    const float l0 = std::sqrt(r0x * r0x + r0y * r0y + r0z * r0z);
    const float l1 = std::sqrt(r1x * r1x + r1y * r1y + r1z * r1z);
    const float l2 = std::sqrt(r2x * r2x + r2y * r2y + r2z * r2z);
    if (std::abs(l0 - 1.0F) > 0.025F ||
        std::abs(l1 - 1.0F) > 0.025F ||
        std::abs(l2 - 1.0F) > 0.025F ||
        std::abs(r0x * r1x + r0y * r1y + r0z * r1z) > 0.025F ||
        std::abs(r0x * r2x + r0y * r2y + r0z * r2z) > 0.025F ||
        std::abs(r1x * r2x + r1y * r2y + r1z * r2z) > 0.025F) {
        return false;
    }
    const float determinant =
        r0x * (r1y * r2z - r1z * r2y) -
        r0y * (r1x * r2z - r1z * r2x) +
        r0z * (r1x * r2y - r1y * r2x);
    if (determinant < 0.95F || determinant > 1.05F) {
        return false;
    }

    const float trace = r0x + r1y + r2z;
    Quaternion q{};
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q = {0.25F * s, (r2y - r1z) / s, (r0z - r2x) / s,
             (r1x - r0y) / s};
    } else if (r0x > r1y && r0x > r2z) {
        const float s = std::sqrt(1.0F + r0x - r1y - r2z) * 2.0F;
        q = {(r2y - r1z) / s, 0.25F * s, (r0y + r1x) / s,
             (r0z + r2x) / s};
    } else if (r1y > r2z) {
        const float s = std::sqrt(1.0F + r1y - r0x - r2z) * 2.0F;
        q = {(r0z - r2x) / s, (r0y + r1x) / s, 0.25F * s,
             (r1z + r2y) / s};
    } else {
        const float s = std::sqrt(1.0F + r2z - r0x - r1y) * 2.0F;
        q = {(r1x - r0y) / s, (r0z + r2x) / s,
             (r1z + r2y) / s, 0.25F * s};
    }
    result = Normalize(q);
    return std::isfinite(result.w) && std::isfinite(result.x) &&
           std::isfinite(result.y) && std::isfinite(result.z);
}

Quaternion Slerp(const Quaternion& from,
                 const Quaternion& to,
                 const float alpha) noexcept {
    Quaternion end = to;
    float cosine = Dot(from, end);
    if (cosine < 0.0F) {
        end = {-end.w, -end.x, -end.y, -end.z};
        cosine = -cosine;
    }
    cosine = std::clamp(cosine, -1.0F, 1.0F);
    if (cosine > 0.9995F) {
        return Normalize({from.w + alpha * (end.w - from.w),
                          from.x + alpha * (end.x - from.x),
                          from.y + alpha * (end.y - from.y),
                          from.z + alpha * (end.z - from.z)});
    }
    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    const float firstWeight = std::sin((1.0F - alpha) * angle) / sine;
    const float secondWeight = std::sin(alpha * angle) / sine;
    return Normalize({from.w * firstWeight + end.w * secondWeight,
                      from.x * firstWeight + end.x * secondWeight,
                      from.y * firstWeight + end.y * secondWeight,
                      from.z * firstWeight + end.z * secondWeight});
}

void WriteRotation(const Quaternion& input, Matrix& matrix) noexcept {
    const Quaternion q = Normalize(input);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    matrix.values[0] = 1.0F - 2.0F * (yy + zz);
    matrix.values[1] = 2.0F * (xy - wz);
    matrix.values[2] = 2.0F * (xz + wy);
    matrix.values[4] = 2.0F * (xy + wz);
    matrix.values[5] = 1.0F - 2.0F * (xx + zz);
    matrix.values[6] = 2.0F * (yz - wx);
    matrix.values[8] = 2.0F * (xz - wy);
    matrix.values[9] = 2.0F * (yz + wx);
    matrix.values[10] = 1.0F - 2.0F * (xx + yy);
}

} // namespace

std::array<float, 3> BasisLengths(const Matrix& matrix) noexcept {
    return {RowLength(matrix, 0), RowLength(matrix, 1), RowLength(matrix, 2)};
}

float DistanceFrom(const Matrix& matrix,
                   const float position[3]) noexcept {
    if (position == nullptr) {
        return std::numeric_limits<float>::infinity();
    }
    const float dx = matrix.values[3] - position[0];
    const float dy = matrix.values[7] - position[1];
    const float dz = matrix.values[11] - position[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool HasUniformScale(const Matrix& matrix,
                     const float expectedScale,
                     const float tolerance) noexcept {
    if (!std::isfinite(expectedScale) || !std::isfinite(tolerance) ||
        expectedScale <= 0.0F || tolerance < 0.0F) {
        return false;
    }
    const auto lengths = BasisLengths(matrix);
    for (const float length : lengths) {
        if (!std::isfinite(length) ||
            std::abs(length - expectedScale) > tolerance) {
            return false;
        }
    }
    return true;
}

bool IsFiniteAffine(const Matrix& matrix) noexcept {
    for (const float value : matrix.values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    constexpr float epsilon = 2.0e-3F;
    return std::abs(matrix.values[12]) <= epsilon &&
           std::abs(matrix.values[13]) <= epsilon &&
           std::abs(matrix.values[14]) <= epsilon &&
           std::abs(matrix.values[15] - 1.0F) <= epsilon;
}

bool IsNearCamera(const Matrix& matrix,
                  const float cameraPosition[3],
                  const float maximumDistance) noexcept {
    if (!IsFiniteAffine(matrix) || cameraPosition == nullptr ||
        !std::isfinite(cameraPosition[0]) ||
        !std::isfinite(cameraPosition[1]) ||
        !std::isfinite(cameraPosition[2]) ||
        !std::isfinite(maximumDistance) || maximumDistance <= 0.0F) {
        return false;
    }
    const float dx = matrix.values[3] - cameraPosition[0];
    const float dy = matrix.values[7] - cameraPosition[1];
    const float dz = matrix.values[11] - cameraPosition[2];
    return dx * dx + dy * dy + dz * dz <= maximumDistance * maximumDistance;
}

bool IsOrdinaryHandsRoot(const Matrix& matrix,
                         const float cameraPosition[3]) noexcept {
    // The installed Bunker scripts support both the legacy quarter-scale
    // hands model and full-scale animations. Live traces prove the ordinary
    // full-scale root is written at the camera origin, while the player-body
    // root remains roughly 1.6--1.8 metres away.
    return (HasUniformScale(matrix, 0.25F) &&
            IsNearCamera(matrix, cameraPosition, 2.0F)) ||
           (HasUniformScale(matrix, 1.0F) &&
            IsNearCamera(matrix, cameraPosition, 0.1F));
}

bool SameMatrix(const Matrix& left, const Matrix& right) noexcept {
    return std::memcmp(left.values.data(), right.values.data(),
                       sizeof(left.values)) == 0;
}

float MaximumAbsoluteDifference(const Matrix& left,
                                const Matrix& right) noexcept {
    float maximum = 0.0F;
    for (std::size_t index = 0; index < left.values.size(); ++index) {
        const float difference =
            std::abs(left.values[index] - right.values[index]);
        if (!std::isfinite(difference)) {
            return std::numeric_limits<float>::infinity();
        }
        maximum = std::max(maximum, difference);
    }
    return maximum;
}

Matrix Multiply(const Matrix& left, const Matrix& right) noexcept {
    Matrix result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            float value = 0.0F;
            for (std::size_t inner = 0; inner < 4; ++inner) {
                value += left.values[row * 4 + inner] *
                         right.values[inner * 4 + column];
            }
            result.values[row * 4 + column] = value;
        }
    }
    return result;
}

bool InvertAffine(const Matrix& matrix, Matrix& inverse) noexcept {
    inverse = {};
    if (!IsFiniteAffine(matrix)) {
        return false;
    }
    const float a = matrix.values[0];
    const float b = matrix.values[1];
    const float c = matrix.values[2];
    const float d = matrix.values[4];
    const float e = matrix.values[5];
    const float f = matrix.values[6];
    const float g = matrix.values[8];
    const float h = matrix.values[9];
    const float i = matrix.values[10];
    const float determinant =
        a * (e * i - f * h) - b * (d * i - f * g) +
        c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-8F) {
        return false;
    }
    const float reciprocal = 1.0F / determinant;
    inverse.values[0] = (e * i - f * h) * reciprocal;
    inverse.values[1] = (c * h - b * i) * reciprocal;
    inverse.values[2] = (b * f - c * e) * reciprocal;
    inverse.values[4] = (f * g - d * i) * reciprocal;
    inverse.values[5] = (a * i - c * g) * reciprocal;
    inverse.values[6] = (c * d - a * f) * reciprocal;
    inverse.values[8] = (d * h - e * g) * reciprocal;
    inverse.values[9] = (b * g - a * h) * reciprocal;
    inverse.values[10] = (a * e - b * d) * reciprocal;
    const float x = matrix.values[3];
    const float y = matrix.values[7];
    const float z = matrix.values[11];
    inverse.values[3] = -(inverse.values[0] * x + inverse.values[1] * y +
                          inverse.values[2] * z);
    inverse.values[7] = -(inverse.values[4] * x + inverse.values[5] * y +
                          inverse.values[6] * z);
    inverse.values[11] = -(inverse.values[8] * x + inverse.values[9] * y +
                           inverse.values[10] * z);
    inverse.values[15] = 1.0F;
    return IsFiniteAffine(inverse);
}

bool InterpolateRigid(const Matrix& older,
                      const Matrix& newer,
                      const double alpha,
                      Matrix& result) noexcept {
    result = {};
    if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0) {
        return false;
    }
    Quaternion first{};
    Quaternion second{};
    if (!ToQuaternion(older, first) || !ToQuaternion(newer, second)) {
        return false;
    }
    const float dx = newer.values[3] - older.values[3];
    const float dy = newer.values[7] - older.values[7];
    const float dz = newer.values[11] - older.values[11];
    if (dx * dx + dy * dy + dz * dz > 25.0F) {
        return false;
    }
    const float orientationAngle = 2.0F * std::acos(
        std::clamp(std::abs(Dot(first, second)), 0.0F, 1.0F));
    constexpr float maximumOrientationDelta =
        2.09439510239F; // 120 degrees per fixed update.
    if (!std::isfinite(orientationAngle) ||
        orientationAngle > maximumOrientationDelta) {
        return false;
    }
    WriteRotation(Slerp(first, second, static_cast<float>(alpha)), result);
    const float t = static_cast<float>(alpha);
    result.values[3] = older.values[3] + dx * t;
    result.values[7] = older.values[7] + dy * t;
    result.values[11] = older.values[11] + dz * t;
    result.values[15] = 1.0F;
    return IsFiniteAffine(result);
}

bool ReadEntityMatrix(void* const entity, Matrix& matrix) noexcept {
    matrix = {};
    void* const function = ResolveVirtual(entity, kGetMatrixVtableOffset);
    if (function == nullptr) {
        return false;
    }
    using GetMatrix = Matrix* (*)(void*, Matrix*);
    Matrix storage{};
    const Matrix* const returned =
        reinterpret_cast<GetMatrix>(function)(entity, &storage);
    if (returned == nullptr) {
        return false;
    }
    if (returned == &storage) {
        matrix = storage;
    } else if (!memory::Copy(&matrix, returned, sizeof(matrix))) {
        matrix = {};
        return false;
    }
    return IsFiniteAffine(matrix);
}

void* GetMeshEntity(void* const entity) noexcept {
    void* const function = ResolveVirtual(entity, kGetMeshEntityVtableOffset);
    if (function == nullptr) {
        return nullptr;
    }
    using GetMesh = void* (*)(void*);
    return reinterpret_cast<GetMesh>(function)(entity);
}

bool WriteEntityMatrix(void* const entity, const Matrix& matrix) noexcept {
    if (!IsFiniteAffine(matrix)) {
        return false;
    }
    void* const function = ResolveVirtual(entity, kSetMatrixVtableOffset);
    if (function == nullptr) {
        return false;
    }
    using SetMatrix = void (*)(void*, const Matrix*);
    reinterpret_cast<SetMatrix>(function)(entity, &matrix);
    return true;
}

} // namespace atb::hands_trace
