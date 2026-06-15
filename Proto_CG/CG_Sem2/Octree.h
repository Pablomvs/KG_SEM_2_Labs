#pragma once
#include <DirectXMath.h>
#include <vector>
#include <cmath>

using namespace DirectX;

// Плоскость фрустума: точка внутри если dot(normal, point) + d >= 0
struct FrustumPlane { float nx, ny, nz, d; };

// Извлечение 6 плоскостей из VP-матрицы (строчный порядок DirectX Math: clip = p * VP)
inline void ExtractFrustumPlanes(const XMFLOAT4X4& m, FrustumPlane out[6])
{
    // Левая:  col3 + col0
    out[0] = { m._11 + m._14, m._21 + m._24, m._31 + m._34, m._41 + m._44 };
    // Правая: col3 - col0
    out[1] = { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 };
    // Нижняя: col3 + col1
    out[2] = { m._12 + m._14, m._22 + m._24, m._32 + m._34, m._42 + m._44 };
    // Верхняя: col3 - col1
    out[3] = { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 };
    // Ближняя: col2
    out[4] = { m._13, m._23, m._33, m._43 };
    // Дальняя: col3 - col2
    out[5] = { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 };

    for (int i = 0; i < 6; ++i)
    {
        float len = sqrtf(out[i].nx * out[i].nx + out[i].ny * out[i].ny + out[i].nz * out[i].nz);
        if (len > 1e-6f)
        {
            float inv = 1.f / len;
            out[i].nx *= inv; out[i].ny *= inv; out[i].nz *= inv; out[i].d *= inv;
        }
    }
}

// Тест сферы против фрустума
inline bool SphereInFrustum(const XMFLOAT3& c, float r, const FrustumPlane p[6])
{
    for (int i = 0; i < 6; ++i)
        if (p[i].nx * c.x + p[i].ny * c.y + p[i].nz * c.z + p[i].d < -r)
            return false;
    return true;
}

// Тест AABB против фрустума (для узлов октодерева)
inline bool AABBInFrustum(const XMFLOAT3& center, float half, const FrustumPlane p[6])
{
    for (int i = 0; i < 6; ++i)
    {
        float px = (p[i].nx >= 0.f) ? center.x + half : center.x - half;
        float py = (p[i].ny >= 0.f) ? center.y + half : center.y - half;
        float pz = (p[i].nz >= 0.f) ? center.z + half : center.z - half;
        if (p[i].nx * px + p[i].ny * py + p[i].nz * pz + p[i].d < 0.f)
            return false;
    }
    return true;
}

// Октодерево для пространственного разбиения объёмов
class Octree
{
public:
    void Build(const std::vector<XMFLOAT3>& positions, float objRadius, float sceneHalf)
    {
        m_positions = positions;
        m_objRadius = objRadius;
        m_nodes.clear();
        m_nodes.reserve(4096);

        Node root;
        root.center    = { 0.f, 0.f, 0.f };
        root.halfSize  = sceneHalf;
        root.firstChild = -1;
        m_nodes.push_back(root);

        std::vector<int> all((int)positions.size());
        for (int i = 0; i < (int)positions.size(); ++i) all[i] = i;
        BuildNode(0, std::move(all), 0);
    }

    void QueryFrustum(const FrustumPlane planes[6], std::vector<int>& out) const
    {
        out.clear();
        if (!m_nodes.empty()) QueryNode(0, planes, out);
    }

private:
    struct Node
    {
        XMFLOAT3        center;
        float           halfSize;
        int             firstChild;
        std::vector<int> objects;
    };

    static constexpr int MaxDepth = 6;
    static constexpr int MaxLeaf  = 16;

    void BuildNode(int idx, std::vector<int> indices, int depth)
    {
        if (indices.empty()) return;

        if (depth >= MaxDepth || (int)indices.size() <= MaxLeaf)
        {
            m_nodes[idx].objects    = std::move(indices);
            m_nodes[idx].firstChild = -1;
            return;
        }

        // Сохраняем до push_back, т.к. вектор может переаллоцироваться
        XMFLOAT3 c = m_nodes[idx].center;
        float    h = m_nodes[idx].halfSize * 0.5f;
        int    first = (int)m_nodes.size();
        m_nodes[idx].firstChild = first;

        const XMFLOAT3 offsets[8] = {
            {-h,-h,-h},{+h,-h,-h},{-h,+h,-h},{+h,+h,-h},
            {-h,-h,+h},{+h,-h,+h},{-h,+h,+h},{+h,+h,+h}
        };
        for (int i = 0; i < 8; ++i)
        {
            Node child;
            child.center     = { c.x + offsets[i].x, c.y + offsets[i].y, c.z + offsets[i].z };
            child.halfSize   = h;
            child.firstChild = -1;
            m_nodes.push_back(child);
        }

        // Распределяем объекты по 8 дочерним узлам
        std::vector<int> buckets[8];
        for (int objIdx : indices)
        {
            const XMFLOAT3& p = m_positions[objIdx];
            int ci = ((p.x >= c.x) ? 1 : 0) | ((p.y >= c.y) ? 2 : 0) | ((p.z >= c.z) ? 4 : 0);
            buckets[ci].push_back(objIdx);
        }
        for (int i = 0; i < 8; ++i)
            BuildNode(first + i, std::move(buckets[i]), depth + 1);
    }

    void QueryNode(int idx, const FrustumPlane planes[6], std::vector<int>& out) const
    {
        const Node& node = m_nodes[idx];
        // Расширяем AABB узла на радиус объектов для консервативного теста
        if (!AABBInFrustum(node.center, node.halfSize + m_objRadius, planes))
            return;

        if (node.firstChild == -1)
        {
            // Лист: проверяем отдельные объекты
            for (int objIdx : node.objects)
                if (SphereInFrustum(m_positions[objIdx], m_objRadius, planes))
                    out.push_back(objIdx);
        }
        else
        {
            for (int i = 0; i < 8; ++i)
                QueryNode(node.firstChild + i, planes, out);
        }
    }

    std::vector<Node>     m_nodes;
    std::vector<XMFLOAT3> m_positions;
    float                  m_objRadius = 1.f;
};
