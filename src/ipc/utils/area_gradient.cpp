#include "area_gradient.hpp"

#include <cmath>

namespace ipc {
namespace autogen {

    // dA is (9×1) flattened in column-major order
    void triangle_area_gradient(
        double t0_x,
        double t0_y,
        double t0_z,
        double t1_x,
        double t1_y,
        double t1_z,
        double t2_x,
        double t2_y,
        double t2_z,
        double dA[9])
    {
        const auto t0 = -t2_y;
        const auto t1 = t0 + t1_y;
        const auto t2 = t0_x - t1_x;
        const auto t3 = t0 + t0_y;
        const auto t4 = -t2_x;
        const auto t5 = t0_x + t4;
        const auto t6 = t0_y - t1_y;
        const auto t7 = t2 * t3 - t5 * t6;
        const auto t8 = -t2_z;
        const auto t9 = t1_z + t8;
        const auto t10 = t0_z + t8;
        const auto t11 = t0_z - t1_z;
        const auto t12 = t10 * t2 - t11 * t5;
        const auto t13 = t10 * t6 - t11 * t3;
        const auto t14 =
            (1.0 / 2.0) / std::sqrt(t12 * t12 + t13 * t13 + t7 * t7);
        const auto t15 = t1_x + t4;
        dA[0] = t14 * (t1 * t7 + t12 * t9);
        dA[1] = -t14 * (-t13 * t9 + t15 * t7);
        dA[2] = -t14 * (t1 * t13 + t12 * t15);
        dA[3] = -t14 * (t10 * t12 + t3 * t7);
        dA[4] = t14 * (-t10 * t13 + t5 * t7);
        dA[5] = t14 * (t12 * t5 + t13 * t3);
        dA[6] = t14 * (t11 * t12 + t6 * t7);
        dA[7] = -t14 * (-t11 * t13 + t2 * t7);
        dA[8] = -t14 * (t12 * t2 + t13 * t6);
    }

} // namespace autogen
} // namespace ipc
