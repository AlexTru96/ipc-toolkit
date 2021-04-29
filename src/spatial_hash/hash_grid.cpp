#include <ipc/spatial_hash/hash_grid.hpp>

#include <tbb/enumerable_thread_specific.h>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#ifdef IPC_TOOLKIT_WITH_LOGGER
#include <ipc/utils/logger.hpp>
#endif

namespace ipc {

AABB::AABB(const Eigen::ArrayMax3d& min, const Eigen::ArrayMax3d& max)
    : min(min)
    , max(max)
{
    assert(min.size() == max.size());
    assert((min <= max).all());
    half_extent = (max - min) / 2;
    center = min + half_extent;
}

bool AABB::are_overlapping(const AABB& a, const AABB& b)
{
    // https://bit.ly/2ZP3tW4
    assert(a.min.size() == b.min.size());
    return (abs(a.center.x() - b.center.x())
            <= (a.half_extent.x() + b.half_extent.x()))
        && (abs(a.center.y() - b.center.y())
            <= (a.half_extent.y() + b.half_extent.y()))
        && (a.min.size() == 2
            || abs(a.center.z() - b.center.z())
                <= (a.half_extent.z() + b.half_extent.z()));
};

typedef tbb::enumerable_thread_specific<std::vector<HashItem>>
    ThreadSpecificHashItems;

void merge_local_items(
    const ThreadSpecificHashItems& storages, std::vector<HashItem>& items)
{
    // size up the hash items
    size_t num_items = items.size();
    for (const auto& local_items : storages) {
        num_items += local_items.size();
    }
    // serial merge!
    items.reserve(num_items);
    for (const auto& local_items : storages) {
        items.insert(items.end(), local_items.begin(), local_items.end());
    }
}

void HashGrid::resize(
    const Eigen::ArrayMax3d& min, const Eigen::ArrayMax3d& max, double cellSize)
{
    clear();
    assert(cellSize != 0.0);
    m_cellSize = cellSize;
    m_domainMin = min;
    m_domainMax = max;
    m_gridSize = ((max - min) / m_cellSize).ceil().cast<int>().max(1);
#ifdef IPC_TOOLKIT_WITH_LOGGER
    logger().debug(
        "hash-grid resized with a size of {:d}x{:d}x{:d}", m_gridSize[0],
        m_gridSize[1], m_gridSize.size() == 3 ? m_gridSize[2] : 1);
#endif
}

/// @brief Compute an AABB around a given 2D mesh.
void calculate_mesh_extents(
    const Eigen::MatrixXd& vertices_t0,
    const Eigen::MatrixXd& vertices_t1,
    Eigen::ArrayMax3d& lower_bound,
    Eigen::ArrayMax3d& upper_bound)
{
    Eigen::ArrayMax3d lower_bound_t0 = vertices_t0.colwise().minCoeff();
    Eigen::ArrayMax3d upper_bound_t0 = vertices_t0.colwise().maxCoeff();
    Eigen::ArrayMax3d lower_bound_t1 = vertices_t1.colwise().minCoeff();
    Eigen::ArrayMax3d upper_bound_t1 = vertices_t1.colwise().maxCoeff();
    lower_bound = lower_bound_t0.min(lower_bound_t1);
    upper_bound = upper_bound_t0.max(upper_bound_t1);
}

/// @brief Compute the average edge length of a mesh.
double average_edge_length(
    const Eigen::MatrixXd& V_t0,
    const Eigen::MatrixXd& V_t1,
    const Eigen::MatrixXi& E)
{
    double avg = 0;
    for (unsigned i = 0; i < E.rows(); i++) {
        avg += (V_t0.row(E(i, 0)) - V_t0.row(E(i, 1))).norm();
        avg += (V_t1.row(E(i, 0)) - V_t1.row(E(i, 1))).norm();
    }
    return avg / (2 * E.rows());
}

/// @brief Compute the average displacement length.
double average_displacement_length(const Eigen::MatrixXd& displacements)
{
    return displacements.rowwise().norm().sum() / displacements.rows();
}

void HashGrid::resize(
    const Eigen::MatrixXd& vertices_t0,
    const Eigen::MatrixXd& vertices_t1,
    const Eigen::MatrixXi& edges,
    const double inflation_radius)
{
    Eigen::ArrayMax3d mesh_min, mesh_max;
    calculate_mesh_extents(vertices_t0, vertices_t1, mesh_min, mesh_max);
    double edge_len = average_edge_length(vertices_t0, vertices_t1, edges);
    double disp_len = average_displacement_length(vertices_t1 - vertices_t0);
    double cell_size = 2 * std::max(edge_len, disp_len) + inflation_radius;
    this->resize(
        mesh_min - inflation_radius, mesh_max + inflation_radius, cell_size);
}

/// @brief Compute a AABB for a vertex moving through time (i.e. temporal edge).
void calculate_vertex_extents(
    const Eigen::VectorX3d& vertex_t0,
    const Eigen::VectorX3d& vertex_t1,
    Eigen::ArrayMax3d& lower_bound,
    Eigen::ArrayMax3d& upper_bound)
{
    lower_bound = vertex_t0.cwiseMin(vertex_t1);
    upper_bound = vertex_t0.cwiseMax(vertex_t1);
}

void HashGrid::addVertex(
    const Eigen::VectorX3d& vertex_t0,
    const Eigen::VectorX3d& vertex_t1,
    const long index,
    const double inflation_radius)
{
    addVertex(vertex_t0, vertex_t1, index, m_vertexItems, inflation_radius);
}

void HashGrid::addVertex(
    const Eigen::VectorX3d& vertex_t0,
    const Eigen::VectorX3d& vertex_t1,
    const long index,
    std::vector<HashItem>& vertex_items,
    const double inflation_radius) const
{
    Eigen::ArrayMax3d lower_bound, upper_bound;
    calculate_vertex_extents(vertex_t0, vertex_t1, lower_bound, upper_bound);
    this->addElement(
        AABB(lower_bound - inflation_radius, upper_bound + inflation_radius),
        index, vertex_items);
}

void HashGrid::addVertices(
    const Eigen::MatrixXd& vertices_t0,
    const Eigen::MatrixXd& vertices_t1,
    const double inflation_radius)
{
    assert(vertices_t0.rows() == vertices_t1.rows());

    ThreadSpecificHashItems storage;

    tbb::parallel_for(
        tbb::blocked_range<long>(0l, long(vertices_t0.rows())),
        [&](const tbb::blocked_range<long>& range) {
            ThreadSpecificHashItems::reference local_items = storage.local();

            for (long i = range.begin(); i != range.end(); i++) {
                addVertex(
                    vertices_t0.row(i), vertices_t1.row(i), i, local_items,
                    inflation_radius);
            }
        });

    merge_local_items(storage, m_vertexItems);
}

void HashGrid::addVerticesFromEdges(
    const Eigen::MatrixXd& vertices_t0,
    const Eigen::MatrixXd& vertices_t1,
    const Eigen::MatrixXi& edges,
    const double inflation_radius)
{
    assert(vertices_t0.rows() == vertices_t1.rows());

    std::vector<size_t> vertex_to_min_edge(
        vertices_t0.rows(), edges.rows() + 1);
    // Column first because colmajor
    for (size_t ej = 0; ej < edges.cols(); ej++) {
        for (size_t ei = 0; ei < edges.rows(); ei++) {
            const size_t vi = edges(ei, ej);
            vertex_to_min_edge[vi] = std::min(vertex_to_min_edge[vi], ei);
        }
    }

    ThreadSpecificHashItems storage;

    tbb::parallel_for(
        tbb::blocked_range<long>(0l, long(edges.rows())),
        [&](const tbb::blocked_range<long>& range) {
            ThreadSpecificHashItems::reference local_items = storage.local();

            for (long ei = range.begin(); ei != range.end(); ei++) {
                for (long ej = 0; ej < edges.cols(); ej++) {
                    const size_t vi = edges(ei, ej);
                    if (vertex_to_min_edge[vi] == ei) {
                        addVertex(
                            vertices_t0.row(vi), vertices_t1.row(vi), vi,
                            local_items, inflation_radius);
                    }
                }
            }
        });

    merge_local_items(storage, m_vertexItems);
}

/// @brief Compute a AABB for an edge moving through time (i.e. temporal quad).
void calculate_edge_extents(
    const Eigen::VectorX3d& edge_vertex0_t0,
    const Eigen::VectorX3d& edge_vertex1_t0,
    const Eigen::VectorX3d& edge_vertex0_t1,
    const Eigen::VectorX3d& edge_vertex1_t1,
    Eigen::ArrayMax3d& lower_bound,
    Eigen::ArrayMax3d& upper_bound)
{
    lower_bound = edge_vertex0_t0.cwiseMin(edge_vertex1_t0)
                      .cwiseMin(edge_vertex0_t1)
                      .cwiseMin(edge_vertex1_t1);
    upper_bound = edge_vertex0_t0.cwiseMax(edge_vertex1_t0)
                      .cwiseMax(edge_vertex0_t1)
                      .cwiseMax(edge_vertex1_t1);
}

void HashGrid::addEdge(
    const Eigen::VectorX3d& edge_vertex0_t0,
    const Eigen::VectorX3d& edge_vertex1_t0,
    const Eigen::VectorX3d& edge_vertex0_t1,
    const Eigen::VectorX3d& edge_vertex1_t1,
    const long index,
    const double inflation_radius)
{
    addEdge(
        edge_vertex0_t0, edge_vertex1_t0, edge_vertex0_t1, edge_vertex1_t1,
        index, m_edgeItems, inflation_radius);
}

void HashGrid::addEdge(
    const Eigen::VectorX3d& edge_vertex0_t0,
    const Eigen::VectorX3d& edge_vertex1_t0,
    const Eigen::VectorX3d& edge_vertex0_t1,
    const Eigen::VectorX3d& edge_vertex1_t1,
    const long index,
    std::vector<HashItem>& edge_items,
    const double inflation_radius) const
{
    Eigen::ArrayMax3d lower_bound, upper_bound;
    calculate_edge_extents(
        edge_vertex0_t0, edge_vertex1_t0, edge_vertex0_t1, edge_vertex1_t1,
        lower_bound, upper_bound);
    this->addElement(
        AABB(lower_bound - inflation_radius, upper_bound + inflation_radius),
        index, edge_items);
}

void HashGrid::addEdges(
    const Eigen::MatrixXd& vertices_t0,
    const Eigen::MatrixXd& vertices_t1,
    const Eigen::MatrixXi& edges,
    const double inflation_radius)
{
    assert(vertices_t0.rows() == vertices_t1.rows());

    ThreadSpecificHashItems storage;

    tbb::parallel_for(
        tbb::blocked_range<long>(0l, long(edges.rows())),
        [&](const tbb::blocked_range<long>& range) {
            ThreadSpecificHashItems::reference local_items = storage.local();

            for (long i = range.begin(); i != range.end(); i++) {
                addEdge(
                    vertices_t0.row(edges(i, 0)), vertices_t0.row(edges(i, 1)),
                    vertices_t1.row(edges(i, 0)), vertices_t1.row(edges(i, 1)),
                    i, local_items, inflation_radius);
            }
        });

    merge_local_items(storage, m_edgeItems);
}

/// @brief Compute a AABB for an edge moving through time (i.e. temporal quad).
void calculate_face_extents(
    const Eigen::VectorX3d& face_vertex0_t0,
    const Eigen::VectorX3d& face_vertex1_t0,
    const Eigen::VectorX3d& face_vertex2_t0,
    const Eigen::VectorX3d& face_vertex0_t1,
    const Eigen::VectorX3d& face_vertex1_t1,
    const Eigen::VectorX3d& face_vertex2_t1,
    Eigen::ArrayMax3d& lower_bound,
    Eigen::ArrayMax3d& upper_bound)
{
    lower_bound = face_vertex0_t0.cwiseMin(face_vertex1_t0)
                      .cwiseMin(face_vertex2_t0)
                      .cwiseMin(face_vertex0_t1)
                      .cwiseMin(face_vertex1_t1)
                      .cwiseMin(face_vertex2_t1);
    upper_bound = face_vertex0_t0.cwiseMax(face_vertex1_t0)
                      .cwiseMax(face_vertex2_t0)
                      .cwiseMax(face_vertex0_t1)
                      .cwiseMax(face_vertex1_t1)
                      .cwiseMax(face_vertex2_t1);
}

void HashGrid::addFace(
    const Eigen::VectorX3d& face_vertex0_t0,
    const Eigen::VectorX3d& face_vertex1_t0,
    const Eigen::VectorX3d& face_vertex2_t0,
    const Eigen::VectorX3d& face_vertex0_t1,
    const Eigen::VectorX3d& face_vertex1_t1,
    const Eigen::VectorX3d& face_vertex2_t1,
    const long index,
    const double inflation_radius)
{
    addFace(
        face_vertex0_t0, face_vertex1_t0, face_vertex2_t0, face_vertex0_t1,
        face_vertex1_t1, face_vertex2_t1, index, m_faceItems, inflation_radius);
}

void HashGrid::addFace(
    const Eigen::VectorX3d& face_vertex0_t0,
    const Eigen::VectorX3d& face_vertex1_t0,
    const Eigen::VectorX3d& face_vertex2_t0,
    const Eigen::VectorX3d& face_vertex0_t1,
    const Eigen::VectorX3d& face_vertex1_t1,
    const Eigen::VectorX3d& face_vertex2_t1,
    const long index,
    std::vector<HashItem>& face_items,
    const double inflation_radius) const
{
    Eigen::ArrayMax3d lower_bound, upper_bound;
    calculate_face_extents(
        face_vertex0_t0, face_vertex1_t0, face_vertex2_t0, //
        face_vertex0_t1, face_vertex1_t1, face_vertex2_t1, //
        lower_bound, upper_bound);
    this->addElement(
        AABB(lower_bound - inflation_radius, upper_bound + inflation_radius),
        index, face_items);
}

void HashGrid::addFaces(
    const Eigen::MatrixXd& vertices_t0,
    const Eigen::MatrixXd& vertices_t1,
    const Eigen::MatrixXi& faces,
    const double inflation_radius)
{
    assert(vertices_t0.rows() == vertices_t1.rows());

    ThreadSpecificHashItems storage;

    tbb::parallel_for(
        tbb::blocked_range<long>(0l, long(faces.rows())),
        [&](const tbb::blocked_range<long>& range) {
            ThreadSpecificHashItems::reference local_items = storage.local();

            for (long i = range.begin(); i != range.end(); i++) {
                addFace(
                    vertices_t0.row(faces(i, 0)), vertices_t0.row(faces(i, 1)),
                    vertices_t0.row(faces(i, 2)), vertices_t1.row(faces(i, 0)),
                    vertices_t1.row(faces(i, 1)), vertices_t1.row(faces(i, 2)),
                    i, local_items, inflation_radius);
            }
        });

    merge_local_items(storage, m_faceItems);
}

void HashGrid::addElement(
    const AABB& aabb, const int id, std::vector<HashItem>& items) const
{
    Eigen::ArrayMax3i int_min =
        ((aabb.getMin() - m_domainMin) / m_cellSize).cast<int>();
    // We can round down to -1, but not less
    assert((int_min >= -1).all());
    assert((int_min <= m_gridSize).all());
    int_min = int_min.max(0).min(m_gridSize - 1);

    Eigen::ArrayMax3i int_max =
        ((aabb.getMax() - m_domainMin) / m_cellSize).cast<int>();
    assert((int_max >= -1).all());
    assert((int_max <= m_gridSize).all());
    int_max = int_max.max(0).min(m_gridSize - 1);
    assert((int_min <= int_max).all());

    int min_z = int_min.size() == 3 ? int_min.z() : 0;
    int max_z = int_max.size() == 3 ? int_max.z() : 0;
    for (int x = int_min.x(); x <= int_max.x(); ++x) {
        for (int y = int_min.y(); y <= int_max.y(); ++y) {
            for (int z = min_z; z <= max_z; ++z) {
                items.emplace_back(hash(x, y, z), id, aabb);
            }
        }
    }
}

template <typename T>
void getPairs(
    const std::function<bool(int, int)>& is_endpoint,
    const std::function<bool(int, int)>& is_same_group,
    std::vector<HashItem>& items0,
    std::vector<HashItem>& items1,
    T& candidates)
{
    // Sorted all they (key,value) pairs, where key is the hash key, and value
    // is the element index
    tbb::parallel_sort(items0.begin(), items0.end());
    tbb::parallel_sort(items1.begin(), items1.end());

    // Entries with the same key means they share a cell (that cell index
    // hashes to the same key) and should be flagged for low-level intersection
    // testing. So we loop over the entire sorted set of (key,value) pairs
    // creating Candidate entries for vertex-edge pairs with the same key
    int i = 0, j_start = 0;
    while (i < items0.size() && j_start < items1.size()) {
        const HashItem& item0 = items0[i];

        int j = j_start;
        while (j < items1.size()) {
            const HashItem& item1 = items1[j];

            if (item0.key == item1.key) {
                if (!is_endpoint(item0.id, item1.id)
                    && !is_same_group(item0.id, item1.id)
                    && AABB::are_overlapping(item0.aabb, item1.aabb)) {
                    candidates.emplace_back(item0.id, item1.id);
                }
            } else {
                break;
            }
            j++;
        }

        if (i == items0.size() - 1 || item0.key != items0[i + 1].key) {
            j_start = j;
        }
        i++;
    }

    // Remove the duplicate candidates
    tbb::parallel_sort(candidates.begin(), candidates.end());
    auto new_end = std::unique(candidates.begin(), candidates.end());
    candidates.erase(new_end, candidates.end());
}

template <typename T>
void getPairs(
    const std::function<bool(int, int)>& is_endpoint,
    const std::function<bool(int, int)>& is_same_group,
    std::vector<HashItem>& items,
    T& candidates)
{
    // Sorted all they (key,value) pairs, where key is the hash key, and value
    // is the element index
    tbb::parallel_sort(items.begin(), items.end());

    // Entries with the same key means they share a cell (that cell index
    // hashes to the same key) and should be flagged for low-level intersection
    // testing. So we loop over the entire sorted set of (key,value) pairs
    // creating Candidate entries for vertex-edge pairs with the same key
    for (int i = 0; i < items.size(); i++) {
        const HashItem& item0 = items[i];
        for (int j = i + 1; j < items.size(); j++) {
            const HashItem& item1 = items[j];
            if (item0.key == item1.key) {
                if (!is_endpoint(item0.id, item1.id)
                    && !is_same_group(item0.id, item1.id)
                    && AABB::are_overlapping(item0.aabb, item1.aabb)) {
                    candidates.emplace_back(item0.id, item1.id);
                }
            } else {
                break; // This avoids a brute force comparison
            }
        }
    }

    /*
    tbb::enumerable_thread_specific<T> storages;

    tbb::parallel_for(
        tbb::blocked_range2d<int>(0, items.size(), 0, items.size()),
        [&](const tbb::blocked_range2d<int>& r) {
            if (r.rows().begin() >= r.cols().end()) {
                return; // i needs to be less than j
            }

            auto& local_candidates = storages.local();

            for (int i = r.rows().begin(); i < r.rows().end(); i++) {
                const HashItem& item0 = items[i];

                if (i >= r.cols().end()) {
                    return; // i will increase but r.cols().end() will not
                }

                // i < r.cols().end() → i + 1 <= r.cols().end()
                int j_start = std::max(i + 1, r.cols().begin());
                assert(j_start > i);

                for (int j = j_start; j < r.cols().end(); j++) {
                    const HashItem& item1 = items[j];

                    if (item0.key == item1.key) {
                        if (!is_endpoint(item0.id, item1.id)
                            && !is_same_group(item0.id, item1.id)
                            && AABB::are_overlapping(item0.aabb, item1.aabb)) {
                            local_candidates.emplace_back(item0.id, item1.id);
                        }
                    } else {
                        break; // This avoids a brute force comparison
                    }
                }
            }
        });

    // size up the candidates
    size_t num_candidates = candidates.size();
    for (const auto& local_candidates : storages) {
        num_candidates += local_candidates.size();
    }
    // serial merge!
    candidates.reserve(num_candidates);
    for (const auto& local_candidates : storages) {
        candidates.insert(
            candidates.end(), local_candidates.begin(), local_candidates.end());
    }
    */

    // Remove the duplicate candidates
    tbb::parallel_sort(candidates.begin(), candidates.end());
    auto new_end = std::unique(candidates.begin(), candidates.end());
    candidates.erase(new_end, candidates.end());
}

void HashGrid::getVertexEdgePairs(
    const Eigen::MatrixXi& edges,
    const Eigen::VectorXi& group_ids,
    std::vector<EdgeVertexCandidate>& ev_candidates)
{
    auto is_endpoint = [&](int ei, int vi) {
        return edges(ei, 0) == vi || edges(ei, 1) == vi;
    };

    bool check_groups = group_ids.size() > 0;
    auto is_same_group = [&](int ei, int vi) {
        return check_groups
            && (group_ids(vi) == group_ids(edges(ei, 0))
                || group_ids(vi) == group_ids(edges(ei, 1)));
    };

    getPairs(
        is_endpoint, is_same_group, m_edgeItems, m_vertexItems, ev_candidates);
}

void HashGrid::getEdgeEdgePairs(
    const Eigen::MatrixXi& edges,
    const Eigen::VectorXi& group_ids,
    std::vector<EdgeEdgeCandidate>& ee_candidates)
{
    auto is_endpoint = [&](int ei, int ej) {
        return edges(ei, 0) == edges(ej, 0) || edges(ei, 0) == edges(ej, 1)
            || edges(ei, 1) == edges(ej, 0) || edges(ei, 1) == edges(ej, 1);
    };

    bool check_groups = group_ids.size() > 0;
    auto is_same_group = [&](int ei, int ej) {
        return check_groups
            && (group_ids(edges(ei, 0)) == group_ids(edges(ej, 0))
                || group_ids(edges(ei, 0)) == group_ids(edges(ej, 1))
                || group_ids(edges(ei, 1)) == group_ids(edges(ej, 0))
                || group_ids(edges(ei, 1)) == group_ids(edges(ej, 1)));
    };

    getPairs(is_endpoint, is_same_group, m_edgeItems, ee_candidates);
}

void HashGrid::getEdgeFacePairs(
    const Eigen::MatrixXi& edges,
    const Eigen::MatrixXi& faces,
    const Eigen::VectorXi& group_ids,
    std::vector<EdgeFaceCandidate>& ef_candidates)
{
    auto is_endpoint = [&](int ei, int fi) {
        // Check if the edge and face have a common end-point
        return edges(ei, 0) == faces(fi, 0) || edges(ei, 0) == faces(fi, 1)
            || edges(ei, 0) == faces(fi, 2) || edges(ei, 1) == faces(fi, 0)
            || edges(ei, 1) == faces(fi, 1) || edges(ei, 1) == faces(fi, 2);
    };

    bool check_groups = group_ids.size() > 0;
    auto is_same_group = [&](int ei, int fi) {
        return check_groups
            && (group_ids(edges(ei, 0)) == group_ids(faces(fi, 0))
                || group_ids(edges(ei, 0)) == group_ids(faces(fi, 1))
                || group_ids(edges(ei, 0)) == group_ids(faces(fi, 2))
                || group_ids(edges(ei, 1)) == group_ids(faces(fi, 0))
                || group_ids(edges(ei, 1)) == group_ids(faces(fi, 1))
                || group_ids(edges(ei, 1)) == group_ids(faces(fi, 2)));
    };

    getPairs(
        is_endpoint, is_same_group, m_edgeItems, m_faceItems, ef_candidates);
}

void HashGrid::getFaceVertexPairs(
    const Eigen::MatrixXi& faces,
    const Eigen::VectorXi& group_ids,
    std::vector<FaceVertexCandidate>& fv_candidates)
{
    auto is_endpoint = [&](int fi, int vi) {
        return vi == faces(fi, 0) || vi == faces(fi, 1) || vi == faces(fi, 2);
    };

    bool check_groups = group_ids.size() > 0;
    auto is_same_group = [&](int fi, int vi) {
        return check_groups
            && (group_ids(vi) == group_ids(faces(fi, 0))
                || group_ids(vi) == group_ids(faces(fi, 1))
                || group_ids(vi) == group_ids(faces(fi, 2)));
    };

    getPairs(
        is_endpoint, is_same_group, m_faceItems, m_vertexItems, fv_candidates);
}

} // namespace ipc
